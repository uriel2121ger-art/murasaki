// The libav half of the video textures, built as a plugin (libmurasaki-scene-vk-video.so) that the engine loads with dlopen when a
// scene has a video: linking libavcodec into the engine itself puts 125 libraries (every codec's) in every scene's startup.
#include "assets/videoapi.h"
#include "assets/videodecoder.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#ifdef MURASAKI_VIDEO_EXPORT
#include <libavutil/hwcontext_vaapi.h>
#endif
#include <libavutil/imgutils.h>
#include <libavutil/mem.h>
#include <libswscale/swscale.h>
}

#ifdef MURASAKI_VIDEO_EXPORT
#include <va/va.h>
#include <va/va_drmcommon.h>
#endif

namespace murasaki::scene {

namespace {

constexpr size_t kFramesAhead = 4;           // decoded and waiting for their time
constexpr int kProbeBytes = 4 << 20;         // how much of the stream is read to learn its streams
constexpr int64_t kProbeMicroseconds = 2000000;
constexpr int kMaxSourceDimension = 8192;    // the stream's own size, before it is scaled down
constexpr uint32_t kMaxSurfaceVideoWidth = 3840;
constexpr uint32_t kMaxSurfaceVideoHeight = 2160;
constexpr uint64_t kMaxFileStreamBytes = 2ull << 30; // the package format stores a size in a signed 32 bits

// The decoders a wallpaper may ask for: the video codecs of a screen recording or a stock clip, nothing exotic.
bool decoderAllowed(AVCodecID id)
{
    return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC || id == AV_CODEC_ID_VP8 || id == AV_CODEC_ID_VP9 || id == AV_CODEC_ID_AV1 ||
           id == AV_CODEC_ID_MPEG4;
}

struct Source {
    const uint8_t *data = nullptr; // a stream held in memory ...
    size_t size = 0;
    size_t pos = 0;
    int fd = -1;                   // ... or a region of a file: `base` bytes in, `size` long, read on demand
    uint64_t base = 0;
};

int readSource(void *opaque, uint8_t *buffer, int size)
{
    auto *s = static_cast<Source *>(opaque);
    if (s->pos >= s->size) {
        return AVERROR_EOF;
    }
    const size_t n = std::min(static_cast<size_t>(std::max(size, 0)), s->size - s->pos);
    if (s->fd >= 0) {
        const ssize_t got = ::pread(s->fd, buffer, n, static_cast<off_t>(s->base + s->pos));
        if (got <= 0) {
            return AVERROR(EIO);
        }
        s->pos += static_cast<size_t>(got);
        return static_cast<int>(got);
    }
    std::memcpy(buffer, s->data + s->pos, n);
    s->pos += n;
    return static_cast<int>(n);
}

int64_t seekSource(void *opaque, int64_t offset, int whence)
{
    auto *s = static_cast<Source *>(opaque);
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE) {
        return static_cast<int64_t>(s->size);
    }
    int64_t target = 0;
    if (whence == SEEK_SET) {
        target = offset;
    } else if (whence == SEEK_CUR) {
        target = static_cast<int64_t>(s->pos) + offset;
    } else if (whence == SEEK_END) {
        target = static_cast<int64_t>(s->size) + offset;
    } else {
        return AVERROR(EINVAL);
    }
    if (target < 0 || target > static_cast<int64_t>(s->size)) {
        return AVERROR(EINVAL);
    }
    s->pos = static_cast<size_t>(target);
    return target;
}

struct FrameDeleter {
    void operator()(AVFrame *f) const { av_frame_free(&f); }
};

struct Picture {
    double time = 0.0;
    double pts = 0.0; // where in the stream (time without the loops before it)
    std::vector<uint8_t> rgba;
    std::unique_ptr<AVFrame, FrameDeleter> surface; // zero-copy: the decoder's own surface, kept alive while it may be shown
};

uint32_t evenAtLeastTwo(double v)
{
    const uint32_t n = static_cast<uint32_t>(std::max(2.0, v));
    return n & ~1u;
}

} // namespace

struct Decoder {
    const uint8_t *data = nullptr; // the caller's bytes: they outlive the decoder
    size_t size = 0;
    int fileFd = -1;               // a file region instead of bytes: opened by apiOpenFile, closed with the decoder
    uint64_t fileBase = 0;
    Source source;
    AVIOContext *avio = nullptr;
    AVFormatContext *format = nullptr;
    AVCodecContext *codec = nullptr;
    SwsContext *scaler = nullptr;
    AVBufferRef *hwDevice = nullptr;
    AVFrame *transfer = nullptr; // hardware frames come down to the CPU here
    bool usesHardware = false;
    bool wantSurfaces = false;   // the engine asked for kMurasakiVideoZeroCopy
    bool surfaceMode = false;    // the pictures stay GPU surfaces (exportFrame); converted on the spot when frameAt asks for RGBA
    bool exportBroken = false;   // an export failed: no more tries
    bool pendingCopy = false;    // the picture an export could not give is to be delivered by the next frameAt
    std::atomic<double> maxFps{0.0};
    double lastKept = -1e30;     // the time of the last picture handed on
    int stream = -1;
    AVRational timeBase{1, 1};
    double frameDuration = 1.0 / 30.0;
    double duration = 0.0;
    uint32_t width = 0, height = 0;

    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;      // the consumer made room, or the decoder is to stop
    std::condition_variable produced;  // a picture is in the queue, or the thread ended
    std::deque<Picture> queue;
    Picture current;
    bool haveCurrent = false;
    bool stop = false;
    std::atomic<bool> loop{true};  // the stream starts over at its end; false: it waits there
    bool seekPending = false;      // a seek was asked for (under the mutex) ...
    double seekTarget = 0.0;       // ... to here
    bool atEnd = false;            // the decoder has delivered the last picture of a stream that does not loop and waits
    double shownPos = 0.0;         // stream position of the picture on show, or of the seek that has no picture yet
    double skipUntil = -1e30;      // decoder thread: pictures before this time (a seek lands on the keyframe before its target) are dropped
    AVFrame *skipped = nullptr;    // decoder thread: the last of those, shown when the run ends before the target (a seek to the end)
    double skippedTime = 0.0;
    uint64_t generation = 0;       // under the mutex: one more for each seek; a picture decoded before it is not queued
    bool finished = false;             // the thread has ended (a failure or a stream with nothing in it)

    double loopOffset = 0.0;
    double lastEnd = 0.0;
    int picturesThisRun = 0;

    ~Decoder()
    {
        teardown();
        if (fileFd >= 0) {
            ::close(fileFd);
        }
    }

    void teardown()
    {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        wake.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
        queue.clear();   // the surfaces go back to the decoder's pool before the decoder goes
        current = {};
        sws_freeContext(scaler);
        scaler = nullptr;
        av_frame_free(&transfer);
        av_frame_free(&skipped);
        avcodec_free_context(&codec);
        av_buffer_unref(&hwDevice);
        if (format) {
            avformat_close_input(&format);
        }
        if (avio) {
            av_freep(&avio->buffer);
            avio_context_free(&avio);
        }
        queue.clear();
        haveCurrent = false;
        stop = false;
        finished = false;
        seekPending = false;
        atEnd = false;
        shownPos = 0.0;
        skipUntil = -1e30;
        loopOffset = lastEnd = 0.0;
        lastKept = -1e30;
        picturesThisRun = 0;
        usesHardware = false;
        surfaceMode = false;
        exportBroken = false;
        pendingCopy = false;
    }

    // get_format: the VAAPI surface format when the decoder offers it, else the first (software) one
    static AVPixelFormat pickFormat(AVCodecContext *, const AVPixelFormat *formats)
    {
        for (const AVPixelFormat *f = formats; *f != AV_PIX_FMT_NONE; ++f) {
            if (*f == AV_PIX_FMT_VAAPI) {
                return *f;
            }
        }
        return formats[0];
    }

    bool start(std::string &error, bool hardware, const std::string &renderNode)
    {
        source = {data, size, 0, fileFd, fileBase};
        format = avformat_alloc_context();
        auto *buffer = static_cast<unsigned char *>(av_malloc(32768));
        if (!format || !buffer) {
            av_free(buffer);
            error = "out of memory";
            return false;
        }
        avio = avio_alloc_context(buffer, 32768, 0, &source, &readSource, nullptr, &seekSource);
        if (!avio) {
            av_free(buffer);
            error = "out of memory";
            return false;
        }
        format->pb = avio;
        format->flags |= AVFMT_FLAG_CUSTOM_IO;
        format->probesize = kProbeBytes;
        format->max_analyze_duration = kProbeMicroseconds;
        // only MP4: the demuxer is named, never guessed
        const AVInputFormat *mov = av_find_input_format("mov");
        if (!mov) {
            error = "this libav has no MP4 demuxer";
            return false;
        }
        if (avformat_open_input(&format, nullptr, mov, nullptr) < 0) {
            error = "not a readable MP4 stream";
            return false; // (avformat_open_input frees the context itself on failure)
        }
        if (avformat_find_stream_info(format, nullptr) < 0) {
            error = "the MP4 stream has no stream information";
            return false;
        }
        const AVCodec *decoder = nullptr;
        stream = av_find_best_stream(format, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
        if (stream < 0 || !decoder) {
            error = "the MP4 stream has no video";
            return false;
        }
        if (!decoderAllowed(decoder->id)) {
            error = std::string("video codec ") + avcodec_get_name(decoder->id) + " is not one of the allowed ones";
            return false;
        }
        AVStream *st = format->streams[stream];
        const int srcW = st->codecpar->width, srcH = st->codecpar->height;
        if (srcW <= 0 || srcH <= 0 || srcW > kMaxSourceDimension || srcH > kMaxSourceDimension) {
            error = "the video's size is out of range";
            return false;
        }
        codec = avcodec_alloc_context3(decoder);
        if (!codec || avcodec_parameters_to_context(codec, st->codecpar) < 0) {
            error = "cannot set up the video decoder";
            return false;
        }
        codec->thread_count = 2;
        if (hardware) {
            // VAAPI when this decoder has a configuration for it and the device opens; any other outcome is the software path
            bool offered = false;
            for (int i = 0; const AVCodecHWConfig *config = avcodec_get_hw_config(decoder, i); ++i) {
                offered = offered || ((config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) && config->device_type == AV_HWDEVICE_TYPE_VAAPI);
            }
            if (offered && av_hwdevice_ctx_create(&hwDevice, AV_HWDEVICE_TYPE_VAAPI, renderNode.empty() ? nullptr : renderNode.c_str(), nullptr, 0) >= 0) {
                codec->hw_device_ctx = av_buffer_ref(hwDevice);
                codec->get_format = &Decoder::pickFormat;
                codec->thread_count = 1;
                usesHardware = true;
                surfaceMode = wantSurfaces;
                // pictures held for the engine (the queue and the one on show) are surfaces the decoder cannot reuse
                codec->extra_hw_frames = static_cast<int>(kFramesAhead) + 4;
            } else {
                av_buffer_unref(&hwDevice);
            }
        }
        if (avcodec_open2(codec, decoder, nullptr) < 0) {
            error = "cannot open the video decoder";
            return false;
        }
        timeBase = st->time_base.den > 0 && st->time_base.num > 0 ? st->time_base : AVRational{1, 1000};
        const AVRational rate = av_guess_frame_rate(format, st, nullptr);
        if (rate.num > 0 && rate.den > 0) {
            frameDuration = std::clamp(static_cast<double>(rate.den) / rate.num, 1.0 / 240.0, 1.0);
        }
        if (st->duration > 0) {
            duration = static_cast<double>(st->duration) * av_q2d(timeBase);
        } else if (format->duration > 0) {
            duration = static_cast<double>(format->duration) / AV_TIME_BASE;
        }
        // The picture is as big as the stream up to what it costs: through the CPU (software scaling, a copy and an upload) 1080p, as a
        // surface drawn by the GPU 4K (33 MB of video memory for the texture, nothing on the CPU).
        const uint32_t limitW = surfaceMode ? kMaxSurfaceVideoWidth : kMaxVideoWidth;
        const uint32_t limitH = surfaceMode ? kMaxSurfaceVideoHeight : kMaxVideoHeight;
        const double scale = std::min({1.0, static_cast<double>(limitW) / srcW, static_cast<double>(limitH) / srcH});
        width = evenAtLeastTwo(srcW * scale);
        height = evenAtLeastTwo(srcH * scale);
        worker = std::thread([this] { run(); });
        // the first picture is there before the texture is made
        std::unique_lock lock(mutex);
        if (!produced.wait_for(lock, std::chrono::seconds(10), [&] { return !queue.empty() || finished; }) || queue.empty()) {
            error = "the video has no picture to decode";
            return false;
        }
        return true;
    }

    // One decoded frame into a Picture; false when it cannot be converted.
    bool convert(const AVFrame *source, Picture &out)
    {
        const AVFrame *frame = source;
        if (source->format == AV_PIX_FMT_VAAPI) {
            // a GPU surface: its pixels come down to the CPU (NV12) before the scaler makes RGBA of them
            if (!transfer) {
                transfer = av_frame_alloc();
            }
            av_frame_unref(transfer);
            if (!transfer || av_hwframe_transfer_data(transfer, source, 0) < 0) {
                return false;
            }
            transfer->colorspace = source->colorspace;
            transfer->color_range = source->color_range;
            frame = transfer;
        }
        scaler = sws_getCachedContext(scaler, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), static_cast<int>(width),
                                      static_cast<int>(height), AV_PIX_FMT_RGBA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!scaler) {
            return false;
        }
        // the matrix the stream says it was made with, else HD for a picture of HD size: WE's own players do the same
        const bool hd = frame->colorspace == AVCOL_SPC_BT709 || (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720);
        const int *srcMatrix = sws_getCoefficients(hd ? SWS_CS_ITU709 : SWS_CS_ITU601);
        sws_setColorspaceDetails(scaler, srcMatrix, frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0, sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16,
                                 1 << 16);
        out.rgba.resize(static_cast<size_t>(width) * height * 4);
        uint8_t *dst[4] = {out.rgba.data(), nullptr, nullptr, nullptr};
        const int dstStride[4] = {static_cast<int>(width) * 4, 0, 0, 0};
        return sws_scale(scaler, frame->data, frame->linesize, 0, frame->height, dst, dstStride) == static_cast<int>(height);
    }

    // Waits for room in the queue; false when the decoder is to stop.
    bool room()
    {
        std::unique_lock lock(mutex);
        wake.wait(lock, [&] { return stop || seekPending || queue.size() < kFramesAhead; });
        return !stop;
    }

    bool seekRequested(uint64_t *generationNow = nullptr)
    {
        std::lock_guard lock(mutex);
        if (generationNow) {
            *generationNow = generation;
        }
        return seekPending;
    }

    // Carries out the seek asked for: what is queued is from before it, the decoder starts again from the keyframe at or before the target.
    bool applySeek()
    {
        double target = 0.0;
        {
            std::lock_guard lock(mutex);
            if (!seekPending) {
                return false;
            }
            seekPending = false;
            target = seekTarget;
            queue.clear();
            atEnd = false;
        }
        avcodec_flush_buffers(codec);
        const int64_t ts = static_cast<int64_t>(target / av_q2d(timeBase));
        if (av_seek_frame(format, stream, ts, AVSEEK_FLAG_BACKWARD) < 0) {
            av_seek_frame(format, stream, 0, AVSEEK_FLAG_BACKWARD);
            target = 0.0;
        }
        loopOffset = 0.0;
        lastEnd = target;
        lastKept = -1e30;
        picturesThisRun = 0;
        skipUntil = target - frameDuration;
        if (skipped) {
            av_frame_unref(skipped);
        }
        return true;
    }

    // false when a seek came after the picture was decoded (`decodedIn`): it is from before the seek and is dropped
    bool push(Picture &&p, uint64_t decodedIn)
    {
        {
            std::lock_guard lock(mutex);
            if (decodedIn != generation) {
                return false;
            }
            queue.push_back(std::move(p));
        }
        produced.notify_all();
        return true;
    }

    // A decoded frame (unreferenced here) is queued as a picture: the surface itself, or its RGBA; false when it cannot be converted.
    bool deliver(AVFrame *frame, Picture &&picture, uint64_t decodedIn)
    {
        lastKept = picture.time;
        if (surfaceMode && frame->format == AV_PIX_FMT_VAAPI) {
            picture.surface.reset(av_frame_clone(frame));
            av_frame_unref(frame);
            if (!picture.surface) {
                return false;
            }
        } else {
            const bool converted = convert(frame, picture);
            av_frame_unref(frame);
            if (!converted) {
                return false;
            }
        }
        lastEnd = std::max(lastEnd, picture.time + frameDuration);
        if (push(std::move(picture), decodedIn)) {
            ++picturesThisRun;
        }
        return true;
    }

    // Takes every frame the decoder has ready.
    bool drain(AVFrame *frame)
    {
        for (;;) {
            const int r = avcodec_receive_frame(codec, frame);
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF) {
                return true;
            }
            if (r < 0) {
                return false;
            }
            if (!room()) {
                av_frame_unref(frame);
                return false;
            }
            uint64_t decodedIn = 0;
            if (seekRequested(&decodedIn)) {
                av_frame_unref(frame); // from before the seek
                return true;
            }
            Picture picture;
            const int64_t pts = frame->best_effort_timestamp;
            // a frame without a time follows the one before it
            picture.time = pts == AV_NOPTS_VALUE ? lastEnd : static_cast<double>(pts) * av_q2d(timeBase) + loopOffset;
            picture.pts = picture.time - loopOffset;
            if (picture.time < skipUntil) {
                if (!skipped) {
                    skipped = av_frame_alloc();
                }
                if (skipped) {
                    av_frame_unref(skipped);
                    av_frame_move_ref(skipped, frame);
                    skippedTime = picture.time;
                } else {
                    av_frame_unref(frame);
                }
                continue;
            }
            // a picture the scene would not show is decoded (the stream needs it) and not converted
            const double cap = maxFps.load(std::memory_order_relaxed);
            if (cap > 0.0 && picture.time < lastKept + 0.9 / cap && picture.time >= lastKept) {
                av_frame_unref(frame);
                lastEnd = std::max(lastEnd, picture.time + frameDuration);
                continue;
            }
            if (!deliver(frame, std::move(picture), decodedIn)) {
                return false;
            }
        }
    }

    void run()
    {
        AVPacket *packet = av_packet_alloc();
        AVFrame *frame = av_frame_alloc();
        bool ok = packet && frame;
        while (ok) {
            if (applySeek()) {
                continue;
            }
            if (!room()) {
                break;
            }
            if (seekRequested()) {
                continue;
            }
            const int r = av_read_frame(format, packet);
            if (r == AVERROR_EOF) {
                // the end of a run: what the decoder still holds, then back to the start for the next one (or, for a stream that does
                // not loop, nothing until a seek or the loop comes)
                avcodec_send_packet(codec, nullptr);
                ok = drain(frame);
                if (seekRequested()) {
                    continue;
                }
                // a seek past the last picture (to the end, or a container longer than its video): the last picture there is is shown
                uint64_t decodedIn = 0;
                if (ok && picturesThisRun == 0 && skipped && skipped->buf[0] && room() && !seekRequested(&decodedIn)) {
                    Picture last;
                    last.time = skippedTime;
                    last.pts = skippedTime - loopOffset;
                    ok = deliver(skipped, std::move(last), decodedIn);
                }
                if (seekRequested()) {
                    continue;
                }
                if (!ok || picturesThisRun == 0) {
                    break; // a stream that gives nothing would spin
                }
                if (!loop.load()) {
                    std::unique_lock lock(mutex);
                    atEnd = true;
                    wake.wait(lock, [&] { return stop || seekPending || loop.load(); });
                    atEnd = false;
                    if (stop) {
                        break;
                    }
                    if (seekPending) {
                        continue;
                    }
                }
                avcodec_flush_buffers(codec);
                if (av_seek_frame(format, stream, 0, AVSEEK_FLAG_BACKWARD) < 0) {
                    break;
                }
                loopOffset = lastEnd;
                skipUntil = -1e30;
                if (skipped) {
                    av_frame_unref(skipped);
                }
                picturesThisRun = 0;
                continue;
            }
            if (r < 0) {
                break;
            }
            if (packet->stream_index == stream) {
                if (avcodec_send_packet(codec, packet) >= 0) {
                    ok = drain(frame);
                }
            }
            av_packet_unref(packet);
        }
        av_frame_free(&frame);
        av_packet_free(&packet);
        {
            std::lock_guard lock(mutex);
            finished = true;
        }
        produced.notify_all();
    }
};


// Moves the queue on to the picture of `seconds`; true when the picture on show changed.
bool advance(Decoder &d, double seconds)
{
    bool changed = false;
    {
        std::lock_guard lock(d.mutex);
        // the first picture is shown whatever the clock says; after it, every picture whose time has come replaces the one before
        while (!d.queue.empty() && (!d.haveCurrent || d.queue.front().time <= seconds)) {
            d.current = std::move(d.queue.front());
            d.queue.pop_front();
            d.haveCurrent = true;
            d.shownPos = d.current.pts;
            changed = true;
        }
    }
    if (changed) {
        d.wake.notify_all();
    }
    return changed;
}

const uint8_t *frameAtOf(Decoder &d, double seconds)
{
    const bool changed = advance(d, seconds);
    if (!changed && !d.pendingCopy) {
        return nullptr;
    }
    d.pendingCopy = false;
    if (d.current.rgba.empty() && d.current.surface) {
        // a surface the engine did not take as a dma-buf: its pixels come down here, on the caller's thread
        Picture copy;
        if (!d.convert(d.current.surface.get(), copy)) {
            return nullptr;
        }
        d.current.rgba = std::move(copy.rgba);
    }
    return d.current.rgba.empty() ? nullptr : d.current.rgba.data();
}

void exportFrameOf(Decoder &d, double seconds, MurasakiVideoExport *out)
{
    *out = {};
    out->fd = -1;
    out->status = -1;
    if (!advance(d, seconds) && !d.pendingCopy) {
        out->status = d.exportBroken ? -1 : 0;
        return;
    }
#ifdef MURASAKI_VIDEO_EXPORT
    const AVFrame *frame = d.current.surface.get();
    if (!frame || d.exportBroken || !d.hwDevice || frame->format != AV_PIX_FMT_VAAPI) {
        d.pendingCopy = true;
        return;
    }
    const auto *device = reinterpret_cast<const AVHWDeviceContext *>(d.hwDevice->data);
    const auto *vaapi = static_cast<const AVVAAPIDeviceContext *>(device->hwctx);
    const VASurfaceID surface = static_cast<VASurfaceID>(reinterpret_cast<uintptr_t>(frame->data[3]));
    VADRMPRIMESurfaceDescriptor desc{};
    // the decoder must be done with the surface before anything else reads it
    if (vaSyncSurface(vaapi->display, surface) != VA_STATUS_SUCCESS ||
        vaExportSurfaceHandle(vaapi->display, surface, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_READ_ONLY | VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                              &desc) != VA_STATUS_SUCCESS) {
        d.exportBroken = true;
        d.pendingCopy = true;
        return;
    }
    const auto closeAll = [&] {
        for (uint32_t i = 0; i < desc.num_objects && i < 4; ++i) {
            if (desc.objects[i].fd >= 0) {
                ::close(desc.objects[i].fd);
            }
        }
    };
    // one buffer holding every plane is what the engine can import
    if (desc.num_objects != 1 || desc.num_layers != 1 || desc.layers[0].num_planes < 1 || desc.layers[0].num_planes > 4) {
        closeAll();
        d.exportBroken = true;
        d.pendingCopy = true;
        return;
    }
    const auto &layer = desc.layers[0];
    out->status = 1;
    out->fourcc = layer.drm_format;
    out->modifier = desc.objects[0].drm_format_modifier;
    out->surfaceWidth = desc.width;
    out->surfaceHeight = desc.height;
    out->pictureWidth = static_cast<uint32_t>(frame->width);
    out->pictureHeight = static_cast<uint32_t>(frame->height);
    out->planes = layer.num_planes;
    out->fd = desc.objects[0].fd;
    out->size = desc.objects[0].size;
    for (uint32_t i = 0; i < layer.num_planes; ++i) {
        out->offsets[i] = layer.offset[i];
        out->pitches[i] = layer.pitch[i];
    }
    out->colorspace = frame->colorspace == AVCOL_SPC_BT2020_NCL || frame->colorspace == AVCOL_SPC_BT2020_CL                       ? 2u
                      : (frame->colorspace == AVCOL_SPC_BT709 || (frame->colorspace == AVCOL_SPC_UNSPECIFIED && frame->height >= 720)) ? 1u
                                                                                                                                     : 0u;
    out->fullRange = frame->color_range == AVCOL_RANGE_JPEG ? 1u : 0u;
    d.pendingCopy = false;
#else
    d.exportBroken = true;
    d.pendingCopy = true;
#endif
}

void *openDecoder(std::unique_ptr<Decoder> decoder, const char *renderNode, uint32_t flags, char *error, size_t errorSize)
{
    std::string why;
    const auto fail = [&](const std::string &text) -> void * {
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "%s", text.c_str());
        }
        return nullptr;
    };
    const std::string node = renderNode ? renderNode : "";
    const bool hardware = (flags & kMurasakiVideoHardware) != 0;
    decoder->wantSurfaces = (flags & kMurasakiVideoZeroCopy) != 0;
    // the GPU first; when it cannot set up, or gives no first picture, everything is undone and the CPU decodes
    if (hardware && decoder->start(why, true, node)) {
        return decoder.release();
    }
    if (hardware) {
        decoder->teardown();
    }
    if (!decoder->start(why, false, {})) {
        return fail(why);
    }
    return decoder.release();
}

void *apiOpen(const uint8_t *bytes, size_t size, const char *renderNode, uint32_t flags, char *error, size_t errorSize)
{
    if (!bytes || size == 0) {
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "%s", "the video texture has no stream");
        }
        return nullptr;
    }
    auto decoder = std::make_unique<Decoder>();
    decoder->data = bytes;
    decoder->size = size;
    return openDecoder(std::move(decoder), renderNode, flags, error, errorSize);
}

void *apiOpenFile(const char *path, uint64_t offset, uint64_t size, const char *renderNode, uint32_t flags, char *error, size_t errorSize)
{
    const auto fail = [&](const char *text) -> void * {
        if (error && errorSize > 0) {
            std::snprintf(error, errorSize, "%s", text);
        }
        return nullptr;
    };
    if (!path || size == 0 || size > kMaxFileStreamBytes) {
        return fail(size > kMaxFileStreamBytes ? "the video stream is too long to play" : "the video texture has no stream");
    }
    auto decoder = std::make_unique<Decoder>();
    decoder->fileFd = ::open(path, O_RDONLY | O_CLOEXEC);
    if (decoder->fileFd < 0) {
        return fail("the file with the video stream cannot be opened");
    }
    decoder->fileBase = offset;
    decoder->size = static_cast<size_t>(size);
    return openDecoder(std::move(decoder), renderNode, flags, error, errorSize);
}

void apiClose(void *handle) { delete static_cast<Decoder *>(handle); }
uint32_t apiWidth(void *handle) { return static_cast<Decoder *>(handle)->width; }
uint32_t apiHeight(void *handle) { return static_cast<Decoder *>(handle)->height; }
double apiDuration(void *handle) { return static_cast<Decoder *>(handle)->duration; }
int apiHardware(void *handle) { return static_cast<Decoder *>(handle)->usesHardware ? 1 : 0; }
void apiSetMaxFps(void *handle, double fps)
{
    static_cast<Decoder *>(handle)->maxFps.store(std::isfinite(fps) && fps > 0.0 ? fps : 0.0, std::memory_order_relaxed);
}
const uint8_t *apiFrameAt(void *handle, double seconds) { return frameAtOf(*static_cast<Decoder *>(handle), seconds); }

void apiExportFrame(void *handle, double seconds, MurasakiVideoExport *out) { exportFrameOf(*static_cast<Decoder *>(handle), seconds, out); }

void apiSeek(void *handle, double seconds)
{
    auto &d = *static_cast<Decoder *>(handle);
    {
        std::lock_guard lock(d.mutex);
        const double longest = d.duration > 0.0 ? d.duration : 1e9;
        d.seekTarget = std::isfinite(seconds) ? std::clamp(seconds, 0.0, longest) : 0.0;
        d.seekPending = true;
        ++d.generation;
        d.queue.clear();
        d.atEnd = false;
        d.haveCurrent = false; // the first picture of there is shown whatever the clock says
        d.shownPos = d.seekTarget;
    }
    d.wake.notify_all();
}

void apiSetLoop(void *handle, int loop)
{
    auto &d = *static_cast<Decoder *>(handle);
    {
        std::lock_guard lock(d.mutex);
        d.loop.store(loop != 0);
    }
    d.wake.notify_all();
}

double apiPosition(void *handle)
{
    auto &d = *static_cast<Decoder *>(handle);
    std::lock_guard lock(d.mutex);
    return d.shownPos;
}

int apiEnded(void *handle)
{
    auto &d = *static_cast<Decoder *>(handle);
    std::lock_guard lock(d.mutex);
    return d.atEnd && d.queue.empty() ? 1 : 0;
}

const MurasakiVideoApi kApi = {kMurasakiVideoApiVersion, &apiOpen, &apiClose, &apiWidth, &apiHeight, &apiDuration, &apiHardware, &apiSetMaxFps, &apiFrameAt,
                               &apiExportFrame, &apiOpenFile, &apiSeek, &apiSetLoop, &apiPosition, &apiEnded};

} // namespace murasaki::scene

extern "C" const MurasakiVideoApi *murasaki_video_api()
{
    return &murasaki::scene::kApi;
}
