// Mouse Tracking of one pet as a pure machine (src/overlay/pettracker.h): scripted cursor paths, the clock is a number.

#include "pettracker.h"

#include <QTest>

#include <cmath>
#include <limits>
#include <random>

using namespace murasaki;

namespace {

constexpr PetRect kHome{400, 300, 200, 100}; // centre (500, 350)
constexpr PetRect kOutput{0, 0, 1920, 1080};

PetTrackerConfig look(bool facesLeft = false)
{
    PetTrackerConfig c;
    c.mode = PetTrackingMode::Look;
    c.facesLeft = facesLeft;
    return c;
}

PetTrackerConfig follow(double distance = 160, double speed = 1, bool returnHome = true)
{
    PetTrackerConfig c;
    c.mode = PetTrackingMode::Follow;
    c.distance = distance;
    c.speed = speed;
    c.returnHome = returnHome;
    return c;
}

// Runs the clock from `from` for `ms` with a tick every `every` ms; returns the number of ticks that moved the pet.
int run(PetTracker &t, std::int64_t from, std::int64_t ms, int every = 1)
{
    int moved = 0;
    for (std::int64_t at = from; at < from + ms; at += every) {
        t.tick(at);
        moved += t.moving() ? 1 : 0;
    }
    return moved;
}

double centreX(const PetTracker &t)
{
    return kHome.center().x + t.offset().x;
}

PetTracker make(const PetTrackerConfig &config)
{
    PetTracker t;
    t.setGeometry(kHome, kOutput);
    t.setConfig(config, 0);
    return t;
}

} // namespace

class TstPetTracker : public QObject
{
    Q_OBJECT

private slots:
    void lookIsNothingUntilTheCursorIsSeen();
    void lookMirrorsTowardsTheCursorAndAnItemThatFacesLeftIsTheOtherWayRound();
    void jitterInsideTheDeadZoneChangesNothing();
    void crossingTheDeadZoneFlipsOnceItIsLeft();
    void atMostFourChangesASecond();
    void aHeldBackFlipIsAppliedWhenTheIntervalIsOverAndDroppedIfTheCursorWentBack();
    void nonFiniteCursorsAreIgnored();
    void aCursorOnAnotherOutputStillTurnsThePet();
    void resetAndOffForgetEverything();
    void noModeNoOutputs();
    void followConvergesAndStopsAtTheDistanceWithoutOvershoot();
    void followNeverMovesFasterThanTheCapAndSpeedScalesIt();
    void followStepsAtMostThirtyTimesASecond();
    void followStaysInsideItsOutput();
    void followDoesNotBackAwayFromACloseCursor();
    void followChasesAMovingCursorInAnyDirection();
    void whenFollowingEndsThePetGlidesHomeOrHandsOverItsPlaceOnce();
    void aHeldPetDoesNotMoveAndAParkedOneNeitherAndBothResume();
    void haltEndsItAtOnceAndAdoptFoldsTheOffsetIntoTheStoredPlace();
    void aHoldWhoseReleaseNeverCameDoesNotFreezeIt();
    void aPictureBiggerThanTheOutputDoesNotJump();
    void followIgnoresNonFiniteCursorsAndNeedsOne();
    void lookAndFollowDoNotMixModes();
};

void TstPetTracker::lookIsNothingUntilTheCursorIsSeen()
{
    PetTracker t = make(look());
    QVERIFY(!t.mirror());
    QVERIFY(!t.nextWakeMs());
    t.tick(1000);
    QVERIFY(!t.mirror());
}

void TstPetTracker::lookMirrorsTowardsTheCursorAndAnItemThatFacesLeftIsTheOtherWayRound()
{
    PetTracker right = make(look());
    right.cursor(0, {900, 350});
    QCOMPARE(right.mirror().value(), false); // the item faces right and the cursor is on the right
    PetTracker rightLeft = make(look());
    rightLeft.cursor(0, {100, 350});
    QCOMPARE(rightLeft.mirror().value(), true);
    PetTracker left = make(look(true));
    left.cursor(0, {900, 350});
    QCOMPARE(left.mirror().value(), true);
    PetTracker leftLeft = make(look(true));
    leftLeft.cursor(0, {100, 350});
    QCOMPARE(leftLeft.mirror().value(), false);
}

void TstPetTracker::jitterInsideTheDeadZoneChangesNothing()
{
    PetTracker t = make(look());
    t.cursor(0, {900, 350});
    QCOMPARE(t.mirror().value(), false);
    // 500 is the centre: within 24 px either side asks for nothing, however often it goes from one side to the other
    for (int i = 0; i < 100; ++i)
        t.cursor(1000 + i * 300, {500 + (i % 2 ? 23.0 : -23.0), 350});
    QCOMPARE(t.mirror().value(), false);
    // before any side was asked, the dead zone keeps it undecided
    PetTracker fresh = make(look());
    fresh.cursor(0, {510, 350});
    QVERIFY(!fresh.mirror());
}

void TstPetTracker::crossingTheDeadZoneFlipsOnceItIsLeft()
{
    PetTracker t = make(look());
    t.cursor(0, {900, 350});
    for (double x = 900; x > 520; x -= 20)
        t.cursor(1000, {x, 350});
    QCOMPARE(t.mirror().value(), false); // still on the right side of the zone
    t.cursor(1500, {476, 350});
    QCOMPARE(t.mirror().value(), true);
}

void TstPetTracker::atMostFourChangesASecond()
{
    PetTracker t = make(look());
    int changes = 0;
    std::optional<bool> last;
    for (int ms = 0; ms < 5000; ms += 10) {
        t.cursor(ms, {(ms / 10) % 2 ? 100.0 : 900.0, 350}); // the cursor flies from side to side every 10 ms
        t.tick(ms);
        if (t.mirror() != last) {
            ++changes;
            last = t.mirror();
        }
    }
    QVERIFY2(changes <= 4 * 5 + 1, qPrintable(QString::number(changes)));
    QVERIFY(changes >= 10); // and it does follow
}

void TstPetTracker::aHeldBackFlipIsAppliedWhenTheIntervalIsOverAndDroppedIfTheCursorWentBack()
{
    PetTracker t = make(look());
    t.cursor(1000, {900, 350});
    QCOMPARE(t.mirror().value(), false);
    t.cursor(1100, {100, 350}); // 100 ms after the first: held back
    QCOMPARE(t.mirror().value(), false);
    QCOMPARE(t.nextWakeMs().value(), std::int64_t(1250));
    t.tick(1249);
    QCOMPARE(t.mirror().value(), false);
    t.tick(1250);
    QCOMPARE(t.mirror().value(), true);
    QVERIFY(!t.nextWakeMs());
    // asked and then taken back before the time: nothing happens
    t.cursor(1300, {900, 350});
    t.cursor(1350, {100, 350});
    t.tick(5000);
    QCOMPARE(t.mirror().value(), true);
    QVERIFY(!t.nextWakeMs());
}

void TstPetTracker::nonFiniteCursorsAreIgnored()
{
    PetTracker t = make(look());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    t.cursor(0, {nan, 350});
    t.cursor(1, {900, inf});
    QVERIFY(!t.mirror());
    t.cursor(2, {900, 350});
    t.cursor(3000, {-inf, 0});
    QCOMPARE(t.mirror().value(), false);
}

void TstPetTracker::aCursorOnAnotherOutputStillTurnsThePet()
{
    PetTracker t = make(look());
    t.cursor(0, {-1200, 400}); // on the output to the left
    QCOMPARE(t.mirror().value(), true);
    t.cursor(1000, {4000, -200}); // and far to the right, above
    QCOMPARE(t.mirror().value(), false);
}

void TstPetTracker::resetAndOffForgetEverything()
{
    PetTracker t = make(look());
    t.cursor(0, {100, 350});
    QVERIFY(t.mirror());
    t.reset(10);
    QVERIFY(!t.mirror());
    t.tick(5000);
    QVERIFY(!t.mirror()); // the cursor was forgotten too
    t.cursor(6000, {100, 350});
    QVERIFY(t.mirror());
    PetTrackerConfig off;
    t.setConfig(off, 7000);
    QVERIFY(!t.mirror());
    t.cursor(8000, {100, 350});
    QVERIFY(!t.mirror());
    // the clock never goes backwards
    t.setConfig(look(), 9000);
    t.cursor(2000, {100, 350});
    QVERIFY(t.mirror().value());
}

void TstPetTracker::noModeNoOutputs()
{
    PetTracker t;
    t.setGeometry(kHome, kOutput);
    t.cursor(0, {100, 100});
    t.tick(10);
    QVERIFY(!t.mirror());
    QVERIFY(!t.nextWakeMs());
}

void TstPetTracker::followConvergesAndStopsAtTheDistanceWithoutOvershoot()
{
    PetTracker t = make(follow(160));
    t.cursor(0, {1500, 350});
    double previous = 0;
    for (std::int64_t at = 0; at < 20000; at += 5) {
        t.tick(at);
        QVERIFY2(centreX(t) >= previous - 1e-9, "went backwards");
        QVERIFY2(centreX(t) <= 1500 - 160 + 1e-9, "overshot");
        previous = centreX(t);
    }
    QVERIFY(std::abs(centreX(t) - (1500 - 160)) <= 1.0 + 1e-9); // within the epsilon of the gap
    QCOMPARE(t.offset().y, 0.0);
    QVERIFY(!t.moving());
    QVERIFY(!t.nextWakeMs()); // arrived: nothing is due until the cursor moves
    // a distance of 0 walks onto the cursor
    PetTracker onto = make(follow(0));
    onto.cursor(0, {900, 350});
    run(onto, 0, 20000, 5);
    QVERIFY(std::abs(centreX(onto) - 900) <= 1.0 + 1e-9);
}

void TstPetTracker::followNeverMovesFasterThanTheCapAndSpeedScalesIt()
{
    for (const double speed : {0.5, 1.0, 2.0, 4.0}) {
        PetTracker t = make(follow(0, speed));
        t.cursor(0, {1800, 350});
        double last = centreX(t);
        double peak = 0;
        for (std::int64_t at = 0; at < 4000; at += 11) {
            t.tick(at);
            peak = std::max(peak, centreX(t) - last);
            last = centreX(t);
        }
        // one step is at most cap x the step interval (the pace is held to the cap, the interval is the machine's own)
        QVERIFY2(peak <= 200 * speed * 0.034 + 1e-6, qPrintable(QString::number(peak)));
        QVERIFY2(peak >= 200 * speed * 0.03 * 0.9 || speed == 4.0, qPrintable(QString::number(peak))); // and it does reach the cap far from the goal
    }
}

void TstPetTracker::followStepsAtMostThirtyTimesASecond()
{
    PetTracker t = make(follow(0));
    t.cursor(0, {1800, 350});
    int moved = 0;
    double last = centreX(t);
    for (int at = 0; at < 1000; ++at) { // ticked every millisecond
        t.tick(at);
        if (centreX(t) != last) {
            ++moved;
            last = centreX(t);
        }
    }
    QVERIFY2(moved <= 31, qPrintable(QString::number(moved)));
    QVERIFY(moved >= 25);
    // and a cursor feed at 1 kHz does not step faster either
    PetTracker fed = make(follow(0));
    int steps = 0;
    last = centreX(fed);
    for (int at = 0; at < 1000; ++at) {
        fed.cursor(at, {1800.0 - (at % 3), 350});
        if (centreX(fed) != last) {
            ++steps;
            last = centreX(fed);
        }
    }
    QVERIFY2(steps <= 31, qPrintable(QString::number(steps)));
}

void TstPetTracker::followStaysInsideItsOutput()
{
    PetTracker t = make(follow(0));
    t.cursor(0, {5000, 350}); // far outside, to the right: the pet goes to the edge and stops there
    run(t, 0, 30000, 5);
    QVERIFY(kHome.x + t.offset().x + kHome.w <= kOutput.right() + 1e-9);
    QVERIFY(std::abs(kHome.x + t.offset().x + kHome.w - kOutput.right()) < 0.5);
    QVERIFY(!t.moving()); // stuck at the edge is stopped, not stepping forever
    QVERIFY(!t.nextWakeMs());
    // a cursor above and left of the output: the corner
    PetTracker c = make(follow(0));
    c.cursor(0, {-4000, -4000});
    run(c, 0, 60000, 5);
    QVERIFY(kHome.x + c.offset().x >= -1e-9);
    QVERIFY(kHome.y + c.offset().y >= -1e-9);
    QVERIFY(std::abs(kHome.x + c.offset().x) < 1.0);
    QVERIFY(std::abs(kHome.y + c.offset().y) < 1.0);
}

void TstPetTracker::followDoesNotBackAwayFromACloseCursor()
{
    PetTracker t = make(follow(300));
    t.cursor(0, {520, 350}); // 20 px away, inside the 300 px gap
    run(t, 0, 5000, 5);
    QCOMPARE(t.offset().x, 0.0);
    QCOMPARE(t.offset().y, 0.0);
}

void TstPetTracker::followChasesAMovingCursorInAnyDirection()
{
    PetTracker t = make(follow(100));
    t.cursor(0, {500, 1000});
    run(t, 0, 20000, 5);
    QVERIFY(std::abs(kHome.center().x + t.offset().x - 500) < 1.0);
    QVERIFY(std::abs(kHome.center().y + t.offset().y - (1000 - 100)) < 1.0 + 70); // (the output's bottom edge keeps the window inside: 1080 - 50)
    // the cursor goes round it: the gap is kept, whatever the angle
    PetTracker r = make(follow(100));
    for (std::int64_t at = 0; at < 60000; at += 10) {
        const double angle = at / 3000.0;
        r.cursor(at, {900 + 400 * std::cos(angle), 540 + 300 * std::sin(angle)});
    }
    const double dx = 900 + 400 * std::cos(60000 / 3000.0) - (kHome.center().x + r.offset().x);
    const double dy = 540 + 300 * std::sin(60000 / 3000.0) - (kHome.center().y + r.offset().y);
    QVERIFY(std::hypot(dx, dy) >= 100 - 1.0); // never closer than the gap (it is clamped inside the output, so may stay farther)
}

void TstPetTracker::whenFollowingEndsThePetGlidesHomeOrHandsOverItsPlaceOnce()
{
    // returnHome: it walks back, at the same pace, and arrives exactly
    PetTracker home = make(follow(100));
    home.cursor(0, {1500, 350});
    run(home, 0, 20000, 5);
    QVERIFY(home.offset().x > 800);
    home.setConfig(look(), 20000);
    QVERIFY(home.busy());
    QVERIFY(home.nextWakeMs());
    double last = home.offset().x;
    for (std::int64_t at = 20000; at < 40000; at += 5) {
        home.tick(at);
        QVERIFY(home.offset().x <= last + 1e-9);
        last = home.offset().x;
    }
    QCOMPARE(home.offset().x, 0.0);
    QCOMPARE(home.offset().y, 0.0);
    QVERIFY(!home.busy());
    QVERIFY(!home.takeCommit());
    // no returnHome: the displacement is handed over once, and then it is the owner's
    PetTracker stay = make(follow(100, 1, false));
    stay.cursor(0, {1500, 350});
    run(stay, 0, 20000, 5);
    const double where = stay.offset().x;
    PetTrackerConfig off;
    off.returnHome = false;
    stay.setConfig(off, 20000); // off
    QVERIFY(stay.busy());
    run(stay, 20000, 5000, 5);
    QCOMPARE(stay.offset().x, where); // it stays where it is
    const auto commit = stay.takeCommit();
    QVERIFY(commit);
    QCOMPARE(commit->x, where);
    QVERIFY(!stay.takeCommit()); // once
    QCOMPARE(stay.offset().x, 0.0); // and the stored place carries it now
    QVERIFY(!stay.busy());
    // following again before it is home turns around
    PetTracker again = make(follow(100));
    again.cursor(0, {1500, 350});
    run(again, 0, 5000, 5);
    again.setConfig(look(), 5000);
    run(again, 5000, 300, 5);
    again.setConfig(follow(100), 5400);
    again.cursor(5400, {1500, 350});
    run(again, 5400, 20000, 5);
    QVERIFY(std::abs(centreX(again) - 1400) <= 1.0 + 1e-9);
}

void TstPetTracker::aHeldPetDoesNotMoveAndAParkedOneNeitherAndBothResume()
{
    PetTracker t = make(follow(0));
    t.cursor(0, {1800, 350});
    run(t, 0, 500, 5);
    const double before = t.offset().x;
    QVERIFY(before > 0);
    t.hold(true, 500);
    run(t, 500, 2000, 5);
    QCOMPARE(t.offset().x, before);
    QVERIFY(!t.moving());
    QVERIFY(!t.nextWakeMs());
    t.hold(false, 2500);
    run(t, 2500, 500, 5);
    QVERIFY(t.offset().x > before);
    const double mid = t.offset().x;
    t.park(true, 3000);
    run(t, 3000, 2000, 5);
    QCOMPARE(t.offset().x, mid);
    QVERIFY(t.parked());
    t.park(false, 5000);
    run(t, 5000, 500, 5);
    QVERIFY(t.offset().x > mid);
    // a long pause does not become one huge step when it resumes
    const double resumed = t.offset().x;
    t.hold(true, 6000);
    t.hold(false, 600000);
    t.tick(600000);
    QVERIFY(t.offset().x - resumed <= 200 * 0.1 + 1e-6);
}

void TstPetTracker::haltEndsItAtOnceAndAdoptFoldsTheOffsetIntoTheStoredPlace()
{
    PetTracker t = make(follow(0));
    t.cursor(0, {1800, 350});
    run(t, 0, 1000, 5);
    QVERIFY(t.offset().x > 0);
    t.halt(1000); // returnHome: it is home at once (hidden, arranged, paused)
    QCOMPARE(t.offset().x, 0.0);
    QVERIFY(!t.moving());
    run(t, 1000, 200, 5); // and it follows again when it can: the mode is still follow
    QVERIFY(t.offset().x > 0);
    // without returnHome the place is kept for the owner to store
    PetTracker stay = make(follow(0, 1, false));
    stay.cursor(0, {1800, 350});
    run(stay, 0, 1000, 5);
    const double where = stay.offset().x;
    stay.halt(1000);
    QCOMPARE(stay.offset().x, where);
    QVERIFY(stay.takeCommit());
    // adopt: the user dropped the pet; what was a displacement is the stored place
    PetTracker d = make(follow(0));
    d.cursor(0, {1800, 350});
    run(d, 0, 1000, 5);
    d.adopt();
    QCOMPARE(d.offset().x, 0.0);
    QVERIFY(!d.takeCommit());
}

void TstPetTracker::aHoldWhoseReleaseNeverCameDoesNotFreezeIt()
{
    PetTracker t = make(follow(0));
    t.cursor(0, {1800, 350});
    run(t, 0, 300, 5);
    t.hold(true, 300); // the window that held the pet dies: no release
    const double at = t.offset().x;
    run(t, 300, 500, 5);
    QCOMPARE(t.offset().x, at);
    t.halt(800);
    run(t, 800, 500, 5);
    QVERIFY(t.offset().x > 0); // it walks again
    // reset (arranging, pausing, hiding) lets go too
    PetTracker r = make(follow(0));
    r.cursor(0, {1800, 350});
    r.hold(true, 10);
    r.reset(20);
    r.cursor(30, {1800, 350});
    run(r, 30, 500, 5);
    QVERIFY(r.offset().x > 0);
}

void TstPetTracker::aPictureBiggerThanTheOutputDoesNotJump()
{
    // 2400x1080 from x -240 in a 1920x1080 output (a `cover` fit): no room on either axis
    PetTracker t;
    t.setGeometry({-240, 0, 2400, 1080}, kOutput);
    t.setConfig(follow(0), 0);
    t.cursor(0, {1800, 100});
    run(t, 0, 3000, 5);
    QCOMPARE(t.offset().x, 0.0);
    QCOMPARE(t.offset().y, 0.0);
    // wider only: it walks on the other axis
    PetTracker w;
    w.setGeometry({-240, 100, 2400, 400}, kOutput);
    w.setConfig(follow(0), 0);
    w.cursor(0, {1800, 900});
    run(w, 0, 20000, 5);
    QCOMPARE(w.offset().x, 0.0);
    QVERIFY(w.offset().y > 0);
    QVERIFY(100 + w.offset().y + 400 <= 1080 + 1e-9);
}

void TstPetTracker::followIgnoresNonFiniteCursorsAndNeedsOne()
{
    PetTracker t = make(follow(0));
    run(t, 0, 1000, 5);
    QCOMPARE(t.offset().x, 0.0); // no cursor yet
    t.cursor(1000, {std::numeric_limits<double>::quiet_NaN(), 5});
    run(t, 1000, 1000, 5);
    QCOMPARE(t.offset().x, 0.0);
    t.cursor(2000, {1800, 350});
    run(t, 2000, 500, 5);
    QVERIFY(t.offset().x > 0);
    t.cursor(2500, {std::numeric_limits<double>::infinity(), 5}); // the last good one still counts
    const double at = t.offset().x;
    run(t, 2500, 500, 5);
    QVERIFY(t.offset().x > at);
}

void TstPetTracker::lookAndFollowDoNotMixModes()
{
    PetTracker t = make(look());
    t.cursor(0, {1800, 350});
    run(t, 0, 2000, 5);
    QCOMPARE(t.offset().x, 0.0); // look never moves the pet
    PetTracker f = make(follow());
    f.cursor(0, {100, 350});
    run(f, 0, 2000, 5);
    QVERIFY(!f.mirror()); // and follow never turns it
}

QTEST_APPLESS_MAIN(TstPetTracker)
#include "tst_pettracker.moc"
