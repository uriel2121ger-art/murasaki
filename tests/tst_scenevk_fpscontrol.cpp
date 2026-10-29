// murasaki-scene-vk adaptive frame rate and resolution: the controllers that step the rate and the resolution down when
// the GPU cannot keep up and back up with hysteresis. Pure logic, runs everywhere.

#include "engine/fpscontrol.h"

#include <QTest>

using murasaki::scene::FpsController;
using murasaki::scene::ScaleController;

namespace {
// Feeds `frames` frames of `ms` GPU time; returns the last rate change, if any.
std::optional<int> feed(FpsController &c, double ms, int frames)
{
    std::optional<int> last;
    for (int i = 0; i < frames; ++i) {
        if (const auto changed = c.update(ms)) {
            last = changed;
        }
    }
    return last;
}

std::optional<double> feedLoad(ScaleController &c, double load, int frames, bool atRequested = true)
{
    std::optional<double> last;
    for (int i = 0; i < frames; ++i) {
        if (const auto changed = c.update(load, atRequested)) {
            last = changed;
        }
    }
    return last;
}
} // namespace

class TstSceneVkFpsControl : public QObject
{
    Q_OBJECT
private slots:
    void aLightSceneKeepsTheRequestedRate()
    {
        FpsController c(30);
        QVERIFY(!feed(c, 5.0, 1000).has_value());
        QCOMPARE(c.current(), 30);
    }
    void aSceneThatDoesNotFitStepsDown()
    {
        FpsController c(60);
        // 25 ms of GPU per frame cannot be done at 60 fps (16.7 ms) nor at 45 (22.2 ms); 30 fps leaves 33 ms
        feed(c, 25.0, 200);
        QCOMPARE(c.current(), 30);
        // and at 30 fps it is a load of 0.75: it stays
        QVERIFY(!feed(c, 25.0, 1000).has_value());
        QCOMPARE(c.current(), 30);
    }
    void neverBelowTheLastStep()
    {
        FpsController c(30);
        feed(c, 200.0, 2000);
        QCOMPARE(c.current(), 20);
    }
    void neverAboveTheRequest()
    {
        FpsController c(24);
        feed(c, 100.0, 500);
        QCOMPARE(c.current(), 20);
        feed(c, 1.0, 2000);
        QCOMPARE(c.current(), 24);
    }
    void stepsBackUpOnlyWhenTheHigherRateWouldFit()
    {
        FpsController c(60);
        feed(c, 40.0, 500);
        QCOMPARE(c.current(), 20);
        // 12 ms would be a load of 0.36 at 30 fps (under 0.55): back up to 30 ...
        feed(c, 12.0, 150);
        QCOMPARE(c.current(), 30);
        // ... and 0.54 at 45 fps (under 0.55) too
        feed(c, 12.0, 150);
        QCOMPARE(c.current(), 45);
        // at 60 fps 12 ms is 0.72 of 16.7: it must not go there
        feed(c, 12.0, 1000);
        QCOMPARE(c.current(), 45);
    }
    void aShortSpikeDoesNotStepDown()
    {
        FpsController c(30);
        feed(c, 5.0, 100);
        feed(c, 80.0, 3); // one hitch, e.g. a shader compile
        QVERIFY(!feed(c, 5.0, 100).has_value());
        QCOMPARE(c.current(), 30);
    }
    void hysteresisAvoidsFlapping()
    {
        FpsController c(60);
        // 14 ms: at 60 fps the load is 0.84 (just under the 0.85 down threshold)
        feed(c, 14.0, 2000);
        QCOMPARE(c.current(), 60);
        // 15 ms steps down to 45 (0.9 > 0.85) and stays: at 60 it would be 0.9, not below 0.55
        feed(c, 15.0, 2000);
        QCOMPARE(c.current(), 45);
        feed(c, 15.0, 5000);
        QCOMPARE(c.current(), 45);
    }
    void aNewRequestRestartsTheLadder()
    {
        FpsController c(60);
        feed(c, 40.0, 500);
        QCOMPARE(c.current(), 20);
        c.setRequested(30);
        QCOMPARE(c.current(), 30);
        QCOMPARE(c.requested(), 30);
    }
    void aLightSceneKeepsTheCeilingResolution()
    {
        ScaleController c(1.0);
        QVERIFY(!feedLoad(c, 0.10, 2000).has_value());
        QCOMPARE(c.current(), 1.0);
    }
    void aHeavySceneLosesResolutionDownToTheFloor()
    {
        ScaleController c(1.0);
        feedLoad(c, 0.6, 1000);
        QCOMPARE(c.current(), 0.5);
        QVERIFY(c.atFloor());
        QVERIFY(!feedLoad(c, 0.9, 1000).has_value()); // nothing below the floor
    }
    void theCeilingScalesTheSteps()
    {
        ScaleController c(0.75);
        QCOMPARE(c.current(), 0.75);
        feedLoad(c, 0.6, 1000);
        QCOMPARE(c.current(), 0.375);
    }
    void justAboveTheBudgetStepsOnce()
    {
        ScaleController c(1.0);
        feedLoad(c, 0.28, 48);
        QCOMPARE(c.current(), 0.85);
        // the next window is at the same load: another step, then it fits only if the cost fell
        QVERIFY(feedLoad(c, 0.20, 1000) == std::nullopt || c.current() >= 0.85);
    }
    void resolutionComesBackWhenThereIsRoom()
    {
        ScaleController c(1.0);
        feedLoad(c, 0.6, 1000);
        QCOMPARE(c.current(), 0.5);
        // 0.05 at half scale predicts 0.05 * (0.55/0.5)^2 = 0.06 at the next step: well under the budget
        feedLoad(c, 0.05, 240);
        QCOMPARE(c.current(), 0.55);
        feedLoad(c, 0.05, 2000);
        QCOMPARE(c.current(), 1.0);
    }
    void resolutionStaysDownWhileTheFrameRateIsHeldBack()
    {
        ScaleController c(1.0);
        feedLoad(c, 0.6, 1000);
        QVERIFY(!feedLoad(c, 0.01, 2000, false).has_value());
        QCOMPARE(c.current(), 0.5);
    }
    void aBudgetOfZeroTurnsItOff()
    {
        ScaleController::Config config;
        config.budget = 0.0;
        ScaleController c(1.0, config);
        QVERIFY(!c.enabled());
        QVERIFY(!feedLoad(c, 5.0, 1000).has_value());
        QCOMPARE(c.current(), 1.0);
    }
    void noFlappingAtTheEdge()
    {
        ScaleController c(1.0);
        // at 1.0 the load is 0.30 (over 0.25): step down to 0.85, where it is 0.30*0.7225 = 0.217
        feedLoad(c, 0.30, 48);
        QCOMPARE(c.current(), 0.85);
        // 0.217 predicts 0.30 at the step up: not under budget * 0.7, so it stays
        QVERIFY(!feedLoad(c, 0.217, 3000).has_value());
        QCOMPARE(c.current(), 0.85);
    }
    // The GPU time of a frame includes the time the GPU spent on other clients' work (another output's scene, the compositor
    // while a window is dragged): a second of that is no reason to draw less often. What matters is most of the frames.
    void aBurstOfContentionDoesNotStepDown()
    {
        FpsController c(30);
        feed(c, 5.0, 200);
        for (int round = 0; round < 20; ++round) {
            feed(c, 45.0, 20); // two thirds of a second of waiting for the GPU (over 0.85 of the 33 ms interval)
            feed(c, 5.0, 30);
        }
        QCOMPARE(c.current(), 30);
    }
    void aSceneThatIsSlowMostOfTheTimeStillStepsDown()
    {
        FpsController c(60);
        // two frames in three take 25 ms (over 0.85 of 16.7 ms), the third 5 ms: the median is 25, the scene does not fit
        for (int i = 0; i < 300; ++i) {
            c.update(i % 3 == 2 ? 5.0 : 25.0);
        }
        QVERIFY(c.current() < 60);
    }
    // A rate that steps up only to step down again soon waits longer before the next try: no 30 / 20 / 30 every few seconds.
    void aRateThatFallsBackSoonWaitsLongerToComeBack()
    {
        FpsController c(30);
        int changes = 0;
        // light for 6 s (enough to step up), then 3 s of a load that does not fit at 30: a cycle of 9 s, ten times
        for (int cycle = 0; cycle < 10; ++cycle) {
            for (int i = 0; i < 180; ++i) {
                changes += c.update(5.0).has_value();
            }
            for (int i = 0; i < 90; ++i) {
                changes += c.update(40.0).has_value();
            }
        }
        // without the back-off: a step down and a step up each cycle (20 changes)
        QVERIFY2(changes <= 10, qPrintable(QString::number(changes)));
    }
    void aStableRateForgetsTheBackOff()
    {
        FpsController c(30);
        for (int cycle = 0; cycle < 4; ++cycle) {
            feed(c, 5.0, 180);
            feed(c, 40.0, 90);
        }
        // a long light stretch: back to 30 within the back-off ...
        feed(c, 5.0, 150 * 8);
        QCOMPARE(c.current(), 30);
        // ... and after a long stable time a new fall comes back after the ordinary wait again
        feed(c, 5.0, 150 * 16);
        feed(c, 40.0, 90);
        QCOMPARE(c.current(), 20);
        feed(c, 5.0, 150);
        QCOMPARE(c.current(), 30);
    }
    void theResolutionIgnoresABurstOfContentionToo()
    {
        ScaleController c(1.0);
        feedLoad(c, 0.10, 300);
        for (int round = 0; round < 20; ++round) {
            feedLoad(c, 0.9, 20);
            feedLoad(c, 0.10, 30);
        }
        QCOMPARE(c.current(), 1.0);
    }
    void theResolutionBacksOffToo()
    {
        ScaleController c(1.0);
        int changes = 0;
        for (int cycle = 0; cycle < 10; ++cycle) {
            for (int i = 0; i < 300; ++i) {
                changes += c.update(0.10).has_value();
            }
            for (int i = 0; i < 100; ++i) {
                changes += c.update(0.6).has_value();
            }
        }
        QVERIFY2(changes <= 10, qPrintable(QString::number(changes)));
    }
    // A scene far over the budget goes straight to the step that fits (the cost follows the pixel count, the square of the
    // scale), not one step per window: the user sees one change, seconds earlier.
    void aSceneFarOverTheBudgetJumpsToTheStepThatFits()
    {
        ScaleController c(1.0);
        int changes = 0;
        // 0.6 at full size predicts 0.29 at 0.7 (over 0.25) and 0.18 at 0.55: there, at once
        for (int i = 0; i < 48; ++i) {
            changes += c.update(0.6).has_value();
        }
        QCOMPARE(changes, 1);
        QCOMPARE(c.current(), 0.55);
        // a load no step fits goes to the floor at once
        ScaleController d(1.0);
        feedLoad(d, 2.0, 48);
        QCOMPARE(d.current(), 0.5);
    }
    void aRateFarOverTheIntervalJumpsToTheRateThatFits()
    {
        FpsController c(60);
        int changes = 0;
        // 25 ms is 1.5 of 60 fps' interval and 1.125 of 45's, 0.75 of 30's: 30 at once
        for (int i = 0; i < 48; ++i) {
            changes += c.update(25.0).has_value();
        }
        QCOMPARE(changes, 1);
        QCOMPARE(c.current(), 30);
    }
    void negativeTimesAreIgnored()
    {
        FpsController c(30);
        QVERIFY(!feed(c, -1.0, 500).has_value());
        QCOMPARE(c.current(), 30);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkFpsControl)
#include "tst_scenevk_fpscontrol.moc"
