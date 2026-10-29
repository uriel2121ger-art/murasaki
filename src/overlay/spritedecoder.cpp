#include "spritedecoder.h"

#include <QFile>
#include <QFileInfo>
#include <QImageReader>

#include <algorithm>
#include <cmath>
#include <memory>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/pixdesc.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#ifdef MURASAKI_HAVE_LIBWEBP
#include <webp/decode.h>
#include <webp/demux.h>
#endif

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

struct FormatDeleter {
    void operator()(AVFormatContext *context) const { avformat_close_input(&context); }
};
struct CodecDeleter {
    void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};
struct PacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
struct FrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
struct ScaleDeleter {
    // sws_free_context is FFmpeg 8 only; sws_freeContext pairs with sws_getContext and exists in 7.x and 8.
    void operator()(SwsContext *context) const { sws_freeContext(context); }
};

using FormatPtr = std::unique_ptr<AVFormatContext, FormatDeleter>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using ScalePtr = std::unique_ptr<SwsContext, ScaleDeleter>;

QString averror(int code)
{
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(code, buffer, sizeof buffer);
    return QString::fromUtf8(buffer);
}

// The demuxer for the file's extension, or null for one we do not play.
const AVInputFormat *demuxerFor(const QString &file)
{
    const QString suffix = QFileInfo(file).suffix().toLower();
    if (suffix == u"apng" || suffix == u"png")
        return av_find_input_format("apng");
    if (suffix == u"gif")
        return av_find_input_format("gif");
    if (suffix == u"webp")
        return av_find_input_format("webp_pipe");
    if (suffix == u"webm")
        return av_find_input_format("matroska");
    return nullptr;
}

struct Opened {
    FormatPtr format;
    CodecPtr codec;
    AVStream *stream = nullptr;
};

bool open(const QString &file, Opened *out, QString *error)
{
    static const bool quiet = [] {
        av_log_set_level(AV_LOG_ERROR); // libvpx announces its version on every open otherwise
        return true;
    }();
    Q_UNUSED(quiet);
    const AVInputFormat *demuxer = demuxerFor(file);
    if (!demuxer) {
        *error = u"unsupported file type"_s;
        return false;
    }
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    // An APNG frame that says 0 shows for the default rate (15 fps) otherwise; the pets' rule is 100 ms (petFrameDelayMs).
    // (The gif demuxer already does that, for delays of 0 and 1 centisecond.)
    if (demuxer == av_find_input_format("apng"))
        av_dict_set(&options, "default_fps", "10", 0);
    AVFormatContext *raw = nullptr;
    const QByteArray url = "file:" + QFile::encodeName(file);
    const int opened = avformat_open_input(&raw, url.constData(), demuxer, &options);
    av_dict_free(&options);
    if (opened < 0) {
        *error = u"cannot open: "_s + averror(opened);
        return false;
    }
    out->format.reset(raw);
    const int info = avformat_find_stream_info(raw, nullptr);
    if (info < 0) {
        *error = u"cannot read the streams: "_s + averror(info);
        return false;
    }
    const int index = av_find_best_stream(raw, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (index < 0) {
        *error = u"no video stream"_s;
        return false;
    }
    out->stream = raw->streams[index];
    const AVCodecParameters *parameters = out->stream->codecpar;
    const AVCodec *codec = nullptr;
    switch (parameters->codec_id) {
    case AV_CODEC_ID_VP9:
        // The native decoder discards the alpha plane; libvpx-vp9 outputs yuva420p.
        codec = avcodec_find_decoder_by_name("libvpx-vp9");
        if (!codec) {
            *error = u"the libvpx-vp9 decoder is not available"_s;
            return false;
        }
        break;
    case AV_CODEC_ID_APNG:
    case AV_CODEC_ID_GIF:
    case AV_CODEC_ID_WEBP:
    case AV_CODEC_ID_PNG:
        codec = avcodec_find_decoder(parameters->codec_id);
        break;
    default:
        *error = u"unsupported codec"_s;
        return false;
    }
    if (!codec) {
        *error = u"no decoder for this file"_s;
        return false;
    }
    out->codec.reset(avcodec_alloc_context3(codec));
    if (!out->codec || avcodec_parameters_to_context(out->codec.get(), parameters) < 0) {
        *error = u"cannot set up the decoder"_s;
        return false;
    }
    out->codec->thread_count = 2;
    const int started = avcodec_open2(out->codec.get(), codec, nullptr);
    if (started < 0) {
        *error = u"cannot start the decoder: "_s + averror(started);
        return false;
    }
    return true;
}

// Frames the header promises, 0 when it does not say.
qint64 headerFrames(const AVFormatContext *format, const AVStream *stream)
{
    if (stream->nb_frames > 0)
        return stream->nb_frames;
    const AVRational rate = stream->avg_frame_rate.num > 0 ? stream->avg_frame_rate : stream->r_frame_rate;
    double seconds = 0;
    if (stream->duration > 0 && stream->duration != AV_NOPTS_VALUE)
        seconds = stream->duration * av_q2d(stream->time_base);
    else if (format->duration > 0)
        seconds = double(format->duration) / AV_TIME_BASE;
    if (seconds <= 0 || rate.num <= 0 || rate.den <= 0)
        return 0;
    return std::max<qint64>(1, std::llround(seconds * av_q2d(rate)));
}

QSize targetFor(QSize requested, const AVCodecParameters *parameters)
{
    if (requested.isEmpty())
        return {parameters->width, parameters->height};
    return requested;
}

// Tells swscale how the YUV of the frame maps to RGB, the way mpv reads it: the stream's own tags, else BT.709 for
// HD and BT.601 for SD, and the frame's range (limited unless tagged full). Without this swscale assumes BT.601
// limited for everything and the colors of an HD WebM come out different from the video path's.
void matchColors(SwsContext *scaler, const AVFrame *frame)
{
    const AVPixFmtDescriptor *description = av_pix_fmt_desc_get(AVPixelFormat(frame->format));
    if (!description || (description->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL)))
        return; // not YUV: nothing to match
    int colorspace = SWS_CS_DEFAULT;
    switch (frame->colorspace) {
    case AVCOL_SPC_BT709:
        colorspace = SWS_CS_ITU709;
        break;
    case AVCOL_SPC_BT470BG:
    case AVCOL_SPC_SMPTE170M:
        colorspace = SWS_CS_ITU601;
        break;
    case AVCOL_SPC_SMPTE240M:
        colorspace = SWS_CS_SMPTE240M;
        break;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL:
        colorspace = SWS_CS_BT2020;
        break;
    default:
        colorspace = (frame->width >= 1280 || frame->height > 576) ? SWS_CS_ITU709 : SWS_CS_ITU601;
        break;
    }
    const int sourceRange = frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0;
    sws_setColorspaceDetails(scaler, sws_getCoefficients(colorspace), sourceRange, sws_getCoefficients(SWS_CS_DEFAULT),
                             1, 0, 1 << 16, 1 << 16);
}

// Straight RGBA bytes (from swscale) to premultiplied.
void premultiply(const uint8_t *source, int sourceStride, QImage *image)
{
    for (int y = 0; y < image->height(); ++y) {
        const uint8_t *in = source + qsizetype(y) * sourceStride;
        uchar *out = image->scanLine(y);
        for (int x = 0; x < image->width(); ++x, in += 4, out += 4) {
            const unsigned alpha = in[3];
            if (alpha == 255) {
                out[0] = in[0];
                out[1] = in[1];
                out[2] = in[2];
            } else {
                out[0] = uchar((in[0] * alpha + 127) / 255);
                out[1] = uchar((in[1] * alpha + 127) / 255);
                out[2] = uchar((in[2] * alpha + 127) / 255);
            }
            out[3] = uchar(alpha);
        }
    }
}

constexpr double kMaxFrameDurationMs = 3600.0 * 1000.0; // an hour: more than any frame lasts, and well inside an int

// The transcoder's WebM: its frame times are the pet's retimed copy, not delays an encoder wrote, so the rule for tiny
// delays (petFrameDelayMs) is not applied to them.
bool isWebm(const QString &file)
{
    return QFileInfo(file).suffix().compare(u"webm"_s, Qt::CaseInsensitive) == 0;
}

bool isWebp(const QString &file)
{
    return QFileInfo(file).suffix().compare(u"webp"_s, Qt::CaseInsensitive) == 0;
}

#ifndef MURASAKI_HAVE_LIBWEBP
// FFmpeg's WebP decoder does not read animated files, so these go through Qt's WebP plugin (a local file, nothing
// else is opened).
void prepareWebp(QImageReader &reader)
{
    reader.setFormat("webp");
    reader.setDecideFormatFromContent(false);
    reader.setAutoTransform(false);
}
#endif

// Builds the masks of the frames as they are produced.
class MaskSink
{
public:
    MaskSink(const SpriteDecoder::MaskRequest *request, QSize size, qint64 expectedFrames)
    {
        if (request)
            m_builder = std::make_unique<MaskBuilder>(size.width(), size.height(), int(std::clamp<qint64>(expectedFrames, 1, kMaxMaskFrames)),
                                                      request->budgetBytes);
    }
    void add(const QImage &image) // Format_RGBA8888_Premultiplied: the alpha is the fourth byte
    {
        if (m_builder)
            m_builder->addFrame(image.constBits() + 3, int(image.bytesPerLine()), 4, image.width(), image.height());
    }
    MaskSet finish() { return m_builder ? m_builder->finish() : MaskSet(); }

private:
    std::unique_ptr<MaskBuilder> m_builder;
};

#ifdef MURASAKI_HAVE_LIBWEBP
// An animated (or still) WebP composited by libwebp's own animation decoder, as the WebP container specification says: each frame's
// rectangle, blend mode and disposal onto a canvas that starts transparent. Qt's WebP plugin leaves the part of the canvas that the first
// frame does not cover uninitialised (the same file decodes differently the second time in one process), so it is not used for pixels.
// The file is mapped, never read whole, and WebPAnimDecoder only references the mapping.
class WebpAnim
{
public:
    // The largest WebP file read: the file is read into memory, not mapped (a mapped file that is truncated meanwhile, by a Steam update
    // writing in place, kills the process with SIGBUS; a read only fails the decode). Pets' WebP files are tens of MiB.
    static constexpr qint64 kMaxFileBytes = qint64(512) << 20;
    // Three canvases are alive while decoding (libwebp's two and the QImage made from the current one).
    static constexpr qint64 kMaxCanvasBytes = qint64(1) << 30;

    WebpAnim() = default;
    WebpAnim(const WebpAnim &) = delete;
    WebpAnim &operator=(const WebpAnim &) = delete;
    ~WebpAnim()
    {
        if (m_decoder)
            WebPAnimDecoderDelete(m_decoder);
    }

    // Reads the file and the container's header: the canvas and the frame count, from libwebp's demuxer (no pixel buffer is allocated).
    // The same numbers for every user (decode, probe, nativeSize, promisedFrames, estimateBytes).
    bool open(const QString &path, QString *error)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = u"cannot open: "_s + file.errorString();
            return false;
        }
        const qint64 size = file.size();
        if (size < 12 || size > kMaxFileBytes) {
            *error = size < 12 ? u"not a WebP file"_s : u"too large for the WebP decoder"_s;
            return false;
        }
        m_bytes = file.read(size);
        if (m_bytes.size() != size) {
            *error = u"reading failed"_s;
            return false;
        }
        m_data = {reinterpret_cast<const uint8_t *>(m_bytes.constData()), size_t(m_bytes.size())};
        WebPBitstreamFeatures features;
        if (WebPGetFeatures(m_data.bytes, m_data.size, &features) != VP8_STATUS_OK) {
            *error = u"not a WebP file"_s;
            return false;
        }
        if (features.width < 1 || features.height < 1 || features.width > SpriteDecoder::kMaxSide || features.height > SpriteDecoder::kMaxSide
            || qint64(features.width) * features.height * 4 * 3 > kMaxCanvasBytes) {
            *error = u"bad size"_s;
            return false;
        }
        WebPDemuxer *demux = WebPDemux(&m_data);
        if (!demux) {
            *error = u"truncated or damaged"_s; // the demuxer refuses a file whose chunks do not add up
            return false;
        }
        m_canvas = QSize(int(WebPDemuxGetI(demux, WEBP_FF_CANVAS_WIDTH)), int(WebPDemuxGetI(demux, WEBP_FF_CANVAS_HEIGHT)));
        m_frames = int(WebPDemuxGetI(demux, WEBP_FF_FRAME_COUNT));
        WebPDemuxDelete(demux);
        if (!m_canvas.isValid() || m_canvas.width() > SpriteDecoder::kMaxSide || m_canvas.height() > SpriteDecoder::kMaxSide || m_frames < 1) {
            *error = u"bad size"_s;
            return false;
        }
        return true;
    }

    QSize canvas() const { return m_canvas; }
    int frameCount() const { return m_frames; }

    // Creates the animation decoder, which allocates the canvases: after the caller has checked the sizes against its budget.
    bool begin(QString *error)
    {
        WebPAnimDecoderOptions options;
        if (!WebPAnimDecoderOptionsInit(&options)) {
            *error = u"libwebp version mismatch"_s;
            return false;
        }
        options.color_mode = MODE_RGBA;
        options.use_threads = 0;
        m_decoder = WebPAnimDecoderNew(&m_data, &options);
        if (!m_decoder) {
            *error = u"truncated or damaged"_s;
            return false;
        }
        return true;
    }

    // The next composited frame, premultiplied, at the canvas size, and how long it shows; false at the end or on a frame that does not
    // decode (then error() is set).
    bool next(QImage *image, int *delayMs)
    {
        if (!WebPAnimDecoderHasMoreFrames(m_decoder))
            return false;
        uint8_t *buffer = nullptr;
        int timestamp = 0;
        if (!WebPAnimDecoderGetNext(m_decoder, &buffer, &timestamp)) {
            m_error = u"decoding failed"_s;
            return false;
        }
        *image = QImage(m_canvas, QImage::Format_RGBA8888_Premultiplied);
        if (image->isNull()) {
            m_error = u"out of memory"_s;
            return false;
        }
        premultiply(buffer, m_canvas.width() * 4, image);
        // libwebp's timestamp is the END of the frame, as a (24-bit durations summed) int: take the difference in 64 bits, never negative.
        const qint64 delay = qint64(timestamp) - m_previous;
        *delayMs = int(std::clamp<qint64>(delay, 0, qint64(kMaxFrameDurationMs)));
        m_previous = timestamp;
        return true;
    }
    const QString &error() const { return m_error; }

private:
    QByteArray m_bytes; // the file; WebPAnimDecoder references it
    WebPData m_data{};
    WebPAnimDecoder *m_decoder = nullptr;
    QSize m_canvas;
    int m_frames = 0;
    qint64 m_previous = 0;
    QString m_error;
};

QImage cropToContent(const QImage &image, QPoint *at)
{
    QRect box(0, 0, image.width(), image.height());
    if (image.format() == QImage::Format_RGBA8888_Premultiplied && image.width() > 0 && image.height() > 0) {
        int top = image.height(), bottom = -1, left = image.width(), right = -1;
        for (int y = 0; y < image.height(); ++y) {
            const uchar *row = image.constScanLine(y);
            int first = -1, last = -1;
            for (int x = 0; x < image.width(); ++x) {
                if (row[x * 4 + 3]) {
                    if (first < 0)
                        first = x;
                    last = x;
                }
            }
            if (first < 0)
                continue;
            top = std::min(top, y);
            bottom = y;
            left = std::min(left, first);
            right = std::max(right, last);
        }
        if (bottom < 0) {
            box = QRect(0, 0, 1, 1); // nothing drawn: one transparent pixel
        } else {
            box = QRect(QPoint(std::max(0, left - 1), std::max(0, top - 1)), QPoint(std::min(image.width() - 1, right + 1), std::min(image.height() - 1, bottom + 1)));
        }
    }
    *at = box.topLeft();
    return box.size() == image.size() ? image : image.copy(box);
}

// Keeps a converted frame in the result: whole, or (with `crop`) cut to what is drawn, the bytes being what is kept.
void keepFrame(SpriteFrames *frames, QImage image, qint64 wholeBytes, bool crop)
{
    if (!crop) {
        frames->images.append(std::move(image));
        frames->bytes += wholeBytes;
        return;
    }
    QPoint at;
    QImage piece = cropToContent(image, &at);
    frames->bytes += piece.sizeInBytes();
    frames->images.append(std::move(piece));
    frames->offsets.append(at);
}

SpriteDecoder::Result decodeWebp(const QString &file, QSize target, qint64 maxBytes, const std::atomic_bool *cancel,
                                 int fixedFps, const SpriteDecoder::MaskRequest *masks, bool crop)
{
    SpriteDecoder::Result result;
    const auto fail = [&result](const QString &error) {
        result.ok = false;
        result.error = error;
        result.frames = {};
        return result;
    };
    WebpAnim anim;
    QString problem;
    if (!anim.open(file, &problem))
        return fail(problem);
    const QSize own = anim.canvas();
    const QSize size = target.isEmpty() ? own : target;
    if (size.width() < 1 || size.height() < 1 || size.width() > SpriteDecoder::kMaxSide || size.height() > SpriteDecoder::kMaxSide)
        return fail(u"bad size"_s);
    const qint64 frameBytes = qint64(size.width()) * size.height() * 4;
    const bool keep = !masks || masks->keepFrames;
    if (keep && !crop && frameBytes > maxBytes)
        return fail(u"over the frame cache budget"_s);
    const int promised = anim.frameCount();
    if (promised > 0 && (promised > SpriteDecoder::kMaxFrames || (keep && !crop && promised * frameBytes > maxBytes)))
        return fail(u"over the frame cache budget"_s);
    MaskSink sink(masks, size, promised);
    if (!anim.begin(&problem)) // only now do the canvases get allocated
        return fail(problem);
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.frames = {};
            return result;
        }
        if (result.decodedFrames >= SpriteDecoder::kMaxFrames || (keep && !crop && result.frames.bytes + frameBytes > maxBytes))
            return fail(u"over the frame cache budget"_s);
        QImage image;
        int delay = 0;
        if (!anim.next(&image, &delay)) {
            if (!anim.error().isEmpty())
                return fail(anim.error());
            break;
        }
        if (image.size() != size)
            image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation).convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        sink.add(image);
        ++result.decodedFrames;
        if (keep) {
            keepFrame(&result.frames, std::move(image), frameBytes, crop);
            result.frames.durationsMs.append(int(std::lround(petFrameDelayMs(delay))));
            if (crop && result.frames.bytes > maxBytes)
                return fail(u"over the frame cache budget"_s); // (what is kept, not the whole frames, is what the budget counts)
        }
    }
    if (result.decodedFrames == 0)
        return fail(u"no frames"_s);
    if (keep && fixedFps > 0)
        result.frames.durationsMs = SpriteDecoder::fixedDurations(int(result.frames.images.size()), fixedFps);
    result.frames.size = size;
    result.masks = sink.finish();
    result.ok = true;
    return result;
}

#else // no libwebp: Qt's reader (its animated frames can carry uninitialised pixels where the first frame does not cover the canvas)

SpriteDecoder::Result decodeWebp(const QString &file, QSize target, qint64 maxBytes, const std::atomic_bool *cancel,
                                 int fixedFps, const SpriteDecoder::MaskRequest *masks, bool crop)
{
    SpriteDecoder::Result result;
    const auto fail = [&result](const QString &error) {
        result.ok = false;
        result.error = error;
        result.frames = {};
        return result;
    };
    QImageReader reader(file);
    prepareWebp(reader);
    if (!reader.canRead())
        return fail(u"cannot open: "_s + reader.errorString());
    const QSize size = target.isEmpty() ? reader.size() : target;
    const QSize own = reader.size();
    if (own.isValid() && (own.width() > SpriteDecoder::kMaxSide || own.height() > SpriteDecoder::kMaxSide))
        return fail(u"bad size"_s);
    if (size.width() < 1 || size.height() < 1 || size.width() > SpriteDecoder::kMaxSide
        || size.height() > SpriteDecoder::kMaxSide)
        return fail(u"bad size"_s);
    const qint64 frameBytes = qint64(size.width()) * size.height() * 4;
    const bool keep = !masks || masks->keepFrames;
    if (keep && !crop && frameBytes > maxBytes)
        return fail(u"over the frame cache budget"_s);
    const int promised = reader.imageCount();
    if (promised > 0 && (promised > SpriteDecoder::kMaxFrames || (keep && !crop && promised * frameBytes > maxBytes)))
        return fail(u"over the frame cache budget"_s);
    MaskSink sink(masks, size, promised);
    // Frames that are not kept are decoded at the size they are wanted at when the plugin can scale while decoding, so a huge
    // canvas never exists in full (the plugin ignores the option otherwise and the scaling below does it).
    if (!keep && size != own)
        reader.setScaledSize(size);
    for (;;) {
        if (cancel && cancel->load(std::memory_order_relaxed)) {
            result.cancelled = true;
            result.frames = {};
            return result;
        }
        if (result.decodedFrames >= SpriteDecoder::kMaxFrames || (keep && !crop && result.frames.bytes + frameBytes > maxBytes))
            return fail(u"over the frame cache budget"_s);
        QImage image = reader.read();
        if (image.isNull())
            break;
        const int delay = reader.nextImageDelay();
        image = image.convertToFormat(QImage::Format_ARGB32_Premultiplied);
        if (image.size() != size)
            image = image.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
        sink.add(image);
        ++result.decodedFrames;
        if (keep) {
            keepFrame(&result.frames, std::move(image), frameBytes, crop);
            result.frames.durationsMs.append(int(std::lround(petFrameDelayMs(delay))));
            if (crop && result.frames.bytes > maxBytes)
                return fail(u"over the frame cache budget"_s); // (what is kept, not the whole frames, is what the budget counts)
        }
        if (reader.imageCount() <= 1 && !reader.supportsAnimation())
            break; // a still image: one frame
    }
    if (result.decodedFrames == 0)
        return fail(u"no frames: "_s + reader.errorString());
    if (keep && fixedFps > 0)
        result.frames.durationsMs = SpriteDecoder::fixedDurations(int(result.frames.images.size()), fixedFps);
    result.frames.size = size;
    result.masks = sink.finish();
    result.ok = true;
    return result;
}

#endif // MURASAKI_HAVE_LIBWEBP

} // namespace

void SpriteFrames::cropTransparentMargins()
{
    if (images.isEmpty() || !offsets.isEmpty())
        return;
    QList<QPoint> places;
    places.reserve(images.size());
    qint64 total = 0;
    for (QImage &image : images) {
        QPoint at;
        QImage piece = cropToContent(image, &at);
        image = QImage(); // the whole frame goes as soon as its piece is cut: the peak is the full set, not the full set and the cut one
        total += piece.sizeInBytes();
        image = std::move(piece);
        places.append(at);
    }
    offsets = std::move(places);
    bytes = total;
}

QList<int> SpriteDecoder::fixedDurations(int frames, int fps)
{
    QList<int> durations;
    if (fps <= 0)
        return durations;
    for (int i = 0; i < frames; ++i) {
        const qint64 from = std::llround(i * 1000.0 / fps);
        const qint64 to = std::llround((i + 1) * 1000.0 / fps);
        durations.append(std::max<qint64>(kMinFrameMs, to - from));
    }
    return durations;
}

qint64 SpriteDecoder::estimateBytes(const QString &file, QSize target, QString *error)
{
#ifdef MURASAKI_HAVE_LIBWEBP
    if (isWebp(file)) {
        WebpAnim anim;
        QString problem;
        if (!anim.open(file, &problem)) {
            if (error)
                *error = problem;
            return 0;
        }
        const QSize size = target.isEmpty() ? anim.canvas() : target;
        return qint64(anim.frameCount()) * size.width() * size.height() * 4;
    }
#else
    if (isWebp(file)) {
        QImageReader reader(file);
    prepareWebp(reader);
        if (!reader.canRead()) {
            if (error)
                *error = u"cannot open: "_s + reader.errorString();
            return 0;
        }
        const QSize size = target.isEmpty() ? reader.size() : target;
        return qint64(std::max(0, reader.imageCount())) * size.width() * size.height() * 4;
    }
#endif
    Opened opened;
    QString problem;
    if (!open(file, &opened, &problem)) {
        if (error)
            *error = problem;
        return 0;
    }
    const QSize size = targetFor(target, opened.stream->codecpar);
    return headerFrames(opened.format.get(), opened.stream) * size.width() * size.height() * 4;
}

SpriteDecoder::Result SpriteDecoder::decode(const QString &file, QSize target, qint64 maxBytes,
                                            const std::atomic_bool *cancel, int fixedFps, const MaskRequest *masks, bool crop)
{
    if (isWebp(file))
        return decodeWebp(file, target, maxBytes, cancel, fixedFps, masks, crop);
    Result result;
    const auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };
    const auto fail = [&result](const QString &error) {
        result.ok = false;
        result.error = error;
        result.frames = {};
        return result;
    };

    // Tasks that wait in the queue behind another pet's decode may be cancelled before they start.
    if (cancelled()) {
        result.cancelled = true;
        return result;
    }
    Opened opened;
    QString problem;
    if (!open(file, &opened, &problem))
        return fail(problem);
    if (cancelled()) {
        result.cancelled = true;
        return result;
    }
    const AVCodecParameters *parameters = opened.stream->codecpar;
    // Judged on the header, before anything is allocated: a file that claims a huge canvas is refused.
    if (parameters->width < 1 || parameters->height < 1 || parameters->width > kMaxSide || parameters->height > kMaxSide)
        return fail(u"bad size"_s);
    const QSize size = targetFor(target, parameters);
    if (size.width() < 1 || size.height() < 1 || size.width() > kMaxSide || size.height() > kMaxSide)
        return fail(u"bad size"_s);
    const qint64 frameBytes = qint64(size.width()) * size.height() * 4;
    const bool keep = !masks || masks->keepFrames;
    if (keep && !crop && frameBytes > maxBytes)
        return fail(u"over the frame cache budget"_s);
    const qint64 promisedFrames = headerFrames(opened.format.get(), opened.stream);
    if (promisedFrames > 0) {
        if (promisedFrames > kMaxFrames || (keep && !crop && promisedFrames * frameBytes > maxBytes))
            return fail(u"over the frame cache budget"_s);
    }
    MaskSink sink(masks, size, promisedFrames);

    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame)
        return fail(u"out of memory"_s);
    ScalePtr scaler;
    QByteArray rgba(qsizetype(frameBytes), Qt::Uninitialized);
    const int rgbaStride = size.width() * 4;
    const AVRational timeBase = opened.stream->time_base;

    QList<double> startsMs; // presentation time of every frame, -1 when unknown
    double lastFrameMs = 0; // duration the container gives the last frame, 0 if none
    int sourceFormat = -1;
    int sourceWidth = 0;
    int sourceHeight = 0;
    bool aborted = false;
    QString abortReason;

    // Converts the frame in `frame` and keeps it; false stops the decoding (cancelled, over budget or an error).
    const auto take = [&]() -> bool {
        if (cancelled()) {
            aborted = true;
            result.cancelled = true;
            return false;
        }
        if (result.decodedFrames >= kMaxFrames || (keep && !crop && result.frames.bytes + frameBytes > maxBytes)) {
            aborted = true;
            abortReason = u"over the frame cache budget"_s;
            return false;
        }
        if (!scaler || frame->format != sourceFormat || frame->width != sourceWidth || frame->height != sourceHeight) {
            sourceFormat = frame->format;
            sourceWidth = frame->width;
            sourceHeight = frame->height;
            SwsContext *context = sws_getContext(frame->width, frame->height, AVPixelFormat(frame->format),
                                                 size.width(), size.height(), AV_PIX_FMT_RGBA, SWS_AREA, nullptr,
                                                 nullptr, nullptr);
            if (!context) {
                aborted = true;
                abortReason = u"cannot convert the frames"_s;
                return false;
            }
            scaler.reset(context);
            matchColors(scaler.get(), frame.get());
        }
        uint8_t *destination[4] = {reinterpret_cast<uint8_t *>(rgba.data()), nullptr, nullptr, nullptr};
        const int strides[4] = {rgbaStride, 0, 0, 0};
        sws_scale(scaler.get(), frame->data, frame->linesize, 0, frame->height, destination, strides);
        QImage image(size, QImage::Format_RGBA8888_Premultiplied);
        premultiply(reinterpret_cast<const uint8_t *>(rgba.constData()), rgbaStride, &image);
        sink.add(image);
        ++result.decodedFrames;
        if (keep) {
            keepFrame(&result.frames, std::move(image), frameBytes, crop);
            if (crop && result.frames.bytes > maxBytes) {
                aborted = true;
                abortReason = u"over the frame cache budget"_s;
                return false;
            }
        }
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        startsMs.append(pts != AV_NOPTS_VALUE ? pts * av_q2d(timeBase) * 1000.0 : -1.0);
        lastFrameMs = frame->duration > 0 ? frame->duration * av_q2d(timeBase) * 1000.0 : 0;
        return true;
    };

    const auto drain = [&]() -> bool {
        for (;;) {
            const int received = avcodec_receive_frame(opened.codec.get(), frame.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0) {
                aborted = true;
                abortReason = u"decoding failed: "_s + averror(received);
                return false;
            }
            const bool kept = take();
            av_frame_unref(frame.get());
            if (!kept)
                return false;
        }
    };

    for (;;) {
        const int read = av_read_frame(opened.format.get(), packet.get());
        if (read == AVERROR_EOF) {
            avcodec_send_packet(opened.codec.get(), nullptr); // flush
            drain();
            break;
        }
        if (read < 0) {
            // Anything but the end of the file is a damaged or truncated one: no half animation.
            aborted = true;
            abortReason = u"reading failed: "_s + averror(read);
            break;
        }
        bool keepGoing = true;
        if (packet->stream_index == opened.stream->index) {
            const int sent = avcodec_send_packet(opened.codec.get(), packet.get());
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                aborted = true;
                abortReason = u"decoding failed: "_s + averror(sent);
                keepGoing = false;
            } else {
                keepGoing = drain();
            }
        }
        av_packet_unref(packet.get());
        if (!keepGoing)
            break;
    }
    if (result.cancelled) {
        result.frames = {};
        return result;
    }
    if (aborted)
        return fail(abortReason);
    if (result.decodedFrames == 0)
        return fail(u"no frames"_s);
    if (!keep) { // the low-resolution pass: only the masks are wanted
        result.frames.size = size;
        result.masks = sink.finish();
        result.ok = true;
        return result;
    }

    const bool video = isWebm(file);
    // Durations from the presentation times; the last frame lasts as long as the container says, else as long as
    // the others do on average.
    const qsizetype count = result.frames.images.size();
    double averageMs = 0;
    if (count > 1 && startsMs.first() >= 0 && startsMs.last() >= startsMs.first())
        averageMs = (startsMs.last() - startsMs.first()) / double(count - 1);
    if (averageMs <= 0) {
        const AVRational rate = opened.stream->avg_frame_rate.num > 0 ? opened.stream->avg_frame_rate
                                                                       : opened.stream->r_frame_rate;
        averageMs = rate.num > 0 && rate.den > 0 ? 1000.0 / av_q2d(rate) : 100.0;
    }
    for (qsizetype i = 0; i < count; ++i) {
        double duration;
        if (i + 1 < count && startsMs.at(i) >= 0 && startsMs.at(i + 1) >= startsMs.at(i))
            duration = startsMs.at(i + 1) - startsMs.at(i);
        else
            duration = (i + 1 == count && lastFrameMs > 0) ? lastFrameMs : averageMs;
        if (!std::isfinite(duration) || duration < 0)
            duration = 100.0;
        duration = std::min(duration, kMaxFrameDurationMs); // a huge gap between two timestamps must not overflow the conversion
        result.frames.durationsMs.append(video ? std::max(kMinFrameMs, int(std::lround(duration)))
                                               : int(std::lround(petFrameDelayMs(duration))));
    }
    if (fixedFps > 0)
        result.frames.durationsMs = fixedDurations(int(count), fixedFps);
    result.frames.size = size;
    result.masks = sink.finish();
    result.ok = true;
    return result;
}

QSize SpriteDecoder::maskPassSize(QSize native, qint64 maxPixels)
{
    if (native.width() < 1 || native.height() < 1 || maxPixels < 1)
        return {};
    const qint64 pixels = qint64(native.width()) * native.height();
    if (pixels <= maxPixels)
        return native;
    const double scale = std::sqrt(double(maxPixels) / double(pixels));
    QSize size(std::max(1, int(native.width() * scale)), std::max(1, int(native.height() * scale)));
    if (qint64(size.width()) * size.height() > maxPixels) // an extreme aspect ratio: a side stuck at 1 pixel
        size.setWidth(int(std::max<qint64>(1, maxPixels / size.height())));
    return size;
}

QSize SpriteDecoder::nativeSize(const QString &file)
{
#ifdef MURASAKI_HAVE_LIBWEBP
    if (isWebp(file)) {
        // The canvas is in the first bytes (VP8X, or the VP8 / VP8L header of a still): libwebp reads it without the rest of the file.
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QByteArray head = f.read(256);
        int w = 0, h = 0;
        if (head.size() < 12 || !WebPGetInfo(reinterpret_cast<const uint8_t *>(head.constData()), size_t(head.size()), &w, &h))
            return {};
        return QSize(w, h);
    }
#else
    if (isWebp(file)) {
        QImageReader reader(file);
        prepareWebp(reader);
        return reader.canRead() ? reader.size() : QSize();
    }
#endif
    Opened opened;
    QString problem;
    if (!open(file, &opened, &problem))
        return {};
    return QSize(opened.stream->codecpar->width, opened.stream->codecpar->height);
}

SpriteDecoder::MaskPassResult SpriteDecoder::decodeMasks(const QString &file, const std::atomic_bool *cancel, std::size_t budgetBytes)
{
    MaskPassResult out;
    out.size = maskPassSize(nativeSize(file));
    if (!out.size.isValid()) {
        out.result.error = u"cannot read the size"_s;
        return out;
    }
    const MaskRequest request{budgetBytes, false};
    out.result = decode(file, out.size, 0, cancel, 0, &request);
    return out;
}

qint64 SpriteDecoder::promisedFrames(const QString &file, QString *error)
{
#ifdef MURASAKI_HAVE_LIBWEBP
    if (isWebp(file)) {
        WebpAnim anim;
        QString problem;
        if (!anim.open(file, &problem)) {
            if (error)
                *error = problem;
            return 0;
        }
        return anim.frameCount();
    }
#else
    if (isWebp(file)) {
        QImageReader reader(file);
        prepareWebp(reader);
        if (!reader.canRead()) {
            if (error)
                *error = u"cannot open: "_s + reader.errorString();
            return 0;
        }
        return std::max(0, reader.imageCount());
    }
#endif
    Opened opened;
    QString problem;
    if (!open(file, &opened, &problem)) {
        if (error)
            *error = problem;
        return 0;
    }
    return headerFrames(opened.format.get(), opened.stream);
}

SpriteDecoder::Probe SpriteDecoder::probe(const QString &file, const QList<qint64> &indexes, qint64 maxBytes, [[maybe_unused]] const std::atomic_bool *cancel)
{
    Probe result;
    const auto wanted = [&indexes](qint64 index) { return indexes.contains(index); };
    // what the kept pictures and the one being made cost: a canvas each, for the ones asked for, the last and one in flight
    const auto overBudget = [&](QSize size) {
        return qint64(size.width()) * size.height() * 4 * (indexes.size() + 2) > maxBytes;
    };
#ifdef MURASAKI_HAVE_LIBWEBP
    if (isWebp(file)) {
        WebpAnim anim;
        QString problem;
        if (!anim.open(file, &problem)) {
            result.error = problem;
            return result;
        }
        const QSize own = anim.canvas();
        if (overBudget(own)) {
            result.error = u"over the memory budget of a probe"_s;
            return result;
        }
        if (anim.frameCount() > kMaxFrames) {
            result.error = u"more frames than the frame cache takes"_s;
            return result;
        }
        if (!anim.begin(&problem)) {
            result.error = problem;
            return result;
        }
        QImage last;
        double total = 0;
        for (;;) {
            if (cancel && cancel->load(std::memory_order_relaxed)) {
                result.error = u"cancelled"_s;
                result.images.clear();
                return result;
            }
            if (result.frames >= kMaxFrames) {
                result.error = u"more frames than the frame cache takes"_s;
                result.images.clear();
                return result;
            }
            QImage image;
            int delay = 0;
            if (!anim.next(&image, &delay)) {
                if (!anim.error().isEmpty()) {
                    result.error = anim.error();
                    result.images.clear();
                    return result;
                }
                break;
            }
            total += petFrameDelayMs(delay);
            if (wanted(result.frames))
                result.images.insert(result.frames, image);
            last = image;
            result.size = own;
            ++result.frames;
        }
        if (result.frames == 0) {
            result.error = u"no frames"_s;
            return result;
        }
        result.images.insert(result.frames - 1, last);
        result.durationMs = total;
        result.ok = true;
        return result;
    }
#else
    if (isWebp(file)) {
        QImageReader reader(file);
        prepareWebp(reader);
        if (!reader.canRead()) {
            result.error = u"cannot open: "_s + reader.errorString();
            return result;
        }
        const QSize own = reader.size();
        if (!own.isValid() || own.width() > kMaxSide || own.height() > kMaxSide) {
            result.error = u"bad size"_s;
            return result;
        }
        if (overBudget(own)) {
            result.error = u"over the memory budget of a probe"_s;
            return result;
        }
        QImage last;
        double total = 0;
        for (;;) {
            QImage image = reader.read();
            if (image.isNull())
                break;
            total += petFrameDelayMs(reader.nextImageDelay());
            image = image.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
            if (image.isNull()) {
                result.error = u"cannot convert the frames"_s;
                return result;
            }
            if (image.size() != own) {
                if (result.resizedFrames++ == 0)
                    result.otherSize = image.size();
            }
            if (wanted(result.frames))
                result.images.insert(result.frames, image);
            last = image;
            result.size = own;
            ++result.frames;
            if (reader.imageCount() <= 1 && !reader.supportsAnimation())
                break;
        }
        if (result.frames == 0) {
            result.error = u"no frames: "_s + reader.errorString();
            return result;
        }
        // the plugin ends a truncated file without an error of its own: the frames it promised are the check
        if (reader.imageCount() > 1 && result.frames != reader.imageCount()) {
            result.error = u"truncated or damaged: "_s + QString::number(result.frames) + u" of "_s + QString::number(reader.imageCount()) + u" frames"_s;
            result.images.clear();
            return result;
        }
        result.images.insert(result.frames - 1, last);
        result.durationMs = total;
        result.ok = true;
        return result;
    }
#endif
    Opened opened;
    QString problem;
    if (!open(file, &opened, &problem)) {
        result.error = problem;
        return result;
    }
    const AVCodecParameters *parameters = opened.stream->codecpar;
    if (parameters->width < 1 || parameters->height < 1 || parameters->width > kMaxSide || parameters->height > kMaxSide) {
        result.error = u"bad size"_s;
        return result;
    }
    const QSize size(parameters->width, parameters->height);
    if (overBudget(size)) {
        result.error = u"over the memory budget of a probe ("_s + QString::number(size.width()) + u"x"_s + QString::number(size.height()) + u")"_s;
        return result;
    }
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    FramePtr last(av_frame_alloc());
    if (!packet || !frame || !last) {
        result.error = u"out of memory"_s;
        return result;
    }
    ScalePtr scaler;
    int sourceFormat = -1, sourceWidth = 0, sourceHeight = 0;
    const bool video = isWebm(file);
    const AVRational timeBase = opened.stream->time_base;
    double firstMs = -1, lastMs = -1, lastDurationMs = 0;
    double stepsMs = 0;       // the frames but the last, each by the pets' delay rule
    bool timesBroken = false; // a presentation time is missing or goes back
    // converts straight into the image's own bytes and premultiplies there (no second canvas)
    const auto convert = [&](const AVFrame *source, QImage *image) -> bool {
        if (!scaler || source->format != sourceFormat || source->width != sourceWidth || source->height != sourceHeight) {
            sourceFormat = source->format;
            sourceWidth = source->width;
            sourceHeight = source->height;
            SwsContext *context = sws_getContext(source->width, source->height, AVPixelFormat(source->format), size.width(), size.height(),
                                                 AV_PIX_FMT_RGBA, SWS_AREA, nullptr, nullptr, nullptr);
            if (!context)
                return false;
            scaler.reset(context);
            matchColors(scaler.get(), source);
        }
        QImage out(size, QImage::Format_RGBA8888_Premultiplied);
        if (out.isNull())
            return false;
        uint8_t *destination[4] = {out.bits(), nullptr, nullptr, nullptr};
        const int strides[4] = {int(out.bytesPerLine()), 0, 0, 0};
        sws_scale(scaler.get(), source->data, source->linesize, 0, source->height, destination, strides);
        premultiply(out.constBits(), int(out.bytesPerLine()), &out); // (each pixel is read, then written: in place is safe)
        *image = out;
        return true;
    };
    bool failed = false;
    const auto take = [&]() {
        if (frame->width != parameters->width || frame->height != parameters->height) {
            if (result.resizedFrames++ == 0)
                result.otherSize = QSize(frame->width, frame->height);
        }
        if (wanted(result.frames)) {
            QImage image;
            if (!convert(frame.get(), &image)) {
                result.error = u"cannot convert the frames"_s;
                failed = true;
                return;
            }
            result.images.insert(result.frames, image);
        }
        av_frame_unref(last.get());
        if (av_frame_ref(last.get(), frame.get()) < 0) {
            result.error = u"out of memory"_s;
            failed = true;
            return;
        }
        const int64_t pts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
        const double ms = pts != AV_NOPTS_VALUE ? pts * av_q2d(timeBase) * 1000.0 : -1.0;
        if (result.frames == 0)
            firstMs = ms;
        else if (ms >= 0 && lastMs >= 0 && ms >= lastMs)
            stepsMs += video ? ms - lastMs : petFrameDelayMs(ms - lastMs);
        else
            timesBroken = true;
        lastMs = ms;
        lastDurationMs = frame->duration > 0 ? frame->duration * av_q2d(timeBase) * 1000.0 : 0;
        ++result.frames;
    };
    const auto drain = [&]() -> bool {
        for (;;) {
            const int received = avcodec_receive_frame(opened.codec.get(), frame.get());
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF)
                return true;
            if (received < 0) {
                result.error = u"decoding failed: "_s + averror(received);
                return false;
            }
            take();
            av_frame_unref(frame.get());
            if (failed)
                return false;
        }
    };
    for (;;) {
        const int read = av_read_frame(opened.format.get(), packet.get());
        if (read == AVERROR_EOF) {
            avcodec_send_packet(opened.codec.get(), nullptr);
            drain();
            break;
        }
        if (read < 0) {
            result.error = u"reading failed: "_s + averror(read);
            return result;
        }
        bool keepGoing = true;
        if (packet->stream_index == opened.stream->index) {
            const int sent = avcodec_send_packet(opened.codec.get(), packet.get());
            if (sent < 0 && sent != AVERROR(EAGAIN)) {
                result.error = u"decoding failed: "_s + averror(sent);
                keepGoing = false;
            } else {
                keepGoing = drain();
            }
        }
        av_packet_unref(packet.get());
        if (!keepGoing)
            break;
    }
    if (!result.error.isEmpty()) {
        result.images.clear();
        return result;
    }
    if (result.frames == 0) {
        result.error = u"no frames"_s;
        return result;
    }
    QImage lastImage;
    if (!convert(last.get(), &lastImage)) {
        result.error = u"cannot convert the frames"_s;
        return result;
    }
    result.images.insert(result.frames - 1, lastImage);
    double average = 0;
    if (result.frames > 1 && firstMs >= 0 && lastMs >= firstMs)
        average = (lastMs - firstMs) / double(result.frames - 1);
    if (average <= 0) {
        const AVRational rate = opened.stream->avg_frame_rate.num > 0 ? opened.stream->avg_frame_rate : opened.stream->r_frame_rate;
        average = rate.num > 0 && rate.den > 0 ? 1000.0 / av_q2d(rate) : 100.0;
    }
    const double tail = video ? (lastDurationMs > 0 ? lastDurationMs : average) : petFrameDelayMs(lastDurationMs > 0 ? lastDurationMs : average);
    result.durationMs = firstMs >= 0 && lastMs >= firstMs && !timesBroken ? stepsMs + tail : (video ? average : petFrameDelayMs(average)) * double(result.frames);
    result.size = size;
    result.ok = true;
    return result;
}

} // namespace murasaki
