// The picture's mapping, the hit mask and its message (src/puppet/view.*, src/bridge/puppetview.h).
#include "bridge/puppetview.h"
#include "puppet/view.h"
#include "puppetfake.h"

#include <QTest>

#include <QElapsedTimer>

#include <cmath>

using namespace murasaki::puppet;
using namespace puppetfake;
namespace bridge = murasaki::bridge;

class TestPuppetView : public QObject
{
    Q_OBJECT
private slots:
    void fitCentersAndKeepsTheAspect();
    void fitHandlesDegenerateBounds();
    void maskOfAQuad();
    void maskOfATriangleAndThinThings();
    void maskIgnoresInvisibleAndGroups();
    void maskOffTheEdges();
    void largePicturesGetLargerCells();
    void maskWorkIsBounded();
    void maskSurvivesHugeNumbers();
    void frameCostCountsClippedAreaMasksAndGroups();
    void aRealisticModelIsWithinTheBudget();
    void frameCostWorkIsBounded();
    void aMaskNotSentIsOfferedAgain();
    void trackerSendsWhatMatters();
    void messageRoundTrip();
    void messageValidation_data();
    void messageValidation();
};

static Frame frameOf(FakeModel &m)
{
    Frame f;
    ParameterSet p = m.parameters();
    m.evaluate(p, m.parts(), f);
    return f;
}

void TestPuppetView::fitCentersAndKeepsTheAspect()
{
    // a 100 x 50 model in a 200 x 200 picture: scale 2, centred vertically
    const ViewTransform v = fitBounds({0, 0, 100, 50}, 200, 200);
    const auto px = [&](double x, double y) { return std::pair{((x * v.scaleX + v.offsetX) + 1) * 0.5 * 200, ((y * v.scaleY + v.offsetY) + 1) * 0.5 * 200}; };
    QCOMPARE(px(0, 0).first, 0.0);
    QCOMPARE(px(100, 0).first, 200.0);
    QVERIFY(std::abs(px(0, 0).second - 50) < 1e-9);
    QVERIFY(std::abs(px(0, 50).second - 150) < 1e-9);
    // negative coordinates and a tall model
    const ViewTransform t = fitBounds({-50, -100, 50, 100}, 100, 400);
    const auto q = [&](double x, double y) { return std::pair{((x * t.scaleX + t.offsetX) + 1) * 0.5 * 100, ((y * t.scaleY + t.offsetY) + 1) * 0.5 * 400}; };
    QVERIFY(std::abs(q(-50, -100).first - 0) < 1e-9 && std::abs(q(-50, -100).second - 100) < 1e-9); // the model is 100 x 200 in a 100 x 400 picture: centred
    QVERIFY(std::abs(q(50, 100).first - 100) < 1e-9 && std::abs(q(50, 100).second - 300) < 1e-9);
}

void TestPuppetView::fitHandlesDegenerateBounds()
{
    for (const Bounds &b : {Bounds{0, 0, 0, 0}, Bounds{5, 5, 5, 10}, Bounds{0, 0, 10, 0}, Bounds{10, 10, 0, 0}, Bounds{0, 0, INFINITY, 1}, Bounds{NAN, 0, 1, 1}}) {
        const ViewTransform v = fitBounds(b, 100, 100);
        QCOMPARE(v.scaleX, 1.0);
        QCOMPARE(v.offsetX, 0.0);
    }
    const ViewTransform z = fitBounds({0, 0, 10, 10}, 0, 100);
    QCOMPARE(z.scaleX, 1.0);
}

void TestPuppetView::maskOfAQuad()
{
    FakeModel m;
    m.shapes = {FakeModel::quadShape()};
    m.extent = {0, 0, 100, 100};
    m.fill = [](const ParameterSet &, const PartSet &, Frame &f) { f.items.push_back(FakeModel::mesh(0, 25, 25, 50, 50)); };
    const Frame f = frameOf(m);
    const ViewTransform v = fitBounds(m.bounds(), 100, 100);
    const HitMask mask = hitMaskOf(f, m, v, 100, 100, 10);
    QCOMPARE(mask.cols, 10u);
    QCOMPARE(mask.rows, 10u);
    QCOMPARE(mask.cell, 10u);
    for (uint32_t r = 0; r < 10; ++r)
        for (uint32_t c = 0; c < 10; ++c) {
            const bool expected = c >= 2 && c <= 7 && r >= 2 && r <= 7; // the cells that the square 25..75 touches
            QVERIFY2(mask.at(c, r) == expected || (c == 2 || c == 7 || r == 2 || r == 7), qPrintable(QString("cell %1,%2").arg(c).arg(r)));
        }
    QVERIFY(mask.at(4, 4));
    QVERIFY(mask.at(5, 5));
    QVERIFY(!mask.at(0, 0));
    QVERIFY(!mask.at(9, 9));
    QVERIFY(!mask.at(100, 0)); // out of range is false
    QCOMPARE(mask.bits.size(), size_t((100 + 7) / 8));
}

void TestPuppetView::maskOfATriangleAndThinThings()
{
    FakeModel m;
    Shape tri;
    tri.uvs = {{0, 0}, {1, 0}, {0, 1}};
    tri.indices = {0, 1, 2};
    Shape sliver;
    sliver.uvs = {{0, 0}, {1, 0}, {0, 1}};
    sliver.indices = {0, 1, 2};
    m.shapes = {tri, sliver};
    m.fill = [](const ParameterSet &, const PartSet &, Frame &f) {
        DrawItem a;
        a.drawable = 0;
        a.positions = {{0, 0}, {80, 0}, {0, 80}};
        f.items.push_back(a);
        DrawItem b;
        b.drawable = 1;
        b.positions = {{90, 0}, {90.3, 0}, {90, 100}}; // thinner than a cell
        f.items.push_back(b);
    };
    const Frame f = frameOf(m);
    const HitMask mask = hitMaskOf(f, m, fitBounds({0, 0, 100, 100}, 100, 100), 100, 100, 10);
    QVERIFY(mask.at(1, 1));
    QVERIFY(mask.at(0, 7));
    QVERIFY(!mask.at(7, 7));  // beyond the hypotenuse
    QVERIFY(!mask.at(8, 8));
    // a feature thinner than a pixel is seen only where a vertex or a sample falls in it: the end here (a documented limit of a coarse mask)
    QVERIFY(mask.at(9, 0));
    QVERIFY(!mask.at(9, 5));
}

void TestPuppetView::maskIgnoresInvisibleAndGroups()
{
    FakeModel m;
    m.shapes = {FakeModel::quadShape(), FakeModel::quadShape()};
    m.fill = [](const ParameterSet &, const PartSet &, Frame &f) {
        DrawItem hidden = FakeModel::mesh(0, 0, 0, 50, 50);
        hidden.opacity = 0;
        f.items.push_back(hidden);
        DrawItem begin;
        begin.kind = DrawKind::BeginGroup;
        begin.drawable = 1;
        f.items.push_back(begin);
        f.items.push_back(FakeModel::mesh(1, 60, 60, 30, 30)); // inside a group: drawn, so it counts
        DrawItem end;
        end.kind = DrawKind::EndGroup;
        end.drawable = 1;
        end.positions = FakeModel::rect(0, 0, 100, 100); // a group's own entry carries no geometry of the hit area
        f.items.push_back(end);
        DrawItem bad = FakeModel::mesh(7, 0, 0, 10, 10); // a drawable the model does not know has no triangles
        f.items.push_back(bad);
        DrawItem neg = FakeModel::mesh(0, 0, 0, 10, 10);
        neg.opacity = -1;
        f.items.push_back(neg);
    };
    const Frame f = frameOf(m);
    const HitMask mask = hitMaskOf(f, m, fitBounds({0, 0, 100, 100}, 100, 100), 100, 100, 10);
    QVERIFY(!mask.at(1, 1));
    QVERIFY(mask.at(7, 7));
    QCOMPARE(mask.count() > 0, true);
}

void TestPuppetView::maskOffTheEdges()
{
    FakeModel m;
    m.shapes = {FakeModel::quadShape()};
    m.fill = [](const ParameterSet &, const PartSet &, Frame &f) {
        f.items.push_back(FakeModel::mesh(0, -1e6, -1e6, 2e6, 2e6)); // covers everything and far more
        f.items.push_back(FakeModel::mesh(0, 1e300, 1e300, 1, 1));
        DrawItem nan = FakeModel::mesh(0, 0, 0, 1, 1);
        nan.positions[1].x = NAN;
        f.items.push_back(nan);
    };
    const Frame f = frameOf(m);
    const HitMask mask = hitMaskOf(f, m, fitBounds({0, 0, 100, 100}, 100, 100), 100, 100, 10);
    QCOMPARE(mask.count(), size_t(100));
}

void TestPuppetView::largePicturesGetLargerCells()
{
    FakeModel m;
    m.shapes = {FakeModel::quadShape()};
    m.fill = [](const ParameterSet &, const PartSet &, Frame &f) { f.items.push_back(FakeModel::mesh(0, 0, 0, 100, 100)); };
    const Frame f = frameOf(m);
    const HitMask mask = hitMaskOf(f, m, fitBounds({0, 0, 100, 100}, 4096, 4096), 4096, 4096, 8);
    QVERIFY(mask.cols <= bridge::kMaxHitMaskSide);
    QVERIFY(mask.rows <= bridge::kMaxHitMaskSide);
    QCOMPARE(mask.cell, 16u);
    QCOMPARE(mask.count(), size_t(mask.cols) * mask.rows);
    QVERIFY(bridge::encodeHitMask({mask.cols, mask.rows, mask.cell, 1}, mask.bits).has_value());
}

void TestPuppetView::maskWorkIsBounded()
{
    // 5000 triangles that each cover the whole picture, as one model would draw them: far over the work budget if each were walked, but the cells they set are looked at once
    FakeModel m;
    Shape s = FakeModel::quadShape();
    s.indices.clear();
    for (int i = 0; i < 2500; ++i)
        s.indices.insert(s.indices.end(), {0, 1, 2, 0, 2, 3});
    m.shapes = {s};
    m.extent = {0, 0, 2048, 2048};
    m.fill = [](const ParameterSet &, const PartSet &, Frame &f) { f.items.push_back(FakeModel::mesh(0, 0, 0, 2048, 2048)); };
    Frame f = frameOf(m);
    const ViewTransform v = fitBounds(m.bounds(), 2048, 2048);
    // (the best of three: the machine may be busy with other tests)
    const auto fastest = [&](const Frame &frame, const FakeModel &model, HitMask &out) {
        qint64 best = 1 << 30;
        for (int i = 0; i < 3; ++i) {
            QElapsedTimer timer;
            timer.start();
            out = hitMaskOf(frame, model, v, 2048, 2048, 8);
            best = std::min<qint64>(best, timer.elapsed());
        }
        return best;
    };
    HitMask full;
    const qint64 fullMs = fastest(f, m, full);
    QVERIFY2(fullMs < 50, qPrintable(QString::number(fullMs) + " ms"));
    QCOMPARE(full.count(), size_t(full.cols) * full.rows);

    // thin diagonal slivers, each with the whole picture as its box and almost no cell of its own: the budget ends the walk, and the rest of the item is covered by one box (more than it covers)
    FakeModel thin;
    Shape ts;
    ts.uvs = {{0, 0}, {0, 0}, {0, 0}};
    ts.indices.clear();
    std::vector<Vec2> positions;
    for (int i = 0; i < 5000; ++i) {
        const double o = (i % 50) * 0.01;
        positions.insert(positions.end(), {{0.0 + o, 0.0}, {2048.0, 2048.0 - 0.02 - o}, {2048.0, 2048.0}});
        const uint16_t base = static_cast<uint16_t>(i * 3);
        ts.indices.insert(ts.indices.end(), {base, uint16_t(base + 1), uint16_t(base + 2)});
        ts.uvs.insert(ts.uvs.end(), {{0, 0}, {0, 0}, {0, 0}});
    }
    ts.uvs.resize(positions.size());
    thin.shapes = {ts};
    thin.extent = {0, 0, 2048, 2048};
    Frame tf;
    DrawItem item = FakeModel::mesh(0, 0, 0, 1, 1);
    item.positions = positions;
    tf.items.push_back(item);
    HitMask slivers;
    const qint64 sliverMs = fastest(tf, thin, slivers);
    QVERIFY2(sliverMs < 100, qPrintable(QString::number(sliverMs) + " ms"));
    // the mask is a superset of what the first sliver alone sets
    Frame one = tf;
    one.items[0].positions.resize(3);
    Shape first = ts;
    first.indices.resize(3);
    thin.shapes = {first};
    const HitMask exact = hitMaskOf(one, thin, v, 2048, 2048, 8);
    QVERIFY(exact.count() > 0);
    for (uint32_t r = 0; r < exact.rows; ++r)
        for (uint32_t c = 0; c < exact.cols; ++c)
            if (exact.at(c, r))
                QVERIFY(slivers.at(c, r));
}

void TestPuppetView::maskSurvivesHugeNumbers()
{
    FakeModel m;
    Shape s;
    s.uvs = {{0, 0}, {0, 0}, {0, 0}};
    s.indices = {0, 1, 2};
    m.shapes = {s};
    m.extent = {0, 0, 64, 64};
    const ViewTransform v = fitBounds(m.bounds(), 64, 64);
    const auto maskOf = [&](Vec2 a, Vec2 b, Vec2 c) {
        Frame f;
        DrawItem item = FakeModel::mesh(0, 0, 0, 1, 1);
        item.positions = {a, b, c};
        f.items.push_back(item);
        return hitMaskOf(f, m, v, 64, 64, 8);
    };
    // finite, but far beyond what an integer holds once it is in pixels: the cell range is clamped before it is converted
    const HitMask corner = maskOf({0, 0}, {1e30, 0}, {0, 1e30});
    QCOMPARE(corner.count(), size_t(corner.cols) * corner.rows);
    const HitMask around = maskOf({-1e30, -1e30}, {1e30, -1e30}, {0, 1e30});
    QCOMPARE(around.count(), size_t(around.cols) * around.rows);
    // wholly off the picture: nothing
    QCOMPARE(maskOf({1e30, 1e30}, {2e30, 1e30}, {1e30, 2e30}).count(), size_t(0));
    QCOMPARE(maskOf({-1e30, -1e30}, {-2e30, -1e30}, {-1e30, -2e30}).count(), size_t(0));
    // the cost of such a frame is a number, not a trap
    Frame f;
    DrawItem item = FakeModel::mesh(0, 0, 0, 1, 1);
    item.positions = {{0, 0}, {1e30, 0}, {0, 1e30}};
    f.items.push_back(item);
    const FrameCost cost = frameCost(f, m, v, 64, 64);
    QVERIFY(std::isfinite(cost.pixels));
    QVERIFY(cost.pixels <= 64.0 * 64.0 + 1e-6);
}

void TestPuppetView::frameCostCountsClippedAreaMasksAndGroups()
{
    FakeModel m;
    m.shapes = {FakeModel::quadShape(), FakeModel::quadShape()};
    m.extent = {0, 0, 100, 100};
    const ViewTransform v = fitBounds(m.bounds(), 100, 100);
    const double picture = 100.0 * 100.0;
    const auto cost = [&](Frame &f) { return frameCost(f, m, v, 100, 100); };
    Frame f;
    // a quad over the whole picture is one picture (two triangles, half their boxes each)
    f.items.push_back(FakeModel::mesh(0, 0, 0, 100, 100));
    QVERIFY(std::abs(cost(f).pixels - picture) < 1e-6);
    QCOMPARE(cost(f).vertices, std::uint64_t(4));
    // one half outside: the true area is half a picture; the estimate is an upper bound (the clipped boxes), at most twice it
    f.items.push_back(FakeModel::mesh(0, 50, 0, 100, 100));
    const double both = cost(f).pixels;
    QVERIFY(both >= picture + picture / 2 - 1e-6 && both <= picture + picture + 1e-6);
    // an invisible one costs nothing
    f.items.push_back(FakeModel::mesh(0, 0, 0, 100, 100));
    f.items.back().opacity = 0;
    QCOMPARE(cost(f).pixels, both);
    QCOMPARE(cost(f).vertices, std::uint64_t(8));
    // a mesh wholly off the picture costs nothing
    Frame off;
    off.items.push_back(FakeModel::mesh(0, 500, 500, 10, 10));
    QCOMPARE(cost(off).pixels, 0.0);
    // masks: a stencil clear is a fraction of a picture, the source is drawn again for each use (its area and its vertices)
    Frame masked;
    masked.maskSources.push_back(FakeModel::mesh(1, 0, 0, 50, 100)); // half a picture
    for (int i = 0; i < 3; ++i) {
        DrawItem item = FakeModel::mesh(0, 0, 0, 10, 10); // 100 pixels
        item.masks = {MaskRef{1, false}};
        masked.items.push_back(item);
    }
    QVERIFY(std::abs(cost(masked).pixels - 3 * (100 + picture / 16 + picture / 2)) < 1e-6);
    QCOMPARE(cost(masked).vertices, std::uint64_t(3 * (4 + 4)));
    // a group: cleared (cheap), its members, and the box of what it holds when it is put on the picture
    Frame grouped;
    DrawItem begin;
    begin.kind = DrawKind::BeginGroup;
    DrawItem end;
    end.kind = DrawKind::EndGroup;
    grouped.items = {begin, FakeModel::mesh(0, 0, 0, 100, 100), end};
    QVERIFY(std::abs(cost(grouped).pixels - (picture / 16 + picture + picture)) < 1e-6);
    Frame small;
    small.items = {begin, FakeModel::mesh(0, 10, 10, 10, 10), end};
    QVERIFY(std::abs(cost(small).pixels - (picture / 16 + 100 + 100)) < 1e-6); // the clear, the member (two halves of its 10 x 10 box: 100), then the 10 x 10 box of what the group holds
    // 2000 full-picture quads are 2000 pictures: far over the budget of kMaxOverdrawFactor
    Frame heavy;
    for (int i = 0; i < 2000; ++i)
        heavy.items.push_back(FakeModel::mesh(0, 0, 0, 100, 100));
    QVERIFY(cost(heavy).pixels > 64 * picture);
}

void TestPuppetView::aRealisticModelIsWithinTheBudget()
{
    // 20 groups of small parts and 40 masked parts, as a character with layered clothing and hair could have: a few pictures of area, nowhere near 64
    FakeModel m;
    for (int i = 0; i < 3; ++i)
        m.shapes.push_back(FakeModel::quadShape());
    m.extent = {0, 0, 1000, 1000};
    const ViewTransform v = fitBounds(m.bounds(), 1000, 1000);
    Frame f;
    f.maskSources.push_back(FakeModel::mesh(2, 300, 300, 300, 300)); // (a face-sized mask, drawn again for each of the 40 parts)
    for (int g = 0; g < 20; ++g) {
        DrawItem begin;
        begin.kind = DrawKind::BeginGroup;
        f.items.push_back(begin);
        f.items.push_back(FakeModel::mesh(0, g * 40, 100, 300, 300));
        f.items.push_back(FakeModel::mesh(0, g * 40, 500, 200, 200));
        DrawItem end;
        end.kind = DrawKind::EndGroup;
        f.items.push_back(end);
    }
    for (int k = 0; k < 40; ++k) {
        DrawItem part = FakeModel::mesh(1, k * 20, 50 + k * 10, 150, 150);
        part.masks = {MaskRef{2, false}};
        f.items.push_back(part);
    }
    const FrameCost cost = frameCost(f, m, v, 1000, 1000);
    const double pictures = cost.pixels / 1e6;
    QVERIFY2(pictures < 64 / 4.0, qPrintable(QString::number(pictures)));
}

void TestPuppetView::frameCostWorkIsBounded()
{
    // ten items of 500 000 empty triangles each: past kMaxFrameCostWork triangles the rest count as a picture each, and the work stops there
    FakeModel m;
    Shape s = FakeModel::quadShape();
    s.indices.clear();
    for (int i = 0; i < 500000; ++i)
        s.indices.insert(s.indices.end(), {0, 0, 0});
    // (indices are 16 bit: 1.5 million of them is far over kMaxIndices, which a real model never reaches; the cost must not care)
    m.shapes = {s};
    m.extent = {0, 0, 100, 100};
    const ViewTransform v = fitBounds(m.bounds(), 100, 100);
    Frame f;
    for (int i = 0; i < 10; ++i)
        f.items.push_back(FakeModel::mesh(0, 0, 0, 100, 100));
    QElapsedTimer timer;
    timer.start();
    const FrameCost cost = frameCost(f, m, v, 100, 100);
    QVERIFY2(timer.elapsed() < 500, qPrintable(QString::number(timer.elapsed())));
    // 4 194 304 triangles are looked at: eight items whole (4 M), the ninth partly, then the rest as pictures
    QVERIFY(cost.pixels >= 1 * 1e4 && cost.pixels <= 2 * 1e4 + 1);
}

void TestPuppetView::aMaskNotSentIsOfferedAgain()
{
    HitMask mask;
    mask.cols = mask.rows = 4;
    mask.bits.assign(2, 0xFF);
    HitMaskTracker tracker;
    QVERIFY(tracker.update(mask));
    QVERIFY(!tracker.update(mask)); // nothing changed
    tracker.reset();                // the send failed (the socket was full)
    QVERIFY(tracker.update(mask));  // the same mask is offered at once
}

void TestPuppetView::trackerSendsWhatMatters()
{
    HitMask a;
    a.cols = 10;
    a.rows = 10;
    a.cell = 8;
    a.bits.assign(13, 0);
    HitMaskTracker t(0.05, 5);
    QVERIFY(t.update(a));   // the first
    QVERIFY(!t.update(a));  // unchanged
    HitMask b = a;
    b.bits[0] = 1;          // one cell of 100: below 5%
    QVERIFY(!t.update(b));
    QVERIFY(!t.update(b));
    HitMask c = a;
    c.bits[0] = 0x1F;       // five cells: 5%
    QVERIFY(t.update(c));
    QVERIFY(!t.update(c));
    HitMask d = c;
    d.bits[1] = 1;          // one more cell, small, but it stays different
    int sent = 0;
    for (int i = 0; i < 6; ++i)
        sent += t.update(d) ? 1 : 0;
    QCOMPARE(sent, 1);      // after maxStaleFrames updates the small change goes out, once
    // another grid is always sent
    HitMask e = d;
    e.cols = 8;
    QVERIFY(t.update(e));
    t.reset();
    QVERIFY(t.update(e));
}

void TestPuppetView::messageRoundTrip()
{
    QCOMPARE(bridge::hitMaskBytes(10, 5), size_t(7));
    QCOMPARE(bridge::hitMaskBytes(8, 1), size_t(1));
    QCOMPARE(bridge::hitMaskBytes(1, 1), size_t(1));
    std::vector<uint8_t> bits(7, 0);
    bits[0] = 0xA5;
    bits[6] = 0x03; // 50 bits: the last byte holds 2
    const auto payload = bridge::encodeHitMask({10, 5, 8, 42}, bits);
    QVERIFY(payload);
    QCOMPARE(payload->size(), sizeof(bridge::HitMaskHeader) + 7);
    const auto view = bridge::decodeHitMask(*payload);
    QVERIFY(view);
    QCOMPARE(view->header.cols, 10u);
    QCOMPARE(view->header.rows, 5u);
    QCOMPARE(view->header.cell, 8u);
    QCOMPARE(view->header.generation, 42u);
    QCOMPARE(view->bits.size(), size_t(7));
    QCOMPARE(view->bits[0], uint8_t(0xA5));
    // the largest grid
    std::vector<uint8_t> big(bridge::hitMaskBytes(256, 256), 0xFF);
    const auto large = bridge::encodeHitMask({256, 256, 64, 1}, big);
    QVERIFY(large);
    QVERIFY(large->size() <= bridge::kMaxPropertiesJson); // a message is far under the channel's payload cap
    QVERIFY(bridge::decodeHitMask(*large));
    QCOMPARE(int(bridge::kHitMaskType), 49);
}

void TestPuppetView::messageValidation_data()
{
    QTest::addColumn<QByteArray>("payload");
    const auto make = [](uint32_t cols, uint32_t rows, uint32_t cell, size_t bytes, uint8_t fill = 0) {
        bridge::HitMaskHeader h{cols, rows, cell, 1};
        QByteArray out(reinterpret_cast<const char *>(&h), sizeof h);
        out += QByteArray(int(bytes), char(fill));
        return out;
    };
    QTest::newRow("empty") << QByteArray();
    QTest::newRow("short header") << QByteArray(15, 'x');
    QTest::newRow("header only") << make(8, 8, 8, 0);
    QTest::newRow("one byte short") << make(8, 8, 8, 7);
    QTest::newRow("one byte long") << make(8, 8, 8, 9);
    QTest::newRow("zero cols") << make(0, 8, 8, 0);
    QTest::newRow("zero rows") << make(8, 0, 8, 0);
    QTest::newRow("zero cell") << make(8, 8, 0, 8);
    QTest::newRow("too many cols") << make(257, 1, 8, bridge::hitMaskBytes(257, 1));
    QTest::newRow("too many rows") << make(1, 257, 8, bridge::hitMaskBytes(1, 257));
    QTest::newRow("huge cell") << make(8, 8, 65, 8);
    QTest::newRow("padding bits set") << make(10, 5, 8, 7, 0xFF);
    QTest::newRow("huge grid") << make(0xFFFFFFFF, 0xFFFFFFFF, 8, 16);
}

void TestPuppetView::messageValidation()
{
    QFETCH(QByteArray, payload);
    const std::span<const uint8_t> span(reinterpret_cast<const uint8_t *>(payload.constData()), size_t(payload.size()));
    QVERIFY(!bridge::decodeHitMask(span));
    // the encoder refuses what the decoder would
    QVERIFY(!bridge::encodeHitMask({0, 1, 8, 0}, {}));
    std::vector<uint8_t> wrong(3, 0);
    QVERIFY(!bridge::encodeHitMask({8, 8, 8, 0}, wrong));
}

QTEST_GUILESS_MAIN(TestPuppetView)
#include "tst_puppetview.moc"
