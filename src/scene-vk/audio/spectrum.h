#pragma once

// The audio spectrum Wallpaper Engine's shaders and scripts read (`g_AudioSpectrum16Left` ... `64Right`): per channel
// 16, 32 and 64 bands from low to high pitch, each 0 (silence) to 1 (full scale).
//
// The curve chosen here (WE documents none): the last 2048 samples at 48 kHz (43 ms) under a Hann window, an FFT, the
// amplitude of each bin in decibels against a full scale sine, the loudest bin of each band, bands spaced evenly on a
// logarithmic axis from 30 Hz to 16 kHz, and -60 dB mapped to 0 and 0 dB to 1. Smoothing: a band rises towards a louder
// value with a 30 ms time constant and falls with 150 ms, so a beat flashes and fades like a level meter.

#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <vector>

namespace murasaki::scene {

struct AudioSpectrum {
    std::array<float, 16> left16{}, right16{};
    std::array<float, 32> left32{}, right32{};
    std::array<float, 64> left64{}, right64{};
};

class SpectrumAnalyzer
{
public:
    static constexpr size_t kWindow = 2048;
    static constexpr double kSampleRate = 48000.0;
    static constexpr float kFloorDb = -60.0f;

    SpectrumAnalyzer();

    // Appends interleaved stereo frames. Lock-free and wait-free for the one producer thread (the backend's real-time
    // callback); the render thread reads with update(). More than one producer is not allowed.
    void push(const float *stereo, size_t frames);
    // Forgets the samples and the smoothed state (the source went silent or the scene paused). Render thread only.
    void reset();
    // Computes the spectrum of what was pushed so far and advances the smoothing by `dt` seconds (the render thread).
    const AudioSpectrum &update(double dt);
    const AudioSpectrum &current() const { return m_spectrum; }

private:
    void bands(const std::vector<float> &magnitude, float *out, size_t count, double dt);

    static constexpr size_t kRing = 8192; // a power of two, four windows: the reader copies a window long before the writer laps it
    std::array<std::array<float, kRing>, 2> m_ring{};
    std::atomic<uint64_t> m_written{0}; // frames ever pushed
    uint64_t m_resetAt = 0;             // frames before this count were forgotten by reset() (render thread's)
    std::vector<float> m_window;
    std::vector<float> m_re, m_im; // the transform's buffers, kept between frames
    std::vector<float> m_cos, m_sin;
    AudioSpectrum m_spectrum;
};

// Radix-2 FFT of `n` complex points in place (n a power of two); exposed for the tests.
void fft(float *re, float *im, size_t n);

} // namespace murasaki::scene
