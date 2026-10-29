#include "audio/spectrum.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr float kLowHz = 30.0f;
constexpr float kHighHz = 16000.0f;
constexpr double kAttackSeconds = 0.030;
constexpr double kReleaseSeconds = 0.150;
}

void fft(float *re, float *im, size_t n)
{
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const float wr = static_cast<float>(std::cos(angle));
        const float wi = static_cast<float>(std::sin(angle));
        for (size_t i = 0; i < n; i += len) {
            float cr = 1.0f;
            float ci = 0.0f;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k;
                const size_t b = i + k + len / 2;
                const float tr = re[b] * cr - im[b] * ci;
                const float ti = re[b] * ci + im[b] * cr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
                const float ncr = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = ncr;
            }
        }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer()
{
    m_window.resize(kWindow);
    for (size_t i = 0; i < kWindow; ++i) {
        m_window[i] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(kWindow)));
    }
}

void SpectrumAnalyzer::push(const float *stereo, size_t frames)
{
    uint64_t at = m_written.load(std::memory_order_relaxed);
    for (size_t i = 0; i < frames; ++i, ++at) {
        m_ring[0][at & (kRing - 1)] = stereo[i * 2];
        m_ring[1][at & (kRing - 1)] = stereo[i * 2 + 1];
    }
    m_written.store(at, std::memory_order_release);
}

void SpectrumAnalyzer::reset()
{
    m_resetAt = m_written.load(std::memory_order_acquire);
    m_spectrum = {};
}

void SpectrumAnalyzer::bands(const std::vector<float> &magnitude, float *out, size_t count, double dt)
{
    const double binHz = kSampleRate / static_cast<double>(kWindow);
    const double attack = 1.0 - std::exp(-dt / kAttackSeconds);
    const double release = std::exp(-dt / kReleaseSeconds);
    for (size_t b = 0; b < count; ++b) {
        const double lo = kLowHz * std::pow(static_cast<double>(kHighHz / kLowHz), static_cast<double>(b) / static_cast<double>(count));
        const double hi = kLowHz * std::pow(static_cast<double>(kHighHz / kLowHz), static_cast<double>(b + 1) / static_cast<double>(count));
        size_t first = static_cast<size_t>(std::floor(lo / binHz));
        size_t last = static_cast<size_t>(std::ceil(hi / binHz));
        first = std::clamp<size_t>(first, 1, magnitude.size() - 1);
        last = std::clamp<size_t>(std::max(last, first + 1), first + 1, magnitude.size());
        float peak = 0.0f;
        for (size_t i = first; i < last; ++i) {
            peak = std::max(peak, magnitude[i]);
        }
        const float db = 20.0f * std::log10(std::max(peak, 1e-9f));
        const float level = std::clamp((db - kFloorDb) / -kFloorDb, 0.0f, 1.0f);
        float &v = out[b];
        v = level > v ? static_cast<float>(v + (level - v) * attack) : static_cast<float>(std::max<double>(level, v * release));
    }
}

const AudioSpectrum &SpectrumAnalyzer::update(double dt)
{
    std::array<std::vector<float>, 2> samples;
    {
        const uint64_t written = m_written.load(std::memory_order_acquire);
        const uint64_t available = std::min<uint64_t>(written - m_resetAt, kWindow); // since the last reset
        for (size_t c = 0; c < 2; ++c) {
            samples[c].assign(kWindow, 0.0f);
            for (size_t i = kWindow - static_cast<size_t>(available); i < kWindow; ++i) {
                samples[c][i] = m_ring[c][(written - kWindow + i) & (kRing - 1)]; // oldest first
            }
        }
    }
    // One complex transform for both channels: left in the real part, right in the imaginary part; the two spectra come apart
    // with X_L[k] = (Z[k] + conj(Z[N-k])) / 2 and X_R[k] = (Z[k] - conj(Z[N-k])) / 2i.
    m_re.resize(kWindow);
    m_im.resize(kWindow);
    for (size_t i = 0; i < kWindow; ++i) {
        m_re[i] = samples[0][i] * m_window[i];
        m_im[i] = samples[1][i] * m_window[i];
    }
    fft(m_re.data(), m_im.data(), kWindow);
    std::array<std::vector<float>, 2> magnitude{std::vector<float>(kWindow / 2 + 1), std::vector<float>(kWindow / 2 + 1)};
    for (size_t k = 0; k <= kWindow / 2; ++k) {
        const size_t n = (kWindow - k) % kWindow;
        const float lr = 0.5f * (m_re[k] + m_re[n]), li = 0.5f * (m_im[k] - m_im[n]);
        const float rr = 0.5f * (m_im[k] + m_im[n]), ri = -0.5f * (m_re[k] - m_re[n]);
        // 4 / N turns a Hann windowed bin into the amplitude of the sine that made it
        magnitude[0][k] = std::sqrt(lr * lr + li * li) * 4.0f / static_cast<float>(kWindow);
        magnitude[1][k] = std::sqrt(rr * rr + ri * ri) * 4.0f / static_cast<float>(kWindow);
    }
    AudioSpectrum &s = m_spectrum;
    bands(magnitude[0], s.left16.data(), 16, dt);
    bands(magnitude[0], s.left32.data(), 32, dt);
    bands(magnitude[0], s.left64.data(), 64, dt);
    bands(magnitude[1], s.right16.data(), 16, dt);
    bands(magnitude[1], s.right32.data(), 32, dt);
    bands(magnitude[1], s.right64.data(), 64, dt);
    return m_spectrum;
}

} // namespace murasaki::scene
