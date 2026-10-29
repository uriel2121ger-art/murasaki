// The footprint a puppet engine sends as the pet's hit mask (src/overlay/puppetmask.*).
#include "puppetmask.h"

#include <QTest>

using namespace murasaki;

namespace {

QByteArray bitsOf(int cols, int rows, const std::vector<std::pair<int, int>> &set)
{
    QByteArray bits((cols * rows + 7) / 8, '\0');
    for (const auto &[c, r] : set) {
        const int i = r * cols + c;
        bits[i / 8] = char(static_cast<unsigned char>(bits[i / 8]) | (1u << (i % 8)));
    }
    return bits;
}

} // namespace

class TestPuppetMask : public QObject
{
    Q_OBJECT
private slots:
    void aFootprintBecomesASilhouette();
    void aBigGridIsLoweredAndNeverShrinks();
    void whatDoesNotFitIsRefused();
};

void TestPuppetMask::aFootprintBecomesASilhouette()
{
    // 10 x 8 cells of 8 pixels (an 80 x 64 picture); cells (2..4, 3..5) are set
    std::vector<std::pair<int, int>> set;
    for (int r = 3; r <= 5; ++r)
        for (int c = 2; c <= 4; ++c)
            set.push_back({c, r});
    const auto mask = maskSetFromHitMask(10, 8, 8, bitsOf(10, 8, set));
    QVERIFY(mask);
    QCOMPARE(mask->quality(), MaskQuality::Silhouette);
    const AlphaMask &m = mask->silhouette();
    QCOMPARE(m.spaceWidth(), 80);
    QCOMPARE(m.spaceHeight(), 64);
    QVERIFY(m.contains(20, 28));  // inside cell (2, 3)
    QVERIFY(m.contains(39, 47));  // the last pixel of cell (4, 5)
    QVERIFY(!m.contains(5, 5));
    QVERIFY(!m.contains(60, 28));
    QVERIFY(!m.contains(20, 60));
    // any frame index gives the same mask
    QCOMPARE(&mask->maskFor(7), &mask->silhouette());
    // an empty footprint is a mask with nothing set: nothing takes the pointer
    const auto none = maskSetFromHitMask(10, 8, 8, bitsOf(10, 8, {}));
    QVERIFY(none);
    QVERIFY(none->silhouette().empty());
}

void TestPuppetMask::aBigGridIsLoweredAndNeverShrinks()
{
    // 256 x 256 cells of 8: a 2048 picture; the pet masks have at most 128 cells a side, so a cell of the engine's is half a cell of theirs
    std::vector<std::pair<int, int>> set;
    for (int r = 100; r < 110; ++r)
        for (int c = 50; c < 60; ++c)
            set.push_back({c, r});
    set.push_back({255, 255});
    set.push_back({0, 0});
    const auto mask = maskSetFromHitMask(256, 256, 8, bitsOf(256, 256, set));
    QVERIFY(mask);
    const AlphaMask &m = mask->silhouette();
    QVERIFY(m.cols() <= kMaxMaskGrid && m.rows() <= kMaxMaskGrid);
    // every pixel the engine said was drawn is inside the mask
    for (const auto &[c, r] : set) {
        for (const int dx : {0, 7})
            for (const int dy : {0, 7})
                QVERIFY2(m.contains(c * 8 + dx, r * 8 + dy), qPrintable(QString::number(c) + "," + QString::number(r)));
    }
    // and the mask is not the whole picture
    QVERIFY(!m.contains(1000, 100));
}

void TestPuppetMask::whatDoesNotFitIsRefused()
{
    const QByteArray ok = bitsOf(4, 4, {{1, 1}});
    QVERIFY(maskSetFromHitMask(4, 4, 8, ok));
    QVERIFY(!maskSetFromHitMask(0, 4, 8, ok));
    QVERIFY(!maskSetFromHitMask(4, 0, 8, ok));
    QVERIFY(!maskSetFromHitMask(257, 4, 8, QByteArray(129, '\0')));
    QVERIFY(!maskSetFromHitMask(4, 257, 8, QByteArray(129, '\0')));
    QVERIFY(!maskSetFromHitMask(4, 4, 0, ok));
    QVERIFY(!maskSetFromHitMask(4, 4, 65, ok));
    QVERIFY(!maskSetFromHitMask(4, 4, 8, QByteArray(3, '\0')));  // too short
    QVERIFY(!maskSetFromHitMask(4, 4, 8, QByteArray(5, '\0')));  // too long
    QVERIFY(!maskSetFromHitMask(3, 3, 8, QByteArray("\xFF\xFF", 2))); // a bit set beyond the last of 9 cells
    QVERIFY(!maskSetFromHitMask(4, 4, 8, QByteArray()));
}

QTEST_GUILESS_MAIN(TestPuppetMask)
#include "tst_puppetmask.moc"
