// The matching core with synthetic pictures and scripted tick streams on a fake clock: grey and area scaling, the hash (with golden bits), the structure score, the mode and
// aggregation rules, and the state machine (streaks, sequencer, grace, cooldown, hold). Also the cost of one tick for 1, 8 and 16 regions.

#include "screenmatch.h"

#include <QElapsedTimer>
#include <QRandomGenerator>
#include <QTest>

#include <bit>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

struct Box {
    int x, y, w, h, value;
};

GreyImage flat(int w, int h, int value)
{
    GreyImage g;
    g.width = w;
    g.height = h;
    g.pixels = QByteArray(w * h, char(value));
    return g;
}

// A dark picture with bright boxes: enough structure for the correlation, easy to shift or change.
GreyImage scene(int w, int h, const QList<Box> &boxes, int background = 20, int dx = 0, int gain = 0)
{
    GreyImage g = flat(w, h, background);
    auto *p = reinterpret_cast<uchar *>(g.pixels.data());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p[y * w + x] = uchar(qBound(0, background + gain, 255));
    for (const Box &b : boxes)
        for (int y = b.y; y < b.y + b.h && y < h; ++y)
            for (int x = b.x + dx; x < b.x + dx + b.w && x < w; ++x)
                if (x >= 0 && y >= 0)
                    p[y * w + x] = uchar(qBound(0, b.value + gain, 255));
    return g;
}

const QList<Box> kSceneA = {{20, 15, 60, 40, 220}, {110, 60, 50, 45, 150}, {40, 80, 30, 25, 90}, {150, 10, 35, 30, 250}};
const QList<Box> kSceneB = {{100, 20, 70, 30, 200}, {15, 70, 40, 40, 120}, {130, 85, 55, 25, 240}};

GreyImage horizontalGradient(int w, int h)
{
    GreyImage g = flat(w, h, 0);
    auto *p = reinterpret_cast<uchar *>(g.pixels.data());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p[y * w + x] = uchar(x * 255 / (w - 1));
    return g;
}

GreyImage verticalGradient(int w, int h)
{
    GreyImage g = flat(w, h, 0);
    auto *p = reinterpret_cast<uchar *>(g.pixels.data());
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            p[y * w + x] = uchar(y * 255 / (h - 1));
    return g;
}

// 8x8 cells of `cell` pixels, bright where (cx + cy) is odd.
GreyImage checker(int cell)
{
    GreyImage g = flat(8 * cell, 8 * cell, 0);
    auto *p = reinterpret_cast<uchar *>(g.pixels.data());
    for (int y = 0; y < 8 * cell; ++y)
        for (int x = 0; x < 8 * cell; ++x)
            p[y * 8 * cell + x] = ((x / cell + y / cell) & 1) ? 200 : 40;
    return g;
}

// Natural-ish BGRA: smooth gradients, some filled shapes, a little noise. `variant` moves the shapes and shifts the light so a pair is "the same place, changed".
QByteArray naturalBgra(int w, int h, quint32 seed, int variant)
{
    QRandomGenerator rng(seed);
    const double gx = rng.generateDouble() * 120, gy = rng.generateDouble() * 120, base = 30 + rng.generateDouble() * 100;
    struct Shape { int x, y, w, h, v; };
    QList<Shape> shapes;
    for (int i = 0; i < 6; ++i)
        shapes.append({int(rng.bounded(w)) + variant * (w / 40), int(rng.bounded(h)), 1 + int(rng.bounded(w / 3 + 1)), 1 + int(rng.bounded(h / 3 + 1)), int(rng.bounded(256))});
    QByteArray px(w * h * 4, 0);
    QRandomGenerator noise(seed ^ 0x9E3779B9u);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            double v = base + gx * x / w + gy * y / h + (variant ? 12 * variant : 0);
            for (const Shape &sh : shapes)
                if (x >= sh.x && x < sh.x + sh.w && y >= sh.y && y < sh.y + sh.h)
                    v = sh.v;
            v += (int(noise.bounded(9)) - 4);
            const uchar b = uchar(qBound(0.0, v * 0.9, 255.0)), g = uchar(qBound(0.0, v, 255.0)), r = uchar(qBound(0.0, v * 1.1, 255.0));
            char *o = px.data() + (y * w + x) * 4;
            o[0] = char(b);
            o[1] = char(g);
            o[2] = char(r);
            o[3] = char(255);
        }
    return px;
}

double hashOf(const GreyImage &a, const GreyImage &b)
{
    return hashScore(prepareScreenPicture(a), prepareScreenPicture(b));
}

double structureOf(const GreyImage &a, const GreyImage &b)
{
    return structureScore(prepareScreenPicture(a), prepareScreenPicture(b));
}

QList<double> one(double s)
{
    return {s};
}

// Runs scores at a fixed period; returns the events as a string of letters: '.' none, 'A' activated, 'D' deactivated.
struct Script {
    ScreenMatcher matcher;
    qint64 now = 0;
    qint64 period = 60;
    QString trace;

    explicit Script(const ScreenMatchConfig &c) : matcher(c) {}

    ScreenTickResult step(double score)
    {
        const ScreenTickResult r = matcher.tick(now, one(score));
        trace += r.event == ScreenEvent::Activated ? u'A' : r.event == ScreenEvent::Deactivated ? u'D' : u'.';
        now += period;
        return r;
    }
    void run(const QString &pattern) // '1' passes, '0' fails
    {
        for (const QChar c : pattern)
            step(c == u'1' ? 1.0 : 0.0);
    }
};

ScreenMatchConfig config(int startFrames, qint64 grace = 250, qint64 cooldown = 500)
{
    ScreenMatchConfig c;
    c.startFrames = startFrames;
    c.graceMs = grace;
    c.cooldownMs = cooldown;
    c.threshold = 0.75;
    return c;
}

} // namespace

class TstScreenMatch : public QObject
{
    Q_OBJECT

private slots:
    // ---- Pictures ----
    void greyConversionUsesRec601AndIgnoresAlpha()
    {
        const uchar rgba[] = {255, 0, 0, 9, 0, 255, 0, 9, 0, 0, 255, 9, 255, 255, 255, 0};
        const GreyImage a = greyFromPixels(rgba, qsizetype(16) * (1), 4, 1, 16, ScreenPixelFormat::Rgba32);
        QCOMPARE(int(uchar(a.pixels[0])), 77);   // pure red: 77 * 255 / 256 rounded
        QCOMPARE(int(uchar(a.pixels[1])), 149);
        QCOMPARE(int(uchar(a.pixels[2])), 29);
        QCOMPARE(int(uchar(a.pixels[3])), 255);
        const GreyImage b = greyFromPixels(rgba, qsizetype(16) * (1), 4, 1, 16, ScreenPixelFormat::Bgra32); // same bytes read as B G R A
        QCOMPARE(int(uchar(b.pixels[0])), 29);
        QCOMPARE(int(uchar(b.pixels[2])), 77);
        const uchar rgb[] = {255, 0, 0, 99, 99, 99, 0, 255, 0, 99, 99, 99}; // two rows of one pixel, stride 6 (padding)
        const GreyImage c = greyFromPixels(rgb, qsizetype(6) * (2), 1, 2, 6, ScreenPixelFormat::Rgb24);
        QCOMPARE(int(uchar(c.pixels[0])), 77);
        QCOMPARE(int(uchar(c.pixels[1])), 149);
        const uchar g8[] = {5, 6, 7, 8};
        QCOMPARE(greyFromPixels(g8, qsizetype(2) * (2), 2, 2, 2, ScreenPixelFormat::Grey8).pixels, QByteArray("\x05\x06\x07\x08", 4));
    }

    void greyConversionRefusesBadArguments()
    {
        const uchar px[16] = {};
        QVERIFY(greyFromPixels(nullptr, qsizetype(8) * (2), 2, 2, 8, ScreenPixelFormat::Rgba32).isEmpty());
        QVERIFY(greyFromPixels(px, qsizetype(8) * (2), 0, 2, 8, ScreenPixelFormat::Rgba32).isEmpty());
        QVERIFY(greyFromPixels(px, qsizetype(8) * (0), 2, 0, 8, ScreenPixelFormat::Rgba32).isEmpty());
        QVERIFY(greyFromPixels(px, qsizetype(7) * (2), 2, 2, 7, ScreenPixelFormat::Rgba32).isEmpty()); // stride shorter than a row
        QVERIFY(!prepareScreenPicture(GreyImage()).valid);
    }

    void fusedGreyScalingEqualsTheTwoSteps()
    {
        QRandomGenerator rng(5);
        QByteArray px(997 * 613 * 4, 0);
        for (int i = 0; i < px.size(); ++i)
            px[i] = char(rng.bounded(256));
        for (const ScreenPixelFormat f : {ScreenPixelFormat::Bgra32, ScreenPixelFormat::Rgba32}) {
            const auto *d = reinterpret_cast<const uchar *>(px.constData());
            const GreyImage two = fitLongerSide(greyFromPixels(d, qsizetype(997 * 4) * (613), 997, 613, 997 * 4, f));
            const GreyImage fused = greyScaledFromPixels(d, qsizetype(997 * 4) * (613), 997, 613, 997 * 4, f);
            QCOMPARE(fused.width, two.width);
            QCOMPARE(fused.height, two.height);
            QCOMPARE(fused.pixels, two.pixels);
        }
        // sampling rows: the same shape, and a picture that still matches the full one
        const auto *d = reinterpret_cast<const uchar *>(px.constData());
        const GreyImage full = greyScaledFromPixels(d, qsizetype(997 * 4) * (613), 997, 613, 997 * 4, ScreenPixelFormat::Bgra32);
        const GreyImage sampled = greyScaledFromPixels(d, qsizetype(997 * 4) * (613), 997, 613, 997 * 4, ScreenPixelFormat::Bgra32, kScreenMatchMaxSide, 2);
        QCOMPARE(sampled.width, full.width);
        QCOMPARE(sampled.height, full.height);
        QVERIFY(!sampled.isEmpty());
        const GreyImage scenePx = scene(1280, 720, kSceneA);
        QByteArray rgba(1280 * 720 * 4, 0);
        for (int i = 0; i < 1280 * 720; ++i)
            rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = scenePx.pixels[i];
        const auto *r = reinterpret_cast<const uchar *>(rgba.constData());
        const ScreenPicture whole = prepareScreenPicture(greyScaledFromPixels(r, qsizetype(1280 * 4) * (720), 1280, 720, 1280 * 4, ScreenPixelFormat::Rgba32));
        const ScreenPicture cheap = prepareScreenPicture(greyScaledFromPixels(r, qsizetype(1280 * 4) * (720), 1280, 720, 1280 * 4, ScreenPixelFormat::Rgba32, kScreenMatchMaxSide, 3));
        QVERIFY(structureScore(whole, cheap) > 0.95);
        const GreyImage small = greyScaledFromPixels(reinterpret_cast<const uchar *>(px.constData()), qsizetype(997 * 4) * (50), 100, 50, 997 * 4, ScreenPixelFormat::Bgra32);
        QCOMPARE(small.width, 100); // already fits: only grey
        QVERIFY(greyScaledFromPixels(nullptr, qsizetype(40) * (10), 10, 10, 40, ScreenPixelFormat::Bgra32).isEmpty());
    }

    void areaScalingAveragesTheCoveredPixels()
    {
        GreyImage g = flat(2, 2, 0);
        g.pixels = QByteArray("\x00\x64\xc8\xff", 4); // 0 100 200 255
        QCOMPARE(int(uchar(resizeArea(g, 1, 1).pixels[0])), 139); // (0+100+200+255)/4 = 138.75
        const GreyImage fractional = resizeArea(horizontalGradient(3, 1), 2, 1); // the two halves of 3 pixels are [0, 1.5) and [1.5, 3)
        QCOMPARE(int(uchar(fractional.pixels[0])), 42);  // source 0 127 255: (0 x 1 + 127 x 0.5) / 1.5
        QCOMPARE(int(uchar(fractional.pixels[1])), 212); // (127 x 0.5 + 255 x 1) / 1.5
        const GreyImage up = resizeArea(g, 4, 4);
        QCOMPARE(int(uchar(up.pixels[0])), 0);
        QCOMPARE(int(uchar(up.pixels[3])), 100);
        QCOMPARE(int(uchar(up.pixels[15])), 255);
        const GreyImage same = flat(5, 3, 77);
        QCOMPARE(resizeArea(same, 2, 2).pixels, QByteArray(4, char(77))); // a flat picture stays flat at any size
        QCOMPARE(resizeArea(flat(1920, 1080, 133), 360, 203).pixels, QByteArray(360 * 203, char(133)));
    }

    void rowStepGrowsWithTheCrop()
    {
        QCOMPARE(screenRowStepFor(300), 1);
        QCOMPARE(screenRowStepFor(511), 1);
        QCOMPARE(screenRowStepFor(720), 2);
        QCOMPARE(screenRowStepFor(1080), 4);
        QCOMPARE(screenRowStepFor(2160), 8);
        QCOMPARE(screenRowStepFor(0), 1);
    }

    void longerSideIsFittedTo360()
    {
        const GreyImage wide = fitLongerSide(flat(1920, 1080, 10));
        QCOMPARE(wide.width, 360);
        QCOMPARE(wide.height, 203);
        const GreyImage tall = fitLongerSide(flat(500, 2000, 10));
        QCOMPARE(tall.height, 360);
        QCOMPARE(tall.width, 90);
        const GreyImage small = fitLongerSide(flat(360, 200, 10));
        QCOMPARE(small.width, 360);
        QCOMPARE(small.height, 200);
        const GreyImage sliver = fitLongerSide(flat(4000, 3, 10));
        QCOMPARE(sliver.width, 360);
        QCOMPARE(sliver.height, 1);
    }

    void aReferenceTravelsAsItsThirtyTwoGrid()
    {
        const GreyImage a = scene(200, 120, kSceneA), b = scene(200, 120, kSceneB);
        const QByteArray grid = screenGridOf(a);
        QCOMPARE(grid.size(), kScreenStructureSide * kScreenStructureSide);
        const ScreenPicture viaGrid = screenPictureFromGrid(reinterpret_cast<const uchar *>(grid.constData()));
        const ScreenPicture direct = prepareScreenPicture(a);
        QVERIFY(viaGrid.valid && viaGrid.hasStructure);
        QCOMPARE(viaGrid.hash, direct.hash);
        QVERIFY(qFuzzyCompare(structureScore(viaGrid, direct), 1.0) || structureScore(viaGrid, direct) > 0.9999);
        // scored against a live picture it behaves like the reference built from the pixels
        const ScreenPicture live = prepareScreenPicture(scene(200, 120, kSceneA, 20, 0, 25));
        QVERIFY(qAbs(structureScore(viaGrid, live) - structureScore(direct, live)) < 0.005);
        QVERIFY(structureScore(viaGrid, prepareScreenPicture(b)) < 0.65);
        // flat pictures stay flat
        const QByteArray flatGrid = screenGridOf(flat(100, 100, 90));
        const ScreenPicture flatPic = screenPictureFromGrid(reinterpret_cast<const uchar *>(flatGrid.constData()));
        QCOMPARE(flatPic.hash, quint64(0));
        QCOMPARE(flatPic.norm, 0.0f);
        QVERIFY(!screenPictureFromGrid(nullptr).valid);
        QVERIFY(screenGridOf(GreyImage()).isEmpty());
        GreyImage lying;
        lying.width = 10;
        lying.height = 10;
        QVERIFY(screenGridOf(lying).isEmpty());
    }

    // ---- Hash ----
    void goldenHashBits()
    {
        // Bit y*8+x is set when cell (x, y) is above the mean of the 64 cells.
        QCOMPARE(prepareScreenPicture(horizontalGradient(64, 64)).hash, quint64(0xF0F0F0F0F0F0F0F0));
        QCOMPARE(prepareScreenPicture(horizontalGradient(100, 37)).hash, quint64(0xF0F0F0F0F0F0F0F0)); // not a multiple of 8
        QCOMPARE(prepareScreenPicture(verticalGradient(64, 64)).hash, quint64(0xFFFFFFFF00000000));
        QCOMPARE(prepareScreenPicture(checker(8)).hash, quint64(0x55AA55AA55AA55AA)); // bright where x + y is odd: row 0 is 0xAA, row 1 is 0x55
    }

    void flatPicturesHaveNoBitsSet()
    {
        QCOMPARE(prepareScreenPicture(flat(300, 200, 0)).hash, quint64(0));
        QCOMPARE(prepareScreenPicture(flat(300, 200, 255)).hash, quint64(0));
        QCOMPARE(prepareScreenPicture(flat(7, 5, 91)).hash, quint64(0));
    }

    void hashScoreIsHammingInSixtyFourths()
    {
        ScreenPicture a = prepareScreenPicture(horizontalGradient(64, 64));
        ScreenPicture b = a;
        QCOMPARE(hashScore(a, b), 1.0);
        b.hash ^= 0b111;
        QCOMPARE(hashScore(a, b), 61.0 / 64.0);
        b.hash = ~a.hash;
        QCOMPARE(hashScore(a, b), 0.0);
        QCOMPARE(hashScore(a, ScreenPicture()), 0.0);
        QCOMPARE(hashScore(ScreenPicture(), ScreenPicture()), 0.0);
    }

    void hashIgnoresResolution()
    {
        QCOMPARE(hashOf(scene(200, 120, kSceneA), scene(2000, 1200, [] {
                            QList<Box> big;
                            for (const Box &b : kSceneA)
                                big.append({b.x * 10, b.y * 10, b.w * 10, b.h * 10, b.value});
                            return big;
                        }())),
                 1.0);
    }

    // ---- Structure ----
    void equalPicturesScoreOne()
    {
        const GreyImage a = scene(200, 120, kSceneA);
        QCOMPARE(hashOf(a, a), 1.0);
        QVERIFY(qFuzzyCompare(structureOf(a, a), 1.0));
    }

    void brightnessChangeKeepsStructureHigh()
    {
        const GreyImage a = scene(200, 120, kSceneA);
        const GreyImage brighter = scene(200, 120, kSceneA, 20, 0, 25);
        const GreyImage darker = scene(200, 120, kSceneA, 20, 0, -15);
        QVERIFY2(structureOf(a, brighter) > 0.95, qPrintable(QString::number(structureOf(a, brighter))));
        QVERIFY2(structureOf(a, darker) > 0.95, qPrintable(QString::number(structureOf(a, darker))));
        QCOMPARE(hashOf(a, brighter), 1.0);
    }

    void smallShiftKeepsStructureAboveThreshold()
    {
        const GreyImage a = scene(200, 120, kSceneA);
        const double shifted = structureOf(a, scene(200, 120, kSceneA, 20, 3));
        QVERIFY2(shifted > 0.85, qPrintable(QString::number(shifted)));
        QVERIFY(shifted < 1.0);
    }

    void differentPicturesScoreLow()
    {
        const GreyImage a = scene(200, 120, kSceneA);
        const GreyImage b = scene(200, 120, kSceneB);
        const double s = structureOf(a, b);
        QVERIFY2(s < 0.65, qPrintable(QString::number(s)));
        QVERIFY(hashOf(a, b) < 0.9);
    }

    void flatPicturesAreComparedByBrightness()
    {
        const double same = structureOf(flat(100, 100, 90), flat(100, 100, 90));
        QCOMPARE(same, 0.8 * 1.0 + 0.2 * 1.0);
        const double black_white = structureOf(flat(100, 100, 0), flat(100, 100, 255));
        QCOMPARE(black_white, 0.8 * 0.0 + 0.2 * 1.0);
        const double flat_vs_scene = structureOf(flat(200, 120, 20), scene(200, 120, kSceneA));
        QVERIFY(flat_vs_scene < 0.65);
    }

    void structureScoreNeedsBothPictures()
    {
        const ScreenPicture a = prepareScreenPicture(scene(200, 120, kSceneA));
        const ScreenPicture noStructure = prepareScreenPicture(scene(200, 120, kSceneA), false);
        QVERIFY(!noStructure.hasStructure);
        QCOMPARE(structureScore(a, noStructure), 0.0);
        QCOMPARE(structureScore(a, ScreenPicture()), 0.0);
        QCOMPARE(screenScore(ScreenMatchMode::Hash, a, noStructure), 1.0); // the hash mode does not need it
    }

    // ---- Modes and regions ----
    void modeMappingBySubstring_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<int>("mode");
        QTest::addColumn<bool>("fromFeature");
        QTest::newRow("default") << u"ORB_HASH"_s << int(ScreenMatchMode::Hash) << false;
        QTest::newRow("hash") << u"hash"_s << int(ScreenMatchMode::Hash) << false;
        QTest::newRow("ahash") << u"AHash8"_s << int(ScreenMatchMode::Hash) << false;
        QTest::newRow("sift") << u"SIFT_LIB"_s << int(ScreenMatchMode::Structure) << true;
        QTest::newRow("sift wins over hash") << u"sift_hash"_s << int(ScreenMatchMode::Structure) << true;
        QTest::newRow("orb") << u"orb"_s << int(ScreenMatchMode::Structure) << true;
        QTest::newRow("orb_fast") << u"ORB_FAST"_s << int(ScreenMatchMode::Structure) << true;
        QTest::newRow("ours") << u"structure"_s << int(ScreenMatchMode::Structure) << false;
        QTest::newRow("unknown") << u"banana"_s << int(ScreenMatchMode::Structure) << true;   // anything else is ORB, so it is a feature pack too
        QTest::newRow("padded") << u"  Structure "_s << int(ScreenMatchMode::Structure) << false;
        QTest::newRow("blank") << u"   "_s << int(ScreenMatchMode::Hash) << false;
        QTest::newRow("missing") << QString() << int(ScreenMatchMode::Hash) << false;
    }
    void modeMappingBySubstring()
    {
        QFETCH(QString, name);
        QFETCH(int, mode);
        QFETCH(bool, fromFeature);
        bool mapped = !fromFeature;
        QCOMPARE(int(screenMatchModeFromPack(name, &mapped)), mode);
        QCOMPARE(mapped, fromFeature);
        QCOMPARE(screenMatchModeName(screenMatchModeFromPack(name)), mode == int(ScreenMatchMode::Hash) ? u"hash"_s : u"structure"_s);
    }

    void aggregationOverRegions()
    {
        const QList<double> s = {0.9, 0.5, 0.7};
        QCOMPARE(aggregateScores(s, ScreenAgg::Any), 0.9);
        QCOMPARE(aggregateScores(s, ScreenAgg::All), 0.5);
        QVERIFY(qFuzzyCompare(aggregateScores(s, ScreenAgg::Mean), 0.7));
        QCOMPARE(aggregateScores({}, ScreenAgg::Any), 0.0);
        QCOMPARE(aggregateScores({}, ScreenAgg::All), 0.0);
        QCOMPARE(screenAggFromName(u"min"_s), ScreenAgg::All);
        QCOMPARE(screenAggFromName(u"ALL"_s), ScreenAgg::All);
        QCOMPARE(screenAggFromName(u"mean"_s), ScreenAgg::Mean);
        QCOMPARE(screenAggFromName(u"any"_s), ScreenAgg::Any);
        QCOMPARE(screenAggFromName(u"weighted"_s), ScreenAgg::Any);
        QCOMPARE(screenAggName(ScreenAgg::All), u"all"_s);
    }

    void anyRegionIsTheDefaultRule()
    {
        ScreenMatcher any(config(1));
        QVERIFY(any.tick(0, {0.2, 0.9, 0.1}).detected);
        ScreenMatchConfig all = config(1);
        all.agg = ScreenAgg::All;
        ScreenMatcher m(all);
        QVERIFY(!m.tick(0, {0.2, 0.9, 0.1}).detected);
        QVERIFY(m.tick(60, {0.8, 0.9, 0.76}).detected);
        ScreenMatchConfig mean = config(1);
        mean.agg = ScreenAgg::Mean;
        ScreenMatcher mm(mean);
        QVERIFY(mm.tick(0, {0.5, 1.0}).detected); // 0.75 exactly
        QVERIFY(!mm.tick(60, {0.5, 0.9}).detected);
    }

    void thresholdIsInclusive()
    {
        ScreenMatcher m(config(1));
        QVERIFY(m.tick(0, one(48.0 / 64.0)).detected); // 0.75 = 48/64
        ScreenMatcher n(config(1));
        QVERIFY(!n.tick(0, one(47.0 / 64.0)).detected);
    }

    // ---- Activation, grace and cooldown ----
    void activationNeedsAStreakAndAnyMissBreaksIt()
    {
        Script s(config(3));
        s.run(u"11011"_s);
        QCOMPARE(s.trace, u"....."_s);
        s.run(u"1"_s);
        QCOMPARE(s.trace, u".....A"_s);
        QVERIFY(s.matcher.active());
    }

    void defaultsApplyWhenAPackOmitsThem()
    {
        const ScreenMatchConfig d;
        QCOMPARE(d.threshold, 0.65);
        QCOMPARE(d.cooldownMs, qint64(800));
        QCOMPARE(d.graceMs, qint64(250));
        QCOMPARE(d.startFrames, 1);
        QCOMPARE(d.cooldownFrom, ScreenCooldownFrom::Activate);
        QVERIFY(d.stopOnMismatch && d.agg == ScreenAgg::Any && !d.sequencer);
        ScreenMatcher m;
        QVERIFY(!m.tick(0, one(0.64)).detected);
        QVERIFY(m.tick(60, one(0.65)).event == ScreenEvent::Activated); // one tick is enough by default
        // out-of-range settings are brought back in
        ScreenMatchConfig bad;
        bad.startFrames = -4;
        bad.seqLen = 0;
        bad.threshold = 7;
        bad.graceMs = -1;
        bad.cooldownMs = -1;
        const ScreenMatcher c(bad);
        QCOMPARE(c.config().startFrames, 1);
        QCOMPARE(c.config().seqLen, 1);
        QCOMPARE(c.config().threshold, 1.0);
        QCOMPARE(c.config().graceMs, qint64(0));
        QCOMPARE(c.config().cooldownMs, qint64(0));
    }

    void holdLastsWhileMatchingAndEndsAfterTheGrace()
    {
        Script s(config(1, 250, 0));
        s.run(u"1"_s);       // t=0 activated
        QVERIFY(s.matcher.active());
        s.run(u"111"_s);     // hits at 60, 120, 180: held
        QVERIFY(s.matcher.active());
        s.run(u"0000"_s);    // misses at 240 to 420: 60 to 240 ms since the last hit
        QVERIFY(s.matcher.active());
        s.run(u"0"_s);       // 480: 300 ms since the last hit, more than 250
        QVERIFY(!s.matcher.active());
        QCOMPARE(s.trace, u"A"_s + QString(7, u'.') + u"D"_s);
    }

    void graceIsStrictlyMoreThan()
    {
        ScreenMatcher m(config(1, 250, 0));
        QCOMPARE(m.tick(0, one(1)).event, ScreenEvent::Activated);
        QCOMPARE(m.tick(250, one(0)).event, ScreenEvent::None);     // exactly the grace: still held
        QCOMPARE(m.tick(251, one(0)).event, ScreenEvent::Deactivated);
    }

    void aDetectionInsideTheGraceRefreshesIt()
    {
        ScreenMatcher m(config(1, 250, 0));
        m.tick(0, one(1));
        m.tick(200, one(0));
        m.tick(240, one(1));
        QVERIFY(m.tick(480, one(0)).active); // 240 after the refresh
        QVERIFY(!m.tick(500, one(0)).active);
    }

    void cooldownFromActivateBlocksReactivation()
    {
        Script s(config(1, 100, 800));
        s.period = 100;
        s.run(u"1"_s);       // t=0 activated, cooldown until 800
        s.run(u"00"_s);      // t=200: 200 ms since the hit, more than the grace: ended
        s.run(u"1111"_s);    // t=300 to 600, inside the cooldown
        QVERIFY(!s.matcher.active());
        s.run(u"1"_s);       // t=700, still cooling
        QVERIFY(!s.matcher.active());
        s.run(u"1"_s);       // t=800, free
        QCOMPARE(s.trace, u"A.D.....A"_s);
    }

    void cooldownFromDeactivateStartsAtTheEnd()
    {
        ScreenMatchConfig c = config(1, 100, 500);
        c.cooldownFrom = ScreenCooldownFrom::Deactivate;
        ScreenMatcher m(c);
        QCOMPARE(m.tick(0, one(1)).event, ScreenEvent::Activated);
        QVERIFY(m.tick(50, one(1)).cooling);                     // reported as cooling while held: the cooldown starts at the end
        QCOMPARE(m.tick(300, one(0)).event, ScreenEvent::Deactivated); // cooldown until 800
        QCOMPARE(m.tick(700, one(1)).event, ScreenEvent::None);
        QVERIFY(m.tick(700, one(1)).cooling);
        QCOMPARE(m.tick(800, one(1)).event, ScreenEvent::Activated);
    }

    void aTickInsideTheCooldownResetsTheStreak()
    {
        ScreenMatchConfig c = config(3, 100, 400);
        ScreenMatcher m(c);
        m.tick(0, one(1));
        m.tick(60, one(1));
        QCOMPARE(m.tick(120, one(1)).event, ScreenEvent::Activated); // cooldown until 520
        m.tick(180, one(0));
        m.tick(300, one(0));                                         // ended
        QVERIFY(!m.active());
        // two hits in the cooldown, the third outside: the streak restarts, so it does not activate
        m.tick(480, one(1));
        m.tick(500, one(1));
        QCOMPARE(m.tick(540, one(1)).event, ScreenEvent::None);
        m.tick(600, one(1));
        QCOMPARE(m.tick(660, one(1)).event, ScreenEvent::Activated);
    }

    void withoutStopOnMismatchTheTriggerEndsOnlyWhenTold()
    {
        ScreenMatchConfig c = config(1, 100, 300);
        c.stopOnMismatch = false;
        ScreenMatcher m(c);
        QCOMPARE(m.tick(0, one(1)).event, ScreenEvent::Activated);
        for (qint64 t = 60; t < 5000; t += 60)
            QVERIFY(m.tick(t, one(0)).active);
        QVERIFY(m.end(5000));
        QVERIFY(!m.active());
        QVERIFY(!m.end(5001));
        QCOMPARE(m.tick(5100, one(1)).event, ScreenEvent::Activated); // the activation cooldown (300) is long over
    }

    void resetClearsEverything()
    {
        ScreenMatcher m(config(1, 100, 10000));
        m.tick(0, one(1));
        m.tick(500, one(0));
        QVERIFY(m.cooling(600));
        m.reset();
        QVERIFY(!m.active() && !m.cooling(600));
        QCOMPARE(m.tick(600, one(1)).event, ScreenEvent::Activated);
    }

    // ---- Sequencer ----
    void sequencerWalksTheStepsInOrder()
    {
        ScreenMatchConfig c = config(2, 250, 500);
        c.sequencer = true;
        c.seqLen = 3;
        Script s(c);
        QCOMPARE(s.matcher.wantedStep(), 0);
        s.step(1);
        QCOMPARE(s.matcher.wantedStep(), 0);  // one of two
        s.step(1);
        QCOMPARE(s.matcher.wantedStep(), 1);  // step 0 passed
        s.step(1);
        s.step(1);
        QCOMPARE(s.matcher.wantedStep(), 2);
        QVERIFY(!s.matcher.active());
        s.step(1);
        const ScreenTickResult r = s.step(1);
        QCOMPARE(r.event, ScreenEvent::Activated);
        QCOMPARE(s.trace, u".....A"_s);
        QCOMPARE(r.step, 2); // holds on the last reference
        QVERIFY(r.active);
    }

    void aFailingTickResetsTheWalkToStepZero()
    {
        ScreenMatchConfig c = config(1);
        c.sequencer = true;
        c.seqLen = 3;
        ScreenMatcher m(c);
        m.tick(0, one(1));
        QCOMPARE(m.wantedStep(), 1);
        m.tick(60, one(1));
        QCOMPARE(m.wantedStep(), 2);
        m.tick(120, one(0));
        QCOMPARE(m.wantedStep(), 0);
        m.tick(180, one(1));
        m.tick(240, one(1));
        QCOMPARE(m.tick(300, one(1)).event, ScreenEvent::Activated);
    }

    void aFailureInsideAStepStreakRestartsTheStreak()
    {
        ScreenMatchConfig c = config(2);
        c.sequencer = true;
        c.seqLen = 2;
        ScreenMatcher m(c);
        m.tick(0, one(1));
        m.tick(60, one(0));
        m.tick(120, one(1));
        QCOMPARE(m.wantedStep(), 0); // the earlier hit does not count
        m.tick(180, one(1));
        QCOMPARE(m.wantedStep(), 1);
    }

    void sequencerHoldFollowsTheLastReferenceAndGrace()
    {
        ScreenMatchConfig c = config(1, 200, 0);
        c.sequencer = true;
        c.seqLen = 2;
        ScreenMatcher m(c);
        m.tick(0, one(1));
        QCOMPARE(m.tick(60, one(1)).event, ScreenEvent::Activated);
        QVERIFY(m.tick(120, one(1)).active);
        QVERIFY(m.tick(300, one(0)).active);
        QCOMPARE(m.tick(321, one(0)).event, ScreenEvent::Deactivated);
        QCOMPARE(m.wantedStep(), 0); // starts again from the first reference
    }

    void loopDelayBlocksANewWalkAfterACompletedRun()
    {
        ScreenMatchConfig c = config(1, 100, 0);
        c.sequencer = true;
        c.seqLen = 2;
        c.loopDelayMs = 2000;
        ScreenMatcher m(c);
        m.tick(0, one(1));
        QCOMPARE(m.tick(60, one(1)).event, ScreenEvent::Activated); // completed at 60, blocked until 2060
        m.tick(200, one(0));
        QVERIFY(!m.active());
        for (qint64 t = 260; t < 2060; t += 60)
            QCOMPARE(m.tick(t, one(1)).event, ScreenEvent::None);
        QCOMPARE(m.wantedStep(), 0);
        m.tick(2060, one(1));
        QCOMPARE(m.tick(2120, one(1)).event, ScreenEvent::Activated);
    }

    void aLongSequenceWithOneStepIsTheAnchor()
    {
        ScreenMatchConfig c = config(2);
        c.sequencer = true;
        c.seqLen = 1;
        ScreenMatcher m(c);
        m.tick(0, one(1));
        QCOMPARE(m.tick(60, one(1)).event, ScreenEvent::Activated);
    }

    // ---- Properties ----
    void randomStreamsKeepTheCooldownAndTheGrace()
    {
        QRandomGenerator rng(20261023);
        for (int run = 0; run < 400; ++run) {
            ScreenMatchConfig c;
            c.threshold = 0.5;
            c.startFrames = 1 + int(rng.bounded(4));
            c.graceMs = qint64(rng.bounded(400));
            c.cooldownMs = qint64(rng.bounded(1500));
            c.cooldownFrom = rng.bounded(2) ? ScreenCooldownFrom::Activate : ScreenCooldownFrom::Deactivate;
            c.stopOnMismatch = rng.bounded(5) != 0;
            c.sequencer = rng.bounded(3) == 0;
            c.seqLen = 1 + int(rng.bounded(3));
            c.loopDelayMs = c.sequencer ? qint64(rng.bounded(1000)) : 0;
            c.agg = ScreenAgg(rng.bounded(3));
            ScreenMatcher m(c);

            qint64 now = 0;
            qint64 lastHit = 0;
            qint64 cooldownEnd = -1; // a mirror of the rule, kept by the test
            qint64 blockedEnd = -1;
            bool held = false;
            const double hitRate = 0.1 + 0.8 * rng.generateDouble(); // streams from rare to constant hits
            for (int i = 0; i < 400; ++i) {
                now += 1 + qint64(rng.bounded(120));
                const bool hit = rng.generateDouble() < hitRate;
                const QList<double> scores = {hit ? 0.9 : 0.1, hit ? 0.8 : 0.0};
                const ScreenTickResult r = m.tick(now, scores);

                if (r.event == ScreenEvent::Activated) {
                    QVERIFY2(!held, "activated while already active");
                    QVERIFY2(now >= cooldownEnd, "activated inside the cooldown");
                    QVERIFY2(now >= blockedEnd, "activated inside the loop delay");
                    held = true;
                    lastHit = now;
                    cooldownEnd = c.cooldownFrom == ScreenCooldownFrom::Activate ? now + c.cooldownMs : std::numeric_limits<qint64>::max();
                    blockedEnd = c.sequencer ? now + c.loopDelayMs : -1;
                } else if (r.event == ScreenEvent::Deactivated) {
                    QVERIFY2(held, "deactivated while not active");
                    held = false;
                    if (c.cooldownFrom == ScreenCooldownFrom::Deactivate)
                        cooldownEnd = now + c.cooldownMs;
                } else if (held && r.detected) {
                    lastHit = now;
                }
                QCOMPARE(r.active, held);
                QCOMPARE(m.active(), held);
                if (held && c.stopOnMismatch)
                    QVERIFY2(now - lastHit <= c.graceMs, "still active past the grace");
                if (held)
                    QCOMPARE(r.step, c.sequencer ? c.seqLen - 1 : 0);
            }
        }
    }

    void randomStreamsWithoutStopOnMismatchEndOnlyByEnd()
    {
        QRandomGenerator rng(7);
        ScreenMatchConfig c = config(1, 100, 200);
        c.stopOnMismatch = false;
        ScreenMatcher m(c);
        qint64 now = 0;
        bool active = false;
        for (int i = 0; i < 2000; ++i) {
            now += 1 + qint64(rng.bounded(300));
            const ScreenTickResult r = m.tick(now, one(rng.bounded(2) ? 1.0 : 0.0));
            QVERIFY(r.event != ScreenEvent::Deactivated);
            if (r.event == ScreenEvent::Activated)
                active = true;
            QCOMPARE(m.active(), active);
            if (active && rng.bounded(10) == 0) {
                QVERIFY(m.end(now));
                active = false;
            }
        }
    }

    // ---- The fused path against the reference path ----
    void fusedPathAgreesWithTheReferencePath()
    {
        const QSize sizes[] = {{1920, 1080}, {1280, 720}, {997, 613}, {640, 480}, {400, 300}, {360, 200}, {130, 90}, {33, 70}, {20, 15}};
        int pictures = 0, bitDiffs = 0, maxBitDiff = 0, worstLevels = 0;
        double maxStructureDelta = 0.0, maxHashDelta = 0.0, maxSteppedDelta = 0.0;
        int steppedBits = 0;
        quint32 seed = 100;
        for (const QSize &size : sizes) {
            for (int rep = 0; rep < 4; ++rep, ++seed) {
                const QByteArray a = naturalBgra(size.width(), size.height(), seed, 0);
                // B: the same place with a change (light and a moved shape) every other time, a different picture otherwise
                const QByteArray b = naturalBgra(size.width(), size.height(), rep % 2 ? seed + 5000 : seed, rep % 2 ? 0 : 1);
                ScreenPicture refPic[2], fusedPic[2], steppedPic[2];
                const QByteArray *src[2] = {&a, &b};
                for (int i = 0; i < 2; ++i) {
                    const auto *d = reinterpret_cast<const uchar *>(src[i]->constData());
                    const GreyImage grey = greyFromPixels(d, qsizetype(size.width() * 4) * (size.height()), size.width(), size.height(), size.width() * 4, ScreenPixelFormat::Bgra32);
                    refPic[i] = prepareScreenPicture(grey);
                    fusedPic[i] = prepareScreenPictureFromPixels(d, qsizetype(size.width() * 4) * (size.height()), size.width(), size.height(), size.width() * 4, ScreenPixelFormat::Bgra32);
                    steppedPic[i] = prepareScreenPictureFromPixels(d, qsizetype(size.width() * 4) * (size.height()), size.width(), size.height(), size.width() * 4, ScreenPixelFormat::Bgra32, true, screenRowStepFor(size.height()));
                    QVERIFY(fusedPic[i].valid && fusedPic[i].hasStructure);
                    // bits that differ must belong to cells within a few grey levels of the mean
                    const GreyImage cells = resizeArea(fitLongerSide(grey), kScreenHashSide, kScreenHashSide);
                    double mean = 0.0;
                    for (char c : cells.pixels)
                        mean += uchar(c);
                    mean /= 64.0;
                    const quint64 diff = refPic[i].hash ^ fusedPic[i].hash;
                    const int n = std::popcount(diff);
                    bitDiffs += n;
                    maxBitDiff = std::max(maxBitDiff, n);
                    for (int bit = 0; bit < 64; ++bit)
                        if (diff >> bit & 1)
                            worstLevels = std::max(worstLevels, int(std::ceil(std::abs(uchar(cells.pixels[bit]) - mean))));
                    ++pictures;
                }
                maxStructureDelta = std::max(maxStructureDelta, std::abs(structureScore(refPic[0], refPic[1]) - structureScore(fusedPic[0], fusedPic[1])));
                maxHashDelta = std::max(maxHashDelta, std::abs(hashScore(refPic[0], refPic[1]) - hashScore(fusedPic[0], fusedPic[1])));
                maxSteppedDelta = std::max(maxSteppedDelta, std::abs(structureScore(refPic[0], refPic[1]) - structureScore(steppedPic[0], steppedPic[1])));
                steppedBits += std::popcount(refPic[0].hash ^ steppedPic[0].hash) + std::popcount(refPic[1].hash ^ steppedPic[1].hash);
                QVERIFY(structureScore(fusedPic[0], fusedPic[0]) > 0.999);
            }
        }
        qInfo().noquote() << u"S1 agreement: %1 pictures, hash bits differing: mean %2, max %3, farthest from the mean %4 grey levels; max structure delta %5, max hash delta %6"_s
                                 .arg(pictures)
                                 .arg(double(bitDiffs) / pictures, 0, 'f', 3)
                                 .arg(maxBitDiff)
                                 .arg(worstLevels)
                                 .arg(maxStructureDelta, 0, 'f', 4)
                                 .arg(maxHashDelta, 0, 'f', 4);
        qInfo().noquote() << u"S1 agreement with screenRowStepFor rows: max structure delta %1, differing hash bits over all pictures %2"_s.arg(maxSteppedDelta, 0, 'f', 4).arg(steppedBits);
        QVERIFY2(maxSteppedDelta <= 0.01, qPrintable(QString::number(maxSteppedDelta)));
        QVERIFY2(maxStructureDelta <= 0.005, qPrintable(QString::number(maxStructureDelta)));
        QVERIFY2(worstLevels <= 1, qPrintable(QString::number(worstLevels)));
    }

    void fusedPathOnTheSyntheticScenes()
    {
        const GreyImage g = scene(200, 120, kSceneA);
        QByteArray px(200 * 120 * 4, 0);
        for (int i = 0; i < 200 * 120; ++i)
            px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = g.pixels[i];
        const auto *d = reinterpret_cast<const uchar *>(px.constData());
        const ScreenPicture fused = prepareScreenPictureFromPixels(d, qsizetype(800) * (120), 200, 120, 800, ScreenPixelFormat::Bgra32);
        QCOMPARE(fused.hash, prepareScreenPicture(g).hash);
        QVERIFY(structureScore(fused, prepareScreenPicture(g)) > 0.98);
        // golden bits through the fused path too
        auto bgra = [](const GreyImage &i) {
            QByteArray o(i.width * i.height * 4, 0);
            for (int k = 0; k < i.width * i.height; ++k)
                o[k * 4] = o[k * 4 + 1] = o[k * 4 + 2] = i.pixels[k];
            return o;
        };
        const QByteArray h = bgra(horizontalGradient(64, 64)), v = bgra(verticalGradient(64, 64)), c = bgra(checker(8)), f = bgra(flat(300, 200, 90));
        const auto u = [](const QByteArray &a) { return reinterpret_cast<const uchar *>(a.constData()); };
        QCOMPARE(prepareScreenPictureFromPixels(u(h), qsizetype(256) * (64), 64, 64, 256, ScreenPixelFormat::Bgra32).hash, quint64(0xF0F0F0F0F0F0F0F0));
        QCOMPARE(prepareScreenPictureFromPixels(u(v), qsizetype(256) * (64), 64, 64, 256, ScreenPixelFormat::Bgra32).hash, quint64(0xFFFFFFFF00000000));
        QCOMPARE(prepareScreenPictureFromPixels(u(c), qsizetype(256) * (64), 64, 64, 256, ScreenPixelFormat::Bgra32).hash, quint64(0x55AA55AA55AA55AA));
        QCOMPARE(prepareScreenPictureFromPixels(u(f), qsizetype(1200) * (200), 300, 200, 1200, ScreenPixelFormat::Bgra32).hash, quint64(0));
        // refusals
        QVERIFY(!prepareScreenPictureFromPixels(nullptr, qsizetype(40) * (10), 10, 10, 40, ScreenPixelFormat::Bgra32).valid);
        QVERIFY(!prepareScreenPictureFromPixels(u(f), qsizetype(100) * (200), 300, 200, 100, ScreenPixelFormat::Bgra32).valid);
        // formats and row sampling
        const ScreenPicture grey8 = prepareScreenPictureFromPixels(reinterpret_cast<const uchar *>(g.pixels.constData()), qsizetype(200) * (120), 200, 120, 200, ScreenPixelFormat::Grey8);
        QCOMPARE(grey8.hash, fused.hash);
        const ScreenPicture sampled = prepareScreenPictureFromPixels(d, qsizetype(800) * (120), 200, 120, 800, ScreenPixelFormat::Bgra32, true, 2);
        QVERIFY(structureScore(fused, sampled) > 0.95);
    }

    // ---- Flat pictures through the fused path ----
    void flatPicturesThroughTheFusedPath()
    {
        const QSize sizes[] = {{1920, 1080}, {997, 613}, {33, 70}, {400, 300}, {64, 33}};
        const int values[] = {0, 90, 255};
        for (const QSize &size : sizes) {
            ScreenPicture pics[3];
            for (int i = 0; i < 3; ++i) {
                const QByteArray px(size.width() * size.height() * 4, char(values[i]));
                pics[i] = prepareScreenPictureFromPixels(reinterpret_cast<const uchar *>(px.constData()), px.size(), size.width(), size.height(), size.width() * 4, ScreenPixelFormat::Bgra32);
                QVERIFY(pics[i].valid);
                QCOMPARE(pics[i].norm, 0.0f); // exactly flat, whatever the size
                QCOMPARE(pics[i].hash, quint64(0));
            }
            for (int i = 0; i < 3; ++i) {
                QCOMPARE(structureScore(pics[i], pics[i]), 1.0); // equal flat pictures match
                for (int j = 0; j < 3; ++j) {
                    if (i == j)
                        continue;
                    const double expected = 0.8 * (1.0 - std::abs(values[i] - values[j]) / 255.0) + 0.2;
                    QVERIFY2(qAbs(structureScore(pics[i], pics[j]) - expected) < 1e-4, qPrintable(QStringLiteral("%1x%2 %3 vs %4").arg(size.width()).arg(size.height()).arg(values[i]).arg(values[j])));
                }
            }
            QVERIFY(structureScore(pics[2], pics[1]) < 0.65); // white against mid grey is not a match
            QVERIFY(structureScore(pics[0], pics[2]) < 0.65);
            // flat against a scene
            const QByteArray scenePx = naturalBgra(size.width(), size.height(), 3, 0);
            const ScreenPicture sc = prepareScreenPictureFromPixels(reinterpret_cast<const uchar *>(scenePx.constData()), scenePx.size(), size.width(), size.height(), size.width() * 4, ScreenPixelFormat::Bgra32);
            QVERIFY(structureScore(pics[1], sc) < 0.65);
        }
    }

    // ---- Refusals ----
    void picturesFromPixelsRefuseBadInput()
    {
        const QByteArray px(64 * 8 * 4, char(7));
        const auto *d = reinterpret_cast<const uchar *>(px.constData());
        const qsizetype n = px.size();
        const auto f = ScreenPixelFormat::Bgra32;
        auto refused = [&](const uchar *data, qsizetype size, int w, int h, qsizetype stride, ScreenPixelFormat fmt) {
            return greyFromPixels(data, size, w, h, stride, fmt).isEmpty() && greyScaledFromPixels(data, size, w, h, stride, fmt).isEmpty()
                && !prepareScreenPictureFromPixels(data, size, w, h, stride, fmt).valid;
        };
        QVERIFY(!refused(d, n, 64, 8, 256, f));                       // the good one is accepted
        QVERIFY(refused(nullptr, n, 64, 8, 256, f));
        QVERIFY(refused(d, n, 0, 8, 256, f));
        QVERIFY(refused(d, n, 64, 0, 256, f));
        QVERIFY(refused(d, n, -1, 8, 256, f));
        QVERIFY(refused(d, n, 64, 8, 255, f));                        // stride under a row
        QVERIFY(refused(d, n - 1, 64, 8, 256, f));                    // one byte short
        QVERIFY(refused(d, n, 64, 8, 260, f));                        // a stride that runs past the buffer
        QVERIFY(refused(d, 0, 64, 8, 256, f));
        QVERIFY(refused(d, -5, 64, 8, 256, f));
        QVERIFY(refused(d, n, 64, 8, qsizetype(1) << 40, f));         // absurd stride
        QVERIFY(refused(d, n, 64, 8, -256, f));
        QVERIFY(refused(d, n, 1 << 30, 8, 256, f));                   // absurd sizes: refused before any allocation
        QVERIFY(refused(d, n, 8, 1 << 30, 256, f));
        QVERIFY(refused(d, n, 100000, 100000, qsizetype(400000), f));
        QVERIFY(refused(d, n, 16385, 1, 70000, f));                   // over the cap on one side
        QVERIFY(refused(d, n, 1, 16385, 4, f));
        QVERIFY(refused(d, n, 64, 8, 256, ScreenPixelFormat(99)));    // unknown formats
        QVERIFY(refused(d, n, 64, 8, 256, ScreenPixelFormat(-1)));
        // a refused picture is invalid and scores 0 against anything
        const ScreenPicture good = prepareScreenPictureFromPixels(d, n, 64, 8, 256, f);
        const ScreenPicture bad = prepareScreenPictureFromPixels(d, n, 64, 8, 255, f);
        QVERIFY(good.valid && !bad.valid);
        QCOMPARE(screenScore(ScreenMatchMode::Hash, good, bad), 0.0);
        QCOMPARE(screenScore(ScreenMatchMode::Structure, bad, good), 0.0);
        QCOMPARE(screenScore(ScreenMatchMode::Structure, bad, bad), 0.0);
        // the last row may be short of its padding: stride * (h - 1) + row is enough
        QByteArray padded(260 * 7 + 256, char(9));
        QVERIFY(prepareScreenPictureFromPixels(reinterpret_cast<const uchar *>(padded.constData()), padded.size(), 64, 8, 260, f).valid);
        QVERIFY(!prepareScreenPictureFromPixels(reinterpret_cast<const uchar *>(padded.constData()), padded.size() - 1, 64, 8, 260, f).valid);
    }

    void greyImagesOverTheCapsAreRefused()
    {
        GreyImage huge;
        huge.width = kScreenMatchMaxDimension + 1;
        huge.height = 1;
        huge.pixels = QByteArray(huge.width, char(1));
        QVERIFY(!prepareScreenPicture(huge).valid);
        QVERIFY(fitLongerSide(huge).isEmpty());
        QVERIFY(resizeArea(huge, 8, 8).isEmpty());
        QVERIFY(resizeArea(flat(8, 8, 1), kScreenMatchMaxDimension + 1, 8).isEmpty());
        GreyImage lying; // sizes that do not match the bytes
        lying.width = 100;
        lying.height = 100;
        lying.pixels = QByteArray(10, char(1));
        QVERIFY(!prepareScreenPicture(lying).valid);
        QVERIFY(resizeArea(lying, 8, 8).isEmpty());
    }

    void tinyPicturesWork_data()
    {
        QTest::addColumn<int>("w");
        QTest::addColumn<int>("h");
        for (const QSize &sz : {QSize(1, 1), QSize(1, 50), QSize(50, 1), QSize(3, 3), QSize(7, 5), QSize(31, 7), QSize(7, 31), QSize(8, 8), QSize(31, 31), QSize(33, 33), QSize(1, 300), QSize(300, 1)})
            QTest::newRow(qPrintable(QStringLiteral("%1x%2").arg(sz.width()).arg(sz.height()))) << sz.width() << sz.height();
    }
    void tinyPicturesWork()
    {
        QFETCH(int, w);
        QFETCH(int, h);
        const QByteArray a = naturalBgra(w, h, 21, 0);
        const QByteArray b = naturalBgra(w, h, 22, 0);
        auto prep = [&](const QByteArray &px, bool fused) {
            const auto *d = reinterpret_cast<const uchar *>(px.constData());
            return fused ? prepareScreenPictureFromPixels(d, px.size(), w, h, w * 4, ScreenPixelFormat::Bgra32)
                         : prepareScreenPicture(greyFromPixels(d, px.size(), w, h, w * 4, ScreenPixelFormat::Bgra32));
        };
        const ScreenPicture fa = prep(a, true), fb = prep(b, true), ra = prep(a, false), rb = prep(b, false);
        QVERIFY(fa.valid && fb.valid && ra.valid && rb.valid);
        QVERIFY(fa.hasStructure && ra.hasStructure);
        QCOMPARE(hashScore(fa, fa), 1.0);
        QVERIFY(qAbs(structureScore(fa, fa) - 1.0) < 1e-6 || fa.norm <= 1e-3f);
        QVERIFY(qAbs(structureScore(fa, fb) - structureScore(ra, rb)) < 0.05);
        const GreyImage g = greyScaledFromPixels(reinterpret_cast<const uchar *>(a.constData()), a.size(), w, h, w * 4, ScreenPixelFormat::Bgra32);
        QVERIFY(!g.isEmpty());
        QCOMPARE(g.width, w);
        QCOMPARE(g.height, h);
    }

    // ---- Clock, durations, empty regions ----
    void aClockThatGoesBackIsTakenAsTheLastTime()
    {
        ScreenMatcher m(config(1, 250, 500));
        QCOMPARE(m.tick(1000, one(1)).event, ScreenEvent::Activated); // cooldown until 1500
        m.tick(1100, one(0));
        QVERIFY(m.active());
        // the wall clock steps back an hour: the hold still ends 250 ms after the last detection, not an hour later
        QVERIFY(m.tick(1100 - 3600000, one(0)).active);
        QVERIFY(m.tick(1200, one(0)).active); // 200 ms since the hit at 1000
        QCOMPARE(m.tick(1251, one(0)).event, ScreenEvent::Deactivated);
        // the cooldown also ends on time, because time never ran back
        QVERIFY(m.tick(1300 - 3600000, one(1)).cooling);
        QVERIFY(!m.active());
        QVERIFY(m.tick(1500, one(1)).event == ScreenEvent::Activated);
        // a tick at an earlier time than the previous one is the same as a tick at that time
        ScreenMatcher a(config(2, 250, 0)), b(config(2, 250, 0));
        a.tick(500, one(1));
        b.tick(500, one(1));
        a.tick(100, one(1));
        b.tick(500, one(1));
        QCOMPARE(a.active(), b.active());
        QCOMPARE(a.active(), true);
        // end() with a clock that went back
        QVERIFY(a.end(0));
    }

    void hugeDurationsAreHeld()
    {
        ScreenMatchConfig c = config(1, std::numeric_limits<qint64>::max(), std::numeric_limits<qint64>::max());
        c.loopDelayMs = std::numeric_limits<qint64>::max();
        ScreenMatcher m(c);
        QCOMPARE(m.config().graceMs, kScreenMaxDurationMs);
        QCOMPARE(m.config().cooldownMs, kScreenMaxDurationMs);
        QCOMPARE(m.config().loopDelayMs, kScreenMaxDurationMs);
        QCOMPARE(m.tick(std::numeric_limits<qint64>::max() - 10, one(1)).event, ScreenEvent::Activated); // no overflow
        QVERIFY(m.tick(std::numeric_limits<qint64>::max(), one(0)).active);
        ScreenMatchConfig n = config(1, 100, 500);
        n.cooldownFrom = ScreenCooldownFrom::Deactivate;
        ScreenMatcher d(n);
        d.tick(0, one(1));
        QCOMPARE(d.tick(std::numeric_limits<qint64>::max() - 1, one(0)).event, ScreenEvent::Deactivated);
        QVERIFY(d.tick(std::numeric_limits<qint64>::max() - 1, one(1)).cooling); // the cooldown saturates instead of wrapping to the past
        QVERIFY(!d.active());
    }

    void noRegionsIsNeverADetection()
    {
        ScreenMatchConfig c = config(1);
        c.threshold = 0.0;
        ScreenMatcher m(c);
        QVERIFY(!m.tick(0, {}).detected);
        QVERIFY(!m.active());
        QVERIFY(m.tick(60, one(0.0)).detected); // a real region at threshold 0 does pass
        QVERIFY(m.active());
    }

    void aLongRunOfHitsAlwaysActivates()
    {
        QRandomGenerator rng(99);
        for (int run = 0; run < 100; ++run) {
            ScreenMatchConfig c = config(1 + int(rng.bounded(5)), 250, qint64(rng.bounded(1000)));
            c.sequencer = rng.bounded(2);
            c.seqLen = 1 + int(rng.bounded(4));
            ScreenMatcher m(c);
            bool activated = false;
            for (int i = 0; i < 100 && !activated; ++i)
                activated = m.tick(i * 60, one(1.0)).event == ScreenEvent::Activated;
            QVERIFY2(activated, "an unbroken run of hits must activate");
        }
    }

    // ---- Cost (S1) ----
    // A benchmark: it prints, it never fails. Off in ctest; run with MURASAKI_BENCH=1.
    void costOfOneTick()
    {
        if (qEnvironmentVariableIsEmpty("MURASAKI_BENCH"))
            QSKIP("benchmark; set MURASAKI_BENCH=1 to run it");
        // A tick: crop of each region (BGRA, two sizes) -> grey and fit to 360 in one pass -> hash + structure -> score against its reference -> aggregate.
        for (const QSize size : {QSize(1280, 720), QSize(400, 300)}) {
            const int w = size.width(), h = size.height();
            const QByteArray bgra = naturalBgra(w, h, 11, 0);
            const auto *d = reinterpret_cast<const uchar *>(bgra.constData());
            const ScreenPicture reference = prepareScreenPictureFromPixels(d, qsizetype(w * 4) * (h), w, h, w * 4, ScreenPixelFormat::Bgra32);
            for (const char *path : {"reference", "fused", "fused, screenRowStepFor rows"}) {
                for (const int regions : {1, 8, 16}) {
                    const int ticks = 40;
                    double sink = 0.0;
                    QElapsedTimer timer;
                    timer.start();
                    for (int t = 0; t < ticks; ++t) {
                        QList<double> scores;
                        for (int r = 0; r < regions; ++r) {
                            ScreenPicture live;
                            if (path[0] == 'r')
                                live = prepareScreenPicture(greyScaledFromPixels(d, qsizetype(w * 4) * (h), w, h, w * 4, ScreenPixelFormat::Bgra32));
                            else
                                live = prepareScreenPictureFromPixels(d, qsizetype(w * 4) * (h), w, h, w * 4, ScreenPixelFormat::Bgra32, true, path[5] == ',' ? screenRowStepFor(h) : 1);
                            scores.append(structureScore(reference, live));
                        }
                        sink += aggregateScores(scores, ScreenAgg::Any);
                    }
                    const double perTickMs = timer.nsecsElapsed() / 1e6 / ticks;
                    qInfo().noquote() << u"S1 cost: %1 regions of %2x%3 BGRA, %4 path: %5 ms per tick (%6 ms per region)"_s.arg(regions).arg(w).arg(h).arg(QString::fromLatin1(path))
                                             .arg(perTickMs, 0, 'f', 3).arg(perTickMs / regions, 0, 'f', 3);
                    QVERIFY(sink > 0.0);
                }
            }
        }
    }
};

QTEST_GUILESS_MAIN(TstScreenMatch)
#include "tst_screenmatch.moc"
