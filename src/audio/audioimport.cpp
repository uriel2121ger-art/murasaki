#include "audioimport.h"

#include "petaudioasset.h"
#include "sha256.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>

#ifdef MURASAKI_HAVE_LIBAV
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/log.h>
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#endif

namespace murasaki::audio {

namespace {

// libavformat's names of the demuxers a sound may come from: MP3, WAV, Ogg (Vorbis and Opus), FLAC, MP4/M4A/MOV (AAC, ALAC; also the sound of a video),
// raw ADTS AAC, ASF (WMA) and Matroska/WebM. Nothing that reads other files or opens a device.
constexpr const char *kContainers[] = {"mp3", "wav", "ogg", "flac", "mov", "mp4", "m4a", "3gp", "3g2", "mj2", "aac", "asf", "matroska", "webm"};
constexpr const char *kWhitelist = "mp3,wav,ogg,flac,mov,mp4,m4a,3gp,3g2,mj2,aac,asf,matroska,webm";

std::string named(std::string_view name, const std::string &detail = {})
{
    std::string s(name);
    if (!detail.empty())
        s += ": " + detail;
    return s;
}

std::string jsonString(const std::string &in)
{
    std::string out = "\"";
    for (unsigned char c : in) {
        if (c == '"' || c == '\\') {
            out += '\\';
            out += char(c);
        } else if (c < 0x20 || c == 0x7f) {
            out += ' ';
        } else {
            out += char(c);
        }
    }
    return out + "\"";
}

} // namespace

bool audioContainerAllowed(const char *demuxerName)
{
    if (!demuxerName)
        return false;
    const std::string all(demuxerName);
    std::size_t from = 0;
    while (from <= all.size()) {
        std::size_t to = all.find(',', from);
        if (to == std::string::npos)
            to = all.size();
        const std::string one = all.substr(from, to - from);
        for (const char *allowed : kContainers)
            if (one == allowed)
                return true;
        from = to + 1;
    }
    return false;
}

const char *audioContainerWhitelist() { return kWhitelist; }

std::string displayName(const std::string &fileName)
{
    std::string out;
    for (unsigned char c : fileName) {
        if (c >= 0x20 && c != 0x7f)
            out += char(c);
        if (out.size() >= 80)
            break;
    }
    return out;
}

#ifndef MURASAKI_HAVE_LIBAV

bool importAvailable() { return false; }
ImportResult importAudio(const std::string &, const std::string &, const std::string &, const ImportSettings &, const ImportCallbacks &)
{
    ImportResult r;
    r.error = named(kAudioNotAudio, "this build has no libav");
    return r;
}

#else

bool importAvailable() { return true; }

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
struct SwrFree {
    void operator()(SwrContext *c) const { swr_free(&c); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;
using CodecPtr = std::unique_ptr<AVCodecContext, CodecFree>;
using PacketPtr = std::unique_ptr<AVPacket, PacketFree>;
using FramePtr = std::unique_ptr<AVFrame, FrameFree>;
using SwrPtr = std::unique_ptr<SwrContext, SwrFree>;

constexpr int kProbeSizeBytes = 4 * 1024 * 1024;
constexpr int kAnalyzeMicros = 5 * 1000 * 1000;
constexpr int kMaxStreams = 32;
constexpr int kMaxBadPackets = 16;
constexpr long long kMaxPackets = 1'000'000;
constexpr int kMinRate = 8000;
constexpr int kMaxRate = 192000;
constexpr int kMaxFrameSamples = 1 << 20;

struct DecoderRule {
    AVCodecID id;
    const char *names[3];
};
// Software decoders only, by codec id: the common sound codecs and nothing exotic (no WMA Pro or lossless, no speech codecs, no tracker formats).
constexpr DecoderRule kDecoders[] = {
    {AV_CODEC_ID_MP3, {"mp3float", "mp3", nullptr}},
    {AV_CODEC_ID_AAC, {"aac", nullptr, nullptr}},
    {AV_CODEC_ID_VORBIS, {"vorbis", "libvorbis", nullptr}},
    {AV_CODEC_ID_OPUS, {"libopus", "opus", nullptr}},
    {AV_CODEC_ID_FLAC, {"flac", nullptr, nullptr}},
    {AV_CODEC_ID_ALAC, {"alac", nullptr, nullptr}},
    {AV_CODEC_ID_WMAV1, {"wmav1", nullptr, nullptr}},
    {AV_CODEC_ID_WMAV2, {"wmav2", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S16LE, {"pcm_s16le", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S16BE, {"pcm_s16be", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S24LE, {"pcm_s24le", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S24BE, {"pcm_s24be", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S32LE, {"pcm_s32le", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S32BE, {"pcm_s32be", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_F32LE, {"pcm_f32le", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_F32BE, {"pcm_f32be", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_U8, {"pcm_u8", nullptr, nullptr}},
    {AV_CODEC_ID_PCM_S8, {"pcm_s8", nullptr, nullptr}},
};

bool decoderAcceptable(const AVCodec *codec, AVCodecID id)
{
    if (!codec || codec->id != id || !av_codec_is_decoder(codec))
        return false;
    if (codec->capabilities & (AV_CODEC_CAP_HARDWARE | AV_CODEC_CAP_EXPERIMENTAL))
        return false;
    const char *wrapper = codec->wrapper_name;
    return !wrapper || !std::strcmp(wrapper, "libopus") || !std::strcmp(wrapper, "libvorbis");
}

bool codecListed(AVCodecID id)
{
    for (const DecoderRule &rule : kDecoders)
        if (rule.id == id)
            return true;
    return false;
}

const AVCodec *chooseDecoder(AVCodecID id)
{
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

struct Guard {
    std::function<bool()> cancelled;
    std::chrono::steady_clock::time_point deadline;
    bool wasCancelled = false;
    bool timedOut = false;
    bool stop()
    {
        if (!wasCancelled && cancelled && cancelled())
            wasCancelled = true;
        if (!timedOut && std::chrono::steady_clock::now() >= deadline)
            timedOut = true;
        return wasCancelled || timedOut;
    }
    std::string why() const { return wasCancelled ? std::string("cancelled") : named(kAudioTimeout); }
    static int interrupt(void *self) { return static_cast<Guard *>(self)->stop() ? 1 : 0; }
};

std::string averror(int code)
{
    char buffer[128];
    av_strerror(code, buffer, sizeof buffer);
    return buffer;
}

void quiet()
{
    static const bool once = [] {
        av_log_set_level(AV_LOG_ERROR);
        return true;
    }();
    (void)once;
}

ImportResult fail(ImportResult r, const std::string &error)
{
    r.ok = false;
    r.error = error;
    return r;
}

// The SHA-256 of the file, read in blocks with the guard consulted between them.
bool hashFile(const std::string &path, Guard &guard, std::string *id)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return false;
    Sha256 sha;
    std::vector<unsigned char> block(64 * 1024);
    for (;;) {
        if (guard.stop()) {
            std::fclose(f);
            return false;
        }
        const std::size_t n = std::fread(block.data(), 1, block.size(), f);
        if (n == 0)
            break;
        sha.update(block.data(), n);
    }
    const bool good = !std::ferror(f);
    std::fclose(f);
    if (good)
        *id = Sha256::hex(sha.finish());
    return good;
}

} // namespace

ImportResult importAudio(const std::string &source, const std::string &outDir, const std::string &name, const ImportSettings &settings,
                         const ImportCallbacks &callbacks)
{
    quiet();
    ImportResult result;
    Guard guard;
    guard.cancelled = callbacks.cancelled;
    guard.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(std::max<long long>(1, settings.deadlineMs));
    const auto stopped = [&]() {
        ImportResult r = result;
        r.ok = false;
        r.cancelled = guard.wasCancelled;
        r.error = guard.why();
        return r;
    };

    struct stat st {};
    if (stat(source.c_str(), &st) != 0)
        return fail(result, "source not found");
    if (!S_ISREG(st.st_mode))
        return fail(result, "source not a regular file");
    if (st.st_size > kMaxPetAudioSourceBytes)
        return fail(result, named(kAudioOverTheLimit, "the source file"));
    if (!hashFile(source, guard, &result.assetId))
        return guard.stop() ? stopped() : fail(result, named(kAudioNotAudio, "the file cannot be read"));

    const std::string assetFile = assetPath(outDir, result.assetId);
    if (std::string ignored; MappedClip::open(assetFile, &ignored)) { // the same source, imported before
        std::string error;
        const auto clip = MappedClip::open(assetFile, &error);
        result.ok = true;
        result.reused = true;
        result.durationMs = static_cast<long long>(clip->frames()) * 1000 / kPetAudioRate;
        result.bytes = static_cast<long long>(clip->frames()) * 4 + static_cast<long long>(kWavHeaderBytes);
        return result;
    }

    AVFormatContext *raw = avformat_alloc_context();
    if (!raw)
        return fail(result, "helper failed: out of memory");
    raw->interrupt_callback.callback = &Guard::interrupt;
    raw->interrupt_callback.opaque = &guard;
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", kWhitelist, 0);
    av_dict_set_int(&options, "probesize", kProbeSizeBytes, 0);
    av_dict_set_int(&options, "analyzeduration", kAnalyzeMicros, 0);
    av_dict_set_int(&options, "max_streams", kMaxStreams, 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    const std::string url = "file:" + source;
    const int opened = avformat_open_input(&raw, url.c_str(), nullptr, &options); // frees raw on failure
    av_dict_free(&options);
    if (opened < 0) {
        if (guard.stop())
            return stopped();
        return fail(result, named(kAudioNotAudio, averror(opened)));
    }
    FormatPtr format(raw);
    const char *demuxer = raw->iformat ? raw->iformat->name : nullptr;
    if (!audioContainerAllowed(demuxer))
        return fail(result, named(kAudioContainerNotAllowed, demuxer ? std::string(demuxer).substr(0, 40) : std::string("?")));
    // The formats with no magic bytes of their own must have been recognised with some confidence.
    if (demuxer && (!std::strcmp(demuxer, "aac") || !std::strcmp(demuxer, "mp3")) && raw->probe_score <= AVPROBE_SCORE_RETRY)
        return fail(result, named(kAudioNotAudio, "the format is not certain"));
    result.sourceFormat = demuxer ? demuxer : "";

    // Before any decoder exists: which stream is ours. Every other stream gets no codec, so find_stream_info opens no decoder for it.
    int chosen = -1;
    for (unsigned i = 0; i < raw->nb_streams; ++i) {
        AVStream *s = raw->streams[i];
        AVCodecParameters *p = s->codecpar;
        const bool candidate = p->codec_type == AVMEDIA_TYPE_AUDIO && codecListed(p->codec_id) && !(s->disposition & AV_DISPOSITION_ATTACHED_PIC);
        if (candidate && chosen < 0) {
            chosen = int(i);
            continue;
        }
        s->discard = AVDISCARD_ALL;
        p->codec_id = AV_CODEC_ID_NONE;
    }
    if (chosen < 0) {
        bool anyAudio = false;
        for (unsigned i = 0; i < raw->nb_streams; ++i)
            anyAudio = anyAudio || raw->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO;
        return fail(result, anyAudio ? named(kAudioDecoderMissing) : named(kAudioNoStream));
    }
    if (const int info = avformat_find_stream_info(raw, nullptr); info < 0)
        return guard.stop() ? stopped() : fail(result, named(kAudioNotAudio, averror(info)));
    AVStream *stream = raw->streams[chosen];
    const AVCodecParameters *p = stream->codecpar;
    const int channels = p->ch_layout.nb_channels;
    if (channels < 1 || channels > kMaxPetAudioChannels || p->sample_rate < kMinRate || p->sample_rate > kMaxRate)
        return fail(result, named(kAudioNotAudio, "channels or sample rate out of range"));
    result.sourceCodec = avcodec_get_name(p->codec_id);
    result.sourceRate = p->sample_rate;
    result.channels = channels;
    long long claimedMs = -1;
    if (stream->duration != AV_NOPTS_VALUE && stream->duration > 0)
        claimedMs = std::llround(double(stream->duration) * av_q2d(stream->time_base) * 1000.0);
    else if (raw->duration != AV_NOPTS_VALUE && raw->duration > 0)
        claimedMs = raw->duration / 1000;
    if (claimedMs > kMaxPetAudioMs + 1000)
        return fail(result, named(kAudioTooLong));
    const AVCodec *codec = chooseDecoder(p->codec_id);
    if (!codec)
        return fail(result, named(kAudioDecoderMissing, avcodec_get_name(p->codec_id)));
    CodecPtr context(avcodec_alloc_context3(codec));
    if (!context || avcodec_parameters_to_context(context.get(), p) < 0)
        return fail(result, named(kAudioDecoderMissing));
    context->thread_count = 1;
    if (const int r = avcodec_open2(context.get(), codec, nullptr); r < 0)
        return fail(result, named(kAudioDecoderMissing, averror(r)));

    WavWriter writer(assetFile);
    if (!writer.ok())
        return fail(result, "not enough disk space");
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame)
        return fail(result, "helper failed: out of memory");

    SwrPtr swr;
    int swrRate = 0, swrChannels = 0;
    AVSampleFormat swrFormat = AV_SAMPLE_FMT_NONE;
    std::vector<std::int16_t> out;
    const long long maxFrames = kMaxPetAudioMs * kPetAudioRate / 1000;
    std::string failure;
    int bad = 0;
    long long packets = 0;
    bool decodedAny = false;

    // Converts what the decoder has (or flushes the converter when `input` is null) and writes it; false: `failure` says why.
    const auto convert = [&](const AVFrame *input) -> bool {
        if (input) {
            const int rate = input->sample_rate > 0 ? input->sample_rate : context->sample_rate;
            const int ch = input->ch_layout.nb_channels;
            if (ch < 1 || ch > kMaxPetAudioChannels || rate < kMinRate || rate > kMaxRate || input->nb_samples <= 0 || input->nb_samples > kMaxFrameSamples) {
                failure = named(kAudioNotAudio, "a frame is outside the limits");
                return false;
            }
            if (!swr || rate != swrRate || ch != swrChannels || AVSampleFormat(input->format) != swrFormat) {
                if (swr) { // the stream changed shape: what the old converter holds is written first
                    out.resize(std::size_t(swr_get_out_samples(swr.get(), 0) + 4096) * 2);
                    std::uint8_t *dst[1] = {reinterpret_cast<std::uint8_t *>(out.data())};
                    const int n = swr_convert(swr.get(), dst, int(out.size() / 2), nullptr, 0);
                    if (n > 0 && !writer.append(out.data(), std::size_t(n))) {
                        failure = named(kAudioOverTheLimit, "the sound is too big");
                        return false;
                    }
                }
                AVChannelLayout inLayout{};
                if (input->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC)
                    av_channel_layout_default(&inLayout, ch);
                else
                    av_channel_layout_copy(&inLayout, &input->ch_layout);
                AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
                SwrContext *raw = nullptr;
                const int made = swr_alloc_set_opts2(&raw, &outLayout, AV_SAMPLE_FMT_S16, kPetAudioRate, &inLayout, AVSampleFormat(input->format), rate, 0, nullptr);
                av_channel_layout_uninit(&inLayout);
                swr.reset(raw);
                if (made < 0 || !swr || swr_init(swr.get()) < 0) {
                    failure = named(kAudioDecoderMissing, "cannot convert the samples");
                    return false;
                }
                swrRate = rate;
                swrChannels = ch;
                swrFormat = AVSampleFormat(input->format);
            }
        }
        if (!swr)
            return true;
        const int room = swr_get_out_samples(swr.get(), input ? input->nb_samples : 0) + 64;
        out.resize(std::size_t(room) * 2);
        std::uint8_t *dst[1] = {reinterpret_cast<std::uint8_t *>(out.data())};
        const int n = swr_convert(swr.get(), dst, room, input ? const_cast<const std::uint8_t **>(input->extended_data) : nullptr, input ? input->nb_samples : 0);
        if (n < 0) {
            failure = named(kAudioNotAudio, averror(n));
            return false;
        }
        if (writer.frames() + std::size_t(n) > std::size_t(maxFrames)) {
            failure = named(kAudioTooLong);
            return false;
        }
        if (n > 0 && !writer.append(out.data(), std::size_t(n))) {
            failure = named(kAudioOverTheLimit, "the sound is too big");
            return false;
        }
        return true;
    };
    const auto drain = [&]() -> bool {
        for (;;) {
            const int r = avcodec_receive_frame(context.get(), frame.get());
            if (r == AVERROR(EAGAIN) || r == AVERROR_EOF)
                return true;
            if (r < 0) {
                if (++bad > kMaxBadPackets) {
                    if (decodedAny)
                        return true;
                    failure = named(kAudioNotAudio, averror(r));
                    return false;
                }
                return true;
            }
            bad = 0;
            decodedAny = true;
            const bool good = convert(frame.get());
            av_frame_unref(frame.get());
            if (!good)
                return false;
        }
    };

    bool eof = false;
    while (!eof) {
        if (guard.stop())
            return stopped();
        const int read = av_read_frame(raw, packet.get());
        if (read < 0) {
            if (guard.stop())
                return stopped();
            if (read != AVERROR_EOF && !decodedAny)
                return fail(result, named(kAudioNotAudio, averror(read)));
            break;
        }
        if (packet->stream_index != chosen) {
            av_packet_unref(packet.get());
            continue;
        }
        if (++packets > kMaxPackets)
            return fail(result, named(kAudioNotAudio, "too many packets"));
        const int sent = avcodec_send_packet(context.get(), packet.get());
        if (callbacks.progress && claimedMs > 0 && packet->pts != AV_NOPTS_VALUE)
            callbacks.progress(std::clamp(double(packet->pts) * av_q2d(stream->time_base) * 1000.0 / double(claimedMs), 0.0, 1.0));
        av_packet_unref(packet.get());
        if (sent < 0 && sent != AVERROR(EAGAIN) && sent != AVERROR_EOF && ++bad > kMaxBadPackets) {
            if (!decodedAny)
                return fail(result, named(kAudioNotAudio, averror(sent)));
            break;
        }
        if (!drain())
            return guard.stop() ? stopped() : fail(result, failure);
    }
    avcodec_send_packet(context.get(), nullptr);
    if (!drain())
        return guard.stop() ? stopped() : fail(result, failure);
    if (!convert(nullptr))
        return fail(result, failure);
    if (guard.stop())
        return stopped();
    const long long frames = static_cast<long long>(writer.frames());
    if (frames * 1000 < kMinPetAudioMs * kPetAudioRate)
        return fail(result, named(kAudioTooShort));
    if (!writer.finish())
        return fail(result, "not enough disk space");
    result.durationMs = frames * 1000 / kPetAudioRate;
    result.bytes = frames * 4 + static_cast<long long>(kWavHeaderBytes);
    if (channels > 2)
        result.degraded.push_back(std::string(kAudioDownmixed));
    if (swrRate != kPetAudioRate && swrRate != 0)
        result.degraded.push_back(std::string(kAudioResampled));
    // The record beside it. A failure to write it loses the name only.
    const std::string meta = assetFile.substr(0, assetFile.size() - 4) + ".json";
    if (FILE *m = std::fopen((meta + ".part").c_str(), "wb")) {
        std::fprintf(m, "{\"name\":%s,\"durationMs\":%lld,\"sourceFormat\":%s,\"sourceCodec\":%s,\"sourceRate\":%d,\"channels\":%d,\"createdAt\":%lld}\n",
                     jsonString(displayName(name)).c_str(), result.durationMs, jsonString(result.sourceFormat).c_str(), jsonString(result.sourceCodec).c_str(),
                     result.sourceRate, result.channels, static_cast<long long>(std::time(nullptr)));
        std::fclose(m);
        std::rename((meta + ".part").c_str(), meta.c_str());
    }
    result.ok = true;
    return result;
}

#endif // MURASAKI_HAVE_LIBAV

} // namespace murasaki::audio
