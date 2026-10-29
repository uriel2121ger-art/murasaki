#include "pettiming.h"

#include <QTest>

using namespace murasaki;

namespace {

// Frames of 100, 200 and 300 ms: a loop of 600 ms.
FrameClock threeFrames(double now = 0)
{
    FrameClock clock;
    clock.setFrames({100, 200, 300}, now);
    return clock;
}

} // namespace

class TestPetTiming : public QObject
{
    Q_OBJECT

private slots:
    void fixedFrameMsIsAnimasStep()
    {
        QCOMPARE(fixedFrameMs(15), 67);
        QCOMPARE(fixedFrameMs(30), 33);
        QCOMPARE(fixedFrameMs(60), 17);
        QCOMPARE(fixedFrameMs(120), 8);
        QCOMPARE(fixedFrameMs(1), 1000);
        QCOMPARE(fixedFrameMs(100000), 1); // never zero
        QCOMPARE(fixedFrameMs(0), 0);
    }
    void theModeFollowsTheTwoFields()
    {
        QCOMPARE((PetTiming{0, 1}).mode(), PetTimingMode::File);
        QCOMPARE((PetTiming{15, 1}).mode(), PetTimingMode::Fixed);
        QCOMPARE((PetTiming{0, 2}).mode(), PetTimingMode::Speed);
    }
    void fileTimingFollowsTheDelays()
    {
        FrameClock clock = threeFrames();
        QCOMPARE(clock.update(0).frame, 0);
        QCOMPARE(clock.update(0).dueMs, 100.0);
        QCOMPARE(clock.update(100).frame, 1);
        QCOMPARE(clock.update(100).dueMs, 300.0);
        QCOMPARE(clock.update(300).frame, 2);
        QCOMPARE(clock.update(300).dueMs, 600.0);
        QCOMPARE(clock.update(600).frame, 0); // loops
        QCOMPARE(clock.update(600).dueMs, 700.0);
        QCOMPARE(clock.update(1650).frame, 2); // 1650 mod 600 = 450
    }
    void aLateTimerSkipsFramesAndKeepsTheWallClock()
    {
        FrameClock clock = threeFrames();
        // asked at 350 ms: frame 2 is the one that shows now, 1 was skipped; it ends at 600 as always
        const auto step = clock.update(350);
        QCOMPARE(step.frame, 2);
        QCOMPARE(step.dueMs, 600.0);
    }
    void fixedRateGivesEveryFrameTheSameTime()
    {
        FrameClock clock = threeFrames();
        clock.setTiming({15, 1}, 0);
        QCOMPARE(clock.frameMs(0), 67.0);
        QCOMPARE(clock.frameMs(2), 67.0);
        QCOMPARE(clock.update(66).frame, 0);
        QCOMPARE(clock.update(67).frame, 1);
        QCOMPARE(clock.update(134).frame, 2);
        QCOMPARE(clock.update(201).frame, 0);
    }
    void speedScalesTheFilesDelays()
    {
        FrameClock clock = threeFrames();
        clock.setTiming({0, 2}, 0);
        QCOMPARE(clock.frameMs(0), 50.0);
        QCOMPARE(clock.frameMs(1), 100.0);
        QCOMPARE(clock.update(50).frame, 1);
        QCOMPARE(clock.update(150).frame, 2);
        QCOMPARE(clock.update(300).frame, 0);
        clock.setTiming({0, 0.5}, 300);
        QCOMPARE(clock.frameMs(0), 200.0);
    }
    void speedIsClamped()
    {
        FrameClock clock = threeFrames();
        clock.setTiming({0, 100}, 0);
        QCOMPARE(clock.timing().speed, kMaxPetSpeed);
        clock.setTiming({0, 0.001}, 0);
        QCOMPARE(clock.timing().speed, kMinPetSpeed);
        clock.setTiming({0, std::nan("")}, 0);
        QCOMPARE(clock.timing().speed, 1.0);
        clock.setTiming({500, 1}, 0);
        QCOMPARE(clock.timing().fps, 0);
    }
    void aLiveChangeKeepsTheFrameOnScreen()
    {
        FrameClock clock = threeFrames();
        QCOMPARE(clock.update(350).frame, 2); // shown since 300
        clock.setTiming({30, 1}, 350);
        // frame 2 goes on, and under the new timing it began at 66 ms of the loop, which is where the clock is put
        QCOMPARE(clock.update(350).frame, 2);
        QCOMPARE(clock.update(350).dueMs, 350 - 66 + 99.0);
        QCOMPARE(clock.update(350 + 33).frame, 0);
        clock.setTiming({0, 1}, 400);
        QCOMPARE(clock.update(400).frame, 0);
    }
    void aCapSkipsFramesButNeverSlowsTheAnimation()
    {
        FrameClock clock;
        clock.setFrames(QList<int>(100, 10), 0); // 100 frames of 10 ms: a loop of one second, 100 fps
        clock.setCap(20);                        // a frame may change every 50 ms at most
        int changes = 0;
        int last = clock.update(0).frame;
        double now = 0;
        for (int i = 0; i < 400; ++i) { // 2 seconds, polled every 5 ms
            now += 5;
            const auto step = clock.update(now);
            if (step.frame != last)
                ++changes;
            last = step.frame;
        }
        QVERIFY2(changes >= 38 && changes <= 41, qPrintable(QString::number(changes))); // ~20 per second
        // and the animation is where the wall clock says, not behind: at 2 s it is back at the start of the loop
        QCOMPARE(clock.update(2000).frame, 0);
        QCOMPARE(clock.update(2500).frame, 50);
    }
    void aCapDoesNotTouchSlowAnimations()
    {
        FrameClock clock = threeFrames();
        clock.setCap(30); // 33 ms; the shortest frame is 100
        QCOMPARE(clock.update(100).frame, 1);
        QCOMPARE(clock.update(100).dueMs, 300.0);
        QCOMPARE(clock.update(300).frame, 2);
    }
    void aHeldFrameAsksAgainWhenTheCapAllows()
    {
        FrameClock clock;
        clock.setFrames(QList<int>(10, 10), 0);
        clock.setCap(10); // every 100 ms
        QCOMPARE(clock.update(0).frame, 0);
        const auto held = clock.update(20); // frame 2 is due, the cap says no
        QCOMPARE(held.frame, 0);
        QCOMPARE(held.dueMs, 100.0);
        QCOMPARE(clock.update(100).frame, 0); // loop of 100 ms: back at frame 0 by itself
        clock.setCap(0);
        QCOMPARE(clock.update(105).frame, 0);
        QCOMPARE(clock.update(115).frame, 1);
    }
    void pausingStopsTheClock()
    {
        FrameClock clock = threeFrames();
        QCOMPARE(clock.update(150).frame, 1);
        clock.pause(150);
        QCOMPARE(clock.update(5000).dueMs, -1.0);
        QCOMPARE(clock.update(5000).frame, 1);
        clock.resume(5000);
        QCOMPARE(clock.update(5000).frame, 1); // where it stopped
        QCOMPARE(clock.update(5000 + 150).frame, 2);
        QCOMPARE(clock.update(5000 + 150).dueMs, 5000.0 + 450);
    }
    void changingTheTimingWhilePausedKeepsTheFrame()
    {
        FrameClock clock = threeFrames();
        QCOMPARE(clock.update(150).frame, 1);
        clock.pause(150);
        clock.setTiming({0, 2}, 9000);
        clock.resume(9500);
        QCOMPARE(clock.update(9500).frame, 1);
    }
    void aStillImageNeverAsksAgain()
    {
        FrameClock clock;
        clock.setFrames({100}, 0);
        QCOMPARE(clock.update(500).dueMs, -1.0);
        FrameClock empty;
        QCOMPARE(empty.update(500).dueMs, -1.0);
    }
    void zeroDelaysDoNotStallTheClock()
    {
        FrameClock clock;
        clock.setFrames({0, 0, -5}, 0);
        const auto step = clock.update(10);
        QVERIFY(step.dueMs > 10);
    }
    void wrapsAreCountedAndTheirTimeIsExact()
    {
        FrameClock clock = threeFrames(1000);
        QCOMPARE(clock.loops(1000), 0LL);
        QCOMPARE(clock.loops(1599), 0LL);
        QCOMPARE(clock.loops(1600), 1LL);
        QCOMPARE(clock.wrapTimeMs(1), 1600.0);
        QCOMPARE(clock.nextWrapMs(1000), 1600.0);
        QCOMPARE(clock.nextWrapMs(1600), 2200.0);
        QCOMPARE(clock.update(1700).loop, 1LL);
        // A stall over three loops: the count says three, the last wrap time is the latest one, not three events.
        QCOMPARE(clock.loops(2900), 3LL);
        QCOMPARE(clock.wrapTimeMs(clock.loops(2900)), 2800.0);
    }
    void theWrapDoesNotDependOnTheFramesBeingDrawnOrCapped()
    {
        FrameClock clock = threeFrames();
        clock.setCap(5);
        QCOMPARE(clock.update(599).loop, 0LL);
        QCOMPARE(clock.update(601).loop, 1LL);
        QCOMPARE(clock.wrapTimeMs(1), 600.0);
    }
    void fixedAndSpeedTimingMoveTheLoop()
    {
        FrameClock fixed = threeFrames();
        fixed.setTiming({10, 1}, 0); // 3 frames of 100 ms
        QCOMPARE(fixed.nextWrapMs(0), 300.0);
        FrameClock fast = threeFrames();
        fast.setTiming({0, 2}, 0);
        QCOMPARE(fast.nextWrapMs(0), 300.0);
    }
    void aLiveTimingChangeKeepsTheCountAndMovesTheNextWrap()
    {
        FrameClock clock = threeFrames();
        QCOMPARE(clock.update(650).loop, 1LL);
        clock.setTiming({0, 2}, 650); // frame 0 shows at 650; the loop is now 300 ms
        QCOMPARE(clock.loops(650), 1LL);
        QCOMPARE(clock.loops(949), 1LL);
        QCOMPARE(clock.loops(950), 2LL);
        QCOMPARE(clock.nextWrapMs(650), 950.0);
        QVERIFY(clock.wrapTimeMs(2) > 650);
    }
    void pauseFreezesTheCountAndResumeKeepsThePhase()
    {
        FrameClock clock = threeFrames();
        clock.pause(450);
        QCOMPARE(clock.loops(10000), 0LL);
        QCOMPARE(clock.nextWrapMs(10000), -1.0);
        clock.resume(10450); // paused for 10 s, 450 ms into the loop
        QCOMPARE(clock.loops(10450), 0LL);
        QCOMPARE(clock.nextWrapMs(10450), 10600.0);
        QCOMPARE(clock.loops(10600), 1LL);
    }
    void aStillImageNeverWraps()
    {
        FrameClock clock;
        clock.setFrames({100}, 0);
        QCOMPARE(clock.loops(100000), 0LL);
        QCOMPARE(clock.nextWrapMs(0), -1.0);
        FrameClock empty;
        QCOMPARE(empty.nextWrapMs(0), -1.0);
        QCOMPARE(empty.loops(5), 0LL);
    }
    void settingFramesStartsTheCountAgain()
    {
        FrameClock clock = threeFrames();
        QCOMPARE(clock.loops(2000), 3LL);
        clock.setFrames({50, 50}, 2000);
        QCOMPARE(clock.loops(2000), 0LL);
        QCOMPARE(clock.nextWrapMs(2000), 2100.0);
    }
};

QTEST_APPLESS_MAIN(TestPetTiming)
#include "tst_pettiming.moc"
