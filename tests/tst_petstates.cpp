// The states of a pet as a pure machine (src/overlay/petstatemachine.h) and the values it reads (src/core/petstates.h): every row of the plan's transition
// table with scripted events, the sequences, the fallbacks, the clock, and a property test over random event streams. The clock is a number.

#include "petinteraction.h"
#include "petstatemachine.h"
#include "petstates.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>
#include <QTest>

#include <cmath>
#include <limits>
#include <random>

using namespace murasaki;
using namespace Qt::StringLiterals;

namespace {

PetStates statesFrom(const QByteArray &json)
{
    QString error;
    const auto states = applyPetStatesChanges(PetStates{}, QJsonDocument::fromJson(json).object(), &error);
    if (!states)
        qWarning("bad test states: %s", qPrintable(error));
    return states.value_or(PetStates{});
}

PetStateMachine make(const PetStates &states, std::uint32_t seed = 1)
{
    PetStateMachine m({}, seed);
    m.setBaseVariant(u"base"_s, 0);
    m.setStates(states, 0);
    return m;
}

const PetPoint kPress{100, 100};

PetStates fourStates()
{
    return statesFrom(R"({"idle":{"variant":"i"},"grabbed":{"variant":"g"},"moving":{"variant":"m"},"released":{"variant":"r"}})");
}

QString sequenceJson(int count)
{
    QStringList items;
    for (int i = 0; i < count; ++i)
        items << u"\"v%1\""_s.arg(i);
    return u"{\"idle\":{\"sequence\":{\"items\":[%1]}}}"_s.arg(items.join(u','));
}

} // namespace

class TstPetStates : public QObject
{
    Q_OBJECT

private slots:
    void aPetWithoutStatesShowsItsVariantInEveryState();
    void theTransitionTable();
    void aPressThatStartsNoDragChangesNothing();
    void movingNeedsSixPixelsAndFallsBackAfterRest();
    void releasedLastsOnePassOrTwelveHundredMs();
    void aDodgeStepShowsMoving();
    void fallbacks();
    void resetGoesToIdle();
    void aPreviewShowsAStateForAWhile();
    void releasedAndTheHoldsAreCountedFromTheFirstFrame();
    void aSuspendedMachineWaitsForNothingAndResumesWithoutCatchingUp();
    void peekingSaysWhatAStateWouldShow();
    void aPreviewNeverBreaksADragAndADodgeEndsIt();
    void linearAutoSequenceAdvancesAndWraps();
    void randomNeverRepeatsAndCoversAll();
    void clickSequence();
    void playOnceHoldsTheLastFrame();
    void autoLinearOnceStopsAtTheLastEntry();
    void aMissingVariantIsSkipped();
    void theClockNeverGoesBackwardsAndSurvivesHugeJumps();
    void nonFinitePositionsAreIgnored();
    void sequencesAreSixteenLong();
    void changingTheStatesStartsOver();
    void petInteractionDoesNotDependOnTheMachine();
    void propertyNoStreamLeavesTheFourStates();
    void theJsonRoundTripsAndRefusesTypos();
    void trackingJson();
    void theNamesAreInTheDocument();
};

void TstPetStates::aPetWithoutStatesShowsItsVariantInEveryState()
{
    PetStateMachine m;
    m.setBaseVariant(u"base"_s, 0);
    QCOMPARE(m.variant(), u"base"_s);
    QCOMPARE(m.sequenceIndex(), -1);
    QCOMPARE(m.source(), u"variant"_s);
    m.press(10, kPress, true);
    QCOMPARE(m.state(), PetState::Grabbed);
    QCOMPARE(m.variant(), u"base"_s);
    m.motion(20, {120, 100});
    QCOMPARE(m.state(), PetState::Moving);
    QCOMPARE(m.variant(), u"base"_s);
    m.release(30);
    QCOMPARE(m.state(), PetState::Released);
    QCOMPARE(m.variant(), u"base"_s);
    m.tick(5000);
    QCOMPARE(m.state(), PetState::Idle);
    QCOMPARE(m.variant(), u"base"_s);
    QVERIFY(!m.owner());
}

void TstPetStates::theTransitionTable()
{
    PetStateMachine m = make(fourStates());
    QCOMPARE(m.state(), PetState::Idle);
    QCOMPARE(m.variant(), u"i"_s);
    // idle -> grabbed
    m.press(100, kPress, true);
    QCOMPARE(m.state(), PetState::Grabbed);
    QCOMPARE(m.variant(), u"g"_s);
    // grabbed -> moving
    m.motion(110, {110, 100});
    QCOMPARE(m.state(), PetState::Moving);
    QCOMPARE(m.variant(), u"m"_s);
    // moving -> grabbed after 300 ms without motion, button still down
    m.tick(409);
    QCOMPARE(m.state(), PetState::Moving);
    m.tick(410);
    QCOMPARE(m.state(), PetState::Grabbed);
    // grabbed -> released
    m.release(500);
    QCOMPARE(m.state(), PetState::Released);
    QCOMPARE(m.variant(), u"r"_s);
    // released -> idle when its time is up
    m.tick(1699);
    QCOMPARE(m.state(), PetState::Released);
    m.tick(1700);
    QCOMPARE(m.state(), PetState::Idle);
    QCOMPARE(m.variant(), u"i"_s);
    // moving -> released directly
    m.press(2000, kPress, true);
    m.motion(2010, {200, 100});
    QCOMPARE(m.state(), PetState::Moving);
    m.release(2020);
    QCOMPARE(m.state(), PetState::Released);
    // a new press during released grabs again
    m.press(2100, kPress, true);
    QCOMPARE(m.state(), PetState::Grabbed);
}

void TstPetStates::aPressThatStartsNoDragChangesNothing()
{
    PetStateMachine m = make(fourStates());
    const unsigned generation = m.generation();
    m.press(10, kPress, false);
    m.motion(20, {300, 300});
    m.release(30);
    m.click(40);
    QCOMPARE(m.state(), PetState::Idle);
    QCOMPARE(m.generation(), generation);
}

void TstPetStates::movingNeedsSixPixelsAndFallsBackAfterRest()
{
    PetStateMachine m = make(fourStates());
    m.press(0, kPress, true);
    m.motion(10, {105, 100});
    QCOMPARE(m.state(), PetState::Grabbed); // 5 px
    m.motion(20, {105.9, 100});
    QCOMPARE(m.state(), PetState::Grabbed);
    m.motion(30, {106, 100});
    QCOMPARE(m.state(), PetState::Moving);
    // motion keeps it moving
    for (int t = 100; t < 1000; t += 100)
        m.motion(t, {106 + t / 10.0, 100});
    QCOMPARE(m.state(), PetState::Moving);
    // rest, then small jitter does not make it move again; a real move from where it rested does
    m.tick(1400);
    QCOMPARE(m.state(), PetState::Grabbed);
    m.motion(1410, {196, 101});
    QCOMPARE(m.state(), PetState::Grabbed);
    m.motion(1420, {210, 100});
    QCOMPARE(m.state(), PetState::Moving);
}

void TstPetStates::releasedLastsOnePassOrTwelveHundredMs()
{
    // unknown pass: 1.2 s
    {
        PetStateMachine m = make(fourStates());
        m.press(0, kPress, true);
        m.release(100);
        m.tick(1299);
        QCOMPARE(m.state(), PetState::Released);
        m.tick(1300);
        QCOMPARE(m.state(), PetState::Idle);
    }
    // a finite animation of 800 ms
    {
        PetStateMachine m = make(fourStates());
        m.press(0, kPress, true);
        m.release(100);
        m.setAnimation(800, false, 110);
        m.tick(899);
        QCOMPARE(m.state(), PetState::Released);
        m.tick(900);
        QCOMPARE(m.state(), PetState::Idle);
    }
    // every file loops here, and a pass that is known is what released lasts, whatever the file's own loop count says
    {
        PetStateMachine m = make(fourStates());
        m.press(0, kPress, true);
        m.release(100);
        m.setAnimation(5000, true, 110);
        m.tick(5099);
        QCOMPARE(m.state(), PetState::Released);
        m.tick(5100);
        QCOMPARE(m.state(), PetState::Idle);
    }
    // an absurd pass is clamped (never stuck), a tiny one is not a flash
    {
        PetStateMachine m = make(fourStates());
        m.press(0, kPress, true);
        m.release(0);
        m.setAnimation(3600000, false, 0);
        m.tick(9999);
        QCOMPARE(m.state(), PetState::Released);
        m.tick(10000);
        QCOMPARE(m.state(), PetState::Idle);
        m.press(20000, kPress, true);
        m.release(20000);
        m.setAnimation(20, false, 20000);
        m.tick(20199);
        QCOMPARE(m.state(), PetState::Released);
        m.tick(20200);
        QCOMPARE(m.state(), PetState::Idle);
    }
}

void TstPetStates::aDodgeStepShowsMoving()
{
    PetStateMachine m = make(fourStates());
    m.dodgeStep(1000, 400);
    QCOMPARE(m.state(), PetState::Moving);
    QCOMPARE(m.variant(), u"m"_s);
    QCOMPARE(m.nextWakeMs().value_or(-1), 1400);
    m.dodgeStep(1200, 400); // the next step extends it
    m.tick(1500);
    QCOMPARE(m.state(), PetState::Moving);
    m.tick(1600);
    QCOMPARE(m.state(), PetState::Idle);
    // a dodge never overrides a drag
    m.press(2000, kPress, true);
    m.dodgeStep(2010, 400);
    QCOMPARE(m.state(), PetState::Grabbed);
    m.release(2020);
    // nor a released pet
    m.dodgeStep(2030, 400);
    QCOMPARE(m.state(), PetState::Released);
    // a step longer than the cap is capped
    m.tick(10000);
    m.dodgeStep(10000, 1000000);
    m.tick(14999);
    QCOMPARE(m.state(), PetState::Moving);
    m.tick(15000);
    QCOMPARE(m.state(), PetState::Idle);
}

void TstPetStates::fallbacks()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"variant":"i"},"grabbed":{"variant":"g"}})"));
    m.press(0, kPress, true);
    m.motion(10, {200, 100});
    QCOMPARE(m.state(), PetState::Moving);
    QCOMPARE(m.variant(), u"g"_s); // moving falls back to grabbed
    QCOMPARE(m.owner().value(), PetState::Grabbed);
    m.release(20);
    QCOMPARE(m.variant(), u"i"_s); // released falls back to idle
    QCOMPARE(m.owner().value(), PetState::Idle);
    // idle without an entry shows the placement's variant; grabbed only
    PetStateMachine g = make(statesFrom(R"({"grabbed":{"variant":"g"}})"));
    QCOMPARE(g.variant(), u"base"_s);
    g.press(0, kPress, true);
    QCOMPARE(g.variant(), u"g"_s);
    g.release(10);
    QCOMPARE(g.variant(), u"base"_s);
    // changing the placement's variant shows at once while idle
    g.tick(5000);
    g.setBaseVariant(u"other"_s, 5000);
    QCOMPARE(g.variant(), u"other"_s);
}

void TstPetStates::resetGoesToIdle()
{
    PetStateMachine m = make(fourStates());
    m.press(0, kPress, true);
    m.motion(5, {300, 100});
    QCOMPARE(m.state(), PetState::Moving);
    m.reset(10);
    QCOMPARE(m.state(), PetState::Idle);
    // the drag is over: motion does nothing now
    m.motion(20, {500, 100});
    QCOMPARE(m.state(), PetState::Idle);
    m.release(30);
    QCOMPARE(m.state(), PetState::Idle);
    // a dodge in progress ends too
    m.dodgeStep(40, 400);
    m.reset(50);
    QCOMPARE(m.state(), PetState::Idle);
    QVERIFY(!m.nextWakeMs());
    // idle's sequence keeps its place across a reset
    PetStateMachine s = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"holdMs":1000}},"grabbed":{"variant":"g"}})"));
    s.tick(1000);
    s.tick(2000);
    QCOMPARE(s.variant(), u"c"_s);
    s.press(2100, kPress, true);
    QCOMPARE(s.variant(), u"g"_s);
    s.reset(2200);
    QCOMPARE(s.variant(), u"c"_s);
    // but leaving by a release starts it over (released falls back on idle's sequence, which plays on through it)
    s.press(2300, kPress, true);
    s.release(2310);
    QCOMPARE(s.state(), PetState::Released);
    QCOMPARE(s.variant(), u"a"_s);
    s.tick(3510);
    QCOMPARE(s.state(), PetState::Idle);
    QCOMPARE(s.variant(), u"b"_s); // a second after the start, as it would have been
}

void TstPetStates::aPreviewShowsAStateForAWhile()
{
    PetStateMachine m = make(fourStates());
    m.preview(PetState::Moving, 1000, 3000); // moving does not fall back to grabbed: nobody holds the pet
    QCOMPARE(m.state(), PetState::Moving);
    QCOMPARE(m.variant(), u"m"_s);
    m.tick(3999);
    QCOMPARE(m.state(), PetState::Moving);
    m.tick(4000);
    QCOMPARE(m.state(), PetState::Idle);
    m.preview(PetState::Grabbed, 5000, 2000);
    QCOMPARE(m.variant(), u"g"_s);
    // a real press takes over, and the preview's end does not pull the pet out of the drag
    m.press(5500, kPress, true);
    m.tick(7500);
    QCOMPARE(m.state(), PetState::Grabbed);
    m.release(7600);
    m.tick(20000);
    QCOMPARE(m.state(), PetState::Idle);
    // a released preview ends by itself, and reset ends any
    m.preview(PetState::Released, 30000, 60000);
    m.tick(31200);
    QCOMPARE(m.state(), PetState::Idle);
    m.preview(PetState::Grabbed, 40000, 5000);
    m.reset(40100);
    QCOMPARE(m.state(), PetState::Idle);
    QVERIFY(!m.nextWakeMs());
}

void TstPetStates::releasedAndTheHoldsAreCountedFromTheFirstFrame()
{
    // released of 600 ms; the window needs 300 ms to show its first frame: the animation is seen for its 600 ms
    PetStateMachine m = make(fourStates());
    m.press(0, kPress, true);
    m.release(1000);
    m.setAnimation(600, true, 1000);
    m.tick(1299);
    QCOMPARE(m.state(), PetState::Released);
    m.animationStarted(1300);
    m.tick(1600); // (it would have ended here, counted from the order to load the file)
    QCOMPARE(m.state(), PetState::Released);
    m.tick(1899);
    QCOMPARE(m.state(), PetState::Released);
    m.tick(1900);
    QCOMPARE(m.state(), PetState::Idle);
    // an automatic sequence waits holdMs from the first frame; a once pass ends a pass after it
    PetStateMachine s = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b"],"holdMs":1000}}})"));
    s.animationStarted(400);
    s.tick(1000);
    QCOMPARE(s.variant(), u"a"_s);
    s.tick(1400);
    QCOMPARE(s.variant(), u"b"_s);
    PetStateMachine o = make(statesFrom(R"({"idle":{"sequence":{"items":["a"],"advance":"click","once":true}}})"));
    o.setAnimation(500, true, 0);
    o.animationStarted(300);
    o.tick(799);
    QVERIFY(!o.holding());
    o.tick(800);
    QVERIFY(o.holding());
    // a dodge step is shown for its length from the first frame
    PetStateMachine d = make(fourStates());
    d.dodgeStep(100, 220);
    d.animationStarted(400);
    d.tick(619);
    QCOMPARE(d.state(), PetState::Moving);
    d.tick(620);
    QCOMPARE(d.state(), PetState::Idle);
}

void TstPetStates::aSuspendedMachineWaitsForNothingAndResumesWithoutCatchingUp()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"holdMs":1000}}})"));
    m.setSuspended(true, 100);
    QVERIFY(!m.nextWakeMs());
    m.tick(10000);
    QCOMPARE(m.variant(), u"a"_s); // nothing advanced while nobody saw it
    m.setSuspended(false, 10000);
    QCOMPARE(m.nextWakeMs().value(), std::int64_t(11000)); // counted from the resume
    m.tick(10999);
    QCOMPARE(m.variant(), u"a"_s);
    m.tick(11000);
    QCOMPARE(m.variant(), u"b"_s);
    // released and a held pet are not waited for either
    PetStateMachine r = make(fourStates());
    r.press(0, kPress, true);
    r.release(10);
    r.setSuspended(true, 20);
    r.tick(100000);
    QCOMPARE(r.state(), PetState::Released);
    QVERIFY(!r.nextWakeMs());
    r.reset(100001);
    r.setSuspended(false, 100001);
    QCOMPARE(r.state(), PetState::Idle);
}

void TstPetStates::aPreviewNeverBreaksADragAndADodgeEndsIt()
{
    PetStateMachine m = make(fourStates());
    m.press(0, kPress, true);
    QVERIFY(!m.preview(PetState::Released, 10, 3000)); // the user holds the pet
    QCOMPARE(m.state(), PetState::Grabbed);
    m.motion(20, {300, 100});
    QCOMPARE(m.state(), PetState::Moving); // and the drag goes on
    m.release(30);
    QCOMPARE(m.state(), PetState::Released);
    m.tick(5000);
    // a preview of grabbed, then a dodge step: the step is shown and ends it
    QVERIFY(m.preview(PetState::Grabbed, 6000, 3000));
    QVERIFY(m.previewing());
    m.dodgeStep(6100, 220);
    QVERIFY(!m.previewing());
    QCOMPARE(m.state(), PetState::Moving);
    m.tick(6320);
    QCOMPARE(m.state(), PetState::Idle);
    // the previewed released ends by itself and takes the preview with it: a later step shows
    QVERIFY(m.preview(PetState::Released, 10000, 60000));
    m.tick(11200);
    QCOMPARE(m.state(), PetState::Idle);
    QVERIFY(!m.previewing());
    m.dodgeStep(12000, 220);
    QCOMPARE(m.state(), PetState::Moving);
}

void TstPetStates::peekingSaysWhatAStateWouldShow()
{
    PetStateMachine m = make(statesFrom(R"({"grabbed":{"variant":"g"},"moving":{"sequence":{"items":["m1","m2"]}},"released":{"sequence":{"items":["r1","r2"],"order":"random"}}})"));
    QCOMPARE(m.peekVariant(PetState::Idle).value(), u"base"_s);
    QCOMPARE(m.peekVariant(PetState::Grabbed).value(), u"g"_s);
    QCOMPARE(m.peekVariant(PetState::Moving).value(), u"m1"_s);
    QVERIFY(!m.peekVariant(PetState::Released)); // random: not known beforehand
    const unsigned generation = m.generation();
    m.peekVariant(PetState::Moving);
    QCOMPARE(m.generation(), generation); // peeking changes nothing
    QCOMPARE(m.likelyNext(), (std::vector<PetState>{PetState::Grabbed, PetState::Moving}));
    m.press(0, kPress, true);
    QCOMPARE(m.likelyNext(), (std::vector<PetState>{PetState::Moving, PetState::Released, PetState::Idle}));
    // a click sequence: where it stands
    PetStateMachine c = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"advance":"click"}}})"));
    c.click(10);
    QCOMPARE(c.peekVariant(PetState::Idle).value(), u"b"_s);
    QCOMPARE(c.peekVariant(PetState::Grabbed).value(), u"b"_s); // (grabbed falls back to idle)
}

void TstPetStates::linearAutoSequenceAdvancesAndWraps()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"holdMs":1000}}})"));
    QCOMPARE(m.variant(), u"a"_s);
    QCOMPARE(m.sequenceIndex(), 0);
    QCOMPARE(m.source(), u"sequence:0"_s);
    QCOMPARE(m.nextWakeMs().value_or(-1), 1000);
    m.tick(999);
    QCOMPARE(m.variant(), u"a"_s);
    m.tick(1000);
    QCOMPARE(m.variant(), u"b"_s);
    m.tick(2000);
    QCOMPARE(m.variant(), u"c"_s);
    m.tick(3000);
    QCOMPARE(m.variant(), u"a"_s);
    QCOMPARE(m.sequenceIndex(), 0);
    // a finite animation advances after one pass instead of holdMs
    m.setAnimation(300, false, 3010);
    m.tick(3299);
    QCOMPARE(m.variant(), u"a"_s);
    m.tick(3300);
    QCOMPARE(m.variant(), u"b"_s);
    // a looping one with a known pass still waits holdMs
    m.setAnimation(300, true, 3320);
    m.tick(4299);
    QCOMPARE(m.variant(), u"b"_s);
    m.tick(4300);
    QCOMPARE(m.variant(), u"c"_s);
}

void TstPetStates::randomNeverRepeatsAndCoversAll()
{
    for (const std::uint32_t seed : {1u, 2u, 77u, 12345u}) {
        PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c","d"],"order":"random","holdMs":500}}})"), seed);
        QSet<QString> seen{m.variant()};
        QString previous = m.variant();
        for (int i = 1; i <= 400; ++i) {
            m.tick(i * 500);
            QVERIFY2(m.variant() != previous, "a random sequence repeated an entry");
            previous = m.variant();
            seen.insert(previous);
        }
        QCOMPARE(seen.size(), 4);
    }
    // one entry: it stays, it does not spin
    PetStateMachine one = make(statesFrom(R"({"idle":{"sequence":{"items":["only"],"order":"random","holdMs":500}}})"));
    one.tick(100000);
    QCOMPARE(one.variant(), u"only"_s);
    // the same seed gives the same draws
    PetStateMachine a = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"order":"random","holdMs":500}}})"), 9);
    PetStateMachine b = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"order":"random","holdMs":500}}})"), 9);
    for (int i = 1; i <= 20; ++i) {
        a.tick(i * 500);
        b.tick(i * 500);
        QCOMPARE(a.variant(), b.variant());
    }
}

void TstPetStates::clickSequence()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"advance":"click"}}})"));
    QCOMPARE(m.variant(), u"a"_s);
    QVERIFY(!m.nextWakeMs()); // nothing advances by itself
    m.tick(100000);
    QCOMPARE(m.variant(), u"a"_s);
    m.click(100001);
    QCOMPARE(m.variant(), u"b"_s);
    m.click(100002);
    QCOMPARE(m.variant(), u"c"_s);
    m.click(100003);
    QCOMPARE(m.variant(), u"a"_s);
    // a click is a press and a release: the pet goes through grabbed and released, and the click sequence keeps the place the click gave it
    m.press(200000, kPress, true);
    m.release(200010);
    m.click(200011); // moves the idle sequence on, for the idle the pet goes back to
    m.tick(205000);
    QCOMPARE(m.state(), PetState::Idle);
    QCOMPARE(m.variant(), u"b"_s);
    // an automatic sequence ignores clicks
    PetStateMachine autoMachine = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b"],"holdMs":1000}}})"));
    autoMachine.click(10);
    QCOMPARE(autoMachine.variant(), u"a"_s);
}

void TstPetStates::playOnceHoldsTheLastFrame()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b"],"advance":"click","once":true}}})"));
    QVERIFY(!m.holding());
    QVERIFY(!m.nextWakeMs()); // the pass length is not known yet
    m.setAnimation(700, true, 0);
    QCOMPARE(m.nextWakeMs().value_or(-1), 700);
    m.tick(699);
    QVERIFY(!m.holding());
    m.tick(700);
    QVERIFY(m.holding());
    QCOMPARE(m.variant(), u"a"_s); // the last frame of a stays
    m.tick(100000);
    QVERIFY(m.holding());
    const unsigned generation = m.generation();
    m.click(100001);
    QCOMPARE(m.variant(), u"b"_s);
    QVERIFY(!m.holding());
    QVERIFY(m.generation() > generation);
    // one entry played once: a click plays it again
    PetStateMachine one = make(statesFrom(R"({"idle":{"sequence":{"items":["a"],"advance":"click","once":true}}})"));
    one.setAnimation(500, true, 0);
    one.tick(600);
    QVERIFY(one.holding());
    const unsigned before = one.generation();
    one.click(700);
    QVERIFY(!one.holding());
    QVERIFY(one.generation() > before);
    one.setAnimation(500, true, 700);
    one.tick(1200);
    QVERIFY(one.holding());
}

void TstPetStates::autoLinearOnceStopsAtTheLastEntry()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"once":true,"holdMs":1000}}})"));
    m.setAnimation(300, true, 0);
    m.tick(300);
    QCOMPARE(m.variant(), u"b"_s);
    m.setAnimation(300, true, 300);
    m.tick(600);
    QCOMPARE(m.variant(), u"c"_s);
    m.setAnimation(300, true, 600);
    m.tick(900);
    QCOMPARE(m.variant(), u"c"_s);
    QVERIFY(m.holding());
    QVERIFY(!m.nextWakeMs());
    m.tick(1000000);
    QCOMPARE(m.variant(), u"c"_s);
}

void TstPetStates::aMissingVariantIsSkipped()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","gone","c"],"holdMs":1000}},"grabbed":{"variant":"nope"},"released":{"sequence":{"items":["x","y"]}}})"));
    m.setVariantExists([](const QString &name) { return name == u"a"_s || name == u"c"_s || name == u"base"_s; }, 0);
    QVERIFY(m.missingVariant());
    QCOMPARE(m.variant(), u"a"_s);
    m.tick(1000);
    QCOMPARE(m.variant(), u"c"_s);
    QCOMPARE(m.sequenceIndex(), 2);
    m.tick(2000);
    QCOMPARE(m.variant(), u"a"_s);
    // grabbed names a variant that is not there: it shows idle's, released with no valid entry too
    m.press(2100, kPress, true);
    QCOMPARE(m.state(), PetState::Grabbed);
    QCOMPARE(m.owner().value(), PetState::Idle);
    m.release(2200);
    QCOMPARE(m.owner().value(), PetState::Idle);
    // nothing valid at all: the placement's variant
    PetStateMachine none = make(statesFrom(R"({"idle":{"variant":"zz"}})"));
    none.setVariantExists([](const QString &name) { return name == u"base"_s; }, 0);
    QCOMPARE(none.variant(), u"base"_s);
    QVERIFY(none.missingVariant());
    // an item that has them all
    PetStateMachine fine = make(fourStates());
    fine.setVariantExists([](const QString &) { return true; }, 0);
    QVERIFY(!fine.missingVariant());
}

void TstPetStates::theClockNeverGoesBackwardsAndSurvivesHugeJumps()
{
    PetStateMachine m = make(statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"holdMs":1000}},"released":{"variant":"r"}})"));
    m.tick(1000);
    QCOMPARE(m.variant(), u"b"_s);
    m.tick(0); // earlier: counts as the latest
    m.tick(500);
    QCOMPARE(m.variant(), u"b"_s);
    // a jump of three days moves one step, it does not run the sequence through every entry it missed
    m.tick(1000LL + 3LL * 24 * 3600 * 1000);
    QCOMPARE(m.variant(), u"c"_s);
    m.tick(1000LL + 3LL * 24 * 3600 * 1000 + 999);
    QCOMPARE(m.variant(), u"c"_s);
    m.tick(1000LL + 3LL * 24 * 3600 * 1000 + 1000);
    QCOMPARE(m.variant(), u"a"_s);
    // an input stamped earlier than the last one counts as the latest: released still ends
    const std::int64_t late = 1000LL + 3LL * 24 * 3600 * 1000 + 5000;
    m.press(late, kPress, true);
    m.release(100);
    QCOMPARE(m.state(), PetState::Released);
    m.tick(late + 1199);
    QCOMPARE(m.state(), PetState::Released);
    m.tick(late + 1200);
    QCOMPARE(m.state(), PetState::Idle);
}

void TstPetStates::nonFinitePositionsAreIgnored()
{
    PetStateMachine m = make(fourStates());
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    m.press(0, {nan, 0}, true);
    QCOMPARE(m.state(), PetState::Idle);
    m.press(1, kPress, true);
    m.motion(2, {inf, 0});
    m.motion(3, {0, nan});
    QCOMPARE(m.state(), PetState::Grabbed);
    m.motion(4, {200, 100});
    QCOMPARE(m.state(), PetState::Moving);
}

void TstPetStates::sequencesAreSixteenLong()
{
    QString error;
    QVERIFY(applyPetStatesChanges({}, QJsonDocument::fromJson(sequenceJson(16).toUtf8()).object(), &error).has_value());
    QVERIFY(!applyPetStatesChanges({}, QJsonDocument::fromJson(sequenceJson(17).toUtf8()).object(), &error).has_value());
    QVERIFY(error.contains(u"1 to 16"_s));
    QVERIFY(!applyPetStatesChanges({}, QJsonDocument::fromJson(sequenceJson(0).toUtf8()).object(), &error).has_value());
}

void TstPetStates::changingTheStatesStartsOver()
{
    PetStateMachine m = make(fourStates());
    m.press(0, kPress, true);
    QCOMPARE(m.state(), PetState::Grabbed);
    m.setStates(statesFrom(R"({"idle":{"variant":"z"}})"), 10);
    QCOMPARE(m.state(), PetState::Idle);
    QCOMPARE(m.variant(), u"z"_s);
    const unsigned generation = m.generation();
    m.setStates(statesFrom(R"({"idle":{"variant":"z"}})"), 20); // equal: nothing
    QCOMPARE(m.generation(), generation);
    m.setStates(PetStates{}, 30);
    QCOMPARE(m.variant(), u"base"_s);
}

void TstPetStates::petInteractionDoesNotDependOnTheMachine()
{
    // The same scripted pointer path through two PetInteraction: one with a state machine listening to what it does, one alone. The answers are identical,
    // step by step: the machine reads the events and changes nothing in the drag, dodge or fade rules.
    const auto run = [](bool withMachine) {
        PetInteraction in(PetInteraction::Level::Global);
        PetBehaviourConfig config;
        config.clickThrough = false;
        config.smartDodge = true;
        in.setConfig(config, 0);
        PetRect window{100, 100, 200, 200};
        in.setGeometry(window, {0, 0, 1280, 720});
        PetStateMachine machine;
        machine.setBaseVariant(u"base"_s, 0);
        machine.setStates(fourStates(), 0);
        QList<double> trace;
        const auto note = [&](std::int64_t now) {
            if (withMachine && in.dragging() && machine.state() == PetState::Idle)
                machine.press(now, {0, 0}, true);
            if (const auto move = in.takeMove()) {
                trace << move->dx << move->dy;
                if (withMachine)
                    machine.dodgeStep(now, 220);
                window = window.translated(move->dx, move->dy);
                in.setGeometry(window, {0, 0, 1280, 720});
            }
            trace << in.opacityTarget() << double(in.inputRegion()) << double(in.layout()) << double(in.dragging());
            if (withMachine)
                machine.tick(now);
        };
        std::int64_t now = 0;
        const auto step = [&](auto &&input) {
            now += 40;
            input(now);
            note(now);
        };
        step([&](std::int64_t t) { in.motionGlobal(t, {150, 150}); });
        step([&](std::int64_t t) { in.press(t, {50, 50}); });
        for (int i = 0; i < 6; ++i)
            step([&](std::int64_t t) { in.motionGlobal(t, {150 + 20.0 * (i + 1), 150 + 5.0 * i}); });
        step([&](std::int64_t t) { in.release(t); });
        step([&](std::int64_t t) { in.motionGlobal(t, {900, 600}); });
        step([&](std::int64_t t) { in.tick(t); });
        return trace;
    };
    QCOMPARE(run(true), run(false));
}

void TstPetStates::propertyNoStreamLeavesTheFourStates()
{
    const PetStates configs[] = {
        fourStates(),
        statesFrom(R"({"idle":{"sequence":{"items":["a","b","c"],"order":"random","holdMs":500}},"grabbed":{"variant":"g"},"released":{"sequence":{"items":["r1","r2"],"once":true}}})"),
        statesFrom(R"({"idle":{"sequence":{"items":["a","b"],"advance":"click","once":true}},"moving":{"sequence":{"items":["m1","m2"],"holdMs":500}}})"),
        PetStates{},
    };
    for (const PetStates &config : configs) {
        for (std::uint32_t seed = 1; seed <= 40; ++seed) {
            std::mt19937 rng(seed);
            PetStateMachine m({}, seed);
            m.setBaseVariant(u"base"_s, 0);
            m.setStates(config, 0);
            std::int64_t now = 0;
            bool released = false;
            std::int64_t releasedAt = 0;
            for (int step = 0; step < 400; ++step) {
                now += int(rng() % 700);
                const unsigned event = rng() % 9;
                switch (event) {
                case 0:
                    m.press(now, {double(rng() % 300), double(rng() % 300)}, rng() % 4 != 0);
                    break;
                case 1:
                case 2:
                    m.motion(now, {double(rng() % 300), double(rng() % 300)});
                    break;
                case 3:
                    m.release(now);
                    if (m.state() == PetState::Released) {
                        released = true;
                        releasedAt = now;
                    }
                    break;
                case 4:
                    m.click(now);
                    break;
                case 5:
                    m.dodgeStep(now, int(rng() % 1000));
                    break;
                case 6:
                    m.setAnimation(int(rng() % 2000), rng() % 2 == 0, now);
                    break;
                case 7:
                    if (rng() % 10 == 0)
                        m.reset(now);
                    break;
                default:
                    m.tick(now);
                    break;
                }
                const PetState state = m.state();
                QVERIFY(state == PetState::Idle || state == PetState::Grabbed || state == PetState::Moving || state == PetState::Released);
                QVERIFY(!m.variant().isEmpty());
                if (m.state() != PetState::Released)
                    released = false;
            }
            // whatever happened, with no more input everything settles to idle
            m.release(now);
            m.tick(now + 20000);
            QCOMPARE(m.state(), PetState::Idle);
            Q_UNUSED(releasedAt);
            Q_UNUSED(released);
        }
    }
}

void TstPetStates::theJsonRoundTripsAndRefusesTypos()
{
    QString error;
    const PetStates states = statesFrom(R"({"idle":{"sequence":{"items":["a","b"],"order":"random","advance":"click","once":true,"holdMs":2500}},"grabbed":{"variant":"g"},"colour":1})");
    QVERIFY(states.of(PetState::Idle)->isSequence());
    QCOMPARE(states.of(PetState::Idle)->sequence.holdMs, 2500);
    QVERIFY(states.of(PetState::Idle)->sequence.once);
    QCOMPARE(states.variants(), (QStringList{u"a"_s, u"b"_s, u"g"_s}));
    const auto again = applyPetStatesChanges(PetStates{}, states.toJson(), &error);
    QVERIFY2(again.has_value(), qPrintable(error));
    QCOMPARE(*again, states);
    // partial merge: a state is replaced whole, the others stay; null clears one, and null as a whole clears all
    const auto merged = applyPetStatesChanges(states, QJsonDocument::fromJson(R"({"moving":{"variant":"m"},"grabbed":null})").object());
    QVERIFY(merged);
    QVERIFY(merged->of(PetState::Idle));
    QVERIFY(!merged->of(PetState::Grabbed));
    QCOMPARE(merged->of(PetState::Moving)->variant, u"m"_s);
    QVERIFY(applyPetStatesChanges(states, QJsonValue(QJsonValue::Null))->isDefault());
    QVERIFY(PetStates{}.toJson().isEmpty());
    // wrong types and values
    for (const char *bad : {R"(7)", R"({"idle":7})", R"({"idle":{}})", R"({"idle":{"variant":"a","sequence":{"items":["b"]}}})", R"({"idle":{"variant":""}})",
                            R"({"idle":{"variant":3}})", R"({"idle":{"sequence":5}})", R"({"idle":{"sequence":{"items":"a"}}})", R"({"idle":{"sequence":{"items":[1]}}})",
                            R"({"idle":{"sequence":{"items":["a"],"order":"sideways"}}})", R"({"idle":{"sequence":{"items":["a"],"advance":"never"}}})",
                            R"({"idle":{"sequence":{"items":["a"],"once":"yes"}}})", R"({"idle":{"sequence":{"items":["a"],"holdMs":100}}})",
                            R"({"idle":{"sequence":{"items":["a"],"holdMs":70000}}})", R"({"idle":{"sequence":{"items":["a"],"holdMs":1000.5}}})"}) {
        const QJsonValue value = QJsonDocument::fromJson(QByteArray("[") + bad + "]").array().at(0);
        error.clear();
        QVERIFY2(!applyPetStatesChanges(PetStates{}, value, &error).has_value(), bad);
        QVERIFY2(!error.isEmpty(), bad);
    }
    QVERIFY(PetStates{}.variants().isEmpty());
    QCOMPARE(QString::fromLatin1(petStateName(PetState::Released)), u"released"_s);
    QVERIFY(!petStateFromName(u"static"_s)); // Anima's name is the importer's to map
}

void TstPetStates::trackingJson()
{
    QString error;
    const auto t = applyPetTrackingChanges(PetTracking{}, QJsonDocument::fromJson(R"({"mode":"follow","distance":300,"speed":2.5,"returnHome":false,"extra":1})").object(), &error);
    QVERIFY2(t.has_value(), qPrintable(error));
    QCOMPARE(t->mode, PetTrackingMode::Follow);
    QCOMPARE(t->distance, 300.0);
    QVERIFY(!t->returnHome);
    QCOMPARE(*applyPetTrackingChanges(*t, t->toJson()), *t);
    // key by key
    const auto partial = applyPetTrackingChanges(*t, QJsonDocument::fromJson(R"({"speed":1})").object());
    QCOMPARE(partial->distance, 300.0);
    QCOMPARE(partial->speed, 1.0);
    QVERIFY(applyPetTrackingChanges(*t, QJsonValue(QJsonValue::Null))->isDefault());
    QVERIFY(PetTracking{}.isDefault());
    QVERIFY(!PetTracking{}.active());
    for (const char *bad : {R"(true)", R"({"mode":"chase"})", R"({"mode":1})", R"({"distance":-1})", R"({"distance":4001})", R"({"distance":"far"})", R"({"speed":0.05})",
                            R"({"speed":4.5})", R"({"returnHome":"yes"})"}) {
        const QJsonValue value = QJsonDocument::fromJson(QByteArray("[") + bad + "]").array().at(0);
        QVERIFY2(!applyPetTrackingChanges(PetTracking{}, value, &error).has_value(), bad);
    }
    QVERIFY(applyPetTrackingChanges(PetTracking{}, QJsonDocument::fromJson(R"({"distance":0})").object()).has_value());
    QVERIFY(applyPetTrackingChanges(PetTracking{}, QJsonDocument::fromJson(R"({"distance":4000,"speed":4})").object()).has_value());
}

void TstPetStates::theNamesAreInTheDocument()
{
    QFile doc(QStringLiteral(PETS_STATES_DOC));
    QVERIFY2(doc.open(QIODevice::ReadOnly), "docs/pets/states.md is missing");
    const QString text = QString::fromUtf8(doc.readAll());
    for (const std::string_view name : statesdegrade::kAll)
        QVERIFY2(text.contains(u"| `"_s + QString::fromUtf8(name.data(), int(name.size())) + u"` |"_s), name.data());
    for (const PetState state : {PetState::Idle, PetState::Grabbed, PetState::Moving, PetState::Released})
        QVERIFY2(text.contains(u"`"_s + QString::fromLatin1(petStateName(state)) + u"`"_s), petStateName(state));
    QVERIFY(text.contains(QString::number(kMaxStateSequenceItems)));
    QVERIFY(text.contains(QString::number(kMinStateHoldMs)));
    QVERIFY(text.contains(QString::number(kMaxStateHoldMs)));
    QVERIFY(text.contains(QString::number(kDefaultStateHoldMs)));
    QVERIFY(text.contains(QString::number(int(kMaxTrackingDistance))));
}

QTEST_APPLESS_MAIN(TstPetStates)
#include "tst_petstates.moc"
