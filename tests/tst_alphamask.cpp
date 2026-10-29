// Hit masks (src/overlay/alphamask.h): building from alpha, the budget, rectangles, transforms against a brute-force rotation of
// a pixel image, the .mask file with hostile input, the adapter to PetInteraction, and the document of the degrade names.

#include "alphamask.h"
#include "petinteraction.h"

#include <QDir>
#include <QFile>
#include <QImage>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <random>

using namespace murasaki;
using namespace Qt::StringLiterals;

namespace {

QImage blank(int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    img.fill(Qt::transparent);
    return img;
}

AlphaMask maskOf(const QImage &img, int level = 128)
{
    return AlphaMask::fromAlpha(img.constBits() + 3, int(img.bytesPerLine()), 4, img.width(), img.height(), level);
}

QImage disc(int size, int radius, int alpha = 255)
{
    QImage img = blank(size, size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double dx = x + 0.5 - size / 2.0, dy = y + 0.5 - size / 2.0;
            if (dx * dx + dy * dy <= double(radius) * radius)
                img.setPixelColor(x, y, QColor(255, 0, 0, alpha));
        }
    }
    return img;
}

// Something asymmetric: an L, a diagonal and a lone pixel.
QImage asymmetric(int w, int h)
{
    QImage img = blank(w, h);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const bool inL = (x < w / 4 && y < h * 3 / 4) || (y > h * 5 / 8 && x < w * 3 / 4 && y < h * 7 / 8);
            const bool diag = std::abs(x * h - y * w) < w;
            if (inL || diag)
                img.setPixelColor(x, y, QColor(0, 255, 0, 200));
        }
    }
    img.setPixelColor(w - 3, 2, QColor(0, 0, 255, 255));
    return img;
}

std::uint64_t fnv(const std::uint8_t *p, std::size_t n)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

// Writes a valid checksum for the body of `file` (after a test edited it).
void resign(std::vector<std::uint8_t> &file)
{
    const std::uint64_t h = fnv(file.data() + kMaskFileHeaderBytes, file.size() - kMaskFileHeaderBytes);
    for (int i = 0; i < 8; ++i)
        file[32 + std::size_t(i)] = std::uint8_t(h >> (8 * i));
}

void put32(std::vector<std::uint8_t> &file, std::size_t at, std::uint32_t v)
{
    if (at + 4 > file.size())
        return;
    for (int i = 0; i < 4; ++i)
        file[at + std::size_t(i)] = std::uint8_t(v >> (8 * i));
}

MaskSet sample(int frames = 3)
{
    MaskBuilder builder(200, 100, frames);
    for (int i = 0; i < frames; ++i) {
        QImage img = blank(200, 100);
        QPainter p(&img);
        p.fillRect(10 + i * 20, 10, 40, 30, QColor(255, 0, 0, 255));
        p.end();
        builder.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 200, 100);
    }
    return builder.finish();
}

bool covered(const std::vector<MaskRect> &rects, int x, int y)
{
    for (const MaskRect &r : rects) {
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return true;
    }
    return false;
}

} // namespace

class TstAlphaMask : public QObject
{
    Q_OBJECT

private slots:
    void aDiscHasItsCentreAndNotItsCorner();
    void aRingKeepsItsHole();
    void theThresholdIs16Of255();
    void aOnePixelLineStaysSet();
    void theCellSizeFollowsTheLongestSide();
    void degenerateInputGivesAnEmptyMask();
    void unionAndSubset();
    void coarseningOnlyGrows();

    void aFilledRectangleIsOneRectangle();
    void anLShapeIsTwoRectangles();
    void rectanglesAreBounded();
    void aCheckerboardFallsBackToTheBoundingBox();
    void anEmptyMaskHasNoRectangles();

    void quarterTurnsAndFlipsMatchABruteForceRotation_data();
    void quarterTurnsAndFlipsMatchABruteForceRotation();
    void anArbitraryAngleIsAConservativeSuperset();
    void quarterTurnsAreExactWhereCellsTileThePicture_data();
    void quarterTurnsAreExactWhereCellsTileThePicture();
    void quarterTurnsOfAnUntiledPictureAreASuperset();
    void theWorstCaseBudgetOf20000FramesHolds();
    void aBudgetSmallerThanOneMaskKeepsTheUnion();
    void rectanglesWithNoRoomAreNone();
    void filesOverTheBoundAreNotWritten();
    void coarseCellsMustBePowersOfTwo();
    void benchmarks();

    void aBigBudgetKeepsEveryFrameFull();
    void levelsDegradeInOrderUnderASmallBudget();
    void aTinyBudgetKeepsTheUnionOnly();
    void theBudgetIsNeverExceeded();
    void aWrongSizedFrameIsRefused();
    void degradeNamesFollowTheQuality();

    void theFileRoundTrips();
    void everyTruncationIsRefused();
    void hostileHeadersAreRefused();
    void hostileBodiesAreRefused();
    void aCacheOfAnotherVariantIsRefused();
    void mutationsNeverProduceAnInconsistentSet();
    void filesAreBoundedOnDisk();

    void theAdapterScalesWindowToPicture();
    void theAdapterDrivesTheStateMachine();
    void theDocumentListsTheNames();
};

void TstAlphaMask::aDiscHasItsCentreAndNotItsCorner()
{
    const AlphaMask m = maskOf(disc(128, 50));
    QVERIFY(m.contains(64, 64));
    QVERIFY(m.contains(64 + 49, 64));
    QVERIFY(!m.contains(2, 2));
    QVERIFY(!m.contains(125, 125));
    QVERIFY(!m.contains(-1, 5));
    QVERIFY(!m.contains(128, 5));
    QVERIFY(!m.contains(std::nan(""), 5));
}

void TstAlphaMask::aRingKeepsItsHole()
{
    QImage img = disc(128, 60);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            const double dx = x + 0.5 - 64, dy = y + 0.5 - 64;
            if (dx * dx + dy * dy < 30.0 * 30.0)
                img.setPixelColor(x, y, Qt::transparent);
        }
    }
    const AlphaMask m = maskOf(img);
    QVERIFY(!m.contains(64, 64));
    QVERIFY(m.contains(64 + 45, 64));
}

void TstAlphaMask::theThresholdIs16Of255()
{
    QVERIFY(!maskOf(disc(64, 20, 15)).contains(32, 32)); // a faint halo below the threshold
    QVERIFY(maskOf(disc(64, 20, 16)).contains(32, 32));
}

void TstAlphaMask::aOnePixelLineStaysSet()
{
    QImage img = blank(1000, 10);
    for (int x = 0; x < 1000; ++x)
        img.setPixelColor(x, 4, QColor(0, 0, 0, 255));
    const AlphaMask m = maskOf(img);
    QCOMPARE(m.cellPx(), 8); // ceil(1000 / 128)
    for (int x = 0; x < 1000; x += 7)
        QVERIFY2(m.contains(x, 4), qPrintable(QString::number(x)));
    // The generous side: the whole cell around the line is set, so the mask is never smaller than the drawing.
    QVERIFY(m.contains(500, 0));
}

void TstAlphaMask::theCellSizeFollowsTheLongestSide()
{
    QCOMPARE(AlphaMask(128, 64, 128).cellPx(), 1);
    QCOMPARE(AlphaMask(129, 64, 128).cellPx(), 2);
    QCOMPARE(AlphaMask(300, 10, 128).cellPx(), 3);
    const AlphaMask m(300, 10, 128);
    QCOMPARE(m.cols(), 100);
    QCOMPARE(m.rows(), 4);
    QCOMPARE(AlphaMask(1000, 1000, 128).payloadBytes(), std::size_t(125 * 2 * 8));
    QCOMPARE(AlphaMask(1000, 1000, 128).bytes(), std::size_t(125 * 2 * 8) + kMaskOverheadBytes);
    QCOMPARE(AlphaMask().bytes(), std::size_t(0));
    // // cell 8: 125 x 125 cells, two words a row, under 2 KiB
    QCOMPARE(AlphaMask(1000, 1000, 32).payloadBytes(), std::size_t(32 * 8));
}

void TstAlphaMask::degenerateInputGivesAnEmptyMask()
{
    QVERIFY(AlphaMask().isNull());
    QVERIFY(AlphaMask(0, 10, 128).isNull());
    QVERIFY(AlphaMask(10, -1, 128).isNull());
    QVERIFY(AlphaMask(kMaxMaskSpace + 1, 10, 128).isNull());
    QVERIFY(AlphaMask::fromAlpha(nullptr, 4, 4, 1, 1).empty());
    const std::uint8_t px[4] = {255, 255, 255, 255};
    QVERIFY(AlphaMask::fromAlpha(px, 2, 4, 1, 1).empty()); // a stride shorter than a row
    QVERIFY(!AlphaMask().contains(0, 0));
    QVERIFY(AlphaMask().toRects(128).rects.empty());
}

void TstAlphaMask::unionAndSubset()
{
    QImage a = blank(64, 64), b = blank(64, 64);
    a.setPixelColor(5, 5, QColor(0, 0, 0, 255));
    b.setPixelColor(50, 50, QColor(0, 0, 0, 255));
    const AlphaMask ma = maskOf(a), mb = maskOf(b);
    const AlphaMask u = ma.united(mb);
    QVERIFY(u.contains(5, 5) && u.contains(50, 50));
    QVERIFY(ma.isSubsetOf(u) && mb.isSubsetOf(u));
    QVERIFY(!u.isSubsetOf(ma));
    QVERIFY(!ma.isSubsetOf(AlphaMask(32, 32, 128))); // another shape
}

void TstAlphaMask::coarseningOnlyGrows()
{
    const AlphaMask fine = maskOf(asymmetric(256, 128));
    AlphaMask m = fine;
    QCOMPARE(m.cellPx(), 2);
    for (int i = 0; i < 2; ++i) {
        const AlphaMask c = m.coarsened();
        QCOMPARE(c.cellPx(), m.cellPx() * 2);
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 256; ++x) {
                if (m.contains(x, y))
                    QVERIFY(c.contains(x, y));
            }
        }
        m = c;
    }
    QCOMPARE(m.level(), 32);
    QVERIFY(!m.canCoarsen());
    QCOMPARE(m.coarsened(), m);
}

void TstAlphaMask::aFilledRectangleIsOneRectangle()
{
    QImage img = blank(128, 128);
    QPainter p(&img);
    p.fillRect(10, 20, 40, 30, QColor(1, 2, 3, 255));
    p.end();
    const auto r = maskOf(img).toRects(128);
    QCOMPARE(int(r.rects.size()), 1);
    QCOMPARE(r.rects[0], (MaskRect{10, 20, 40, 30}));
    QCOMPARE(r.coarsenSteps, 0);
    QVERIFY(!r.boundingBox);
}

void TstAlphaMask::anLShapeIsTwoRectangles()
{
    QImage img = blank(128, 128);
    QPainter p(&img);
    p.fillRect(10, 10, 20, 60, QColor(1, 2, 3, 255));
    p.fillRect(10, 70, 60, 20, QColor(1, 2, 3, 255));
    p.end();
    const auto r = maskOf(img).toRects(128);
    QCOMPARE(int(r.rects.size()), 2);
    QVERIFY(std::find(r.rects.begin(), r.rects.end(), MaskRect{10, 10, 20, 60}) != r.rects.end());
    QVERIFY(std::find(r.rects.begin(), r.rects.end(), MaskRect{10, 70, 60, 20}) != r.rects.end());
}

void TstAlphaMask::rectanglesAreBounded()
{
    // A disc of radius 60 on the 128 grid needs about 120 different rows.
    const QImage img = disc(128, 62);
    const AlphaMask m = maskOf(img);
    for (const int limit : {kMaxFrameRects, kMaxFixedRects, 16}) {
        const auto r = m.toRects(limit);
        QVERIFY2(int(r.rects.size()) <= limit, qPrintable(QString::number(limit)));
        // A superset of what is drawn, whatever the grid became.
        for (int y = 0; y < 128; ++y) {
            for (int x = 0; x < 128; ++x) {
                if (img.pixelColor(x, y).alpha() >= kMaskAlphaThreshold)
                    QVERIFY(covered(r.rects, x, y));
            }
        }
    }
    QVERIFY(m.toRects(16).coarsenSteps > 0);
}

void TstAlphaMask::aCheckerboardFallsBackToTheBoundingBox()
{
    QImage img = blank(128, 128);
    for (int y = 0; y < 128; ++y) {
        for (int x = 0; x < 128; ++x) {
            if (((x / 4) + (y / 4)) % 2 == 0)
                img.setPixelColor(x, y, QColor(0, 0, 0, 255));
        }
    }
    const auto r = maskOf(img).toRects(8);
    QVERIFY(r.boundingBox);
    QCOMPARE(int(r.rects.size()), 1);
    QVERIFY(r.rects[0].w <= 128 && r.rects[0].h <= 128 && r.rects[0].w > 100);
}

void TstAlphaMask::anEmptyMaskHasNoRectangles()
{
    QVERIFY(AlphaMask(100, 100, 128).toRects(128).rects.empty());
}

void TstAlphaMask::quarterTurnsAndFlipsMatchABruteForceRotation_data()
{
    QTest::addColumn<int>("degrees");
    QTest::addColumn<bool>("flipH");
    QTest::addColumn<bool>("flipV");
    for (const int d : {0, 90, 180, 270}) {
        for (const bool h : {false, true}) {
            for (const bool v : {false, true})
                QTest::addRow("%d_%d_%d", d, h, v) << d << h << v;
        }
    }
}

void TstAlphaMask::quarterTurnsAndFlipsMatchABruteForceRotation()
{
    QFETCH(int, degrees);
    QFETCH(bool, flipH);
    QFETCH(bool, flipV);
    const QImage img = asymmetric(128, 64); // 128 wide: one pixel a cell, so the grid is exact
    const AlphaMask m = maskOf(img);
    QCOMPARE(m.cellPx(), 1);
    // The picture is flipped in its own frame, then turned clockwise.
    Qt::Orientations flips;
    if (flipH)
        flips |= Qt::Horizontal;
    if (flipV)
        flips |= Qt::Vertical;
    QImage expected = img.flipped(flips);
    if (degrees)
        expected = expected.transformed(QTransform().rotate(degrees));
    const AlphaMask got = m.transformed(degrees, flipH, flipV);
    QCOMPARE(got.spaceWidth(), expected.width());
    QCOMPARE(got.spaceHeight(), expected.height());
    const AlphaMask want = maskOf(expected);
    for (int y = 0; y < expected.height(); ++y) {
        for (int x = 0; x < expected.width(); ++x)
            QVERIFY2(got.contains(x, y) == want.contains(x, y), qPrintable(QStringLiteral("%1,%2").arg(x).arg(y)));
    }
}

void TstAlphaMask::anArbitraryAngleIsAConservativeSuperset()
{
    // Pictures whose cells are larger than a pixel (cell 3 and 8), turned by angles that are not quarter turns, flipped or not:
    // every pixel the brute-force rotation draws opaque is inside the mask. Never smaller than what is drawn.
    for (const QSize size : {QSize(300, 200), QSize(1001, 777)}) {
        const QImage img = asymmetric(size.width(), size.height());
        const AlphaMask m = maskOf(img);
        QVERIFY(m.cellPx() > 1);
        for (const double angle : {33.3, 1.0, 89.5, 123.0, 271.7, 359.0}) {
            for (const bool flip : {false, true}) {
                const AlphaMask got = m.transformed(angle, flip, false);
                const PetSize fp = petFootprint({double(size.width()), double(size.height())}, angle);
                QCOMPARE(got.spaceWidth(), petCeilPx(fp.w));
                QCOMPARE(got.spaceHeight(), petCeilPx(fp.h));
                QImage drawn = blank(got.spaceWidth(), got.spaceHeight());
                {
                    QPainter p(&drawn);
                    p.translate(drawn.width() / 2.0, drawn.height() / 2.0);
                    p.rotate(angle);
                    if (flip)
                        p.scale(-1, 1);
                    p.translate(-size.width() / 2.0, -size.height() / 2.0);
                    p.drawImage(0, 0, img);
                }
                int opaque = 0, missed = 0, maskArea = 0;
                for (int y = 0; y < drawn.height(); ++y) {
                    for (int x = 0; x < drawn.width(); ++x) {
                        if (got.contains(x + 0.5, y + 0.5))
                            ++maskArea;
                        if (drawn.pixelColor(x, y).alpha() >= kMaskAlphaThreshold) {
                            ++opaque;
                            if (!got.contains(x + 0.5, y + 0.5))
                                ++missed;
                        }
                    }
                }
                QVERIFY(opaque > 500);
                QVERIFY2(missed == 0, qPrintable(QStringLiteral("%1x%2 at %3 (flip %4): %5 of %6 drawn pixels outside")
                                                     .arg(size.width()).arg(size.height()).arg(angle).arg(flip).arg(missed).arg(opaque)));
                // And not absurdly generous: a cell of margin around the drawing, not a bounding box.
                QVERIFY2(maskArea <= opaque * 2.5, qPrintable(QStringLiteral("%1 vs %2").arg(maskArea).arg(opaque)));
            }
        }
    }
}

void TstAlphaMask::quarterTurnsAreExactWhereCellsTileThePicture_data()
{
    QTest::addColumn<QSize>("size");
    QTest::addColumn<int>("degrees");
    QTest::addColumn<bool>("flipH");
    QTest::addColumn<bool>("flipV");
    for (const QSize size : {QSize(256, 128), QSize(1024, 512), QSize(256, 256)}) { // cells 2, 8, 2
        for (const int d : {0, 90, 180, 270}) {
            for (const bool h : {false, true}) {
                for (const bool v : {false, true})
                    QTest::addRow("%dx%d_%d_%d_%d", size.width(), size.height(), d, h, v) << size << d << h << v;
            }
        }
    }
}

void TstAlphaMask::quarterTurnsAreExactWhereCellsTileThePicture()
{
    QFETCH(QSize, size);
    QFETCH(int, degrees);
    QFETCH(bool, flipH);
    QFETCH(bool, flipV);
    const QImage img = asymmetric(size.width(), size.height());
    const AlphaMask m = maskOf(img);
    QVERIFY(m.cellPx() > 1);
    QCOMPARE(size.width() % m.cellPx(), 0);
    QCOMPARE(size.height() % m.cellPx(), 0);
    Qt::Orientations flips;
    if (flipH)
        flips |= Qt::Horizontal;
    if (flipV)
        flips |= Qt::Vertical;
    QImage expected = img.flipped(flips);
    if (degrees)
        expected = expected.transformed(QTransform().rotate(degrees));
    const AlphaMask got = m.transformed(degrees, flipH, flipV);
    const AlphaMask want = maskOf(expected);
    QVERIFY(got == want); // the same cells, not a superset
}

void TstAlphaMask::quarterTurnsOfAnUntiledPictureAreASuperset()
{
    // 129 x 65 (cell 2, a half cell left over on both sides) and 1001 x 1001 (cell 8, 126 columns): the grid does not tile the
    // picture, so the turned grid is misaligned; the mask is a superset of the exact one, by at most a cell around it.
    for (const QSize size : {QSize(129, 65), QSize(1001, 1001), QSize(131, 67)}) {
        const QImage img = asymmetric(size.width(), size.height());
        const AlphaMask m = maskOf(img);
        for (const int d : {90, 180, 270}) {
            const QImage expected = img.transformed(QTransform().rotate(d));
            const AlphaMask got = m.transformed(d, false, false);
            QCOMPARE(got.spaceWidth(), expected.width());
            int missed = 0, drawn = 0, area = 0;
            for (int y = 0; y < expected.height(); ++y) {
                for (int x = 0; x < expected.width(); ++x) {
                    area += got.contains(x, y);
                    if (expected.pixelColor(x, y).alpha() >= kMaskAlphaThreshold) {
                        ++drawn;
                        missed += !got.contains(x, y);
                    }
                }
            }
            QVERIFY(drawn > 100);
            QVERIFY2(missed == 0, qPrintable(QStringLiteral("%1x%2 at %3: %4 missed").arg(size.width()).arg(size.height()).arg(d).arg(missed)));
            QVERIFY(area <= drawn * 3);
        }
    }
}

void TstAlphaMask::aBigBudgetKeepsEveryFrameFull()
{
    const MaskSet set = sample(10);
    QCOMPARE(int(set.quality()), int(MaskQuality::Full));
    QCOMPARE(set.frameCount(), 10);
    QVERIFY(set.silhouette().contains(15, 15) && set.silhouette().contains(15 + 9 * 20, 15));
    QVERIFY(set.maskFor(0).contains(15, 15));
    QVERIFY(!set.maskFor(0).contains(15 + 9 * 20, 15));
    QVERIFY(set.maskFor(10).contains(15, 15)); // wraps
    QVERIFY(set.maskFor(-10).contains(15, 15));
    QVERIFY(!set.degradeName());
}

void TstAlphaMask::levelsDegradeInOrderUnderASmallBudget()
{
    // 1000 frames of a 256 x 256 picture: 1001 x 2 KiB = 2 MB at 128, 0.5 MB at 64, 0.26 MB at 32.
    QImage img = blank(256, 256);
    QPainter(&img).fillRect(10, 10, 200, 200, QColor(1, 2, 3, 255));
    const auto build = [&](std::size_t budget) {
        MaskBuilder b(256, 256, 1000, budget);
        for (int i = 0; i < 3; ++i)
            b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 256, 256);
        const int level = b.level();
        return std::make_pair(b.finish(), level);
    };
    auto [full, l0] = build(4u << 20);
    QCOMPARE(int(full.quality()), int(MaskQuality::Full));
    QCOMPARE(l0, 128);
    auto [c64, l1] = build(700u << 10);
    QCOMPARE(int(c64.quality()), int(MaskQuality::Coarse));
    QCOMPARE(l1, 64);
    QCOMPARE(c64.silhouette().level(), 64);
    auto [c32, l2] = build(340u << 10);
    QCOMPARE(int(c32.quality()), int(MaskQuality::Coarse));
    QCOMPARE(l2, 32);
    QCOMPARE(*c32.degradeName(), degrade::kHitMaskCoarse);
    // Coarser masks are supersets of the finer ones.
    QVERIFY(full.maskFor(0).isSubsetOf(full.silhouette()));
    for (int y = 0; y < 256; ++y) {
        for (int x = 0; x < 256; ++x) {
            if (full.maskFor(1).contains(x, y)) {
                QVERIFY(c64.maskFor(1).contains(x, y));
                QVERIFY(c32.maskFor(1).contains(x, y));
            }
        }
    }
}

void TstAlphaMask::aTinyBudgetKeepsTheUnionOnly()
{
    QImage img = blank(256, 256);
    QPainter(&img).fillRect(10, 10, 100, 100, QColor(1, 2, 3, 255));
    MaskBuilder b(256, 256, 20000, 1000);
    for (int i = 0; i < 4; ++i)
        QVERIFY(b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 256, 256));
    QVERIFY(b.unionOnly());
    const MaskSet set = b.finish();
    QCOMPARE(int(set.quality()), int(MaskQuality::Silhouette));
    QCOMPARE(set.frameCount(), 0);
    QVERIFY(set.maskFor(3).contains(50, 50)); // the union answers for every frame
    QCOMPARE(set.silhouette().level(), 128);   // at the finest grid: the best silhouette
    QCOMPARE(*set.degradeName(), degrade::kHitMaskSilhouette);
}

void TstAlphaMask::theBudgetIsNeverExceeded()
{
    QImage img = blank(512, 512);
    QPainter(&img).fillRect(100, 100, 300, 300, QColor(1, 2, 3, 255));
    MaskBuilder b(512, 512, 20000);
    for (int i = 0; i < 40; ++i)
        QVERIFY(b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 512, 512));
    const MaskSet set = b.finish();
    // Built for 20000 frames: the grid was chosen for all of them, not for the 40 added.
    QVERIFY(set.quality() == MaskQuality::Coarse || set.quality() == MaskQuality::Silhouette);
    QVERIFY((std::size_t(20000) + 1) * set.silhouette().bytes() <= kMaskBudgetBytes || set.quality() == MaskQuality::Silhouette);
}

void TstAlphaMask::theWorstCaseBudgetOf20000FramesHolds()
{
    QImage img = blank(128, 128);
    QPainter painter(&img);
    painter.fillRect(20, 20, 60, 60, QColor(1, 2, 3, 255));
    painter.end();
    for (const int expected : {20000, 1}) { // announced, and not announced at all
        MaskBuilder b(128, 128, expected);
        for (int i = 0; i < 20000; ++i)
            QVERIFY(b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 128, 128));
        QVERIFY(!b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 128, 128)); // the 20001st
        const MaskSet set = b.finish();
        QCOMPARE(set.frameCount(), 20000);
        QCOMPARE(int(set.quality()), int(MaskQuality::Coarse)); // 20001 x (2 KiB + 80) would be 42 MB
        QVERIFY2(set.bytes() <= kMaskBudgetBytes, qPrintable(QString::number(set.bytes())));
        QVERIFY(set.maskFor(19999).contains(30, 30));
        QVERIFY(!set.maskFor(0).contains(120, 120));
    }
    // A custom budget is honoured as well, for a smaller number of frames.
    MaskBuilder small(128, 128, 1000, 300 * 1024);
    for (int i = 0; i < 1000; ++i)
        QVERIFY(small.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 128, 128));
    const MaskSet set = small.finish();
    QVERIFY2(set.bytes() <= 300 * 1024, qPrintable(QString::number(set.bytes())));
    QVERIFY(set.quality() != MaskQuality::None);
}

void TstAlphaMask::aBudgetSmallerThanOneMaskKeepsTheUnion()
{
    // Decided: the union (at most ~2 KiB) is always kept and named `hit mask: silhouette`; the budget is a cap for the frames.
    QImage img = blank(256, 256);
    QPainter(&img).fillRect(10, 10, 100, 100, QColor(1, 2, 3, 255));
    for (const std::size_t budget : {std::size_t(0), std::size_t(1), std::size_t(100)}) {
        MaskBuilder b(256, 256, 10, budget);
        QVERIFY(b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 256, 256));
        const MaskSet set = b.finish();
        QCOMPARE(int(set.quality()), int(MaskQuality::Silhouette));
        QCOMPARE(set.frameCount(), 0);
        QVERIFY(set.silhouette().contains(50, 50));
        QCOMPARE(set.bytes(), set.silhouette().bytes()); // only the union is held
        QCOMPARE(*set.degradeName(), degrade::kHitMaskSilhouette);
    }
}

void TstAlphaMask::rectanglesWithNoRoomAreNone()
{
    const AlphaMask m = maskOf(disc(64, 20));
    QVERIFY(m.toRects(0).rects.empty());
    QVERIFY(m.toRects(-5).rects.empty());
    QCOMPARE(int(m.toRects(1).rects.size()), 1);
}

void TstAlphaMask::filesOverTheBoundAreNotWritten()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QImage img = blank(128, 128);
    img.setPixelColor(5, 5, QColor(0, 0, 0, 255));
    MaskBuilder b(128, 128, 5000, std::size_t(100) << 20); // a budget the file bound does not allow
    for (int i = 0; i < 5000; ++i)
        b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 128, 128);
    const MaskSet set = b.finish();
    QVERIFY(serializeMaskFile(set).size() > kMaxMaskFileBytes);
    const QString path = dir.path() + u"/big.mask"_s;
    QVERIFY(!writeMaskFile(path.toStdString(), set));
    QVERIFY(!QFile::exists(path));
    QCOMPARE(QDir(dir.path()).entryList(QDir::Files | QDir::NoDotAndDotDot).size(), 0); // and no temporary file is left
}

void TstAlphaMask::coarseCellsMustBePowersOfTwo()
{
    QImage img = blank(256, 256);
    QPainter(&img).fillRect(10, 10, 200, 200, QColor(1, 2, 3, 255));
    MaskBuilder b(256, 256, 1000, 700u << 10);
    for (int i = 0; i < 3; ++i)
        b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 256, 256);
    const MaskSet coarse = b.finish();
    QCOMPARE(int(coarse.quality()), int(MaskQuality::Coarse));
    const auto good = serializeMaskFile(coarse);
    QVERIFY(parseMaskFile(good.data(), good.size()));
    // Halved once: cell 4 over a finest cell of 2. A cell of 6 is not 2^k times 2, whatever else is made consistent.
    std::vector<std::uint8_t> f = good;
    QCOMPARE(int(f[26]), 4);
    f[26] = 6;
    f[22] = 43; // ceil(256 / 6) columns and rows
    f[24] = 43;
    resign(f);
    QVERIFY(!parseMaskFile(f.data(), f.size()));
    // A full-quality header on a coarse body is refused too.
    std::vector<std::uint8_t> g = good;
    g[20] = 1;
    QVERIFY(!parseMaskFile(g.data(), g.size()));
}

void TstAlphaMask::benchmarks()
{
    if (qEnvironmentVariableIsEmpty("MURASAKI_BENCH"))
        QSKIP("set MURASAKI_BENCH=1 to time toRects and transformed");
    const QImage img = asymmetric(1024, 768);
    const AlphaMask m = maskOf(img);
    const QImage ring = disc(1024, 480);
    const AlphaMask r = maskOf(ring);
    QBENCHMARK {
        const auto rects = m.toRects(kMaxFrameRects);
        QVERIFY(!rects.rects.empty());
    }
    QBENCHMARK {
        const auto rects = r.toRects(kMaxFrameRects);
        QVERIFY(!rects.rects.empty());
    }
    QBENCHMARK {
        const AlphaMask t = m.transformed(33.3, false, false);
        QVERIFY(!t.empty());
    }
    QBENCHMARK {
        const AlphaMask t = r.transformed(90, true, false);
        QVERIFY(!t.empty());
    }
    QBENCHMARK {
        const AlphaMask c = AlphaMask::fromAlpha(img.constBits() + 3, int(img.bytesPerLine()), 4, img.width(), img.height());
        QVERIFY(!c.empty());
    }
}

void TstAlphaMask::aWrongSizedFrameIsRefused()
{
    MaskBuilder b(100, 100, 2);
    QImage other = blank(50, 50);
    QVERIFY(!b.addFrame(other.constBits() + 3, int(other.bytesPerLine()), 4, 50, 50));
    QVERIFY(b.finish().isNull()); // nothing was added
    MaskBuilder zero(0, 0, 2);
    QVERIFY(!zero.addFrame(nullptr, 0, 4, 0, 0));
    QVERIFY(zero.finish().isNull());
}

void TstAlphaMask::degradeNamesFollowTheQuality()
{
    QCOMPARE(*MaskSet().degradeName(), degrade::kHitMaskNone);
    QVERIFY(MaskSet().isNull());
}

void TstAlphaMask::theFileRoundTrips()
{
    for (const MaskSet &set : {sample(3), sample(1)}) {
        const std::vector<std::uint8_t> bytes = serializeMaskFile(set);
        QVERIFY(!bytes.empty());
        QVERIFY(bytes.size() <= kMaxMaskFileBytes);
        const auto back = parseMaskFile(bytes.data(), bytes.size(), 200, 100, set.frameCount());
        QVERIFY(back);
        QCOMPARE(back->frameCount(), set.frameCount());
        QCOMPARE(int(back->quality()), int(set.quality()));
        QVERIFY(back->silhouette() == set.silhouette());
        for (int i = 0; i < set.frameCount(); ++i)
            QVERIFY(back->maskFor(i) == set.maskFor(i));
        QCOMPARE(serializeMaskFile(*back), bytes); // canonical
    }
    // A silhouette-only set.
    QImage img = blank(256, 256);
    QPainter(&img).fillRect(10, 10, 100, 100, QColor(1, 2, 3, 255));
    MaskBuilder b(256, 256, 20000, 1000);
    b.addFrame(img.constBits() + 3, int(img.bytesPerLine()), 4, 256, 256);
    const MaskSet silhouette = b.finish();
    const auto bytes = serializeMaskFile(silhouette);
    const auto back = parseMaskFile(bytes.data(), bytes.size(), 256, 256, 777); // any frame count matches
    QVERIFY(back);
    QCOMPARE(int(back->quality()), int(MaskQuality::Silhouette));
    QVERIFY(serializeMaskFile(MaskSet()).empty());
}

void TstAlphaMask::everyTruncationIsRefused()
{
    const auto bytes = serializeMaskFile(sample(3));
    for (std::size_t n = 0; n < bytes.size(); ++n)
        QVERIFY2(!parseMaskFile(bytes.data(), n), qPrintable(QString::number(n)));
    QVERIFY(!parseMaskFile(nullptr, 100));
}

void TstAlphaMask::hostileHeadersAreRefused()
{
    const auto good = serializeMaskFile(sample(3));
    QVERIFY(parseMaskFile(good.data(), good.size()));
    const auto expectRefused = [&](const char *what, auto &&edit) {
        std::vector<std::uint8_t> f = good;
        edit(f);
        QVERIFY2(!parseMaskFile(f.data(), f.size()), what);
    };
    expectRefused("magic", [](auto &f) { f[0] = 'X'; });
    expectRefused("version 2", [](auto &f) { put32(f, 4, 2); });
    expectRefused("version 0", [](auto &f) { put32(f, 4, 0); });
    expectRefused("width 0", [](auto &f) { put32(f, 8, 0); });
    expectRefused("width huge", [](auto &f) { put32(f, 8, 0xFFFFFFFFu); });
    expectRefused("height over the bound", [](auto &f) { put32(f, 12, kMaxMaskSpace + 1); });
    expectRefused("frame count 2^32 - 1", [](auto &f) { put32(f, 16, 0xFFFFFFFFu); });
    expectRefused("frame count over the limit", [](auto &f) { put32(f, 16, kMaxMaskFrames + 1); });
    expectRefused("frame count 0 for a full set", [](auto &f) { put32(f, 16, 0); });
    expectRefused("frame count off by one", [](auto &f) { put32(f, 16, 4); });
    expectRefused("quality 0", [](auto &f) { f[20] = 0; });
    expectRefused("quality 9", [](auto &f) { f[20] = 9; });
    expectRefused("silhouette quality with frames", [](auto &f) { f[20] = 3; });
    expectRefused("coarse quality on a finest grid", [](auto &f) { f[20] = 2; });
    expectRefused("reserved byte", [](auto &f) { f[21] = 1; });
    expectRefused("reserved word", [](auto &f) { f[28] = 1; });
    expectRefused("cols 0", [](auto &f) { f[22] = f[23] = 0; });
    expectRefused("cols 129", [](auto &f) { f[22] = 129; f[23] = 0; });
    expectRefused("cols 65535", [](auto &f) { f[22] = f[23] = 0xFF; });
    expectRefused("cell 0", [](auto &f) { f[26] = f[27] = 0; });
    expectRefused("cell 255", [](auto &f) { f[26] = 255; f[27] = 0; });
    expectRefused("trailing byte", [](auto &f) { f.push_back(0); });
    expectRefused("checksum", [](auto &f) { f[32] ^= 1; });
    expectRefused("body bit without a new checksum", [](auto &f) { f[kMaskFileHeaderBytes + 3] ^= 0x10; });
    // Over the bound: refused before anything is read from it.
    const std::vector<std::uint8_t> huge(kMaxMaskFileBytes + 1, 0);
    QVERIFY(!parseMaskFile(huge.data(), huge.size()));
}

void TstAlphaMask::hostileBodiesAreRefused()
{
    const auto good = serializeMaskFile(sample(3));
    // 200 x 100 at 128: cell 2, 100 x 50 cells, 2 words a row; masks of 100 words.
    const std::size_t perMask = AlphaMask::wordsFor(100, 50) * 8;
    {
        // A set bit in the padding of a row (cols 100 of 128).
        std::vector<std::uint8_t> f = good;
        f[kMaskFileHeaderBytes + 15] |= 0x80; // the last bit of the first row's second word
        resign(f);
        QVERIFY(!parseMaskFile(f.data(), f.size()));
    }
    {
        // A frame with a cell the union does not have: clear the whole union, keep the frames.
        std::vector<std::uint8_t> f = good;
        for (std::size_t i = 0; i < perMask && kMaskFileHeaderBytes + i < f.size(); ++i)
            f[kMaskFileHeaderBytes + i] = 0;
        resign(f);
        QVERIFY(!parseMaskFile(f.data(), f.size()));
    }
    {
        // The same edit that keeps the masks nested is accepted: the union gains a cell.
        std::vector<std::uint8_t> f = good;
        f[kMaskFileHeaderBytes + 8 * 99] |= 1; // last row region of the union, a cell no frame has
        resign(f);
        QVERIFY(parseMaskFile(f.data(), f.size()));
    }
}

void TstAlphaMask::aCacheOfAnotherVariantIsRefused()
{
    const auto bytes = serializeMaskFile(sample(3));
    QVERIFY(parseMaskFile(bytes.data(), bytes.size(), 200, 100, 3));
    QVERIFY(!parseMaskFile(bytes.data(), bytes.size(), 199, 100, 3));
    QVERIFY(!parseMaskFile(bytes.data(), bytes.size(), 200, 101, 3));
    QVERIFY(!parseMaskFile(bytes.data(), bytes.size(), 200, 100, 4));
    QVERIFY(parseMaskFile(bytes.data(), bytes.size(), 0, 0, 0)); // nothing expected, nothing checked
}

void TstAlphaMask::mutationsNeverProduceAnInconsistentSet()
{
    const auto good = serializeMaskFile(sample(3));
    std::mt19937 rng(12345);
    int accepted = 0;
    for (int i = 0; i < 4000; ++i) {
        std::vector<std::uint8_t> f = good;
        const int edits = 1 + int(rng() % 4);
        for (int e = 0; e < edits; ++e)
            f[rng() % f.size()] = std::uint8_t(rng());
        if (rng() % 2)
            resign(f);
        const auto parsed = parseMaskFile(f.data(), f.size());
        if (!parsed)
            continue;
        ++accepted;
        // Whatever was accepted is internally consistent: it re-serialises to exactly the bytes read, and nests.
        QCOMPARE(serializeMaskFile(*parsed), f);
        for (int k = 0; k < parsed->frameCount(); ++k)
            QVERIFY(parsed->maskFor(k).isSubsetOf(parsed->silhouette()));
    }
    QVERIFY(accepted < 4000);
}

void TstAlphaMask::filesAreBoundedOnDisk()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const std::string path = (dir.path() + u"/a.mask"_s).toStdString();
    const MaskSet set = sample(3);
    QVERIFY(writeMaskFile(path, set));
    QVERIFY(!QFile::exists(dir.path() + u"/a.mask.tmp"_s));
    const auto back = readMaskFile(path, 200, 100, 3);
    QVERIFY(back);
    QCOMPARE(back->frameCount(), 3);
    QVERIFY(!readMaskFile((dir.path() + u"/missing.mask"_s).toStdString()));
    // A file over the bound is refused from its size, not read.
    QFile big(dir.path() + u"/big.mask"_s);
    QVERIFY(big.open(QIODevice::WriteOnly));
    QVERIFY(big.resize(qint64(kMaxMaskFileBytes) + 1));
    big.close();
    QVERIFY(!readMaskFile((dir.path() + u"/big.mask"_s).toStdString()));
    // Garbage of the right size.
    QFile junk(dir.path() + u"/junk.mask"_s);
    QVERIFY(junk.open(QIODevice::WriteOnly));
    junk.write(QByteArray(4096, 'x'));
    junk.close();
    QVERIFY(!readMaskFile((dir.path() + u"/junk.mask"_s).toStdString()));
    QVERIFY(!writeMaskFile((dir.path() + u"/nodir/x.mask"_s).toStdString(), set));
    QVERIFY(!writeMaskFile(path, MaskSet()));
}

void TstAlphaMask::theAdapterScalesWindowToPicture()
{
    auto mask = std::make_shared<const AlphaMask>(maskOf(disc(100, 30)));
    const auto test = maskTest([mask] { return mask; }, {200, 200});
    QVERIFY(test({100, 100}));    // the middle of the picture
    QVERIFY(!test({5, 5}));       // a corner
    QVERIFY(test({100 + 55, 100})); // 55 window px = 27.5 picture px from the centre: inside the radius 30
    QVERIFY(!test({100 + 70, 100}));
    QVERIFY(!test({-1, 100}));    // outside the window
    QVERIFY(!test({200, 100}));
    QVERIFY(!test({std::nan(""), 1}));
    // No mask yet: the whole window takes input.
    const auto none = maskTest([] { return std::shared_ptr<const AlphaMask>(); }, {200, 200});
    QVERIFY(none({5, 5}));
    QVERIFY(!none({205, 5}));
    QVERIFY(!maskTest({}, {0, 0})({0, 0}));
}

void TstAlphaMask::theAdapterDrivesTheStateMachine()
{
    auto current = std::make_shared<const AlphaMask>(maskOf(disc(100, 30)));
    PetInteraction p(PetInteraction::Level::Global);
    p.setGeometry({100, 100, 200, 200}, {0, 0, 1280, 720});
    p.setMask(maskTest([&current] { return current; }, {200, 200}));
    PetBehaviourConfig c;
    c.hoverHide = true;
    c.clickThrough = false;
    p.setConfig(c, 0);
    p.motionGlobal(0, {200, 200}); // the disc
    p.tick(32);
    QVERIFY(p.hidden());
    p.motionGlobal(40, {110, 110}); // inside the window, outside the disc
    p.tick(64);
    QVERIFY(!p.hidden());
    // The caller swaps the frame's mask: the same pointer position now answers differently.
    current = std::make_shared<const AlphaMask>(maskOf(disc(100, 49)));
    p.motionGlobal(80, {110, 110});
    p.tick(96);
    QVERIFY(!p.hidden()); // (110,110) is window (10,10), picture (5,5): still the corner of a 49-radius disc
    p.motionGlobal(100, {130, 130});
    p.tick(128);
    QVERIFY(p.hidden());
}

void TstAlphaMask::theDocumentListsTheNames()
{
    QFile doc(QStringLiteral(PETS_BEHAVIOURS_DOC));
    QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/behaviours.md is missing");
    const QString text = QString::fromUtf8(doc.readAll());
    for (const std::string_view name : degrade::kAll) {
        const QString row = u"| `"_s + QString::fromUtf8(name.data(), qsizetype(name.size())) + u"` |"_s;
        QVERIFY2(text.contains(row), qPrintable(row));
    }
    // And no row names something the table does not know.
    int rows = 0;
    for (const QString &line : text.split(u'\n')) {
        if (line.startsWith(u"| `"_s) && (line.contains(u": "_s) || line.startsWith(u"| `pointer"_s) || line.startsWith(u"| `hit mask"_s))) {
            const QString name = line.mid(3, line.indexOf(u'`', 3) - 3);
            bool known = false;
            for (const std::string_view k : degrade::kAll)
                known = known || QString::fromUtf8(k.data(), qsizetype(k.size())) == name;
            QVERIFY2(known, qPrintable(name));
            ++rows;
        }
    }
    QCOMPARE(rows, int(degrade::kAll.size()));
}

QTEST_APPLESS_MAIN(TstAlphaMask)
#include "tst_alphamask.moc"
