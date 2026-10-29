// The geometry of a stage (src/overlay/stagegeometry.h): fit modes, the bounding box of the desktop, seam-free slices, limits.

#include "stagegeometry.h"

#include <QFile>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

using namespace murasaki;
using namespace Qt::StringLiterals;

namespace {

StageOutput out(const QString &name, QRect rect, double scale = 1)
{
    StageOutput o;
    o.name = name;
    o.rect = rect;
    o.scale = scale;
    return o;
}

const StageOutput kFullHd = out(u"DP-1"_s, QRect(0, 0, 1920, 1080));

} // namespace

class TstStageGeometry : public QObject
{
    Q_OBJECT

private slots:
    void fitModes_data();
    void fitModes();
    void anUnusableSizeGivesNothing();
    void boundingBoxOfAnLShapeWithNegativeCoordinates();
    void anOutputStageFitsTheOneOutput();
    void anUnknownOutputIsNoStage();
    void containLeavesTransparentBands();
    void coverCropsTheExcess();
    void desktopSplitsOneRoundedPictureWithoutASeam();
    void gapsAreTransparent();
    void mixedScalesShareTheEdge();
    void ninthOutputIsRefused();
    void hugeOutputsAreLeftOut();
    void nonFiniteAndTinyInputs();
    void deviceBudget();
    void randomLayoutsCoverThePictureExactlyOnce();
    void theOutputsAreSortedAndTheStarIsTheirPrimaryOrTheBiggest();
    void aMixedScaleDeskIsSizedInDevicePixels();
    void theCapFactorIsTheSameForEveryoneWhoComputesIt();
    void thePetRectCoversThePictureWhenTurned();
    void theNamesAreInTheDocument();
};

void TstStageGeometry::fitModes_data()
{
    QTest::addColumn<QSizeF>("target");
    QTest::addColumn<QSizeF>("content");
    QTest::addColumn<int>("fit");
    QTest::addColumn<QSizeF>("expected");
    // landscape output
    QTest::newRow("contain landscape source") << QSizeF(1920, 1080) << QSizeF(400, 100) << int(StageFit::Contain) << QSizeF(1920, 480);
    QTest::newRow("contain portrait source") << QSizeF(1920, 1080) << QSizeF(100, 400) << int(StageFit::Contain) << QSizeF(270, 1080);
    QTest::newRow("contain square source") << QSizeF(1920, 1080) << QSizeF(50, 50) << int(StageFit::Contain) << QSizeF(1080, 1080);
    QTest::newRow("cover landscape source") << QSizeF(1920, 1080) << QSizeF(400, 100) << int(StageFit::Cover) << QSizeF(4320, 1080);
    QTest::newRow("cover portrait source") << QSizeF(1920, 1080) << QSizeF(100, 400) << int(StageFit::Cover) << QSizeF(1920, 7680);
    QTest::newRow("cover square source") << QSizeF(1920, 1080) << QSizeF(50, 50) << int(StageFit::Cover) << QSizeF(1920, 1920);
    QTest::newRow("stretch") << QSizeF(1920, 1080) << QSizeF(50, 50) << int(StageFit::Stretch) << QSizeF(1920, 1080);
    // portrait output
    QTest::newRow("contain landscape source, portrait output") << QSizeF(1080, 1920) << QSizeF(400, 100) << int(StageFit::Contain) << QSizeF(1080, 270);
    QTest::newRow("cover landscape source, portrait output") << QSizeF(1080, 1920) << QSizeF(400, 100) << int(StageFit::Cover) << QSizeF(7680, 1920);
    QTest::newRow("contain same aspect") << QSizeF(1920, 1080) << QSizeF(960, 540) << int(StageFit::Contain) << QSizeF(1920, 1080);
}

void TstStageGeometry::fitModes()
{
    QFETCH(QSizeF, target);
    QFETCH(QSizeF, content);
    QFETCH(int, fit);
    QFETCH(QSizeF, expected);
    const QRectF place(100, 50, target.width(), target.height());
    const QRectF r = fitRect(place, content, StageFit(fit));
    QCOMPARE(r.size(), expected);
    // centred on the target in every mode
    QCOMPARE(r.center(), place.center());
}

void TstStageGeometry::anUnusableSizeGivesNothing()
{
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    for (const QSizeF bad : {QSizeF(0, 10), QSizeF(10, 0), QSizeF(-1, 10), QSizeF(nan, 10), QSizeF(10, inf), QSizeF(0, 0)}) {
        QVERIFY(fitRect(QRectF(0, 0, 100, 100), bad, StageFit::Contain).isNull());
        QVERIFY(fitRect(QRectF(0, 0, 100, 100), bad, StageFit::Stretch).isNull());
        QVERIFY(fitRect(QRectF(0, 0, bad.width(), bad.height()), QSizeF(5, 5), StageFit::Cover).isNull());
        QVERIFY(stageLayout({kFullHd}, StageMode::Output, u"DP-1"_s, bad, StageFit::Contain).isEmpty());
    }
    QVERIFY(stageLayout({kFullHd}, StageMode::Off, u"DP-1"_s, QSizeF(10, 10), StageFit::Contain).isEmpty());
}

void TstStageGeometry::boundingBoxOfAnLShapeWithNegativeCoordinates()
{
    const QList<QRect> l = {QRect(-1920, 0, 1920, 1080), QRect(0, 0, 2560, 1440), QRect(0, 1440, 1920, 1080)};
    QCOMPARE(boundingBox(l), QRect(-1920, 0, 4480, 2520));
    QCOMPARE(boundingBox({}), QRect());
    QCOMPARE(boundingBox({QRect(), QRect(5, 5, 0, 10)}), QRect());
    QCOMPARE(boundingBox({QRect(), QRect(5, 5, 10, 10)}), QRect(5, 5, 10, 10));
}

void TstStageGeometry::anOutputStageFitsTheOneOutput()
{
    const StageOutput second = out(u"HDMI-A-1"_s, QRect(1920, 0, 1280, 1024));
    const StageLayout l = stageLayout({kFullHd, second}, StageMode::Output, u"HDMI-A-1"_s, QSizeF(640, 512), StageFit::Contain);
    QCOMPARE(l.slices.size(), 1);
    QCOMPARE(l.slices.first().output, u"HDMI-A-1"_s);
    QCOMPARE(l.slices.first().global, QRect(1920, 0, 1280, 1024));
    QCOMPARE(l.slices.first().window, QRect(0, 0, 1280, 1024));
    QCOMPARE(l.slices.first().source, QRectF(0, 0, 1, 1));
    QCOMPARE(l.picture, QRectF(1920, 0, 1280, 1024));
    QVERIFY(l.degraded.isEmpty());
}

void TstStageGeometry::anUnknownOutputIsNoStage()
{
    QVERIFY(stageLayout({kFullHd}, StageMode::Output, u"DP-9"_s, QSizeF(10, 10), StageFit::Contain).isEmpty());
    QVERIFY(stageLayout({kFullHd}, StageMode::Output, QString(), QSizeF(10, 10), StageFit::Contain).isEmpty());
    QVERIFY(stageLayout({}, StageMode::Desktop, QString(), QSizeF(10, 10), StageFit::Contain).isEmpty());
}

void TstStageGeometry::containLeavesTransparentBands()
{
    // 400x100 in a 1920x1080 output: a 1920x480 band in the middle, the slice is that band and the whole picture.
    const StageLayout l = stageLayout({kFullHd}, StageMode::Output, u"DP-1"_s, QSizeF(400, 100), StageFit::Contain);
    QCOMPARE(l.picture, QRectF(0, 300, 1920, 480));
    QCOMPARE(l.slices.size(), 1);
    QCOMPARE(l.slices.first().window, QRect(0, 300, 1920, 480));
    QCOMPARE(l.slices.first().source, QRectF(0, 0, 1, 1));
}

void TstStageGeometry::coverCropsTheExcess()
{
    // 400x100 covers 1920x1080 at 4320x1080: the output shows the middle 1920 of 4320.
    const StageLayout l = stageLayout({kFullHd}, StageMode::Output, u"DP-1"_s, QSizeF(400, 100), StageFit::Cover);
    QCOMPARE(l.picture, QRectF(-1200, 0, 4320, 1080));
    QCOMPARE(l.slices.size(), 1);
    QCOMPARE(l.slices.first().window, QRect(0, 0, 1920, 1080));
    QVERIFY(qFuzzyCompare(l.slices.first().source.x(), 1200.0 / 4320.0));
    QVERIFY(qFuzzyCompare(l.slices.first().source.width(), 1920.0 / 4320.0));
    QCOMPARE(l.slices.first().source.y(), 0.0);
    QCOMPARE(l.slices.first().source.height(), 1.0);
}

void TstStageGeometry::desktopSplitsOneRoundedPictureWithoutASeam()
{
    // Two 1920x1080 side by side with a 2:1 picture contained in 3840x1080: the bezel is the middle of the picture.
    const StageOutput left = out(u"L"_s, QRect(0, 0, 1920, 1080));
    const StageOutput right = out(u"R"_s, QRect(1920, 0, 1920, 1080));
    const StageLayout l = stageLayout({left, right}, StageMode::Desktop, QString(), QSizeF(333, 100), StageFit::Contain);
    QCOMPARE(l.slices.size(), 2);
    QCOMPARE(l.target, QRect(0, 0, 3840, 1080));
    const StageSlice a = l.slices.at(0);
    const StageSlice b = l.slices.at(1);
    QCOMPARE(a.global.right() + 1, b.global.left());                       // touching to the pixel
    QCOMPARE(a.source.right(), b.source.left());                           // the same double on the shared edge
    QCOMPARE(a.source.left(), 0.0);
    QCOMPARE(b.source.right(), 1.0);
    QVERIFY(l.picture.left() == std::round(l.picture.left()) && l.picture.width() == std::round(l.picture.width()));
}

void TstStageGeometry::gapsAreTransparent()
{
    // A gap of 100 px between two outputs: the picture spans it, the gap has no slice, the sides join up in picture coordinates.
    const StageOutput left = out(u"L"_s, QRect(0, 0, 1000, 500));
    const StageOutput right = out(u"R"_s, QRect(1100, 0, 1000, 500));
    const StageLayout l = stageLayout({left, right}, StageMode::Desktop, QString(), QSizeF(2100, 500), StageFit::Stretch);
    QCOMPARE(l.slices.size(), 2);
    QCOMPARE(l.picture, QRectF(0, 0, 2100, 500));
    QVERIFY(qFuzzyCompare(l.slices.at(0).source.right(), 1000.0 / 2100.0));
    QVERIFY(qFuzzyCompare(l.slices.at(1).source.left(), 1100.0 / 2100.0));
    QCOMPARE(l.slices.at(1).window, QRect(0, 0, 1000, 500));
}

void TstStageGeometry::mixedScalesShareTheEdge()
{
    const StageOutput a = out(u"A"_s, QRect(0, 0, 1280, 720), 1.5);
    const StageOutput b = out(u"B"_s, QRect(1280, 0, 1920, 1080), 1.0);
    const StageOutput c = out(u"C"_s, QRect(-1280, 0, 1280, 800), 2.0);
    const StageLayout l = stageLayout({a, b, c}, StageMode::Desktop, QString(), QSizeF(1000, 333), StageFit::Cover);
    QCOMPARE(l.slices.size(), 3);
    QCOMPARE(stageMaxScale(l), 2.0);
    // A and B touch: their shared edge is one number on both sides
    QCOMPARE(l.slices.at(0).source.right(), l.slices.at(1).source.left());
    QCOMPARE(l.slices.at(2).source.right(), l.slices.at(0).source.left());
    QVERIFY(stageDevicePixels(l) > 0);
}

void TstStageGeometry::ninthOutputIsRefused()
{
    QList<StageOutput> outputs;
    for (int i = 0; i < 9; ++i)
        outputs.append(out(QString::number(i), QRect(i * 100, 0, 100, 100)));
    const StageLayout l = stageLayout(outputs, StageMode::Desktop, QString(), QSizeF(900, 100), StageFit::Stretch);
    QCOMPARE(l.slices.size(), kMaxStageOutputs);
    QCOMPARE(l.degraded, QStringList{QString::fromUtf8(stagedegrade::kTooManyOutputs.data())});
    QCOMPARE(l.slices.last().output, u"7"_s);
}

void TstStageGeometry::hugeOutputsAreLeftOut()
{
    // a side beyond 16384, and a layout that would make the box larger than that
    const StageOutput huge = out(u"H"_s, QRect(0, 0, kMaxStageSide + 1, 100));
    QVERIFY(stageLayout({huge}, StageMode::Output, u"H"_s, QSizeF(10, 10), StageFit::Contain).isEmpty());
    const StageOutput a = out(u"A"_s, QRect(0, 0, 9000, 1000));
    const StageOutput b = out(u"B"_s, QRect(9000, 0, 9000, 1000));
    const StageLayout l = stageLayout({a, b}, StageMode::Desktop, QString(), QSizeF(10, 1), StageFit::Stretch);
    QCOMPARE(l.slices.size(), 1);
    QCOMPARE(l.slices.first().output, u"A"_s);
    QVERIFY(!l.degraded.isEmpty());
    // exactly the limit is accepted
    const StageOutput edge = out(u"E"_s, QRect(0, 0, kMaxStageSide, kMaxStageSide));
    QCOMPARE(stageLayout({edge}, StageMode::Output, u"E"_s, QSizeF(1, 1), StageFit::Contain).slices.size(), 1);
}

void TstStageGeometry::nonFiniteAndTinyInputs()
{
    // 1x1 picture, 1x1 output: one pixel
    const StageOutput one = out(u"O"_s, QRect(0, 0, 1, 1));
    const StageLayout l = stageLayout({one}, StageMode::Output, u"O"_s, QSizeF(1, 1), StageFit::Contain);
    QCOMPARE(l.slices.size(), 1);
    QCOMPARE(l.slices.first().window, QRect(0, 0, 1, 1));
    // a picture so thin that it rounds to nothing still has one pixel, never a division by zero
    const StageLayout thin = stageLayout({kFullHd}, StageMode::Output, u"DP-1"_s, QSizeF(100000, 1e-9), StageFit::Contain);
    for (const StageSlice &s : thin.slices) {
        QVERIFY(std::isfinite(s.source.x()) && std::isfinite(s.source.width()));
        QVERIFY(s.source.width() > 0 && s.source.height() > 0);
    }
    // an extreme cover stays finite
    const StageLayout extreme = stageLayout({kFullHd}, StageMode::Output, u"DP-1"_s, QSizeF(1, 1e9), StageFit::Cover);
    for (const StageSlice &s : extreme.slices)
        QVERIFY(std::isfinite(s.source.height()) && s.source.height() > 0);
}

void TstStageGeometry::deviceBudget()
{
    QCOMPARE(stageResolutionFactor(0, 100), 1.0);
    QCOMPARE(stageResolutionFactor(100, 100), 1.0);
    QCOMPARE(stageResolutionFactor(100, 0), 1.0);
    QCOMPARE(stageResolutionFactor(-5, 100), 1.0);
    QCOMPARE(stageResolutionFactor(400, 100), 0.5);
    QVERIFY(stageResolutionFactor(1LL << 40, 1) >= 0.1);
    const StageOutput hi = out(u"A"_s, QRect(0, 0, 1920, 1080), 2.0);
    const StageLayout l = stageLayout({hi}, StageMode::Output, u"A"_s, QSizeF(16, 9), StageFit::Contain);
    QCOMPARE(stageDevicePixels(l), 3840LL * 2160);
    // two outputs share the cap: the factor of the pair
    const StageOutput b = out(u"B"_s, QRect(1920, 0, 1920, 1080), 1.0);
    const StageLayout pair = stageLayout({hi, b}, StageMode::Desktop, QString(), QSizeF(32, 9), StageFit::Contain);
    QCOMPARE(stageDevicePixels(pair), 3840LL * 2160 + 1920LL * 1080);
}

void TstStageGeometry::randomLayoutsCoverThePictureExactlyOnce()
{
    std::mt19937 rng(20261022); // fixed seed
    const auto pick = [&](int low, int high) { return std::uniform_int_distribution<int>(low, high)(rng); };
    const double scales[] = {1.0, 1.25, 1.5, 2.0};
    for (int round = 0; round < 300; ++round) {
        QList<StageOutput> outputs;
        int x = pick(-3000, 0);
        for (int i = 0, n = pick(1, 4); i < n; ++i) {
            const int w = pick(200, 2600);
            const int h = pick(200, 1600);
            const int y = pick(-300, 300);
            outputs.append(out(QString::number(i), QRect(x, y, w, h), scales[pick(0, 3)]));
            x += w + (pick(0, 3) == 0 ? pick(1, 80) : 0); // mostly touching, sometimes a gap
        }
        const QSizeF footprint(pick(1, 4000), pick(1, 4000));
        for (const StageFit fit : {StageFit::Contain, StageFit::Cover, StageFit::Stretch}) {
            const StageLayout l = stageLayout(outputs, StageMode::Desktop, QString(), footprint, fit);
            QVERIFY(!l.picture.isNull());
            long long covered = 0;
            for (int i = 0; i < l.slices.size(); ++i) {
                const StageSlice &s = l.slices.at(i);
                covered += s.global.width() * s.global.height();
                // inside the picture and inside its output
                QVERIFY(l.picture.contains(QRectF(s.global)));
                QVERIFY(s.source.left() >= 0 && s.source.top() >= 0 && s.source.right() <= 1.0 + 1e-12 && s.source.bottom() <= 1.0 + 1e-12);
                for (int j = i + 1; j < l.slices.size(); ++j)
                    QVERIFY2(!l.slices.at(j).global.intersects(s.global), "two slices overlap");
            }
            // the union of the slices is the picture where there are outputs: area of picture ∩ union(outputs) (outputs do not overlap here)
            long long expected = 0;
            for (const StageOutput &o : outputs) {
                const QRectF part = QRectF(o.rect).intersected(l.picture);
                expected += part.isEmpty() ? 0 : qint64(part.width()) * qint64(part.height());
            }
            QCOMPARE(covered, expected);
        }
    }
}

void TstStageGeometry::theOutputsAreSortedAndTheStarIsTheirPrimaryOrTheBiggest()
{
    StageOutput a = out(u"B"_s, QRect(0, 0, 1920, 1080));
    StageOutput b = out(u"A"_s, QRect(1920, 0, 1280, 720), 2.0);
    StageOutput c = out(u"C"_s, QRect(3200, 0, 1000, 1000));
    QCOMPARE(sortedStageOutputs({a, b, c}).first().name, u"A"_s);
    QCOMPARE(resolveStageOutput({a, b, c}, u"C"_s), u"C"_s); // a name is itself
    QCOMPARE(resolveStageOutput({a, b, c}, u"*"_s), u"A"_s); // the most device pixels: 1280x720 at scale 2 is 2560x1440, more than 1920x1080
    b.scale = 1.0;
    QCOMPARE(resolveStageOutput({a, b, c}, u"*"_s), u"B"_s);
    c.primary = true;
    QCOMPARE(resolveStageOutput({a, b, c}, u"*"_s), u"C"_s); // the primary wins when told
    QCOMPARE(resolveStageOutput({}, u"*"_s), QString());
    // the order they are listed in changes nothing, also in a desktop stage that has to leave some out
    QList<StageOutput> nine;
    for (int i = 0; i < 9; ++i)
        nine.append(out(QString(u"N%1"_s).arg(i), QRect(i * 100, 0, 100, 100)));
    StageRequest request;
    request.mode = StageMode::Desktop;
    request.fit = StageFit::Stretch;
    request.native = QSize(900, 100);
    const StageLayout forward = stageLayoutOf(nine, request);
    std::reverse(nine.begin(), nine.end());
    const StageLayout reversed = stageLayoutOf(nine, request);
    QCOMPARE(forward.slices.size(), reversed.slices.size());
    for (int i = 0; i < forward.slices.size(); ++i)
        QCOMPARE(forward.slices.at(i).output, reversed.slices.at(i).output);
}

void TstStageGeometry::aMixedScaleDeskIsSizedInDevicePixels()
{
    // 4K at scale 2 (1920x1080 logical) next to 1440p at scale 1 (2560x1440): "*" with no primary told must be served by a copy for either.
    const StageOutput uhd = out(u"HDMI-A-1"_s, QRect(0, 0, 1920, 1080), 2.0);
    const StageOutput qhd = out(u"DP-1"_s, QRect(1920, 0, 2560, 1440), 1.0);
    StageRequest request;
    request.mode = StageMode::Output;
    request.fit = StageFit::Contain;
    request.output = u"*"_s;
    request.native = QSize(4000, 2250);
    const int onUhd = [&] { StageRequest r = request; r.output = u"HDMI-A-1"_s; return stageRequestNeed({uhd, qhd}, r, 1.0, 4000); }();
    const int onQhd = [&] { StageRequest r = request; r.output = u"DP-1"_s; return stageRequestNeed({uhd, qhd}, r, 1.0, 4000); }();
    QCOMPARE(onUhd, 3840);   // 1920 logical x 2
    QCOMPARE(onQhd, 2560);
    QCOMPARE(stageRequestNeed({uhd, qhd}, request, 1.0, 4000), 3840); // nobody said: the one that needs the most
    // told which is the primary: that one
    StageOutput primaryQhd = qhd;
    primaryQhd.primary = true;
    QCOMPARE(stageRequestNeed({uhd, primaryQhd}, request, 1.0, 4000), 2560);
    StageOutput primaryUhd = uhd;
    primaryUhd.primary = true;
    QCOMPARE(stageRequestNeed({primaryUhd, qhd}, request, 1.0, 4000), 3840);
    // never above the source, and the order they are given in does not matter
    QCOMPARE(stageRequestNeed({qhd, uhd}, request, 1.0, 1000), 1000);
    QCOMPARE(stageRequestNeed({qhd, uhd}, request, 1.0, 4000), stageRequestNeed({uhd, qhd}, request, 1.0, 4000));
    // no outputs or an animation with no size: nothing can be told
    QCOMPARE(stageRequestNeed({}, request, 1.0, 4000), 0);
    request.native = QSize();
    QCOMPARE(stageRequestNeed({uhd, qhd}, request, 1.0, 4000), 0);
}

void TstStageGeometry::theCapFactorIsTheSameForEveryoneWhoComputesIt()
{
    const StageOutput a = out(u"A"_s, QRect(0, 0, 1920, 1080), 2.0);
    const StageOutput b = out(u"B"_s, QRect(1920, 0, 1920, 1080), 1.0);
    StageRequest one;
    one.mode = StageMode::Output;
    one.fit = StageFit::Stretch;
    one.output = u"A"_s;
    one.native = QSize(100, 100);
    StageRequest two = one;
    two.output = u"B"_s;
    QCOMPARE(stageRequestPixels({a, b}, {one}), 3840LL * 2160);
    QCOMPARE(stageRequestPixels({a, b}, {one, two}), 3840LL * 2160 + 1920LL * 1080);
    QCOMPARE(stageRequestPixels({b, a}, {two, one}), stageRequestPixels({a, b}, {one, two}));
    // the factor, and the need it gives
    const double factor = stageResolutionFactor(stageRequestPixels({a, b}, {one, two}), 4000000);
    QVERIFY(factor < 1 && factor > 0.5);
    const int full = stageRequestNeed({a, b}, one, 1.0, 100000);
    const int reduced = stageRequestNeed({a, b}, one, factor, 100000);
    QVERIFY(reduced < full && reduced > full / 2);
}

void TstStageGeometry::thePetRectCoversThePictureWhenTurned()
{
    const StageOutput o = out(u"O"_s, QRect(1920, 0, 1920, 1080));
    // not turned: the rectangle is the picture, in window coordinates
    StageLayout l = stageLayout({o}, StageMode::Output, u"O"_s, QSizeF(400, 100), StageFit::Contain);
    QCOMPARE(stagePetRect(l, l.slices.first(), QSizeF(400, 100), 0, o.rect), QRectF(0, 300, 1920, 480));
    QCOMPARE(stagePetRect(l, l.slices.first(), QSizeF(400, 100), 180, o.rect), QRectF(0, 300, 1920, 480));
    // a quarter turn: the footprint is 100x400, fitted to 270x1080, so the unrotated animation is 1080x270, centred on the same point
    l = stageLayout({o}, StageMode::Output, u"O"_s, stageFootprint(QSizeF(400, 100), 90), StageFit::Contain);
    const QRectF r = stagePetRect(l, l.slices.first(), QSizeF(400, 100), 90, o.rect);
    QCOMPARE(r.size(), QSizeF(1080, 270));
    QCOMPARE(r.center(), QRectF(l.picture).translated(-1920, 0).center());
    // stretch on a quarter turn fills the picture after turning: width and height swap their factors
    l = stageLayout({o}, StageMode::Output, u"O"_s, stageFootprint(QSizeF(400, 100), 270), StageFit::Stretch);
    const QRectF s = stagePetRect(l, l.slices.first(), QSizeF(400, 100), 270, o.rect);
    QCOMPARE(s.size(), QSizeF(1080, 1920));
    // an arbitrary angle uses one factor: the turned rectangle is inside the picture, touching on one side
    l = stageLayout({o}, StageMode::Output, u"O"_s, stageFootprint(QSizeF(400, 100), 30), StageFit::Contain);
    const QRectF t = stagePetRect(l, l.slices.first(), QSizeF(400, 100), 30, o.rect);
    QVERIFY(qFuzzyCompare(t.width() / t.height(), 4.0));
    QVERIFY(stagePetRect(StageLayout{}, StageSlice{}, QSizeF(1, 1), 0, o.rect).isNull());
}

void TstStageGeometry::theNamesAreInTheDocument()
{
    QFile doc(QStringLiteral(PETS_STAGE_DOC));
    QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/stage.md is missing");
    const QString text = QString::fromUtf8(doc.readAll());
    for (const std::string_view name : stagedegrade::kAll)
        QVERIFY2(text.contains(u"| `"_s + QString::fromUtf8(name.data(), int(name.size())) + u"` |"_s), name.data());
    // and the limits it states
    QVERIFY(text.contains(QString::number(kMaxStageSide)));
    QVERIFY(text.contains(QString::number(kDefaultStageMaxPixels)));
}

QTEST_APPLESS_MAIN(TstStageGeometry)
#include "tst_stagegeometry.moc"
