#include "audiospectrum.h"

#include <QTest>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <vector>

using namespace murasaki;

namespace {

constexpr int kRate = 48000;

// Interleaved stereo sine; a channel with zero gain stays silent.
std::vector<float> stereoSine(double hz, qsizetype frames, float left, float right)
{
    std::vector<float> out(static_cast<size_t>(frames) * 2);
    for (qsizetype i = 0; i < frames; ++i) {
        const auto s = static_cast<float>(std::sin(2.0 * std::numbers::pi * hz * static_cast<double>(i) / kRate));
        out[static_cast<size_t>(i) * 2] = s * left;
        out[static_cast<size_t>(i) * 2 + 1] = s * right;
    }
    return out;
}

int bandOf(double hz)
{
    for (int b = 0; b < AudioSpectrum::kBands; ++b) {
        if (hz < AudioSpectrum::bandEdgeHz(b + 1))
            return b;
    }
    return AudioSpectrum::kBands - 1;
}

int argMaxLeft(const std::array<float, AudioSpectrum::kOutputSize> &s)
{
    return static_cast<int>(std::max_element(s.begin(), s.begin() + AudioSpectrum::kBands) - s.begin());
}

} // namespace

class TestAudioSpectrum : public QObject
{
    Q_OBJECT

private slots:
    void silenceIsZero();
    void sineLandsInItsBand();
    void higherFrequencyHigherBand();
    void decaysGeometrically();
    void shortInputIsPadded();
    void amplitudeIsNormalised();
    void overloadClampsToOne();
    void decaysWithoutNewData();
    void partiallyFilledRingHasEnergy();
    void resetClears();
};

void TestAudioSpectrum::silenceIsZero()
{
    AudioSpectrum spectrum;
    const std::vector<float> silence(4096 * 2, 0.0f);
    spectrum.pushInterleaved(silence.data(), 4096);
    for (float v : spectrum.compute())
        QCOMPARE(v, 0.0f);
}

void TestAudioSpectrum::sineLandsInItsBand()
{
    AudioSpectrum spectrum;
    const auto samples = stereoSine(1000.0, 4096, 1.0f, 0.0f);
    spectrum.pushInterleaved(samples.data(), 4096);
    const auto out = spectrum.compute();

    const int band = bandOf(1000.0);
    QCOMPARE(argMaxLeft(out), band);
    QVERIFY(out[static_cast<size_t>(band)] > 0.8f);
    QVERIFY(out[static_cast<size_t>(band)] <= 1.0f);
    for (int b = 0; b < AudioSpectrum::kBands; ++b) {
        const double lo = AudioSpectrum::bandEdgeHz(b);
        const double hi = AudioSpectrum::bandEdgeHz(b + 1);
        if (hi < 800.0 * 0.25 || lo > 5000.0)
            QVERIFY2(out[static_cast<size_t>(b)] < 0.3f, qPrintable(QString::number(b)));
        QCOMPARE(out[static_cast<size_t>(AudioSpectrum::kBands + b)], 0.0f);
    }
}

void TestAudioSpectrum::higherFrequencyHigherBand()
{
    int bands[3] = {};
    const double freqs[3] = {100.0, 1000.0, 8000.0};
    for (int i = 0; i < 3; ++i) {
        AudioSpectrum spectrum;
        const auto samples = stereoSine(freqs[i], 2048, 1.0f, 1.0f);
        spectrum.pushInterleaved(samples.data(), 2048);
        const auto out = spectrum.compute();
        bands[i] = argMaxLeft(out);
        // Both channels carry the same signal.
        const auto right = std::max_element(out.begin() + AudioSpectrum::kBands, out.end()) - out.begin();
        QCOMPARE(static_cast<int>(right) - AudioSpectrum::kBands, bands[i]);
    }
    QVERIFY(bands[0] < bands[1]);
    QVERIFY(bands[1] < bands[2]);
}

void TestAudioSpectrum::decaysGeometrically()
{
    AudioSpectrum spectrum;
    const auto loud = stereoSine(1000.0, 2048, 1.0f, 1.0f);
    spectrum.pushInterleaved(loud.data(), 2048);
    const int band = bandOf(1000.0);
    const float peak = spectrum.compute()[static_cast<size_t>(band)];
    QVERIFY(peak > 0.8f);

    const std::vector<float> silence(2048 * 2, 0.0f);
    spectrum.pushInterleaved(silence.data(), 2048);
    const float first = spectrum.compute()[static_cast<size_t>(band)];
    const float second = spectrum.compute()[static_cast<size_t>(band)];
    const float third = spectrum.compute()[static_cast<size_t>(band)];
    QVERIFY(first > 0.0f);
    QVERIFY(qFuzzyCompare(first, peak * AudioSpectrum::kDecay));
    QVERIFY(qFuzzyCompare(second, first * AudioSpectrum::kDecay));
    QVERIFY(qFuzzyCompare(third, second * AudioSpectrum::kDecay));
}

void TestAudioSpectrum::shortInputIsPadded()
{
    AudioSpectrum spectrum;
    for (float v : spectrum.compute())
        QCOMPARE(v, 0.0f);

    const auto samples = stereoSine(1000.0, 100, 1.0f, 1.0f);
    spectrum.pushInterleaved(samples.data(), 100);
    const auto out = spectrum.compute();
    for (float v : out) {
        QVERIFY(std::isfinite(v));
        QVERIFY(v >= 0.0f && v <= 1.0f);
    }
    spectrum.pushInterleaved(samples.data(), 0);
    spectrum.pushInterleaved(nullptr, 0);
}

void TestAudioSpectrum::amplitudeIsNormalised()
{
    AudioSpectrum spectrum;
    const auto samples = stereoSine(1000.0, 2048, 1.0f, 1.0f);
    spectrum.pushInterleaved(samples.data(), 2048);
    const auto out = spectrum.compute();
    const float peak = *std::max_element(out.begin(), out.begin() + AudioSpectrum::kBands);
    QVERIFY2(peak >= 0.95f && peak <= 1.0f, qPrintable(QString::number(peak)));
}

void TestAudioSpectrum::overloadClampsToOne()
{
    AudioSpectrum spectrum;
    const auto samples = stereoSine(1000.0, 2048, 2.0f, 2.0f);
    spectrum.pushInterleaved(samples.data(), 2048);
    const auto out = spectrum.compute();
    QCOMPARE(*std::max_element(out.begin(), out.end()), 1.0f);
}

void TestAudioSpectrum::decaysWithoutNewData()
{
    AudioSpectrum spectrum;
    const auto loud = stereoSine(1000.0, 2048, 1.0f, 1.0f);
    spectrum.pushInterleaved(loud.data(), 2048);
    const int band = bandOf(1000.0);
    float previous = spectrum.compute()[static_cast<size_t>(band)];
    QVERIFY(previous > 0.8f);
    // An idle sink delivers nothing; the stale window must not keep the bars up.
    for (int i = 0; i < 4; ++i) {
        const float value = spectrum.compute()[static_cast<size_t>(band)];
        QVERIFY(value < previous);
        previous = value;
    }
    for (int i = 0; i < 100; ++i)
        previous = spectrum.compute()[static_cast<size_t>(band)];
    QVERIFY(previous < 0.001f);
}

void TestAudioSpectrum::partiallyFilledRingHasEnergy()
{
    AudioSpectrum spectrum;
    const auto samples = stereoSine(1000.0, 300, 1.0f, 1.0f);
    spectrum.pushInterleaved(samples.data(), 300);
    const auto out = spectrum.compute();
    QVERIFY(*std::max_element(out.begin(), out.end()) > 0.1f);
}

void TestAudioSpectrum::resetClears()
{
    AudioSpectrum spectrum;
    const auto samples = stereoSine(1000.0, 2048, 1.0f, 1.0f);
    spectrum.pushInterleaved(samples.data(), 2048);
    const auto before = spectrum.compute();
    QVERIFY(*std::max_element(before.begin(), before.end()) > 0.5f);
    spectrum.reset();
    for (float v : spectrum.compute())
        QCOMPARE(v, 0.0f);
}

QTEST_GUILESS_MAIN(TestAudioSpectrum)
#include "tst_audiospectrum.moc"
