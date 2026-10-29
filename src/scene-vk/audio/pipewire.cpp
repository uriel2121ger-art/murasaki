#include "audio/backend.h"

#include "util/log.h"

#ifdef MURASAKI_SCENE_VK_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <cstring>
#include <mutex>

namespace murasaki::scene {

namespace {

constexpr uint32_t kRate = 48000;
constexpr uint32_t kStride = 2 * sizeof(float);

class PipeWireBackend final : public AudioBackend
{
public:
    PipeWireBackend()
    {
        pw_init(nullptr, nullptr);
        m_loop = pw_thread_loop_new("murasaki-audio", nullptr);
        if (m_loop) {
            pw_thread_loop_start(m_loop);
        }
    }

    ~PipeWireBackend() override
    {
        stopPlayback();
        stopCapture();
        if (m_loop) {
            pw_thread_loop_stop(m_loop);
            pw_thread_loop_destroy(m_loop);
        }
        pw_deinit();
    }

    bool startPlayback(Pull pull, std::string *error) override { return start(m_playback, true, std::move(pull), {}, error); }
    bool startCapture(Push push, std::string *error) override { return start(m_capture, false, {}, std::move(push), error); }
    void stopPlayback() override { stop(m_playback); }
    void stopCapture() override { stop(m_capture); }

private:
    struct Stream {
        PipeWireBackend *owner = nullptr;
        pw_stream *stream = nullptr;
        spa_hook listener{};
        pw_stream_events events{};
        Pull pull;
        Push push;
        bool playback = false;
    };

    static void onProcess(void *data)
    {
        auto *s = static_cast<Stream *>(data);
        pw_buffer *buffer = pw_stream_dequeue_buffer(s->stream);
        if (!buffer) {
            return;
        }
        spa_data &d = buffer->buffer->datas[0];
        if (!d.data) {
            pw_stream_queue_buffer(s->stream, buffer);
            return;
        }
        if (s->playback) {
            size_t frames = d.maxsize / kStride;
            if (buffer->requested > 0) {
                frames = std::min<size_t>(frames, buffer->requested);
            }
            if (s->pull) {
                s->pull(static_cast<float *>(d.data), frames);
            } else {
                std::memset(d.data, 0, frames * kStride);
            }
            d.chunk->offset = 0;
            d.chunk->stride = kStride;
            d.chunk->size = static_cast<uint32_t>(frames * kStride);
        } else if (s->push && d.chunk->size > 0) {
            const auto *base = static_cast<const uint8_t *>(d.data) + d.chunk->offset;
            s->push(reinterpret_cast<const float *>(base), d.chunk->size / kStride);
        }
        pw_stream_queue_buffer(s->stream, buffer);
    }

    static void onState(void *data, pw_stream_state, pw_stream_state state, const char *error)
    {
        auto *s = static_cast<Stream *>(data);
        if (state == PW_STREAM_STATE_ERROR) {
            logError("audio {} stream: {}", s->playback ? "playback" : "capture", error ? error : "error");
        }
    }

    bool start(Stream &s, bool playback, Pull pull, Push push, std::string *error)
    {
        if (!m_loop) {
            if (error) {
                *error = "PipeWire thread loop unavailable";
            }
            return false;
        }
        stop(s);
        pw_thread_loop_lock(m_loop);
        s.owner = this;
        s.playback = playback;
        s.pull = std::move(pull);
        s.push = std::move(push);
        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, playback ? "Playback" : "Capture",
                                                 PW_KEY_MEDIA_ROLE, playback ? "Music" : "Production", PW_KEY_APP_NAME, "Murasaki scene",
                                                 PW_KEY_NODE_NAME, playback ? "murasaki-scene-audio" : "murasaki-scene-spectrum", nullptr);
        if (!playback) {
            pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true"); // the monitor of the default output
            pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
        }
        s.events = {};
        s.events.version = PW_VERSION_STREAM_EVENTS;
        s.events.process = &PipeWireBackend::onProcess;
        s.events.state_changed = &PipeWireBackend::onState;
        s.stream = pw_stream_new_simple(pw_thread_loop_get_loop(m_loop), playback ? "scene-audio" : "scene-spectrum", props, &s.events, &s);
        bool ok = s.stream != nullptr;
        if (ok) {
            uint8_t buffer[1024];
            spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
            spa_audio_info_raw info = SPA_AUDIO_INFO_RAW_INIT(.format = SPA_AUDIO_FORMAT_F32, .rate = kRate, .channels = 2);
            const spa_pod *params[1] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
            ok = pw_stream_connect(s.stream, playback ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT, PW_ID_ANY,
                                   static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1)
                == 0;
        }
        pw_thread_loop_unlock(m_loop);
        if (!ok && error) {
            *error = "cannot create the PipeWire stream";
        }
        if (!ok) {
            stop(s);
        }
        return ok;
    }

    void setPlaybackActive(bool active) override
    {
        if (!m_loop || !m_playback.stream) {
            return;
        }
        pw_thread_loop_lock(m_loop);
        if (m_playback.stream) {
            pw_stream_set_active(m_playback.stream, active);
        }
        pw_thread_loop_unlock(m_loop);
    }

    void stop(Stream &s)
    {
        if (!m_loop || !s.stream) {
            return;
        }
        pw_thread_loop_lock(m_loop);
        pw_stream_destroy(s.stream);
        s.stream = nullptr;
        pw_thread_loop_unlock(m_loop);
        s.pull = nullptr;
        s.push = nullptr;
    }

    pw_thread_loop *m_loop = nullptr;
    Stream m_playback;
    Stream m_capture;
};

} // namespace

std::unique_ptr<AudioBackend> makePipeWireBackend()
{
    return std::make_unique<PipeWireBackend>();
}

} // namespace murasaki::scene

#else

namespace murasaki::scene {
std::unique_ptr<AudioBackend> makePipeWireBackend()
{
    return nullptr;
}
} // namespace murasaki::scene

#endif
