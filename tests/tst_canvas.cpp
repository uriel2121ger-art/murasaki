#include "canvas.h"

#include <QTest>

#include <cmath>

using namespace Qt::StringLiterals;
using namespace murasaki;

class TestCanvas : public QObject
{
    Q_OBJECT

private slots:
    void parsesFillModes();
    void canvasIsTheBoundingBoxOfTheOutputs();
    void emptyAndRepeatedOutputsAreIgnored();
    void noOutputsMakeNoCanvas();
    void placesContentUnderEachFillMode();
    void fillSplitsTheCoveredContent();
    void fitLeavesBandsAndHidesOutputsInThem();
    void stretchSplitsTheWholePicture();
    void aSingleOutputMapsLikeAPlainFill();
    void mixedScaleLayoutsStayOnePicture();
    void mpvViewportReproducesThePlacement_data();
    void mpvViewportReproducesThePlacement();
    void degenerateInputsAreInvisible();
};

static void compare(const QRectF &actual, const QRectF &expected)
{
    QVERIFY2(std::abs(actual.x() - expected.x()) < 1e-9 && std::abs(actual.y() - expected.y()) < 1e-9
                 && std::abs(actual.width() - expected.width()) < 1e-9
                 && std::abs(actual.height() - expected.height()) < 1e-9,
             qPrintable(u"%1,%2 %3x%4 != %5,%6 %7x%8"_s.arg(actual.x()).arg(actual.y()).arg(actual.width())
                            .arg(actual.height()).arg(expected.x()).arg(expected.y()).arg(expected.width())
                            .arg(expected.height())));
}

void TestCanvas::parsesFillModes()
{
    QCOMPARE(parseFillMode(u"fill"_s), FillMode::Fill);
    QCOMPARE(parseFillMode(u"fit"_s), FillMode::Fit);
    QCOMPARE(parseFillMode(u"stretch"_s), FillMode::Stretch);
    QVERIFY(!parseFillMode(u"cover"_s));
    QVERIFY(!parseFillMode(QString()));
}

void TestCanvas::canvasIsTheBoundingBoxOfTheOutputs()
{
    // Side by side, the right one shorter and lower; the left one has negative coordinates.
    const SpanLayout layout = spanLayout({{u"L"_s, QRectF(-1920, -100, 1920, 1080)}, {u"R"_s, QRectF(0, 0, 1280, 720)}});
    QVERIFY(layout.isValid());
    QCOMPARE(layout.canvas, QSizeF(3200, 1080));
    compare(layout.viewports.value(u"L"_s), QRectF(0, 0, 1920, 1080));
    compare(layout.viewports.value(u"R"_s), QRectF(1920, 100, 1280, 720));

    // stacked
    const SpanLayout stacked = spanLayout({{u"A"_s, QRectF(0, 0, 1920, 1080)}, {u"B"_s, QRectF(0, 1080, 1920, 1080)}});
    QCOMPARE(stacked.canvas, QSizeF(1920, 2160));
    compare(stacked.viewports.value(u"B"_s), QRectF(0, 1080, 1920, 1080));
}

void TestCanvas::emptyAndRepeatedOutputsAreIgnored()
{
    const SpanLayout layout = spanLayout({{u"A"_s, QRectF(0, 0, 1920, 1080)},
                                          {u"dead"_s, QRectF(1920, 0, 0, 1080)},
                                          {u"A"_s, QRectF(5000, 5000, 100, 100)}});
    QCOMPARE(layout.canvas, QSizeF(1920, 1080));
    QCOMPARE(layout.viewports.size(), 1);
}

void TestCanvas::noOutputsMakeNoCanvas()
{
    QVERIFY(!spanLayout({}).isValid());
    QVERIFY(!spanLayout({{u"x"_s, QRectF()}}).isValid());
}

void TestCanvas::placesContentUnderEachFillMode()
{
    const QSizeF canvas(3840, 1080);
    const QSizeF content(1920, 1080);
    compare(placeContent(canvas, content, FillMode::Fill), QRectF(0, -540, 3840, 2160));
    compare(placeContent(canvas, content, FillMode::Fit), QRectF(960, 0, 1920, 1080));
    compare(placeContent(canvas, content, FillMode::Stretch), QRectF(0, 0, 3840, 1080));
    QVERIFY(placeContent(QSizeF(), content, FillMode::Fill).isEmpty());
    QVERIFY(placeContent(canvas, QSizeF(0, 10), FillMode::Fill).isEmpty());
}

void TestCanvas::fillSplitsTheCoveredContent()
{
    const SpanLayout layout = spanLayout({{u"L"_s, QRectF(0, 0, 1920, 1080)}, {u"R"_s, QRectF(1920, 0, 1920, 1080)}});
    const QSizeF content(1920, 1080);
    const ViewportMapping left = mapViewport(layout.canvas, layout.viewports.value(u"L"_s), content, FillMode::Fill);
    const ViewportMapping right = mapViewport(layout.canvas, layout.viewports.value(u"R"_s), content, FillMode::Fill);
    QVERIFY(left.visible && right.visible);
    // The content is drawn twice as big as it is and cropped top and bottom: each output shows one half of the width
    // and the middle half of the height.
    compare(left.source, QRectF(0, 270, 960, 540));
    compare(right.source, QRectF(960, 270, 960, 540));
    compare(left.dest, QRectF(0, 0, 1920, 1080));
    compare(right.dest, QRectF(0, 0, 1920, 1080));
}

void TestCanvas::fitLeavesBandsAndHidesOutputsInThem()
{
    // A square picture on three outputs in a row: it is as tall as the canvas (1080) and centered, so the middle
    // output has all of it, the neighbours have none.
    const SpanLayout layout = spanLayout({{u"A"_s, QRectF(0, 0, 1920, 1080)},
                                          {u"B"_s, QRectF(1920, 0, 1920, 1080)},
                                          {u"C"_s, QRectF(3840, 0, 1920, 1080)}});
    const QSizeF content(1000, 1000);
    QVERIFY(!mapViewport(layout.canvas, layout.viewports.value(u"A"_s), content, FillMode::Fit).visible);
    QVERIFY(!mapViewport(layout.canvas, layout.viewports.value(u"C"_s), content, FillMode::Fit).visible);
    const ViewportMapping middle = mapViewport(layout.canvas, layout.viewports.value(u"B"_s), content, FillMode::Fit);
    QVERIFY(middle.visible);
    compare(middle.source, QRectF(0, 0, 1000, 1000));
    compare(middle.dest, QRectF(420, 0, 1080, 1080));

    // Two outputs: the picture straddles the seam.
    const SpanLayout two = spanLayout({{u"L"_s, QRectF(0, 0, 1920, 1080)}, {u"R"_s, QRectF(1920, 0, 1920, 1080)}});
    const ViewportMapping l = mapViewport(two.canvas, two.viewports.value(u"L"_s), content, FillMode::Fit);
    const ViewportMapping r = mapViewport(two.canvas, two.viewports.value(u"R"_s), content, FillMode::Fit);
    compare(l.source, QRectF(0, 0, 500, 1000));
    compare(l.dest, QRectF(1380, 0, 540, 1080));
    compare(r.source, QRectF(500, 0, 500, 1000));
    compare(r.dest, QRectF(0, 0, 540, 1080));
}

void TestCanvas::stretchSplitsTheWholePicture()
{
    const SpanLayout layout = spanLayout({{u"L"_s, QRectF(0, 0, 1000, 1000)}, {u"R"_s, QRectF(1000, 0, 3000, 1000)}});
    const QSizeF content(400, 100);
    const ViewportMapping left = mapViewport(layout.canvas, layout.viewports.value(u"L"_s), content, FillMode::Stretch);
    const ViewportMapping right = mapViewport(layout.canvas, layout.viewports.value(u"R"_s), content, FillMode::Stretch);
    compare(left.source, QRectF(0, 0, 100, 100));
    compare(right.source, QRectF(100, 0, 300, 100));
    compare(left.dest, QRectF(0, 0, 1000, 1000));
    compare(right.dest, QRectF(0, 0, 3000, 1000));
}

void TestCanvas::aSingleOutputMapsLikeAPlainFill()
{
    const SpanLayout layout = spanLayout({{u"only"_s, QRectF(100, 50, 1920, 1080)}});
    const ViewportMapping fill = mapViewport(layout.canvas, layout.viewports.value(u"only"_s), QSizeF(1000, 1000), FillMode::Fill);
    // a square picture covering 16:9: the full width and the middle 56.25% of the height
    compare(fill.source, QRectF(0, 1000 * (1 - 1080.0 / 1920) / 2, 1000, 1000 * 1080.0 / 1920));
    compare(fill.dest, QRectF(0, 0, 1920, 1080));
    const ViewportMapping fit = mapViewport(layout.canvas, layout.viewports.value(u"only"_s), QSizeF(1000, 1000), FillMode::Fit);
    compare(fit.source, QRectF(0, 0, 1000, 1000));
    compare(fit.dest, QRectF(420, 0, 1080, 1080));
}

void TestCanvas::mixedScaleLayoutsStayOnePicture()
{
    // A 4K output at scale 2 (logical 1920x1080) beside a 1080p one at scale 1: in logical space they are two equal
    // rectangles, so the picture splits evenly whatever the pixel counts.
    const SpanLayout layout = spanLayout({{u"hidpi"_s, QRectF(0, 0, 1920, 1080)}, {u"hd"_s, QRectF(1920, 0, 1920, 1080)}});
    const QSizeF content(3840, 1080);
    const ViewportMapping a = mapViewport(layout.canvas, layout.viewports.value(u"hidpi"_s), content, FillMode::Fill);
    const ViewportMapping b = mapViewport(layout.canvas, layout.viewports.value(u"hd"_s), content, FillMode::Fill);
    compare(a.source, QRectF(0, 0, 1920, 1080));
    compare(b.source, QRectF(1920, 0, 1920, 1080));
}

void TestCanvas::mpvViewportReproducesThePlacement_data()
{
    QTest::addColumn<int>("fill");
    QTest::addColumn<QSizeF>("content");
    QTest::addColumn<QRectF>("viewport");
    QTest::addColumn<bool>("visible");

    const QSizeF canvas(3840, 1080);
    Q_UNUSED(canvas)
    for (const auto &[name, fill] : {std::pair{"fill", FillMode::Fill}, {"fit", FillMode::Fit}, {"stretch", FillMode::Stretch}}) {
        QTest::addRow("%s wide left", name) << int(fill) << QSizeF(1920, 1080) << QRectF(0, 0, 1920, 1080) << true;
        QTest::addRow("%s wide right", name) << int(fill) << QSizeF(1920, 1080) << QRectF(1920, 0, 1920, 1080) << true;
        QTest::addRow("%s square left", name) << int(fill) << QSizeF(1000, 1000) << QRectF(0, 0, 1920, 1080)
                                              << true;
        QTest::addRow("%s tall right", name) << int(fill) << QSizeF(500, 2000) << QRectF(1920, 0, 1920, 1080) << true;
        QTest::addRow("%s portrait output", name) << int(fill) << QSizeF(1920, 1080) << QRectF(2800, 0, 1040, 1080) << true;
    }
}

void TestCanvas::mpvViewportReproducesThePlacement()
{
    QFETCH(int, fill);
    QFETCH(QSizeF, content);
    QFETCH(QRectF, viewport);
    QFETCH(bool, visible);
    const QSizeF canvas(3840, 1080);
    const FillMode mode = FillMode(fill);
    const MpvViewport mpv = mpvViewport(canvas, viewport, content, mode);
    QCOMPARE(mpv.visible, visible);
    if (!visible)
        return;

    // mpv's model: the picture of this aspect is fitted into the window, scaled by 2^zoom and moved by the pan
    // fractions of its own size.
    const double wanted = viewport.width() / viewport.height();
    const QSizeF base = wanted > mpv.aspect ? QSizeF(viewport.height() * mpv.aspect, viewport.height())
                                            : QSizeF(viewport.width(), viewport.width() / mpv.aspect);
    const double scale = std::pow(2.0, mpv.zoom);
    const QSizeF size(base.width() * scale, base.height() * scale);
    const QPointF center(viewport.width() / 2 + mpv.panX * size.width(), viewport.height() / 2 + mpv.panY * size.height());
    const QRectF drawn(QPointF(center.x() - size.width() / 2, center.y() - size.height() / 2), size);
    compare(drawn, placeContent(canvas, content, mode).translated(-viewport.topLeft()));
}

void TestCanvas::degenerateInputsAreInvisible()
{
    const QSizeF canvas(3840, 1080);
    QVERIFY(!mapViewport(canvas, QRectF(), QSizeF(10, 10), FillMode::Fill).visible);
    QVERIFY(!mapViewport(canvas, QRectF(0, 0, 100, 100), QSizeF(), FillMode::Fill).visible);
    QVERIFY(!mapViewport(QSizeF(), QRectF(0, 0, 100, 100), QSizeF(10, 10), FillMode::Fill).visible);
    QVERIFY(!mpvViewport(canvas, QRectF(), QSizeF(10, 10), FillMode::Fill).visible);
    QVERIFY(!mpvViewport(canvas, QRectF(0, 0, 100, 100), QSizeF(0, 0), FillMode::Fit).visible);
    // a viewport that only touches the picture shows nothing
    QVERIFY(!mapViewport(canvas, QRectF(0, 0, 960, 1080), QSizeF(1920, 1080), FillMode::Fit).visible);
}

QTEST_APPLESS_MAIN(TestCanvas)
#include "tst_canvas.moc"
