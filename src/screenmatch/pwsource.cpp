#include "pwsource.h"

#include "bufferguard.h"

#ifdef MURASAKI_HAVE_PIPEWIRE

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/param/buffers.h>

#include <sys/mman.h>

#include <algorithm>
#include <cstring>

namespace murasaki::screenmatch {

struct PipeWireFrameSource::Impl {
    struct Stream {
        Impl *owner = nullptr;
        int index = 0;
        pw_stream *stream = nullptr;
        spa_hook listener{};
        spa_video_info_raw raw{};
        bool negotiated = false;
        pw_buffer *held = nullptr;
    };

    pw_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    spa_hook coreListener{};
    std::vector<std::unique_ptr<Stream>> streams;
    bool allowDmaBuf = false;
    bool failed = false;
    QString failure;
    bool entered = false;
};

namespace {

bool knownFormat(spa_video_format f, ScreenPixelFormat *out)
{
    switch (f) {
    case SPA_VIDEO_FORMAT_BGRx:
    case SPA_VIDEO_FORMAT_BGRA: *out = ScreenPixelFormat::Bgra32; return true;
    case SPA_VIDEO_FORMAT_RGBx:
    case SPA_VIDEO_FORMAT_RGBA: *out = ScreenPixelFormat::Rgba32; return true;
    case SPA_VIDEO_FORMAT_RGB: *out = ScreenPixelFormat::Rgb24; return true;
    default: return false;
    }
}

void onParamChanged(void *data, uint32_t id, const spa_pod *param)
{
    auto *s = static_cast<PipeWireFrameSource::Impl::Stream *>(data);
    if (!param || id != SPA_PARAM_Format)
        return;
    spa_video_info info{};
    if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0 || info.media_type != SPA_MEDIA_TYPE_video || info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    if (spa_format_video_raw_parse(param, &s->raw) < 0)
        return;
    ScreenPixelFormat f;
    s->negotiated = knownFormat(s->raw.format, &f) && s->raw.size.width > 0 && s->raw.size.height > 0 && s->raw.size.width <= 16384 && s->raw.size.height <= 16384;
    // What buffers the server may use: memory we can read (memfd, pointer), and a dma-buf only when we were started to take one.
    uint8_t buffer[512];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    int types = (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr);
    if (s->owner->allowDmaBuf)
        types |= 1 << SPA_DATA_DmaBuf;
    const spa_pod *params[1];
    params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
                                                                       SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types)));
    pw_stream_update_params(s->stream, params, 1);
}

void onProcess(void *data)
{
    auto *s = static_cast<PipeWireFrameSource::Impl::Stream *>(data);
    pw_buffer *newest = nullptr;
    while (pw_buffer *b = pw_stream_dequeue_buffer(s->stream)) {
        if (newest)
            pw_stream_queue_buffer(s->stream, newest); // an older one: back to the server unread
        newest = b;
    }
    if (!newest)
        return;
    if (s->held)
        pw_stream_queue_buffer(s->stream, s->held);
    s->held = newest;
}

void onStreamState(void *data, pw_stream_state, pw_stream_state state, const char *error)
{
    auto *s = static_cast<PipeWireFrameSource::Impl::Stream *>(data);
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        s->owner->failed = true;
        s->owner->failure = QString::fromUtf8(error ? error : pw_stream_state_as_string(state));
    }
}

void onRemoveBuffer(void *data, pw_buffer *buffer)
{
    auto *s = static_cast<PipeWireFrameSource::Impl::Stream *>(data);
    if (s->held == buffer)
        s->held = nullptr;
}

const pw_stream_events kStreamEvents = [] {
    pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = onStreamState;
    e.param_changed = onParamChanged;
    e.process = onProcess;
    e.remove_buffer = onRemoveBuffer;
    return e;
}();

void onCoreError(void *data, uint32_t, int, int res, const char *message)
{
    auto *impl = static_cast<PipeWireFrameSource::Impl *>(data);
    if (res == -EPIPE) { // the remote is gone
        impl->failed = true;
        impl->failure = QStringLiteral("the PipeWire remote closed");
    } else if (message) {
        impl->failure = QString::fromUtf8(message);
    }
}

const pw_core_events kCoreEvents = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.error = onCoreError;
    return e;
}();

} // namespace

PipeWireFrameSource::PipeWireFrameSource() : m_impl(std::make_unique<Impl>()) {}

PipeWireFrameSource::~PipeWireFrameSource()
{
    if (!m_impl)
        return;
    for (auto &s : m_impl->streams) {
        if (!s->stream)
            continue;
        if (s->held)
            pw_stream_queue_buffer(s->stream, s->held);
        spa_hook_remove(&s->listener);
        pw_stream_destroy(s->stream);
    }
    if (m_impl->core) {
        spa_hook_remove(&m_impl->coreListener);
        pw_core_disconnect(m_impl->core);
    }
    if (m_impl->context)
        pw_context_destroy(m_impl->context);
    if (m_impl->loop) {
        if (m_impl->entered)
            pw_loop_leave(m_impl->loop);
        pw_loop_destroy(m_impl->loop);
    }
}

std::unique_ptr<PipeWireFrameSource> PipeWireFrameSource::create(int pwFd, const std::vector<unsigned> &nodes, int fps, bool allowDmaBuf, QString *error)
{
    const auto fail = [&](const QString &why) -> std::unique_ptr<PipeWireFrameSource> {
        if (error)
            *error = why;
        return nullptr;
    };
    static bool initialised = false;
    if (!initialised) {
        pw_init(nullptr, nullptr);
        initialised = true;
    }
    std::unique_ptr<PipeWireFrameSource> source(new PipeWireFrameSource());
    Impl &impl = *source->m_impl;
    impl.allowDmaBuf = allowDmaBuf;
    impl.loop = pw_loop_new(nullptr);
    if (!impl.loop)
        return fail(QStringLiteral("no PipeWire loop"));
    pw_loop_enter(impl.loop);
    impl.entered = true;
    impl.context = pw_context_new(impl.loop, nullptr, 0);
    if (!impl.context)
        return fail(QStringLiteral("no PipeWire context"));
    impl.core = pw_context_connect_fd(impl.context, pwFd, nullptr, 0); // takes the descriptor
    if (!impl.core)
        return fail(QStringLiteral("the descriptor is not a PipeWire remote"));
    pw_core_add_listener(impl.core, &impl.coreListener, &kCoreEvents, &impl);

    const int rate = std::clamp(fps, 1, 60);
    for (const unsigned node : nodes) {
        auto s = std::make_unique<Impl::Stream>();
        s->owner = &impl;
        s->index = int(impl.streams.size());
        pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
        s->stream = pw_stream_new(impl.core, "murasaki-screenmatch", props);
        if (!s->stream)
            return fail(QStringLiteral("no PipeWire stream"));
        pw_stream_add_listener(s->stream, &s->listener, &kStreamEvents, s.get());
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const spa_rectangle def = SPA_RECTANGLE(1920, 1080), minSize = SPA_RECTANGLE(1, 1), maxSize = SPA_RECTANGLE(16384, 16384);
        const spa_fraction wanted = SPA_FRACTION(uint32_t(rate), 1), zero = SPA_FRACTION(0, 1), top = SPA_FRACTION(60, 1);
        const spa_pod *params[1];
        params[0] = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(6, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA, SPA_VIDEO_FORMAT_RGB),
            SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def, &minSize, &maxSize), SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&wanted, &zero, &top)));
        const int result = pw_stream_connect(s->stream, PW_DIRECTION_INPUT, node, static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS), params, 1);
        if (result < 0)
            return fail(QStringLiteral("cannot connect to the screen cast stream"));
        impl.streams.push_back(std::move(s));
    }
    return source;
}

int PipeWireFrameSource::pollFd() const
{
    return int(pw_loop_get_fd(m_impl->loop));
}

void PipeWireFrameSource::iterate()
{
    pw_loop_iterate(m_impl->loop, 0);
}

bool PipeWireFrameSource::failed() const
{
    return m_impl->failed;
}

QString PipeWireFrameSource::failure() const
{
    return m_impl->failure;
}

int PipeWireFrameSource::streamCount() const
{
    return int(m_impl->streams.size());
}

bool PipeWireFrameSource::acquire(int stream, Frame *frame)
{
    if (stream < 0 || stream >= int(m_impl->streams.size()))
        return false;
    Impl::Stream &s = *m_impl->streams[size_t(stream)];
    if (!s.negotiated || !s.held || !s.held->buffer || s.held->buffer->n_datas < 1)
        return false;
    const spa_data &d = s.held->buffer->datas[0];
    // (the chunk is read once into locals: the server may change it under us, and the checks and the use must see the same numbers)
    BufferInput in;
    in.dmaBuf = d.type == SPA_DATA_DmaBuf;
    in.allowDmaBuf = m_impl->allowDmaBuf;
    in.data = d.data;
    in.mapFailed = d.data == MAP_FAILED;
    in.maxSize = d.maxsize;
    if (d.chunk) {
        in.hasChunk = true;
        in.corrupted = (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED) != 0;
        in.offset = d.chunk->offset;
        in.size = d.chunk->size;
        in.stride = d.chunk->stride;
    }
    in.formatKnown = knownFormat(s.raw.format, &in.format);
    in.width = s.raw.size.width;
    in.height = s.raw.size.height;
    return checkBuffer(in, frame) == BufferVerdict::Ok;
}

void PipeWireFrameSource::release(int)
{
    // The frame is held until a newer one replaces it (see the header).
}

} // namespace murasaki::screenmatch

#else // no PipeWire

namespace murasaki::screenmatch {

struct PipeWireFrameSource::Impl {};
PipeWireFrameSource::PipeWireFrameSource() = default;
PipeWireFrameSource::~PipeWireFrameSource() = default;
std::unique_ptr<PipeWireFrameSource> PipeWireFrameSource::create(int, const std::vector<unsigned> &, int, bool, QString *error)
{
    if (error)
        *error = QStringLiteral("this build has no PipeWire");
    return nullptr;
}
int PipeWireFrameSource::pollFd() const { return -1; }
void PipeWireFrameSource::iterate() {}
bool PipeWireFrameSource::failed() const { return false; }
QString PipeWireFrameSource::failure() const { return {}; }
int PipeWireFrameSource::streamCount() const { return 0; }
bool PipeWireFrameSource::acquire(int, Frame *) { return false; }
void PipeWireFrameSource::release(int) {}

} // namespace murasaki::screenmatch

#endif
