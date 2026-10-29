#include "audiocapture.h"

#include "audiospectrum.h"

#include <QLoggingCategory>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <vector>

#ifdef MURASAKI_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace murasaki {

namespace {

Q_LOGGING_CATEGORY(lcAudio, "murasaki.audio")

constexpr int kPublishIntervalMs = 33;
constexpr int kCaptureRate = 48000;
constexpr int kInitialBackoffMs = 2000;
constexpr int kMaxBackoffMs = 30000;
constexpr qsizetype kStagingFrames = 4096; // far more than one publish interval delivers

} // namespace

struct AudioCaptureImpl
{
    // The realtime callback only appends here; the GUI thread drains it and does the FFT outside the lock.
    std::mutex mutex;
    std::array<float, kStagingFrames * 2> staging{};
    qsizetype stagedFrames = 0;
    std::vector<float> drained = std::vector<float>(kStagingFrames * 2);

    AudioSpectrum spectrum{kCaptureRate}; // GUI thread only
    std::atomic<quint64> generation{0};
    std::atomic<bool> closing{false}; // pw_stream_destroy reports UNCONNECTED; that is not a failure
    std::atomic<bool> streamed{false}; // the stream reached STREAMING at least once
    std::function<void(quint64)> onFailure; // queues handleStreamFailure() on the GUI thread
#ifdef MURASAKI_HAVE_PIPEWIRE
    pw_thread_loop *loop = nullptr;
    pw_stream *stream = nullptr;
#endif

    void append(const char *bytes, qsizetype frames)
    {
        if (frames > kStagingFrames) {
            bytes += (frames - kStagingFrames) * 2 * static_cast<qsizetype>(sizeof(float));
            frames = kStagingFrames;
        }
        std::lock_guard lock(mutex);
        if (stagedFrames + frames > kStagingFrames) {
            // Overflow means the GUI thread stalled; keep the newest audio.
            const qsizetype keep = kStagingFrames - frames;
            const qsizetype drop = stagedFrames - keep;
            std::memmove(staging.data(), staging.data() + drop * 2, static_cast<size_t>(keep) * 2 * sizeof(float));
            stagedFrames = keep;
        }
        std::memcpy(staging.data() + stagedFrames * 2, bytes, static_cast<size_t>(frames) * 2 * sizeof(float));
        stagedFrames += frames;
    }

    // Moves staged audio into the spectrum and returns the result.
    std::array<float, AudioSpectrum::kOutputSize> drainAndCompute()
    {
        qsizetype frames = 0;
        {
            std::lock_guard lock(mutex);
            frames = stagedFrames;
            std::memcpy(drained.data(), staging.data(), static_cast<size_t>(frames) * 2 * sizeof(float));
            stagedFrames = 0;
        }
        spectrum.pushInterleaved(drained.data(), frames);
        return spectrum.compute();
    }

    void clear()
    {
        {
            std::lock_guard lock(mutex);
            stagedFrames = 0;
        }
        spectrum.reset();
    }
};

#ifdef MURASAKI_HAVE_PIPEWIRE

namespace {

void onProcess(void *data)
{
    auto *impl = static_cast<AudioCaptureImpl *>(data);
    pw_buffer *buffer = pw_stream_dequeue_buffer(impl->stream);
    if (!buffer)
        return;
    const spa_buffer *buf = buffer->buffer;
    if (buf && buf->n_datas > 0 && buf->datas[0].data && buf->datas[0].chunk) {
        const spa_data &d = buf->datas[0];
        const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
        const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
        constexpr uint32_t frameBytes = 2 * sizeof(float);
        const uint32_t frames = size / frameBytes; // whole frames only
        if (frames > 0)
            impl->append(static_cast<const char *>(d.data) + offset, frames);
    }
    pw_stream_queue_buffer(impl->stream, buffer);
}

void onStateChanged(void *data, pw_stream_state, pw_stream_state state, const char *error)
{
    auto *impl = static_cast<AudioCaptureImpl *>(data);
    if (impl->closing)
        return;
    if (state == PW_STREAM_STATE_STREAMING) {
        impl->streamed = true;
        return;
    }
    if (state != PW_STREAM_STATE_ERROR && state != PW_STREAM_STATE_UNCONNECTED)
        return;
    qCWarning(lcAudio) << "PipeWire capture stream stopped:" << (error ? error : pw_stream_state_as_string(state));
    impl->onFailure(impl->generation.load());
}

const pw_stream_events kStreamEvents = [] {
    pw_stream_events events{};
    events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = onStateChanged;
    events.process = onProcess;
    return events;
}();

void ensurePipewireInit()
{
    static std::once_flag once;
    std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

} // namespace

#endif // MURASAKI_HAVE_PIPEWIRE

AudioCapture::AudioCapture(QObject *parent)
    : QObject(parent)
    , m_impl(std::make_unique<AudioCaptureImpl>())
    , m_backoffMs(kInitialBackoffMs)
{
    m_impl->onFailure = [this](quint64 generation) {
        QMetaObject::invokeMethod(this, [this, generation] { handleStreamFailure(generation); }, Qt::QueuedConnection);
    };
    m_timer.setInterval(kPublishIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &AudioCapture::publish);
    m_retryTimer.setSingleShot(true);
    connect(&m_retryTimer, &QTimer::timeout, this, [this] {
        if (m_wanted && !m_active)
            attempt();
    });
}

AudioCapture::~AudioCapture()
{
    m_wanted = false;
    stop();
}

void AudioCapture::setActive(bool active)
{
    if (active == m_wanted)
        return;
    m_wanted = active;
    if (!active) {
        m_retryTimer.stop();
        stop();
        emitSilence();
        return;
    }
    m_backoffMs = kInitialBackoffMs;
    attempt();
}

bool AudioCapture::attempt()
{
    if (start()) {
        m_active = true;
        m_timer.start();
        return true;
    }
    stop();
    emitSilence();
    scheduleRetry();
    return false;
}

void AudioCapture::scheduleRetry()
{
    m_retryTimer.start(m_backoffMs);
    m_backoffMs = std::min(m_backoffMs * 2, kMaxBackoffMs);
}

bool AudioCapture::start()
{
#ifdef MURASAKI_HAVE_PIPEWIRE
    ensurePipewireInit();
    m_impl->generation.fetch_add(1);
    m_impl->closing = false;
    m_impl->streamed = false;
    m_impl->clear();

    m_impl->loop = pw_thread_loop_new("murasaki-audio", nullptr);
    if (!m_impl->loop) {
        qCWarning(lcAudio) << "cannot create the PipeWire thread loop";
        return false;
    }
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture",
                                             PW_KEY_MEDIA_ROLE, "Music", PW_KEY_STREAM_CAPTURE_SINK, "true",
                                             PW_KEY_NODE_NAME, "murasaki", PW_KEY_APP_NAME, "murasaki",
                                             PW_KEY_NODE_LATENCY, "1024/48000", nullptr);
    if (!props) {
        qCWarning(lcAudio) << "cannot allocate PipeWire stream properties";
        return false;
    }
    m_impl->stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_impl->loop), "murasaki", props, &kStreamEvents,
                                          m_impl.get());
    if (!m_impl->stream) {
        qCWarning(lcAudio) << "cannot reach the PipeWire daemon; audio-reactive wallpapers stay silent";
        return false;
    }
    if (pw_thread_loop_start(m_impl->loop) < 0) {
        qCWarning(lcAudio) << "cannot start the PipeWire thread loop";
        return false;
    }

    uint8_t buffer[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = kCaptureRate;
    info.channels = 2;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod *params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};

    pw_thread_loop_lock(m_impl->loop);
    const int result = pw_stream_connect(
        m_impl->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS),
        params, 1);
    pw_thread_loop_unlock(m_impl->loop);
    if (result < 0) {
        qCWarning(lcAudio) << "pw_stream_connect failed:" << strerror(-result);
        return false;
    }
    return true;
#else
    qCWarning(lcAudio) << "built without PipeWire; audio-reactive wallpapers stay silent";
    return false;
#endif
}

void AudioCapture::stop()
{
    m_timer.stop();
    m_active = false;
    m_impl->closing = true;
    m_impl->generation.fetch_add(1);
#ifdef MURASAKI_HAVE_PIPEWIRE
    // The loop thread must be gone before the stream it drives is destroyed.
    if (m_impl->loop)
        pw_thread_loop_stop(m_impl->loop);
    if (m_impl->stream) {
        pw_stream_destroy(m_impl->stream);
        m_impl->stream = nullptr;
    }
    if (m_impl->loop) {
        pw_thread_loop_destroy(m_impl->loop);
        m_impl->loop = nullptr;
    }
#endif
    m_impl->clear();
}

void AudioCapture::handleStreamFailure(quint64 generation)
{
    if (generation != m_impl->generation.load() || !m_wanted || !m_active)
        return;
    // A stream that did run was healthy, so the next outage starts again from the short delay.
    if (m_impl->streamed)
        m_backoffMs = kInitialBackoffMs;
    stop();
    emitSilence();
    scheduleRetry();
}

void AudioCapture::publish()
{
    const auto values = m_impl->drainAndCompute();
    emit spectrumChanged(QList<float>(values.begin(), values.end()));
}

void AudioCapture::emitSilence()
{
    emit spectrumChanged(QList<float>(AudioSpectrum::kOutputSize, 0.0f));
}

} // namespace murasaki
