#pragma once

#include <QtGlobal>

#include <array>
#include <vector>

namespace murasaki {

// Turns interleaved stereo PCM into the 2 x 64 band spectrum that Wallpaper Engine audio listeners expect.
// Not thread safe; the caller serialises pushInterleaved() and compute().
class AudioSpectrum
{
public:
    // Tuning in one place: these are expected to change once WE's own behaviour is pinned down.
    static constexpr int kFftSize = 1024;
    static constexpr int kBands = 64;
    static constexpr int kOutputSize = kBands * 2;
    static constexpr double kMinHz = 20.0;
    static constexpr double kMaxHz = 16000.0;
    static constexpr float kFloorDb = -70.0f; // level 0
    static constexpr float kDecay = 0.75f;    // per compute() call

    explicit AudioSpectrum(int sampleRate = 48000);

    // Lower edge of band `band` in Hz; bandEdgeHz(kBands) is kMaxHz.
    static double bandEdgeHz(int band);

    void pushInterleaved(const float *samples, qsizetype frames);
    // A call with no pushes since the previous one means the source went quiet, so the window is cleared and
    // the bars decay instead of freezing on stale audio.
    // Indices 0..kBands-1 are the left channel, kBands..kOutputSize-1 the right, low to high frequency.
    std::array<float, kOutputSize> compute();
    void reset();

private:
    struct Channel
    {
        std::array<float, kFftSize> ring{};
        int writePos = 0;
    };
    struct BandBins
    {
        int first;
        int last; // inclusive
    };

    void analyze(const Channel &channel, float *out);
    void fft();

    int m_sampleRate;
    std::array<Channel, 2> m_channels;
    qsizetype m_framesSinceCompute = 0;
    std::array<float, kOutputSize> m_smoothed{};
    std::array<BandBins, kBands> m_bandBins{};
    std::vector<double> m_window;
    std::vector<double> m_cos;
    std::vector<double> m_sin;
    std::vector<int> m_bitReverse;
    std::vector<double> m_re;
    std::vector<double> m_im;
};

} // namespace murasaki
