#include "videosource.h"

#include <QFile>
#include <QFileInfo>
#include <QSize>
#include <QVector>

#include <algorithm>
#include <chrono>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#ifdef MURASAKI_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}
#endif

namespace murasaki::import {

namespace {

// libavformat's names of the demuxers a video may come from: MP4/MOV (and 3GP, M4A, MJ2), Matroska/WebM, AVI, GIF and APNG. Nothing that reads other
// files (concat, HLS and DASH playlists, image2 globs, ffconcat) or opens a device, and no subtitle or raw format.
constexpr const char *kVideoContainers[] = {"mov", "mp4", "m4a", "3gp", "3g2", "mj2", "matroska", "webm", "avi", "gif", "apng"};
constexpr const char *kWhitelist = "mov,mp4,m4a,3gp,3g2,mj2,matroska,webm,avi,gif,apng";

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

} // namespace

QString validateVideoSettings(const VideoSettings &s)
{
    if (s.maxSide < kMinVideoSide || s.maxSide > kMaxCanvasSide)
        return QStringLiteral("videoMaxSide must be %1..%2").arg(kMinVideoSide).arg(kMaxCanvasSide);
    if (!std::isfinite(s.maxFps) || s.maxFps < kMinVideoFps || s.maxFps > kMaxVideoFps)
        return QStringLiteral("videoMaxFps must be %1..%2").arg(kMinVideoFps).arg(kMaxVideoFps);
    if (s.startMs < 0 || s.startMs > kMaxVideoStartMs)
        return QStringLiteral("videoStartMs must be 0..%1").arg(kMaxVideoStartMs);
    if (s.durationMs < 0 || s.durationMs > kMaxVideoStartMs)
        return QStringLiteral("videoDurationMs must be 0..%1").arg(kMaxVideoStartMs);
    if (s.maxFrames < 1 || s.maxFrames > kMaxFrames)
        return QStringLiteral("videoMaxFrames must be 1..%1").arg(kMaxFrames);
    if (s.deadlineMs < kMinDeadlineMs || s.deadlineMs > kMaxDeadlineMs)
        return QStringLiteral("videoDeadlineMs must be %1..%2").arg(kMinDeadlineMs).arg(kMaxDeadlineMs);
    return {};
}

bool videoContainerAllowed(const char *demuxerName)
{
    if (!demuxerName)
        return false;
    const std::string all(demuxerName);
    size_t from = 0;
    while (from <= all.size()) {
        size_t to = all.find(',', from);
        if (to == std::string::npos)
            to = all.size();
        const std::string one = all.substr(from, to - from);
        for (const char *allowed : kVideoContainers)
            if (one == allowed)
                return true;
        from = to + 1;
    }
    return false;
}

const char *videoContainerWhitelist() { return kWhitelist; }

#ifndef MURASAKI_HAVE_LIBAV

bool videoSourceAvailable() { return false; }
VideoInfo probeVideo(const QString &, const VideoSettings &, const VideoCallbacks &)
{
    VideoInfo info;
    info.error = named(kNotAVideo, QStringLiteral("this build has no libav"));
    return info;
}
VideoOutcome decodeVideo(const QString &, const VideoSettings &, FrameSink &, const VideoCallbacks &)
{
    VideoOutcome out;
    out.error = named(kNotAVideo, QStringLiteral("this build has no libav"));
    return out;
}

#else

bool videoSourceAvailable() { return true; }

namespace {

struct FormatCloser {
    void operator()(AVFormatContext *c) const { avformat_close_input(&c); }
};
struct CodecFree {
    void operator()(AVCodecContext *c) const { avcodec_free_context(&c); }
};
struct PacketFree {
    void operator()(AVPacket *p) const { av_packet_free(&p); }
};
struct FrameFree {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};
struct SwsFree {
    void operator()(SwsContext *c) const { sws_freeContext(c); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using SwsPtr = std::unique_ptr<SwsContext, SwsFree>;

constexpr int kProbeBytes = 64 * 1024;
constexpr int kProbeSizeBytes = 8 * 1024 * 1024;   // what find_stream_info may read
constexpr int kAnalyzeMicros = 5 * 1000 * 1000;    // and how much of the stream it may look at
constexpr int kMaxStreams = 32;
constexpr int kFpsProbeFrames = 32;
constexpr double kMaxClockMs = 1e12;     // a timestamp past 30 years is damage
constexpr int kMaxBadPackets = 16;       // in a row, before a damaged stream is given up
constexpr int kDecoderThreads = 2;
constexpr int kMaxDelayMs = 60000; // a frame shows for a minute at most (the spec's kMaxFrameMs)

QString averror(int code)
{
    char buffer[128];
    av_strerror(code, buffer, sizeof buffer);
    return QString::fromUtf8(buffer);
}

void quiet()
{
    static const bool once = [] {
        av_log_set_level(AV_LOG_ERROR);
        return true;
    }();
    Q_UNUSED(once);
}

bool hasAlphaFormat(int format)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(format));
    return d && (d->flags & AV_PIX_FMT_FLAG_ALPHA);
}

// What a file that failed to open is: a container this importer does not take (named), or not a video at all.
QString whyNotOpened(const QString &file, int code)
{
    QFile f(file);
    if (f.open(QIODevice::ReadOnly)) {
        QByteArray head = f.read(kProbeBytes);
        head.append(QByteArray(AVPROBE_PADDING_SIZE, '\0'));
        AVProbeData probe{};
        const QByteArray name = QFile::encodeName(file);
        probe.filename = name.constData();
        probe.buf = reinterpret_cast<unsigned char *>(head.data());
        probe.buf_size = int(head.size()) - AVPROBE_PADDING_SIZE;
        int score = 0;
        const AVInputFormat *found = av_probe_input_format3(&probe, 1, &score);
        if (found && score > AVPROBE_SCORE_RETRY && !videoContainerAllowed(found->name))
            return named(kVideoContainerNotAllowed, QString::fromLatin1(found->name).left(40));
    }
    return named(kNotAVideo, averror(code));
}

// The decoders a video may use, by codec id and the one decoder preferred for it (software only: nothing hardware, experimental, or that starts
// another process). The list is the plan's plus the codecs that carry alpha in a MOV or AVI. AV1 is libdav1d (fuzzed hard, fast, memory bounded by
// max_pixels), then libaom; FFmpeg's own AV1 decoder only drives hardware. VP8 and VP9 are the native decoders, and libvpx when the stream is tagged
// with alpha, which only libvpx reads.
struct DecoderRule {
    AVCodecID id;
    const char *names[3];
};
constexpr DecoderRule kDecoders[] = {
    {AV_CODEC_ID_H264, {"h264", nullptr, nullptr}},
    {AV_CODEC_ID_HEVC, {"hevc", nullptr, nullptr}},
    {AV_CODEC_ID_VP8, {"vp8", nullptr, nullptr}},
    {AV_CODEC_ID_VP9, {"vp9", nullptr, nullptr}},
    {AV_CODEC_ID_AV1, {"libdav1d", "libaom-av1", nullptr}},
    {AV_CODEC_ID_MPEG4, {"mpeg4", nullptr, nullptr}},
    {AV_CODEC_ID_MPEG2VIDEO, {"mpeg2video", nullptr, nullptr}},
    {AV_CODEC_ID_MPEG1VIDEO, {"mpeg1video", nullptr, nullptr}},
    {AV_CODEC_ID_PRORES, {"prores", nullptr, nullptr}},
    {AV_CODEC_ID_PNG, {"png", nullptr, nullptr}},
    {AV_CODEC_ID_APNG, {"apng", nullptr, nullptr}},
    {AV_CODEC_ID_GIF, {"gif", nullptr, nullptr}},
    {AV_CODEC_ID_MJPEG, {"mjpeg", nullptr, nullptr}},
    {AV_CODEC_ID_FFV1, {"ffv1", nullptr, nullptr}},
    {AV_CODEC_ID_QTRLE, {"qtrle", nullptr, nullptr}},
    {AV_CODEC_ID_UTVIDEO, {"utvideo", nullptr, nullptr}},
};

bool decoderAcceptable(const AVCodec *codec, AVCodecID id)
{
    if (!codec || codec->id != id || !av_codec_is_decoder(codec))
        return false;
    if (codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_EXPERIMENTAL))
        return false;
    const char *wrapper = codec->wrapper_name;
    return !wrapper || !std::strcmp(wrapper, "libvpx") || !std::strcmp(wrapper, "libdav1d") || !std::strcmp(wrapper, "libaom");
}

bool codecListed(AVCodecID id)
{
    for (const DecoderRule &rule : kDecoders)
        if (rule.id == id)
            return true;
    return false;
}

const AVCodec *chooseDecoder(AVCodecID id, bool wantAlphaVpx)
{
    if (wantAlphaVpx && (id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_VP8)) {
        const AVCodec *vpx = avcodec_find_decoder_by_name(id == AV_CODEC_ID_VP9 ? "libvpx-vp9" : "libvpx");
        if (decoderAcceptable(vpx, id))
            return vpx;
    }
    for (const DecoderRule &rule : kDecoders) {
        if (rule.id != id)
            continue;
        for (const char *name : rule.names) {
            if (!name)
                break;
            const AVCodec *codec = avcodec_find_decoder_by_name(name);
            if (decoderAcceptable(codec, id))
                return codec;
        }
    }
    return nullptr;
}

// Cancellation and the deadline for libavformat's own I/O (open, probe, every read): it asks this before and during a blocking call.
struct Guard {
    std::function<bool()> cancelled;
    std::chrono::steady_clock::time_point deadline;
    bool wasCancelled = false;
    bool timedOut = false;

    // True when the work must stop; remembers why.
    bool stop()
    {
        if (!wasCancelled && cancelled && cancelled())
            wasCancelled = true;
        if (!timedOut && std::chrono::steady_clock::now() >= deadline)
            timedOut = true;
        return wasCancelled || timedOut;
    }
    QString why() const { return wasCancelled ? named(kCancelled) : named(kVideoTimeout); }
    static int interrupt(void *self) { return static_cast<Guard *>(self)->stop() ? 1 : 0; }
};

// The file, opened here without ever blocking and checked on the descriptor, so that a path swapped for a FIFO or a device between the check and the open
// cannot hang libavformat (its own open() would wait for a writer); libav is given /proc/self/fd/N and so reads the very inode that was checked.
struct FdHolder {
    int fd = -1;
    FdHolder() = default;
    FdHolder(const FdHolder &) = delete;
    FdHolder &operator=(const FdHolder &) = delete;
    ~FdHolder()
    {
        if (fd >= 0)
            ::close(fd);
    }
};

struct Opened {
    FdHolder file;                // (destroyed last: the format reads through it)
    std::unique_ptr<Guard> guard; // before the format: the format holds a pointer to it
    FormatPtr format;
    AVStream *stream = nullptr;
    const AVCodec *codec = nullptr;
    bool hasAudio = false;
    bool alphaTagged = false; // Matroska says the stream carries an alpha channel next to the colour
    QStringList degraded;
};

bool pictureAcceptable(int width, int height)
{
    return width > 0 && height > 0 && width <= kMaxVideoSide && height <= kMaxVideoSide && qint64(width) * height <= kMaxVideoPixels;
}

// Everything up to a decoder: the checks that need no decoding, and nothing is allocated for a picture before its size has been checked.
bool openVideo(const QString &file, const VideoSettings &settings, const VideoCallbacks &callbacks, Opened *out, QString *error)
{
    quiet();
    const QFileInfo info(file);
    if (!info.exists()) {
        *error = named(kSourceNotFound);
        return false;
    }
    if (!info.isFile()) { // follows a symlink: a device, a pipe or a socket is not a file
        *error = named(kSourceNotRegular);
        return false;
    }
    if (info.size() > kMaxSourceBytes) {
        *error = named(kSourceOverLimit);
        return false;
    }
    out->file.fd = ::open(QFile::encodeName(file).constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (out->file.fd < 0 || ::fstat(out->file.fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        *error = named(out->file.fd < 0 ? kSourceNotFound : kSourceNotRegular);
        return false;
    }
    out->guard = std::make_unique<Guard>();
    out->guard->cancelled = callbacks.cancelled;
    out->guard->deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(settings.deadlineMs);
    AVFormatContext *raw = avformat_alloc_context();
    if (!raw) {
        *error = named(kHelperFailed, QStringLiteral("out of memory"));
        return false;
    }
    raw->interrupt_callback.callback = &Guard::interrupt;
    raw->interrupt_callback.opaque = out->guard.get();
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", kWhitelist, 0);
    av_dict_set_int(&options, "probesize", kProbeSizeBytes, 0);
    av_dict_set_int(&options, "analyzeduration", kAnalyzeMicros, 0);
    av_dict_set_int(&options, "fpsprobesize", kFpsProbeFrames, 0);
    av_dict_set_int(&options, "max_streams", kMaxStreams, 0);
    av_dict_set(&options, "enable_drefs", "0", 0); // an MP4 whose samples live in another file
    const QByteArray url = "file:/proc/self/fd/" + QByteArray::number(out->file.fd);
    const int opened = avformat_open_input(&raw, url.constData(), nullptr, &options); // frees raw on failure
    av_dict_free(&options);
    if (opened < 0) {
        *error = out->guard->stop() ? out->guard->why() : whyNotOpened(file, opened);
        return false;
    }
    out->format.reset(raw);
    if (!videoContainerAllowed(raw->iformat ? raw->iformat->name : nullptr)) { // the whitelist already said so; this is the check that does not depend on it
        *error = named(kVideoContainerNotAllowed, QString::fromLatin1(raw->iformat ? raw->iformat->name : "?").left(40));
        return false;
    }
    // Before any decoder exists: which stream is ours, from what the container says. A stream that is not a video this importer decodes gets no codec,
    // so find_stream_info does not open a decoder for it (audio, subtitles, attachments, data), and is discarded.
    int chosen = -1;
    bool tooLarge = false;
    QString largeDetail;
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        AVStream *st = raw->streams[i];
        AVCodecParameters *p = st->codecpar;
        if (p->codec_type == AVMEDIA_TYPE_AUDIO)
            out->hasAudio = true;
        const bool candidate = p->codec_type == AVMEDIA_TYPE_VIDEO && !(st->disposition & AV_DISPOSITION_ATTACHED_PIC) && codecListed(p->codec_id);
        if (candidate && !pictureAcceptable(p->width, p->height) && p->width > 0 && p->height > 0) {
            tooLarge = true; // what the container claims, which is checked again on what is decoded
            largeDetail = QStringLiteral("%1x%2").arg(p->width).arg(p->height);
        }
        if (candidate && chosen < 0 && pictureAcceptable(p->width, p->height)) {
            chosen = int(i);
            continue;
        }
        st->discard = AVDISCARD_ALL;
        p->codec_id = AV_CODEC_ID_NONE;
    }
    if (chosen < 0) {
        *error = tooLarge ? named(kVideoTooLarge, largeDetail) : named(kVideoNoStream);
        return false;
    }
    // The decoders find_stream_info opens for the chosen stream get the same picture cap as ours.
    std::vector<AVDictionary *> perStream(raw->nb_streams, nullptr);
    av_dict_set_int(&perStream[size_t(chosen)], "max_pixels", kMaxVideoPixels, 0);
    const int streams = avformat_find_stream_info(raw, perStream.data());
    for (AVDictionary *d : perStream)
        av_dict_free(&d);
    if (streams < 0) {
        *error = out->guard->stop() ? out->guard->why() : named(kNotAVideo, averror(streams));
        return false;
    }
    AVStream *stream = raw->streams[chosen];
    const AVCodecParameters *p = stream->codecpar;
    if (!pictureAcceptable(p->width, p->height)) { // the decoder's own opinion, after probing
        *error = named(kVideoTooLarge, QStringLiteral("%1x%2").arg(p->width).arg(p->height));
        return false;
    }
    out->stream = stream;
    const AVDictionaryEntry *alpha = av_dict_get(stream->metadata, "alpha_mode", nullptr, 0);
    out->alphaTagged = alpha && alpha->value && std::strcmp(alpha->value, "1") == 0;
    out->codec = chooseDecoder(p->codec_id, out->alphaTagged);
    if (out->alphaTagged && out->codec && !(std::strcmp(out->codec->name, "libvpx") == 0 || std::strcmp(out->codec->name, "libvpx-vp9") == 0))
        out->alphaTagged = false; // no decoder that keeps it: the video is imported opaque and says so
    if (!out->codec) {
        *error = named(kVideoDecoderMissing, QString::fromLatin1(avcodec_get_name(p->codec_id)));
        return false;
    }
    return true;
}

QSize outputSize(int width, int height, int maxSide)
{
    const double scale = std::min(1.0, double(maxSide) / std::max(width, height));
    return {std::max(1, int(std::lround(width * scale))), std::max(1, int(std::lround(height * scale)))};
}

// How swscale maps the frame's YUV to RGB: the stream's own tags, else BT.709 for HD and BT.601 for SD, and the frame's range (limited unless tagged
// full); swscale alone assumes BT.601 limited for everything.
void matchColors(SwsContext *scaler, const AVFrame *frame)
{
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    if (!d || (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)))
        return;
    int space = SWS_CS_DEFAULT;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        space = SWS_CS_ITU709;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        space = SWS_CS_ITU601;
        break;
    case AVCOL_SPC_SMPTE240M:
        space = SWS_CS_SMPTE240M;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        space = SWS_CS_BT2020;
        break;
    default:
        space = (frame->width >= 1280 || frame->height > 576) ? SWS_CS_ITU709 : SWS_CS_ITU601;
        break;
    }
    sws_setColorspaceDetails(scaler, sws_getCoefficients(space), frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0,
                             1 << 16, 1 << 16);
}

} // namespace

VideoInfo probeVideo(const QString &file, const VideoSettings &settings, const VideoCallbacks &callbacks)
{
    VideoInfo info;
    if (const QString bad = validateVideoSettings(settings); !bad.isEmpty()) {
        info.error = named(kSpecRefused, bad);
        return info;
    }
    Opened o;
    if (!openVideo(file, settings, callbacks, &o, &info.error))
        return info;
    info.hasAudio = o.hasAudio;
    const AVCodecParameters *p = o.stream->codecpar;
    info.container = QString::fromLatin1(o.format->iformat->name);
    info.codec = QString::fromLatin1(avcodec_get_name(p->codec_id));
    info.width = p->width;
    info.height = p->height;
    const QSize out = outputSize(p->width, p->height, settings.maxSide);
    info.outWidth = out.width();
    info.outHeight = out.height();
    const AVRational rate = o.stream->avg_frame_rate.num > 0 ? o.stream->avg_frame_rate : o.stream->r_frame_rate;
    info.fps = rate.num > 0 && rate.den > 0 ? av_q2d(rate) : 0;
    if (o.stream->duration != AV_NOPTS_VALUE && o.stream->duration > 0)
        info.durationMs = std::llround(o.stream->duration * av_q2d(o.stream->time_base) * 1000.0);
    else if (o.format->duration != AV_NOPTS_VALUE && o.format->duration > 0)
        info.durationMs = o.format->duration / 1000;
    info.hasAlpha = o.alphaTagged || hasAlphaFormat(p->format);
    if (!info.hasAlpha)
        info.degraded << named(kVideoOpaque);
    if (info.outWidth < info.width || info.outHeight < info.height)
        info.degraded << named(kVideoScaledDown);
    if (info.fps > settings.maxFps + 0.01)
        info.degraded << named(kVideoFpsLowered);
    if (info.durationMs >= 0) { // a claim: for the preview only, the decode counts what it gets
        const double keptFps = info.fps > 0 ? std::min(info.fps, settings.maxFps) : settings.maxFps;
        long long span = std::max<long long>(0, info.durationMs - settings.startMs);
        if (settings.durationMs > 0)
            span = std::min(span, settings.durationMs);
        info.estimatedFrames = int(std::min<double>(kMaxFrames * 4.0, std::ceil(span / 1000.0 * keptFps)));
    }
    info.ok = true;
    return info;
}

namespace {

// Seconds-to-milliseconds clock of the chosen stream: t = 0 is the stream's start (or its first frame when it does not say), never negative, never back.
class Clock
{
public:
    Clock(const AVStream *stream, double averageMs) : m_base(stream->time_base), m_start(stream->start_time), m_averageMs(averageMs) {}

    double next(const AVFrame *frame)
    {
        int64_t ts = frame->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE)
            ts = frame->pts;
        double ms;
        if (ts == AV_NOPTS_VALUE) {
            ms = m_last + (m_any ? m_averageMs : 0);
        } else {
            if (m_origin == AV_NOPTS_VALUE)
                m_origin = m_start != AV_NOPTS_VALUE && m_start <= ts ? m_start : ts;
            ms = (double(ts) - double(m_origin)) * av_q2d(m_base) * 1000.0; // in double: two int64 of opposite signs would overflow
        }
        if (!std::isfinite(ms) || ms < m_last)
            ms = m_last; // damaged or reordered timestamps never move time back
        ms = std::min(ms, kMaxClockMs);
        m_any = true;
        m_last = ms;
        return ms;
    }

private:
    AVRational m_base;
    int64_t m_start;
    int64_t m_origin = AV_NOPTS_VALUE;
    double m_averageMs;
    double m_last = 0;
    bool m_any = false;
};

class Decoder
{
public:
    Decoder(Opened &opened, const VideoSettings &settings, FrameSink &sink, const VideoCallbacks &callbacks)
        : m_o(opened), m_s(settings), m_sink(sink), m_cb(callbacks)
    {
    }

    VideoOutcome run()
    {
        const AVCodecParameters *p = m_o.stream->codecpar;
        m_context.reset(avcodec_alloc_context3(m_o.codec));
        if (!m_context || avcodec_parameters_to_context(m_context.get(), p) < 0)
            return fail(named(kVideoDecoderMissing));
        m_context->max_pixels = kMaxVideoPixels; // a frame buffer is never bigger than the cap, whatever an SPS or a header says later
        m_context->thread_count = kDecoderThreads;
        m_context->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        if (const int r = avcodec_open2(m_context.get(), m_o.codec, nullptr); r < 0)
            return fail(named(kVideoDecoderMissing, averror(r)));
        const AVRational rate = m_o.stream->avg_frame_rate.num > 0 ? m_o.stream->avg_frame_rate : m_o.stream->r_frame_rate;
        const double averageMs = rate.num > 0 && rate.den > 0 ? 1000.0 / av_q2d(rate) : 100.0;
        m_averageMs = std::clamp(averageMs, 1000.0 / kMaxVideoFps, 1000.0);
        m_clock = std::make_unique<Clock>(m_o.stream, m_averageMs);
        m_interval = 1000.0 / m_s.maxFps;
        m_out = outputSize(p->width, p->height, m_s.maxSide);
        m_stride = qsizetype(m_out.width()) * 4;
        m_pending.resize(m_stride * m_out.height());
        m_length = durationOf();

        PacketPtr packet(av_packet_alloc());
        FramePtr frame(av_frame_alloc());
        if (!packet || !frame)
            return fail(named(kHelperFailed, QStringLiteral("out of memory")));
        bool done = false;
        while (!done && !m_gaveUp) {
            if (m_o.guard->stop())
                return stopped();
            const int read = av_read_frame(m_o.format.get(), packet.get());
            if (read < 0) {
                if (m_o.guard->stop())
                    return stopped();
                if (read != AVERROR_EOF && !m_decodedAny)
                    return fail(named(kNotAVideo, averror(read)));
                break; // a truncated file keeps what was decoded before the damage
            }
            if (packet->stream_index != m_o.stream->index) {
                av_packet_unref(packet.get());
                continue;
            }
            const int sent = avcodec_send_packet(m_context.get(), packet.get());
            av_packet_unref(packet.get());
            if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF && !badPacket(averror(sent)))
                return fail(m_error);
            if (!drain(frame.get(), &done))
                return finishedWithError();
        }
        if (!done && !m_gaveUp && m_error.isEmpty()) {
            avcodec_send_packet(m_context.get(), nullptr);
            if (!drain(frame.get(), &done))
                return finishedWithError();
        }
        if (m_o.guard->stop())
            return stopped();
        return complete();
    }

private:
    long long durationOf() const
    {
        const AVStream *s = m_o.stream;
        if (s->duration != AV_NOPTS_VALUE && s->duration > 0)
            return std::llround(s->duration * av_q2d(s->time_base) * 1000.0);
        if (m_o.format->duration != AV_NOPTS_VALUE && m_o.format->duration > 0)
            return m_o.format->duration / 1000;
        return -1;
    }

    // A damaged packet is skipped; many in a row end the stream, and with nothing decoded yet that is an error. False: the import failed (m_error).
    bool badPacket(const QString &reason)
    {
        if (++m_bad <= kMaxBadPackets)
            return true;
        m_gaveUp = true;
        if (m_decodedAny)
            return true;
        m_error = named(kNotAVideo, reason);
        return false;
    }

    VideoOutcome fail(const QString &error)
    {
        VideoOutcome out;
        out.error = error;
        return out;
    }
    VideoOutcome stopped()
    {
        VideoOutcome out;
        out.cancelled = m_o.guard->wasCancelled;
        out.error = m_o.guard->why();
        return out;
    }
    VideoOutcome finishedWithError()
    {
        if (m_o.guard->wasCancelled || m_o.guard->timedOut)
            return stopped();
        VideoOutcome out;
        out.overBudget = m_overBudget;
        out.error = m_error;
        return out;
    }

    // Takes every frame the decoder has. Sets *done when the trim's end is reached; false when the import failed (m_error) or was cancelled.
    bool drain(AVFrame *frame, bool *done)
    {
        for (;;) {
            const int r = avcodec_receive_frame(m_context.get(), frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return true;
            if (r < 0) // a frame that cannot be decoded is skipped; the packets after it may be fine
                return badPacket(averror(r));
            m_bad = 0;
            const bool keep = onFrame(frame, done);
            av_frame_unref(frame);
            if (!keep)
                return false;
            if (*done)
                return true;
        }
    }

    bool onFrame(AVFrame *frame, bool *done)
    {
        m_decodedAny = true;
        if (++m_decoded > kMaxDecodedFrames) {
            m_error = named(kTooManyFrames, QStringLiteral("the video has more than %1 frames; trim it").arg(kMaxDecodedFrames));
            return false;
        }
        if (!pictureAcceptable(frame->width, frame->height)) {
            m_error = named(kVideoTooLarge, QStringLiteral("%1x%2").arg(frame->width).arg(frame->height));
            return false;
        }
        m_pixels += qint64(frame->width) * frame->height;
        if (m_pixels > kMaxDecodedPixels) {
            m_error = named(kTooManyFrames, QStringLiteral("the decoded pictures add up to over %1 pixels; trim the video or lower its size").arg(kMaxDecodedPixels));
            return false;
        }
        const double t = m_clock->next(frame);
        const double end = m_s.durationMs > 0 ? double(m_s.startMs + m_s.durationMs) : -1;
        if (end >= 0 && t >= end) {
            *done = true;
            return true;
        }
        if (t < double(m_s.startMs))
            return true;
        const double r = t - double(m_s.startMs);
        ++m_candidates;
        const double stepMs = frame->duration > 0 ? double(frame->duration) * av_q2d(m_o.stream->time_base) * 1000.0 : m_averageMs;
        m_endAt = r + std::clamp(stepMs, 1.0, double(kMaxDelayMs)); // where the video, or the trim, ends as far as the frames seen say
        if (m_haveKept && r < m_keptAt + m_interval - 1.0)
            return true; // dropped by the frame rate cap
        if (m_cb.progress && m_haveKept)
            m_cb.progress(m_length > 0 ? std::clamp(t / double(m_length), 0.0, 1.0) : -1.0, m_kept);
        if (m_haveKept && !emitPending(r))
            return false;
        if (m_o.guard->stop())
            return false;
        if (!convert(frame))
            return false;
        if (hasAlphaFormat(frame->format))
            m_alpha = true;
        m_haveKept = true;
        m_keptAt = r;
        return true;
    }

    // Straight RGBA at the output size. A picture with alpha that must be scaled goes through premultiplied RGBA, so that the colour of a transparent
    // neighbour does not bleed into the edge (swscale averages straight channels).
    bool convert(const AVFrame *frame)
    {
        const bool alpha = hasAlphaFormat(frame->format);
        const bool scaled = frame->width != m_out.width() || frame->height != m_out.height();
        if (!m_scaler || frame->format != m_format || frame->width != m_width || frame->height != m_height) {
            m_format = frame->format;
            m_width = frame->width;
            m_height = frame->height;
            m_toRgba.reset();
            m_premultiplied = alpha && scaled;
            SwsContext *first = nullptr;
            if (m_premultiplied) {
                first = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format), frame->width, frame->height, AV_PIX_FMT_RGBA, SWS_POINT,
                                       nullptr, nullptr, nullptr);
                if (!first) {
                    m_error = named(kHelperFailed, QStringLiteral("cannot convert the frames"));
                    return false;
                }
                m_toRgba.reset(first);
                matchColors(first, frame);
            }
            SwsContext *c = sws_getContext(frame->width, frame->height, m_premultiplied ? AV_PIX_FMT_RGBA : AVPixelFormat(frame->format), m_out.width(),
                                           m_out.height(), AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr);
            if (!c) {
                m_error = named(kHelperFailed, QStringLiteral("cannot convert the frames"));
                return false;
            }
            m_scaler.reset(c);
            if (!m_premultiplied)
                matchColors(c, frame);
        }
        uint8_t *destination[4] = {reinterpret_cast<uint8_t *>(m_pending.data()), nullptr, nullptr, nullptr};
        const int strides[4] = {int(m_stride), 0, 0, 0};
        if (!m_premultiplied) {
            sws_scale(m_scaler.get(), frame->data, frame->linesize, 0, frame->height, destination, strides);
            return true;
        }
        const qsizetype fullStride = qsizetype(frame->width) * 4;
        m_full.resize(fullStride * frame->height);
        uint8_t *full[4] = {reinterpret_cast<uint8_t *>(m_full.data()), nullptr, nullptr, nullptr};
        const int fullStrides[4] = {int(fullStride), 0, 0, 0};
        sws_scale(m_toRgba.get(), frame->data, frame->linesize, 0, frame->height, full, fullStrides);
        uchar *px = reinterpret_cast<uchar *>(m_full.data());
        for (qsizetype i = 0, n = qsizetype(frame->width) * frame->height; i < n; ++i, px += 4) {
            const unsigned a = px[3];
            px[0] = uchar((px[0] * a + 127) / 255);
            px[1] = uchar((px[1] * a + 127) / 255);
            px[2] = uchar((px[2] * a + 127) / 255);
        }
        const uint8_t *source[4] = {full[0], nullptr, nullptr, nullptr};
        sws_scale(m_scaler.get(), source, fullStrides, 0, frame->height, destination, strides);
        uchar *out = reinterpret_cast<uchar *>(m_pending.data());
        for (qsizetype i = 0, n = qsizetype(m_out.width()) * m_out.height(); i < n; ++i, out += 4) {
            const unsigned a = out[3];
            if (a == 0) {
                out[0] = out[1] = out[2] = 0;
            } else if (a < 255) {
                out[0] = uchar(std::min(255u, (out[0] * 255u + a / 2) / a));
                out[1] = uchar(std::min(255u, (out[1] * 255u + a / 2) / a));
                out[2] = uchar(std::min(255u, (out[2] * 255u + a / 2) / a));
            }
        }
        return true;
    }

    // The kept frame waiting for the time of the next one, which is its delay.
    bool emitPending(double nextAt)
    {
        const long long delay = std::max<long long>(1, std::llround(nextAt) - std::llround(m_keptAt));
        return emitFrame(int(std::min<long long>(delay, kMaxDelayMs)));
    }

    bool emitFrame(int delayMs)
    {
        if (m_kept >= m_s.maxFrames) {
            m_error = named(kTooManyFrames, QStringLiteral("more than %1 frames; lower the frame rate or trim the video").arg(m_s.maxFrames));
            return false;
        }
        if (m_kept == 0 && !m_sink.begin(m_out.width(), m_out.height())) {
            m_overBudget = m_sink.overBudget();
            m_error = m_overBudget ? named(kOutputOverBudget) : named(kHelperFailed, m_sink.error());
            return false;
        }
        if (!m_sink.addFrame(reinterpret_cast<const uchar *>(m_pending.constData()), m_stride, delayMs)) {
            m_overBudget = m_sink.overBudget();
            m_error = m_overBudget ? named(kOutputOverBudget) : named(kHelperFailed, m_sink.error());
            return false;
        }
        ++m_kept;
        m_total += delayMs;
        return true;
    }

    VideoOutcome complete()
    {
        if (!m_haveKept)
            return fail(named(kVideoNoStream, QStringLiteral("no frame inside the trim")));
        // The last kept frame shows until the end of the last frame seen (so that dropping frames keeps the length), and with a trim not past its end.
        double last = m_endAt - m_keptAt;
        if (m_s.durationMs > 0)
            last = std::min(last, double(m_s.durationMs) - m_keptAt);
        last = std::max(last, 1.0);
        if (!emitFrame(int(std::clamp<double>(std::llround(last), 1, kMaxDelayMs))))
            return finishedWithError();
        if (!m_sink.finish()) {
            m_overBudget = m_sink.overBudget();
            m_error = m_overBudget ? named(kOutputOverBudget) : named(kHelperFailed, m_sink.error());
            return finishedWithError();
        }
        VideoOutcome out;
        out.ok = true;
        out.frames = m_kept;
        out.width = m_out.width();
        out.height = m_out.height();
        out.totalDelayMs = m_total;
        out.hasAlpha = m_alpha;
        if (!m_alpha)
            out.degraded << named(kVideoOpaque);
        if (m_out.width() < m_context->width || m_out.height() < m_context->height)
            out.degraded << named(kVideoScaledDown);
        if (m_candidates > m_kept)
            out.degraded << named(kVideoFpsLowered);
        return out;
    }

    Opened &m_o;
    const VideoSettings &m_s;
    FrameSink &m_sink;
    const VideoCallbacks &m_cb;
    CodecPtr m_context;
    SwsPtr m_scaler;
    SwsPtr m_toRgba;
    QByteArray m_full;
    bool m_premultiplied = false;
    std::unique_ptr<Clock> m_clock;
    QByteArray m_pending;
    QSize m_out;
    qsizetype m_stride = 0;
    long long m_length = -1;
    int m_format = -1;
    int m_width = 0;
    int m_height = 0;
    double m_averageMs = 100;
    double m_interval = 33;
    double m_keptAt = 0;
    double m_endAt = 0;
    long long m_decoded = 0;    // every frame the decoder gave
    long long m_candidates = 0; // those inside the trim
    long long m_total = 0;
    int m_kept = 0;
    bool m_haveKept = false;
    bool m_decodedAny = false;
    bool m_alpha = false;
    bool m_gaveUp = false;
    int m_bad = 0;
    qint64 m_pixels = 0;
    bool m_overBudget = false;
    QString m_error;
};

} // namespace

VideoOutcome decodeVideo(const QString &file, const VideoSettings &settings, FrameSink &sink, const VideoCallbacks &callbacks)
{
    VideoOutcome out;
    if (const QString bad = validateVideoSettings(settings); !bad.isEmpty()) {
        out.error = named(kSpecRefused, bad);
        return out;
    }
    Opened o;
    if (!openVideo(file, settings, callbacks, &o, &out.error)) {
        out.cancelled = o.guard && o.guard->wasCancelled;
        return out;
    }
    // A video tagged as carrying alpha that reaches here has its decoder; one that does not is opaque (the outcome says so).
    Decoder decoder(o, settings, sink, callbacks);
    out = decoder.run();
    return out;
}

#endif // MURASAKI_HAVE_LIBAV

} // namespace murasaki::import
