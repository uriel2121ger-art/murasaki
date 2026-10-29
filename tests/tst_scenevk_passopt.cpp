// murasaki-scene-vk pass optimiser, the parts that need no GPU: the picture comparison it judges a pass by.

#include "render/ssim.h"

#include <QTest>

#include <random>

using namespace murasaki::scene;

namespace {

std::vector<uint8_t> noise(uint32_t w, uint32_t h, unsigned seed)
{
    std::mt19937 rng(seed);
    std::vector<uint8_t> p(static_cast<size_t>(w) * h * 4);
    for (uint8_t &v : p) {
        v = static_cast<uint8_t>(rng() & 0xff);
    }
    return p;
}

std::vector<uint8_t> gradient(uint32_t w, uint32_t h)
{
    std::vector<uint8_t> p(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *px = &p[(static_cast<size_t>(y) * w + x) * 4];
            px[0] = static_cast<uint8_t>(x * 255 / (w - 1));
            px[1] = static_cast<uint8_t>(y * 255 / (h - 1));
            px[2] = 128;
            px[3] = 255;
        }
    }
    return p;
}

} // namespace

class PassOptTest : public QObject
{
    Q_OBJECT
private slots:
    void equalPicturesScoreOne()
    {
        const auto p = noise(64, 48, 1);
        QCOMPARE(ssimRgba(p.data(), p.data(), 64, 48), 1.0);
        QCOMPARE(maxChannelDifference(p.data(), p.data(), p.size()), 0);
    }
    void unrelatedNoiseScoresLow()
    {
        const auto a = noise(64, 48, 1), b = noise(64, 48, 2);
        QVERIFY(ssimRgba(a.data(), b.data(), 64, 48) < 0.2);
    }
    void aSingleChannelChangeShows()
    {
        // the alpha alone differs: the score is the lowest channel, not the mean
        auto a = gradient(64, 48);
        auto b = a;
        for (size_t i = 3; i < b.size(); i += 4) {
            b[i] = 0;
        }
        QVERIFY(ssimRgba(a.data(), b.data(), 64, 48) < 0.5);
    }
    void tinyPicturesCompareAsOneWindow()
    {
        const auto a = noise(4, 4, 3);
        QCOMPARE(ssimRgba(a.data(), a.data(), 4, 4), 1.0);
    }
    void maxDifferenceIsTheLargestChannelStep()
    {
        auto a = noise(8, 8, 4);
        auto b = a;
        b[17] = static_cast<uint8_t>(a[17] ^ 0x04);
        QCOMPARE(maxChannelDifference(a.data(), b.data(), a.size()), 4);
    }
    void upsampleKeepsAFlatPictureFlat()
    {
        std::vector<uint8_t> flat(8 * 6 * 4, 77);
        const auto up = upsampleBilinear(flat.data(), 8, 6, 32, 24);
        QCOMPARE(up.size(), size_t(32 * 24 * 4));
        QVERIFY(std::all_of(up.begin(), up.end(), [](uint8_t v) { return v == 77; }));
    }
    void upsampledGradientStaysSimilar()
    {
        // a smooth picture survives halving and enlarging: this is what lets a low-frequency pass run reduced
        const auto full = gradient(128, 96);
        const auto half = gradient(64, 48);
        const auto up = upsampleBilinear(half.data(), 64, 48, 128, 96);
        QVERIFY(ssimRgba(full.data(), up.data(), 128, 96) > 0.995);
    }
    void upsampledNoiseLosesDetail()
    {
        // ... and a picture that is all detail does not
        const auto full = noise(128, 96, 5);
        const auto half = noise(64, 48, 5);
        const auto up = upsampleBilinear(half.data(), 64, 48, 128, 96);
        QVERIFY(ssimRgba(full.data(), up.data(), 128, 96) < 0.5);
    }
};

QTEST_APPLESS_MAIN(PassOptTest)
#include "tst_scenevk_passopt.moc"
