#include "player.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <chrono>
#include <thread>
#include <vector>

#include <time.h>

#ifdef MURASAKI_HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace murasaki::audio {

namespace {

class PacedPlayer final : public Player
{
public:
    explicit PacedPlayer(std::size_t step) : m_step(step) {}
    ~PacedPlayer() override { stop(); }

    bool start(Render render, std::string *) override
    {
        stop();
        m_render = std::move(render);
        m_run = true;
        m_active = false;
        m_thread = std::thread([this] { loop(); });
        return true;
    }
    void setActive(bool active) override { m_active = active; }
    void stop() override
    {
        m_run = false;
        if (m_thread.joinable())
            m_thread.join();
    }

private:
    void loop()
    {
        std::vector<float> buffer(m_step * 2);
        timespec next{};
        clock_gettime(CLOCK_MONOTONIC, &next);
        const long long stepNs = static_cast<long long>(m_step) * 1'000'000'000LL / 48000;
        while (m_run) {
            long long ns = next.tv_nsec + stepNs;
            next.tv_sec += ns / 1'000'000'000LL;
            next.tv_nsec = ns % 1'000'000'000LL;
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
            if (m_active && m_render)
                m_render(buffer.data(), m_step);
        }
    }

    std::size_t m_step;
    Render m_render;
    std::thread m_thread;
    std::atomic<bool> m_run{false};
    std::atomic<bool> m_active{false};
};

} // namespace

std::unique_ptr<Player> makePacedPlayer(std::size_t stepFrames)
{
    return std::make_unique<PacedPlayer>(stepFrames);
}

#ifdef MURASAKI_HAVE_PIPEWIRE

namespace {

constexpr std::uint32_t kStride = 2 * sizeof(float);

class PipeWirePlayer final : public Player
{
public:
    ~PipeWirePlayer() override { stop(); }

    bool start(Render render, std::string *error) override
    {
        stop();
        pw_init(nullptr, nullptr);
        m_inited = true;
        m_render = std::move(render);
        m_loop = pw_thread_loop_new("murasaki-petaudio", nullptr);
        if (!m_loop || pw_thread_loop_start(m_loop) != 0) {
            if (error)
                *error = "PipeWire thread loop unavailable";
            return false;
        }
        pw_thread_loop_lock(m_loop);
        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Event",
                                                 PW_KEY_APP_NAME, "Murasaki pets", PW_KEY_NODE_NAME, "murasaki-pets-audio", PW_KEY_NODE_DESCRIPTION,
                                                 "Murasaki desktop pets", PW_KEY_NODE_LATENCY, "512/48000", nullptr);
        m_events = {};
        m_events.version = PW_VERSION_STREAM_EVENTS;
        m_events.process = &PipeWirePlayer::onProcess;
        m_events.state_changed = &PipeWirePlayer::onState;
        m_stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), "pets", props, &m_events, this);
        bool ok = m_stream != nullptr;
        if (ok) {
            std::uint8_t buffer[1024];
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = 48000, .channels = 2);
            const spa_pod *params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
            ok = pw_stream_connect(m_stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                                   static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS
                                                                | PW_STREAM_FLAG_INACTIVE),
                                   params, 1)
                == 0;
        }
        pw_thread_loop_unlock(m_loop);
        if (!ok && error)
            *error = "cannot create the PipeWire stream (is PipeWire running?)";
        if (!ok)
            stop();
        return ok;
    }

    void setActive(bool active) override
    {
        if (!m_loop || !m_stream)
            return;
        pw_thread_loop_lock(m_loop);
        pw_stream_set_active(m_stream, active);
        pw_thread_loop_unlock(m_loop);
    }

    void stop() override
    {
        if (m_loop) {
            pw_thread_loop_lock(m_loop);
            if (m_stream) {
                pw_stream_destroy(m_stream);
                m_stream = nullptr;
            }
            pw_thread_loop_unlock(m_loop);
            pw_thread_loop_stop(m_loop);
            pw_thread_loop_destroy(m_loop);
            m_loop = nullptr;
        }
        if (m_inited) {
            pw_deinit();
            m_inited = false;
        }
    }

    long long latencyNs() const override { return m_latencyNs.load(std::memory_order_relaxed); }
    bool dead() const override { return m_dead.load(std::memory_order_relaxed); }

private:
    static void onProcess(void *data)
    {
        auto *self = static_cast<PipeWirePlayer *>(data);
        pw_buffer *buffer = pw_stream_dequeue_buffer(self->m_stream);
        if (!buffer)
            return;
        spa_data &d = buffer->buffer->datas[0];
        if (!d.data) {
            pw_stream_queue_buffer(self->m_stream, buffer);
            return;
        }
        std::size_t frames = d.maxsize / kStride;
        if (buffer->requested > 0)
            frames = std::min<std::size_t>(frames, buffer->requested);
        if (self->m_render)
            self->m_render(static_cast<float *>(d.data), frames);
        else
            std::fill_n(static_cast<float *>(d.data), frames * 2, 0.0f);
        d.chunk->offset = 0;
        d.chunk->stride = kStride;
        d.chunk->size = static_cast<std::uint32_t>(frames * kStride);
        pw_time time{};
        if (pw_stream_get_time_n(self->m_stream, &time, sizeof time) == 0 && time.rate.denom > 0)
            self->m_latencyNs.store(static_cast<long long>(time.delay) * 1'000'000'000LL * time.rate.num / time.rate.denom, std::memory_order_relaxed);
        pw_stream_queue_buffer(self->m_stream, buffer);
    }

    static void onState(void *data, pw_stream_state before, pw_stream_state state, const char *error)
    {
        auto *self = static_cast<PipeWirePlayer *>(data);
        if (state == PW_STREAM_STATE_ERROR)
            std::fprintf(stderr, "murasaki-petaudio: stream error: %s\n", error ? error : "error");
        // An error, or a stream that was connected and is not any more (the server went away): this player is finished.
        if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && before != PW_STREAM_STATE_UNCONNECTED))
            self->m_dead.store(true, std::memory_order_relaxed);
    }

    pw_thread_loop *m_loop = nullptr;
    pw_stream *m_stream = nullptr;
    pw_stream_events m_events{};
    Render m_render;
    bool m_inited = false;
    std::atomic<long long> m_latencyNs{0};
    std::atomic<bool> m_dead{false};
};

} // namespace

std::unique_ptr<Player> makePipeWirePlayer()
{
    return std::make_unique<PipeWirePlayer>();
}

#else

std::unique_ptr<Player> makePipeWirePlayer()
{
    return nullptr;
}

#endif

} // namespace murasaki::audio
