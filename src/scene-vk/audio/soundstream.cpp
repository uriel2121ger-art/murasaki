#include "audio/soundstream.h"

#include <dr_flac.h>
#include <dr_mp3.h>
#include <dr_wav.h>

extern "C" {
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
}

#include <algorithm>
#include <cmath>
#include <cstring>

namespace murasaki::scene {

namespace {

// One codec behind the stream: frames of interleaved float in the file's own rate and channel count.
class Decoder
{
public:
    virtual ~Decoder() = default;
    virtual uint32_t rate() const = 0;
    virtual uint32_t channels() const = 0;
    // Up to `frames` frames; 0 at the end.
    virtual size_t read(size_t frames, float *out) = 0;
    virtual void rewind() = 0;
    virtual const char *name() const = 0;
};

class Mp3Decoder final : public Decoder
{
public:
    static std::unique_ptr<Mp3Decoder> open(const std::vector<uint8_t> &bytes)
    {
        auto d = std::unique_ptr<Mp3Decoder>(new Mp3Decoder);
        if (!drmp3_init_memory(&d->m_decoder, bytes.data(), bytes.size(), nullptr)) {
            return nullptr;
        }
        d->m_open = true;
        return d;
    }
    ~Mp3Decoder() override
    {
        if (m_open) {
            drmp3_uninit(&m_decoder);
        }
    }
    uint32_t rate() const override { return m_decoder.sampleRate; }
    uint32_t channels() const override { return m_decoder.channels; }
    size_t read(size_t frames, float *out) override { return static_cast<size_t>(drmp3_read_pcm_frames_f32(&m_decoder, frames, out)); }
    void rewind() override { drmp3_seek_to_pcm_frame(&m_decoder, 0); }
    const char *name() const override { return "mp3"; }

private:
    Mp3Decoder() = default;
    drmp3 m_decoder{};
    bool m_open = false;
};

class FlacDecoder final : public Decoder
{
public:
    static std::unique_ptr<FlacDecoder> open(const std::vector<uint8_t> &bytes)
    {
        auto d = std::unique_ptr<FlacDecoder>(new FlacDecoder);
        d->m_flac = drflac_open_memory(bytes.data(), bytes.size(), nullptr);
        return d->m_flac ? std::move(d) : nullptr;
    }
    ~FlacDecoder() override
    {
        if (m_flac) {
            drflac_close(m_flac);
        }
    }
    uint32_t rate() const override { return m_flac->sampleRate; }
    uint32_t channels() const override { return m_flac->channels; }
    size_t read(size_t frames, float *out) override { return static_cast<size_t>(drflac_read_pcm_frames_f32(m_flac, frames, out)); }
    void rewind() override { drflac_seek_to_pcm_frame(m_flac, 0); }
    const char *name() const override { return "flac"; }

private:
    FlacDecoder() = default;
    drflac *m_flac = nullptr;
};

class WavDecoder final : public Decoder
{
public:
    static std::unique_ptr<WavDecoder> open(const std::vector<uint8_t> &bytes)
    {
        auto d = std::unique_ptr<WavDecoder>(new WavDecoder);
        if (!drwav_init_memory(&d->m_wav, bytes.data(), bytes.size(), nullptr)) {
            return nullptr;
        }
        d->m_open = true;
        return d;
    }
    ~WavDecoder() override
    {
        if (m_open) {
            drwav_uninit(&m_wav);
        }
    }
    uint32_t rate() const override { return m_wav.sampleRate; }
    uint32_t channels() const override { return m_wav.channels; }
    size_t read(size_t frames, float *out) override { return static_cast<size_t>(drwav_read_pcm_frames_f32(&m_wav, frames, out)); }
    void rewind() override { drwav_seek_to_pcm_frame(&m_wav, 0); }
    const char *name() const override { return "wav"; }

private:
    WavDecoder() = default;
    drwav m_wav{};
    bool m_open = false;
};

class VorbisDecoder final : public Decoder
{
public:
    static std::unique_ptr<VorbisDecoder> open(const std::vector<uint8_t> &bytes)
    {
        auto d = std::unique_ptr<VorbisDecoder>(new VorbisDecoder);
        int error = 0;
        d->m_vorbis = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
        if (!d->m_vorbis) {
            return nullptr;
        }
        d->m_info = stb_vorbis_get_info(d->m_vorbis);
        return d;
    }
    ~VorbisDecoder() override
    {
        if (m_vorbis) {
            stb_vorbis_close(m_vorbis);
        }
    }
    uint32_t rate() const override { return m_info.sample_rate; }
    uint32_t channels() const override { return static_cast<uint32_t>(m_info.channels); }
    size_t read(size_t frames, float *out) override
    {
        const int n = stb_vorbis_get_samples_float_interleaved(m_vorbis, m_info.channels, out, static_cast<int>(frames) * m_info.channels);
        return n > 0 ? static_cast<size_t>(n) : 0;
    }
    void rewind() override { stb_vorbis_seek_start(m_vorbis); }
    const char *name() const override { return "vorbis"; }

private:
    VorbisDecoder() = default;
    stb_vorbis *m_vorbis = nullptr;
    stb_vorbis_info m_info{};
};

bool startsWith(const std::vector<uint8_t> &b, const char *magic, size_t at = 0)
{
    const size_t n = std::strlen(magic);
    return b.size() >= at + n && std::memcmp(b.data() + at, magic, n) == 0;
}

} // namespace

struct SoundStream::Impl {
    std::vector<uint8_t> bytes;
    std::unique_ptr<Decoder> decoder;
    uint32_t outputRate = 48000;
    // resampler state: decoded source frames not yet consumed, and the read position inside them
    std::vector<float> scratch; // one chunk of what the decoder gave, in its own channel count
    std::vector<float> pending; // stereo frames
    size_t pendingFrames = 0;
    double position = 0.0;
    bool sourceDone = false;

    bool fill(size_t wanted)
    {
        // keep at least `wanted` + 2 frames ahead of the read position
        const uint32_t channels = decoder->channels();
        while (!sourceDone && pendingFrames < static_cast<size_t>(position) + wanted + 2) {
            constexpr size_t kChunk = 2048;
            scratch.resize(kChunk * channels);
            float *tmp = scratch.data();
            const size_t got = decoder->read(kChunk, tmp);
            if (got == 0) {
                sourceDone = true;
                break;
            }
            if (pending.size() < (pendingFrames + got) * 2) {
                pending.resize((pendingFrames + got) * 2);
            }
            for (size_t i = 0; i < got; ++i) {
                const float l = tmp[i * channels];
                const float r = channels > 1 ? tmp[i * channels + 1] : l;
                pending[(pendingFrames + i) * 2] = l;
                pending[(pendingFrames + i) * 2 + 1] = r;
            }
            pendingFrames += got;
        }
        return pendingFrames > 0;
    }

    void compact()
    {
        const size_t drop = static_cast<size_t>(position);
        if (drop > 4096 && drop <= pendingFrames) {
            std::memmove(pending.data(), pending.data() + drop * 2, (pendingFrames - drop) * 2 * sizeof(float));
            pendingFrames -= drop;
            position -= static_cast<double>(drop);
        }
    }
};

SoundStream::SoundStream(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}

SoundStream::~SoundStream() = default;

std::unique_ptr<SoundStream> SoundStream::open(std::vector<uint8_t> bytes, uint32_t outputRate, std::string *error)
{
    const auto fail = [&](const std::string &why) -> std::unique_ptr<SoundStream> {
        if (error) {
            *error = why;
        }
        return nullptr;
    };
    auto impl = std::make_unique<Impl>();
    impl->bytes = std::move(bytes);
    impl->outputRate = outputRate > 0 ? outputRate : 48000;
    if (impl->bytes.size() > kMaxSoundFileBytes) {
        return fail("sound file over " + std::to_string(kMaxSoundFileBytes >> 20) + " MiB");
    }
    const std::vector<uint8_t> &b = impl->bytes;
    if (b.empty()) {
        return fail("not a sound file");
    }
    // by what the file starts with: RIFF/WAVE, fLaC, OggS; anything else is tried as an mp3 (which may start with an ID3 tag or
    // with a bare frame)
    if (startsWith(b, "RIFF") && startsWith(b, "WAVE", 8)) {
        impl->decoder = WavDecoder::open(b);
    } else if (startsWith(b, "fLaC")) {
        impl->decoder = FlacDecoder::open(b);
    } else if (startsWith(b, "OggS")) {
        impl->decoder = VorbisDecoder::open(b);
    } else {
        impl->decoder = Mp3Decoder::open(b);
    }
    if (!impl->decoder) {
        return fail("not a sound file");
    }
    // (rates outside 8 to 192 kHz are not music; the conversion ratio stays sane)
    const uint32_t rate = impl->decoder->rate(), channels = impl->decoder->channels();
    if (rate < 8000 || rate > 192000 || channels == 0 || channels > 8) {
        return fail(std::string("unsupported ") + impl->decoder->name() + " stream");
    }
    return std::unique_ptr<SoundStream>(new SoundStream(std::move(impl)));
}

uint32_t SoundStream::sourceRate() const
{
    return m_impl->decoder->rate();
}

const char *SoundStream::format() const
{
    return m_impl->decoder->name();
}

size_t SoundStream::read(float *out, size_t frames)
{
    Impl &s = *m_impl;
    const double step = static_cast<double>(s.decoder->rate()) / static_cast<double>(s.outputRate);
    size_t produced = 0;
    while (produced < frames) {
        const size_t batch = std::min<size_t>(frames - produced, 1024);
        if (!s.fill(static_cast<size_t>(step * static_cast<double>(batch)) + 2)) {
            break;
        }
        size_t i = 0;
        for (; i < batch; ++i) {
            const size_t base = static_cast<size_t>(s.position);
            if (base + 1 >= s.pendingFrames) {
                if (!s.sourceDone || base >= s.pendingFrames) {
                    break;
                }
            }
            const float t = static_cast<float>(s.position - static_cast<double>(base));
            const size_t next = std::min(base + 1, s.pendingFrames - 1);
            for (int c = 0; c < 2; ++c) {
                const float a = s.pending[base * 2 + static_cast<size_t>(c)];
                const float b = s.pending[next * 2 + static_cast<size_t>(c)];
                out[(produced + i) * 2 + static_cast<size_t>(c)] = a + (b - a) * t;
            }
            s.position += step;
        }
        produced += i;
        s.compact();
        if (i < batch) {
            break;
        }
    }
    return produced;
}

void SoundStream::rewind()
{
    Impl &s = *m_impl;
    s.decoder->rewind();
    s.pendingFrames = 0;
    s.position = 0.0;
    s.sourceDone = false;
}

} // namespace murasaki::scene
