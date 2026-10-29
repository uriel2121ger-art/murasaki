#include "audiospectrum.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace murasaki {

AudioSpectrum::AudioSpectrum(int sampleRate)
    : m_sampleRate(sampleRate)
    , m_window(kFftSize)
    , m_cos(kFftSize / 2)
    , m_sin(kFftSize / 2)
    , m_bitReverse(kFftSize)
    , m_re(kFftSize)
    , m_im(kFftSize)
{
    static_assert((kFftSize & (kFftSize - 1)) == 0, "kFftSize must be a power of two");
    for (int i = 0; i < kFftSize; ++i)
        m_window[i] = 0.5 - 0.5 * std::cos(2.0 * std::numbers::pi * i / kFftSize);
    for (int i = 0; i < kFftSize / 2; ++i) {
        const double angle = -2.0 * std::numbers::pi * i / kFftSize;
        m_cos[i] = std::cos(angle);
        m_sin[i] = std::sin(angle);
    }
    int bits = 0;
    while ((1 << bits) < kFftSize)
        ++bits;
    for (int i = 0; i < kFftSize; ++i) {
        int reversed = 0;
        for (int b = 0; b < bits; ++b)
            reversed |= ((i >> b) & 1) << (bits - 1 - b);
        m_bitReverse[i] = reversed;
    }

    const double binHz = static_cast<double>(m_sampleRate) / kFftSize;
    const int maxBin = kFftSize / 2 - 1;
    for (int b = 0; b < kBands; ++b) {
        const double lo = bandEdgeHz(b);
        const double hi = bandEdgeHz(b + 1);
        int first = static_cast<int>(std::ceil(lo / binHz));
        int last = static_cast<int>(std::floor(hi / binHz));
        if (first > last) {
            // Narrower than a bin: use the nearest one. Bin 0 is DC and says nothing about the music.
            first = last = static_cast<int>(std::lround(std::sqrt(lo * hi) / binHz));
        }
        first = std::clamp(first, 1, maxBin);
        last = std::clamp(last, first, maxBin);
        m_bandBins[b] = {first, last};
    }
}

double AudioSpectrum::bandEdgeHz(int band)
{
    return kMinHz * std::pow(kMaxHz / kMinHz, static_cast<double>(band) / kBands);
}

void AudioSpectrum::pushInterleaved(const float *samples, qsizetype frames)
{
    if (!samples || frames <= 0)
        return;
    m_framesSinceCompute += frames;
    // Only the newest kFftSize frames can matter.
    if (frames > kFftSize) {
        samples += (frames - kFftSize) * 2;
        frames = kFftSize;
    }
    for (qsizetype i = 0; i < frames; ++i) {
        for (int c = 0; c < 2; ++c) {
            Channel &channel = m_channels[static_cast<size_t>(c)];
            channel.ring[static_cast<size_t>(channel.writePos)] = samples[i * 2 + c];
        }
        for (Channel &channel : m_channels)
            channel.writePos = (channel.writePos + 1) % kFftSize;
    }
}

void AudioSpectrum::fft()
{
    for (int i = 0; i < kFftSize; ++i) {
        const int j = m_bitReverse[i];
        if (j > i) {
            std::swap(m_re[i], m_re[j]);
            std::swap(m_im[i], m_im[j]);
        }
    }
    for (int len = 2; len <= kFftSize; len <<= 1) {
        const int half = len / 2;
        const int step = kFftSize / len;
        for (int start = 0; start < kFftSize; start += len) {
            for (int k = 0; k < half; ++k) {
                const double wr = m_cos[k * step];
                const double wi = m_sin[k * step];
                const int a = start + k;
                const int b = a + half;
                const double tr = m_re[b] * wr - m_im[b] * wi;
                const double ti = m_re[b] * wi + m_im[b] * wr;
                m_re[b] = m_re[a] - tr;
                m_im[b] = m_im[a] - ti;
                m_re[a] += tr;
                m_im[a] += ti;
            }
        }
    }
}

void AudioSpectrum::analyze(const Channel &channel, float *out)
{
    // The ring is zero-initialised, so a partly filled one is simply zero-padded.
    for (int i = 0; i < kFftSize; ++i) {
        const auto idx = static_cast<size_t>((channel.writePos + i) % kFftSize);
        m_re[i] = static_cast<double>(channel.ring[idx]) * m_window[i];
        m_im[i] = 0.0;
    }
    fft();

    // A full-scale sine peaks at amplitude * N / 2 * 0.5 (Hann coherent gain) = N / 4.
    constexpr double normalization = 4.0 / kFftSize;
    for (int b = 0; b < kBands; ++b) {
        double peak = 0.0;
        for (int bin = m_bandBins[b].first; bin <= m_bandBins[b].last; ++bin)
            peak = std::max(peak, std::hypot(m_re[bin], m_im[bin]));
        const double magnitude = peak * normalization;
        double level = 0.0;
        if (magnitude > 0.0) {
            const double db = 20.0 * std::log10(magnitude);
            level = std::clamp(1.0 - db / kFloorDb, 0.0, 1.0);
        }
        out[b] = static_cast<float>(level);
    }
}

std::array<float, AudioSpectrum::kOutputSize> AudioSpectrum::compute()
{
    if (m_framesSinceCompute == 0)
        m_channels = {};
    m_framesSinceCompute = 0;
    for (size_t c = 0; c < 2; ++c) {
        float *levels = m_smoothed.data() + c * kBands;
        float fresh[kBands];
        analyze(m_channels[c], fresh);
        for (int b = 0; b < kBands; ++b)
            levels[b] = std::max(fresh[b], levels[b] * kDecay);
    }
    return m_smoothed;
}

void AudioSpectrum::reset()
{
    m_channels = {};
    m_framesSinceCompute = 0;
    m_smoothed = {};
}

} // namespace murasaki
