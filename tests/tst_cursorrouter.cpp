#include "cursorrouter.h"

#include <QTest>

using namespace Qt::StringLiterals;
using namespace murasaki;

namespace {

// Two outputs side by side, the second one smaller; a third one above the first (negative y, as KWin lays out).
QList<CursorScreen> layout()
{
    return {
        {u"DP-1"_s, QRectF(0, 0, 1920, 1080)},
        {u"HDMI-1"_s, QRectF(1920, 0, 1280, 720)},
        {u"DP-2"_s, QRectF(0, -540, 960, 540)},
    };
}

} // namespace

class TestCursorRouter : public QObject
{
    Q_OBJECT

private slots:
    void mapsToTheOutputUnderTheCursor();
    void edgesBelongToOneOutput();
    void outsideEveryOutputIsNoHit();
    void scaledOutputsMapInLogicalPixels();
    void routerEmitsLeftWhenTheOutputChanges();
    void routerLeavesTheLastOutputWhenOutside();
    void throttleHoldsBackAndReleasesTheLatest();
    void throttleDoesNothingWithoutAnythingHeldBack();
    void resetForgetsTheOutput();
};

void TestCursorRouter::mapsToTheOutputUnderTheCursor()
{
    const auto first = mapCursor(layout(), QPointF(100, 50));
    QVERIFY(first);
    QCOMPARE(first->output, u"DP-1"_s);
    QCOMPARE(first->local, QPointF(100, 50));
    QCOMPARE(first->size, QSizeF(1920, 1080));

    const auto second = mapCursor(layout(), QPointF(2000, 100));
    QVERIFY(second);
    QCOMPARE(second->output, u"HDMI-1"_s);
    QCOMPARE(second->local, QPointF(80, 100));
    QCOMPARE(second->size, QSizeF(1280, 720));

    const auto above = mapCursor(layout(), QPointF(10, -1));
    QVERIFY(above);
    QCOMPARE(above->output, u"DP-2"_s);
    QCOMPARE(above->local, QPointF(10, 539));
}

void TestCursorRouter::edgesBelongToOneOutput()
{
    QCOMPARE(mapCursor(layout(), QPointF(1919, 0))->output, u"DP-1"_s);
    QCOMPARE(mapCursor(layout(), QPointF(1920, 0))->output, u"HDMI-1"_s);
    QCOMPARE(mapCursor(layout(), QPointF(0, 0))->output, u"DP-1"_s);
    QCOMPARE(mapCursor(layout(), QPointF(0, -1))->output, u"DP-2"_s);
}

void TestCursorRouter::outsideEveryOutputIsNoHit()
{
    QVERIFY(!mapCursor(layout(), QPointF(3200, 0)));  // right edge of HDMI-1
    QVERIFY(!mapCursor(layout(), QPointF(2000, 800))); // below the smaller output
    QVERIFY(!mapCursor(layout(), QPointF(-1, 0)));
    QVERIFY(!mapCursor({}, QPointF(0, 0)));
    QVERIFY(!mapCursor({{u"zero"_s, QRectF(0, 0, 0, 0)}}, QPointF(0, 0)));
}

void TestCursorRouter::scaledOutputsMapInLogicalPixels()
{
    // A 3840x2160 panel at scale 2 is 1920x1080 logical pixels; a 4K panel at 1.5 is 2560x1440 logical, beside it.
    const QList<CursorScreen> screens{
        {u"DP-1"_s, QRectF(0, 0, 1920, 1080)},
        {u"DP-2"_s, QRectF(1920, 0, 2560, 1440)},
    };
    const auto hit = mapCursor(screens, QPointF(1920 + 1280, 720));
    QVERIFY(hit);
    QCOMPARE(hit->output, u"DP-2"_s);
    QCOMPARE(hit->local, QPointF(1280, 720));
    QCOMPARE(hit->size, QSizeF(2560, 1440));
    // The fraction the host works with is independent of the scale.
    QCOMPARE(hit->local.x() / hit->size.width(), 0.5);
    QCOMPARE(hit->local.y() / hit->size.height(), 0.5);
    // Fractional positions (a fractional-scale compositor) keep their fraction.
    const auto fractional = mapCursor(screens, QPointF(100.5, 20.25));
    QVERIFY(fractional);
    QCOMPARE(fractional->local, QPointF(100.5, 20.25));
}

void TestCursorRouter::routerEmitsLeftWhenTheOutputChanges()
{
    CursorRouter router(30);
    const auto first = router.update(layout(), QPointF(10, 10), 1000);
    QCOMPARE(first.size(), 1);
    QCOMPARE(first[0].kind, CursorMessage::Kind::Move);
    QCOMPARE(first[0].output, u"DP-1"_s);

    const auto same = router.update(layout(), QPointF(20, 20), 1100);
    QCOMPARE(same.size(), 1);
    QCOMPARE(same[0].kind, CursorMessage::Kind::Move);

    const auto crossing = router.update(layout(), QPointF(2000, 20), 1200);
    QCOMPARE(crossing.size(), 2);
    QCOMPARE(crossing[0].kind, CursorMessage::Kind::Left);
    QCOMPARE(crossing[0].output, u"DP-1"_s);
    QCOMPARE(crossing[1].kind, CursorMessage::Kind::Move);
    QCOMPARE(crossing[1].output, u"HDMI-1"_s);
    QCOMPARE(crossing[1].local, QPointF(80, 20));
}

void TestCursorRouter::routerLeavesTheLastOutputWhenOutside()
{
    CursorRouter router(30);
    router.update(layout(), QPointF(10, 10), 1000);
    const auto out = router.update(layout(), QPointF(-500, 10), 1100);
    QCOMPARE(out.size(), 1);
    QCOMPARE(out[0].kind, CursorMessage::Kind::Left);
    QCOMPARE(out[0].output, u"DP-1"_s);
    // Still outside: nothing more to say.
    QVERIFY(router.update(layout(), QPointF(-600, 10), 1200).isEmpty());
}

void TestCursorRouter::throttleHoldsBackAndReleasesTheLatest()
{
    CursorRouter router(30);
    QCOMPARE(router.pendingDelayMs(0), -1);
    QCOMPARE(router.update(layout(), QPointF(1, 1), 1000).size(), 1);

    // A burst inside the window: nothing goes out, only the latest is kept.
    QVERIFY(router.update(layout(), QPointF(2, 2), 1005).isEmpty());
    QVERIFY(router.update(layout(), QPointF(3, 3), 1010).isEmpty());
    QVERIFY(router.update(layout(), QPointF(4, 4), 1020).isEmpty());
    QCOMPARE(router.pendingDelayMs(1020), 10);
    QVERIFY(router.takePending(layout(), 1020).isEmpty()); // too early
    QCOMPARE(router.pendingDelayMs(1020), 10);

    const auto released = router.takePending(layout(), 1030);
    QCOMPARE(released.size(), 1);
    QCOMPARE(released[0].local, QPointF(4, 4));
    QCOMPARE(router.pendingDelayMs(1030), -1);
    QVERIFY(router.takePending(layout(), 1100).isEmpty());

    // The release restarts the window.
    QVERIFY(router.update(layout(), QPointF(5, 5), 1040).isEmpty());
    QCOMPARE(router.pendingDelayMs(1040), 20);
}

void TestCursorRouter::throttleDoesNothingWithoutAnythingHeldBack()
{
    CursorRouter router(30);
    router.update(layout(), QPointF(1, 1), 1000);
    QCOMPARE(router.pendingDelayMs(1100), -1);
    QVERIFY(router.takePending(layout(), 1100).isEmpty());
    // Spaced calls all go out: at most one per window, never more than the input.
    QCOMPARE(router.update(layout(), QPointF(2, 2), 1030).size(), 1);
    QCOMPARE(router.update(layout(), QPointF(3, 3), 1060).size(), 1);
}

void TestCursorRouter::resetForgetsTheOutput()
{
    CursorRouter router(30);
    router.update(layout(), QPointF(10, 10), 1000);
    router.update(layout(), QPointF(20, 20), 1001); // held back
    router.reset();
    QCOMPARE(router.pendingDelayMs(1002), -1);
    // A fresh start: no Left for the old output, and no throttle window from before.
    const auto fresh = router.update(layout(), QPointF(2000, 20), 1002);
    QCOMPARE(fresh.size(), 1);
    QCOMPARE(fresh[0].output, u"HDMI-1"_s);
}

QTEST_GUILESS_MAIN(TestCursorRouter)
#include "tst_cursorrouter.moc"
