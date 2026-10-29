// The pointer interactions of a pet as a pure state machine (src/overlay/petinteraction.h): hide under cursor and focus fade
// at both pointer levels, smart dodge, the drag, the rescue config. Scripted pointer paths; the clock is a number.

#include "petinteraction.h"
#include "pointersource.h"

#include <QSignalSpy>
#include <QTest>

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

using namespace murasaki;
using Level = PetInteraction::Level;

namespace {

constexpr PetRect kWindow{100, 100, 200, 200};
constexpr PetRect kOutput{0, 0, 1280, 720};

// Opaque on the left half of the window only: x < 100 in window coordinates.
bool leftHalf(PetPoint local)
{
    return local.x < 100;
}

PetBehaviourConfig config(bool hoverHide, bool focusFade, bool smartDodge, bool clickThrough, bool locked)
{
    PetBehaviourConfig c;
    c.hoverHide = hoverHide;
    c.focusFade = focusFade;
    c.smartDodge = smartDodge;
    c.clickThrough = clickThrough;
    c.locked = locked;
    return c;
}

PetInteraction make(Level level, const PetBehaviourConfig &c, PetRect window = kWindow)
{
    PetInteraction p(level);
    p.setGeometry(window, kOutput);
    p.setMask(leftHalf);
    p.setConfig(c, 0);
    return p;
}

bool has(const std::vector<std::string_view> &names, std::string_view name)
{
    return std::find(names.begin(), names.end(), name) != names.end();
}

} // namespace

class TstPetInteraction : public QObject
{
    Q_OBJECT

private slots:
    void nothingOnNothingHappens();
    void globalHoverHideFollowsTheOpaquePixels();
    void globalFocusFadeUsesItsOwnOpacity();
    void hoverHideWinsOverFocusFade();
    void globalFadeOfAClickThroughPetStaysClickThrough();
    void localEnterFadesAndKeepsTheUnionRegion();
    void localPressEmptiesTheRegionFor1200ms();
    void theReleaseUnmapsOnceTheButtonIsUp();
    void localLeaveRestoresAndCancelsTheRelease();
    void leaveDuringTheReleaseKeepsThePetHiddenUntilItEnds();
    void theReleaseEndsAtTheNextInputEvenBeforeATick();
    void aSlowApplyNeverDoublesAStep();
    void dodgeTowardEveryEdgeAndADiagonalCut();
    void nonFiniteInputsAreIgnored();
    void setLevelEndsADragAndClearsPendingMoves();
    void switchingBetweenHideAndFadeUnderAStillPointer();
    void leaveAndEnterDuringADragDoNotEndIt();
    void theSettleWindowIsExactly16ms();
    void aDragEndingOutsideTheSmallWindowForgetsThePointer();
    void aDegenerateDodgeTuningStillWorks();
    void localEnterOnATransparentPixelDoesNothing();
    void localClickThroughPetThatFadesTakesInput();
    void localWithoutBehavioursStaysClickThrough();
    void ownRegionChangesNeverWaitForEvents();
    void dodgeStepsAwayAlongTheLine();
    void dodgeStepClampAndMargin();
    void dodgeCooldown();
    void dodgeClampsToTheOutputAndSpendsNoCooldownWhenCornered();
    void dodgeLocalOnlyWhenTouched();
    void lockedPetsNeverMove();
    void dragUsesTheAbsolutePositionAndIgnoresTheStaleFrame();
    void dragInTheOldFrameForTheWholeGrab();
    void dragSwitchesToTheGrownFrameOnceAndForAll();
    void anAnimatedGrowthIsFollowedByTheRelativeMotion();
    void aStillFrameCorrectsTheRelativeDrift();
    void relativeMotionOutsideADragIsIgnored();
    void dragNeedsAnOpaquePressAnInteractivePet();
    void dragKeepsAPartOfThePetInTheOutput();
    void aPressAndReleaseWithoutADragIsAClick();
    void dragSuspendsTheFade();
    void rescueConfigRestoresEverything();
    void needsPointerOnlyWhileABehaviourIsOn();
    void pointerSourceReportsOnlyWhileWanted();
    void degradeNamesAreDistinct();
};

void TstPetInteraction::nothingOnNothingHappens()
{
    PetInteraction p = make(Level::Global, config(false, false, false, true, false));
    p.motionGlobal(0, {150, 150});
    p.tick(32);
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty)); // click-through, as today
    QVERIFY(!p.takeMove());
    QVERIFY(!p.hidden());
    QVERIFY(p.degraded().empty());
    p.press(40, {50, 50});
    QVERIFY(!p.dragging()); // click-through pets never receive
}

void TstPetInteraction::globalHoverHideFollowsTheOpaquePixels()
{
    PetInteraction p = make(Level::Global, config(true, false, false, false, false));
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    p.motionGlobal(0, {150, 150}); // window-local (50, 50): opaque
    p.tick(32);
    QCOMPARE(p.opacityTarget(), 0.0);
    QVERIFY(p.hidden());
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty)); // click-through while hidden
    p.motionGlobal(40, {250, 150}); // local (150, 50): transparent half
    p.tick(64);
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    p.motionGlobal(80, {150, 150});
    p.tick(96);
    QCOMPARE(p.opacityTarget(), 0.0);
    p.motionGlobal(120, {900, 600}); // off the window
    p.tick(128);
    QCOMPARE(p.opacityTarget(), 1.0);
    QVERIFY(p.degraded().empty()); // exact at the global level
}

void TstPetInteraction::globalFocusFadeUsesItsOwnOpacity()
{
    PetInteraction p = make(Level::Global, config(false, true, false, false, false));
    p.motionGlobal(0, {150, 150});
    p.tick(32);
    QCOMPARE(p.opacityTarget(), 0.18);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.motionGlobal(40, {900, 600});
    p.tick(64);
    QCOMPARE(p.opacityTarget(), 1.0);
}

void TstPetInteraction::hoverHideWinsOverFocusFade()
{
    PetInteraction p = make(Level::Global, config(true, true, false, false, false));
    p.motionGlobal(0, {150, 150});
    p.tick(32);
    QCOMPARE(p.opacityTarget(), 0.0);
}

void TstPetInteraction::globalFadeOfAClickThroughPetStaysClickThrough()
{
    PetInteraction p = make(Level::Global, config(true, false, false, true, false));
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.motionGlobal(0, {150, 150});
    p.tick(32);
    QCOMPARE(p.opacityTarget(), 0.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.motionGlobal(40, {900, 600});
    p.tick(64);
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty)); // back to the user's click-through
}

void TstPetInteraction::localEnterFadesAndKeepsTheUnionRegion()
{
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    p.enter(1000, {50, 50});
    QCOMPARE(p.opacityTarget(), 0.0);
    QVERIFY(p.hidden());
    // The region does not change while the pet is invisible, and it keeps taking the pointer so that `leave` still comes.
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Union));
    p.motionLocal(1032, {60, 60});
    p.tick(1064);
    QCOMPARE(p.opacityTarget(), 0.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Union));
    const auto names = p.degraded();
    QVERIFY(has(names, degrade::kPointerLocal));
    QVERIFY(has(names, degrade::kHideClicksSwallowed));
    QVERIFY(!has(names, degrade::kDodgeOnlyWhenTouched));
}

void TstPetInteraction::localPressEmptiesTheRegionFor1200ms()
{
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    p.enter(1000, {50, 50});
    p.press(2000, {50, 50}); // the first click reaches the pet and is lost
    QVERIFY(!p.dragging());
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.tick(3199);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.tick(3200); // 1.2 s later the region is back; no enter is waited for
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Union));
    QCOMPARE(p.opacityTarget(), 0.0);
    // A second press starts another release.
    p.press(3300, {50, 50});
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
}

// sway and Hyprland keep the pointer focus on a surface whose region was emptied until the pointer moves; no compositor keeps it on an
// unmapped surface (docs/pets/pointer-facts.md, U1). The window unmaps once the button of the swallowed click is up, until the release ends.
void TstPetInteraction::theReleaseUnmapsOnceTheButtonIsUp()
{
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    QVERIFY(!p.released());
    p.enter(1000, {50, 50});
    p.press(2000, {50, 50});
    QVERIFY(!p.released()); // not with the button down: the release of this click must not land elsewhere
    p.release(2080);
    QVERIFY(p.released());
    p.leave(2081); // what the compositor says when the surface goes
    QVERIFY(p.released());
    QCOMPARE(p.opacityTarget(), 0.0);
    p.tick(3199);
    QVERIFY(p.released());
    p.tick(3200);
    QVERIFY(!p.released());
    // At the global level the region is empty while faded, and on KWin no press arrives. sway and Hyprland do not re-pick the surface
    // on a press: the one the pointer entered before the pet faded gets it, and the same release follows.
    PetInteraction g = make(Level::Global, config(true, false, false, false, false));
    g.motionGlobal(0, {150, 150});
    QCOMPARE(int(g.inputRegion()), int(InputRegion::Empty));
    g.press(10, {50, 50});
    QVERIFY(!g.released());
    QVERIFY(!g.dragging());
    g.release(20);
    QVERIFY(g.released());
    g.tick(1209);
    QVERIFY(g.released());
    g.tick(1210);
    QVERIFY(!g.released());
}

void TstPetInteraction::localLeaveRestoresAndCancelsTheRelease()
{
    PetInteraction p = make(Level::Local, config(false, true, false, false, false));
    p.enter(0, {50, 50});
    QCOMPARE(p.opacityTarget(), 0.18);
    p.leave(100);
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
}

void TstPetInteraction::leaveDuringTheReleaseKeepsThePetHiddenUntilItEnds()
{
    // The F11 sequence: press (lost), one pixel of motion -> the compositor re-picks the surface under the pointer and sends
    // leave although the pointer is still over the pet.
    PetInteraction p = make(Level::Local, config(false, true, false, false, false));
    p.enter(200, {50, 50});
    p.press(300, {50, 50});
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.leave(310);
    QVERIFY(p.hidden());                                      // no pop to visible
    QCOMPARE(p.opacityTarget(), 0.18);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty)); // the next clicks keep passing
    p.tick(1499);
    QVERIFY(p.hidden());
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.tick(1500); // the release ended and the compositor said the pointer is not on the surface: normal
    QVERIFY(!p.hidden());
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    // If the pointer is back on the surface (enter) before the release ends, the pet stays hidden afterwards, with the union.
    PetInteraction q = make(Level::Local, config(false, true, false, false, false));
    q.enter(0, {50, 50});
    q.press(100, {50, 50});
    q.leave(110);
    q.enter(120, {60, 60});
    q.tick(1299);
    QCOMPARE(int(q.inputRegion()), int(InputRegion::Empty));
    q.tick(1300);
    QVERIFY(q.hidden());
    QCOMPARE(int(q.inputRegion()), int(InputRegion::Union));
}

void TstPetInteraction::theReleaseEndsAtTheNextInputEvenBeforeATick()
{
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    p.enter(0, {50, 50});
    p.press(1000, {50, 50});
    p.motionLocal(2200, {60, 60}); // exactly 1200 ms later, no tick in between
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Union));
    QVERIFY(p.hidden());
}

void TstPetInteraction::localEnterOnATransparentPixelDoesNothing()
{
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    p.enter(0, {150, 50}); // the mask says transparent (a stale or coarse region)
    QCOMPARE(p.opacityTarget(), 1.0);
    p.motionLocal(30, {50, 50}); // moves onto an opaque pixel
    QCOMPARE(p.opacityTarget(), 0.0);
}

void TstPetInteraction::localClickThroughPetThatFadesTakesInput()
{
    PetInteraction p = make(Level::Local, config(true, false, false, true, false));
    // It can only hear the pointer through its region: the clicks are swallowed (the named degrade).
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    QVERIFY(has(p.degraded(), degrade::kHideClicksSwallowed));
    p.press(0, {50, 50});
    QVERIFY(!p.dragging()); // but it is still click-through for dragging
}

void TstPetInteraction::localWithoutBehavioursStaysClickThrough()
{
    PetInteraction p = make(Level::Local, config(false, false, false, true, false));
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    const auto names = p.degraded();
    QCOMPARE(int(names.size()), 1);
    QVERIFY(has(names, degrade::kPointerLocal));
}

void TstPetInteraction::ownRegionChangesNeverWaitForEvents()
{
    // F11: KWin sends nothing when a region changes. The outputs depend on the inputs seen, never on a pending event: the
    // same inputs give the same outputs however the region was changed in between.
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    p.enter(0, {50, 50});
    const double faded = p.opacityTarget();
    p.tick(10);
    p.tick(20);
    QCOMPARE(p.opacityTarget(), faded);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Union));
    // Turning the behaviour off under a still pointer restores the pet without any event.
    p.setConfig(config(false, false, false, false, false), 30);
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    // And on again under the same pointer: the surface already holds it.
    p.setConfig(config(true, false, false, false, false), 40);
    QCOMPARE(p.opacityTarget(), 0.0);
}

void TstPetInteraction::dodgeStepsAwayAlongTheLine()
{
    // Window 200 x 200 at (100, 100): centre (200, 200); the step is clamp(200 // 3 = 66, 54, 140) = 66.
    PetInteraction p = make(Level::Global, config(false, false, true, false, false));
    p.motionGlobal(0, {200, 300}); // directly below the centre: 100 px, inside the window's bottom edge
    const auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 0.0);
    QCOMPARE(move->dy, -66.0);
    QVERIFY(!p.takeMove()); // reading clears it
}

void TstPetInteraction::dodgeStepClampAndMargin()
{
    // Small window: 90 // 3 = 30 -> 54.
    PetInteraction small = make(Level::Global, config(false, false, true, false, false), {600, 300, 90, 90});
    small.motionGlobal(0, {645, 400}); // 10 px below the bottom edge (390): within 26
    auto move = small.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dy, -54.0);
    // Large window: 600 // 3 = 200 -> 140.
    PetInteraction big = make(Level::Global, config(false, false, true, false, false), {300, 60, 600, 600});
    big.motionGlobal(0, {280, 360}); // 20 px left of the window, level with its centre
    move = big.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 140.0);
    QCOMPARE(move->dy, 0.0);
    // The margin: 26 px inclusive, 27 not.
    PetInteraction edge = make(Level::Global, config(false, false, true, false, false));
    edge.motionGlobal(0, {73, 200}); // 27 px left of the window
    QVERIFY(!edge.takeMove());
    edge.motionGlobal(1, {74, 200}); // 26 px
    QVERIFY(edge.takeMove());
}

void TstPetInteraction::dodgeCooldown()
{
    PetInteraction p = make(Level::Global, config(false, false, true, false, false));
    p.motionGlobal(0, {200, 300});
    QVERIFY(p.takeMove());
    p.setGeometry(kWindow.translated(0, -66), kOutput);
    p.motionGlobal(100, {200, 234}); // still within the margin of the moved window, 100 ms later
    p.tick(100);
    QVERIFY(!p.takeMove());
    p.tick(219);
    QVERIFY(!p.takeMove());
    p.tick(220);
    QVERIFY(p.takeMove());
}

void TstPetInteraction::dodgeClampsToTheOutputAndSpendsNoCooldownWhenCornered()
{
    // Against the top edge with the cursor 15 px below the window (inside the 26 px margin): the step is up, with no room.
    PetInteraction p = make(Level::Global, config(false, false, true, false, false), {500, 0, 200, 200});
    p.motionGlobal(0, {600, 215});
    QVERIFY(!p.takeMove()); // a dodge was tried and cut to nothing
    // The cursor then comes from the left: a step to the right is possible at once (no cooldown was spent).
    p.motionGlobal(10, {480, 100}); // 10 ms later: far inside the 220 ms cooldown
    const auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 66.0);
    // A step that would leave the output is cut to what fits.
    PetInteraction q = make(Level::Global, config(false, false, true, false, false), {1050, 300, 200, 200});
    q.motionGlobal(0, {1030, 400}); // from the left: wants +66, only 30 fit before x = 1080
    const auto cut = q.takeMove();
    QVERIFY(cut);
    QCOMPARE(cut->dx, 30.0);
}

void TstPetInteraction::dodgeLocalOnlyWhenTouched()
{
    PetInteraction p = make(Level::Local, config(false, false, true, true, false));
    QVERIFY(has(p.degraded(), degrade::kDodgeOnlyWhenTouched));
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask)); // it needs the pointer to hear the touch
    p.motionGlobal(0, {200, 300});                           // a global position is not used at this level
    p.tick(32);
    QVERIFY(!p.takeMove());
    p.enter(100, {50, 150}); // touched on an opaque pixel: global (150, 250), below-left of the centre (200, 200)
    const auto move = p.takeMove();
    QVERIFY(move);
    QVERIFY(move->dx > 0 && move->dy < 0); // away from the cursor (below-left of the centre)
    QVERIFY(std::abs(std::hypot(move->dx, move->dy) - 66) < 1.5);
    p.setGeometry(kWindow.translated(move->dx, move->dy), kOutput);
    p.leave(150);
    p.setGeometry(kWindow, kOutput);
    p.enter(200, {50, 150}); // inside the cooldown
    QVERIFY(!p.takeMove());
    p.leave(250);
    p.enter(400, {50, 150}); // after it
    QVERIFY(p.takeMove());
}

void TstPetInteraction::lockedPetsNeverMove()
{
    PetInteraction g = make(Level::Global, config(false, false, true, false, true));
    g.motionGlobal(0, {200, 300});
    g.tick(500);
    QVERIFY(!g.takeMove());
    g.press(600, {50, 50});
    QVERIFY(!g.dragging());
    QCOMPARE(int(g.layout()), int(PetLayout::Small));
    PetInteraction l = make(Level::Local, config(false, false, true, false, true));
    l.enter(0, {50, 150});
    QVERIFY(!l.takeMove());
    l.press(10, {50, 150});
    QVERIFY(!l.dragging());
}

void TstPetInteraction::dragUsesTheAbsolutePositionAndIgnoresTheStaleFrame()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(1000, {20, 30}); // output (120, 130)
    QVERIFY(p.dragging());
    QCOMPARE(int(p.layout()), int(PetLayout::Drag));
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    // Within 16 ms the coordinates may still be the small window's (F12): ignored, however wrong they are.
    p.motionLocal(1004, {30, 40});
    QVERIFY(!p.takeMove());
    // Later events are in the grown window: output coordinates. The reference is the press position, so the dropped events
    // lose nothing.
    p.motionLocal(1020, {140, 150});
    auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 20.0);
    QCOMPARE(move->dy, 20.0);
    p.motionLocal(1040, {150, 145});
    move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 10.0);
    QCOMPARE(move->dy, -5.0);
    // Two events between reads add up.
    p.motionLocal(1060, {160, 145});
    p.motionLocal(1080, {170, 145});
    move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 20.0);
    QCOMPARE(move->dy, 0.0);
    // The window is moved by the caller meanwhile; the drag keeps its own reference.
    p.setGeometry(kWindow.translated(50, 15), kOutput);
    p.motionLocal(1100, {170, 145}); // same pointer position: nothing to add
    QVERIFY(!p.takeMove());
    p.release(1200);
    QVERIFY(!p.dragging());
    QCOMPARE(int(p.layout()), int(PetLayout::Small));
    p.motionLocal(1300, {500, 500});
    QVERIFY(!p.takeMove());
}

// sway keeps the small window's frame for the whole grab (docs/pets/pointer-facts.md, 3.3): the same drag as above, every event
// relative to the window where the press was, must move the pet the same.
void TstPetInteraction::dragInTheOldFrameForTheWholeGrab()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(1000, {20, 30}); // output (120, 130)
    p.motionLocal(1020, {40, 50}); // output (140, 150)
    auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 20.0);
    QCOMPARE(move->dy, 20.0);
    p.motionLocal(1040, {50, 45}); // output (150, 145)
    move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 10.0);
    QCOMPARE(move->dy, -5.0);
    // a long way, still in that frame: output (600, 400)
    for (int i = 1; i <= 10; ++i)
        p.motionLocal(1040 + i * 8, {50 + i * 45.0, 45 + i * 25.5});
    move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 450.0);
    QCOMPARE(move->dy, 255.0);
    p.release(1200);
    QVERIFY(!p.dragging());
}

// KWin and Hyprland switch from the old frame to the grown window's a few milliseconds into the grab, after `dragSettleMs` at
// times: no jump by the window's offset on the switch, and no switching back.
void TstPetInteraction::dragSwitchesToTheGrownFrameOnceAndForAll()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(1000, {20, 30}); // output (120, 130)
    p.motionLocal(1018, {25, 33}); // old frame: output (125, 133)
    p.motionLocal(1020, {128, 135}); // grown frame: output (128, 135)
    auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 8.0);
    QCOMPARE(move->dy, 5.0);
    // (35, 40) read in the old frame would be output (135, 140), only 8.6 px from the last position, nearer than (35, 40);
    // the grown frame stays: the pointer is at output (35, 40).
    p.motionLocal(1030, {35, 40});
    move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, -93.0);
    QCOMPARE(move->dy, -95.0);
}

// F13, Hyprland's way: the grown window slides from the small one's place to the output's origin, so each motion is in a frame
// that is neither; the relative motion in between carries the drag until the frame holds still.
void TstPetInteraction::anAnimatedGrowthIsFollowedByTheRelativeMotion()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    PetMove total;
    const auto take = [&] {
        if (const auto move = p.takeMove()) {
            total.dx += move->dx;
            total.dy += move->dy;
        }
    };
    p.press(1000, {20, 30}); // output (120, 130)
    // the frame's origin at (60, 60), then (20, 20), then (0, 0); the pointer goes (10, 5) each time
    p.motionRelative(1019, 10, 5);
    p.motionLocal(1020, {70, 75}); // output (130, 135)
    take();
    QCOMPARE(total.dx, 10.0);
    QCOMPARE(total.dy, 5.0);
    p.motionRelative(1039, 10, 5);
    p.motionLocal(1040, {120, 120}); // output (140, 140)
    take();
    p.motionRelative(1059, 10, 5);
    p.motionLocal(1060, {150, 145}); // output (150, 145): the frame has arrived
    take();
    QCOMPARE(total.dx, 30.0);
    QCOMPARE(total.dy, 15.0);
    // still now: the local step is the relative motion, and the grown frame is read as is
    p.motionRelative(1079, 10, 5);
    p.motionLocal(1080, {160, 150});
    take();
    QCOMPARE(total.dx, 40.0);
    QCOMPARE(total.dy, 20.0);
    p.release(1100);
    QVERIFY(!p.dragging());
}

// The relative motion can run ahead of the cursor (pushed against the edge of the layout it keeps coming): the first event whose
// local step matches it again puts the drag back on the pointer.
void TstPetInteraction::aStillFrameCorrectsTheRelativeDrift()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    PetMove total;
    const auto take = [&] {
        if (const auto move = p.takeMove()) {
            total.dx += move->dx;
            total.dy += move->dy;
        }
    };
    p.press(1000, {20, 30}); // output (120, 130)
    p.motionRelative(1019, 10, 0);
    p.motionLocal(1020, {130, 130}); // the grown frame at once: output (130, 130)
    take();
    QCOMPARE(total.dx, 10.0);
    p.motionRelative(1030, 10, 0); // the cursor is stuck, no motion event
    p.motionRelative(1039, -5, 0);
    p.motionLocal(1040, {125, 130}); // back by 5: the step (-5) is not the relative 5, the drag follows the relative
    take();
    QCOMPARE(total.dx, 15.0);
    p.motionRelative(1059, -5, 0);
    p.motionLocal(1060, {120, 130}); // the step matches: the frame is read, the pointer is at (120, 130)
    take();
    QCOMPARE(total.dx, 0.0);
    QCOMPARE(total.dy, 0.0);
}

void TstPetInteraction::relativeMotionOutsideADragIsIgnored()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.motionRelative(0, 500, 500);
    p.press(1000, {20, 30});
    p.motionLocal(1020, {130, 140}); // no relative motion in this drag: F12, the grown frame, output (130, 140)
    const auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 10.0);
    QCOMPARE(move->dy, 10.0);
}

void TstPetInteraction::dragNeedsAnOpaquePressAnInteractivePet()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(0, {150, 50}); // transparent half
    QVERIFY(!p.dragging());
    PetInteraction c = make(Level::Local, config(false, false, false, true, false));
    c.press(0, {50, 50});
    QVERIFY(!c.dragging());
    // Becoming click-through or locked ends a drag in progress.
    PetInteraction d = make(Level::Local, config(false, false, false, false, false));
    d.press(0, {50, 50});
    QVERIFY(d.dragging());
    d.setConfig(config(false, false, false, false, true), 10);
    QVERIFY(!d.dragging());
}

void TstPetInteraction::aPressAndReleaseWithoutADragIsAClick()
{
    // an interactive pet: press and release on an opaque pixel
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    QVERIFY(!p.takeClick());
    p.press(0, {50, 50});
    p.release(40);
    QVERIFY(p.takeClick());
    QVERIFY(!p.takeClick()); // read once
    // a tiny wobble is still a click, a real drag is not
    p.press(100, {50, 50});
    p.motionLocal(120, {152, 151}); // the window grew: output coordinates
    p.release(140);
    QVERIFY(p.takeClick());
    p.press(200, {50, 50});
    p.motionLocal(220, {190, 190});
    QVERIFY(p.takeMove());
    p.release(240);
    QVERIFY(!p.takeClick());
    // not on a transparent pixel, not on a click-through pet, not on a faded one, not without a press
    PetInteraction t = make(Level::Local, config(false, false, false, false, false));
    t.press(0, {150, 50});
    t.release(10);
    QVERIFY(!t.takeClick());
    PetInteraction c = make(Level::Local, config(false, false, false, true, false));
    c.press(0, {50, 50});
    c.release(10);
    QVERIFY(!c.takeClick());
    PetInteraction f = make(Level::Local, config(true, false, false, false, false));
    f.enter(0, {50, 50});
    f.press(10, {50, 50});
    f.release(20);
    QVERIFY(!f.takeClick());
    PetInteraction n = make(Level::Local, config(false, false, false, false, false));
    n.release(10);
    QVERIFY(!n.takeClick());
    // a locked pet can not be dragged: its click still counts
    PetInteraction l = make(Level::Local, config(false, false, false, false, true));
    l.press(0, {50, 50});
    l.release(30);
    QVERIFY(l.takeClick());
    // but not when the pointer left before the button came up
    l.press(100, {50, 50});
    l.leave(110);
    l.release(120);
    QVERIFY(!l.takeClick());
    // becoming click-through between press and release cancels it
    PetInteraction g = make(Level::Local, config(false, false, false, false, true));
    g.press(0, {50, 50});
    g.setConfig(config(false, false, false, true, true), 10);
    g.release(20);
    QVERIFY(!g.takeClick());
}

void TstPetInteraction::dragKeepsAPartOfThePetInTheOutput()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(0, {50, 50}); // output (150, 150)
    p.motionLocal(100, {5000, 5000});
    const auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(100 + move->dx, 1280.0 - 48); // the window's x
    QCOMPARE(100 + move->dy, 720.0 - 48);
}

void TstPetInteraction::dragSuspendsTheFade()
{
    PetInteraction p = make(Level::Global, config(true, false, false, false, false));
    p.motionGlobal(0, {250, 150}); // transparent half: not faded
    p.tick(32);
    p.press(40, {150, 50});
    QVERIFY(!p.dragging()); // transparent pixel
    p.motionGlobal(80, {150, 150});
    p.tick(96);
    QVERIFY(p.hidden()); // opaque: faded, click-through, a press can not arrive
    p.press(100, {50, 50});
    QVERIFY(!p.dragging());
}

void TstPetInteraction::rescueConfigRestoresEverything()
{
    PetInteraction p = make(Level::Local, config(true, false, true, true, true));
    p.enter(0, {50, 50});
    QVERIFY(p.hidden());
    p.setConfig(PetBehaviourConfig::rescued(), 10);
    QCOMPARE(p.opacityTarget(), 1.0);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Mask));
    QVERIFY(!p.takeMove());
    QVERIFY(!p.needsPointer());
    // And at the global level.
    PetInteraction g = make(Level::Global, config(true, false, true, true, true));
    g.motionGlobal(0, {150, 150});
    g.tick(32);
    QVERIFY(g.hidden());
    g.setConfig(PetBehaviourConfig::rescued(), 40);
    QCOMPARE(g.opacityTarget(), 1.0);
    QCOMPARE(int(g.inputRegion()), int(InputRegion::Mask));
    g.motionGlobal(50, {200, 300});
    g.tick(100);
    QVERIFY(!g.takeMove());
    // The rescued pet is draggable.
    g.press(200, {50, 50});
    QVERIFY(g.dragging());
}

void TstPetInteraction::needsPointerOnlyWhileABehaviourIsOn()
{
    QVERIFY(!make(Level::Global, config(false, false, false, true, false)).needsPointer());
    QVERIFY(make(Level::Global, config(true, false, false, true, false)).needsPointer());
    QVERIFY(make(Level::Global, config(false, true, false, true, false)).needsPointer());
    QVERIFY(make(Level::Global, config(false, false, true, true, false)).needsPointer());
    QVERIFY(!make(Level::Global, config(false, false, false, false, true)).needsPointer());
}

void TstPetInteraction::pointerSourceReportsOnlyWhileWanted()
{
    ManualPointerSource source(PointerSource::Level::Global);
    QSignalSpy spy(&source, &PointerSource::moved);
    source.moveTo({1, 2});
    QCOMPARE(spy.count(), 0);
    source.wanted(true);
    source.moveTo({3, 4});
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.at(0).at(0).toPointF(), QPointF(3, 4));
    source.wanted(false);
    source.moveTo({5, 6});
    QCOMPARE(spy.count(), 1);
    QCOMPARE(source.wantedCalls(), 2);
    // Wired to the machine as the window code will.
    PetInteraction p = make(Level::Global, config(true, false, false, false, false));
    QObject::connect(&source, &PointerSource::moved, [&p](QPointF g) { p.motionGlobal(0, {g.x(), g.y()}); });
    source.wanted(p.needsPointer());
    source.moveTo({150, 150});
    p.tick(32);
    QVERIFY(p.hidden());
}

void TstPetInteraction::degradeNamesAreDistinct()
{
    std::set<std::string_view> seen;
    for (const auto name : degrade::kAll) {
        QVERIFY(!name.empty());
        QVERIFY(seen.insert(name).second);
    }
    QCOMPARE(int(seen.size()), 8);
}

void TstPetInteraction::aSlowApplyNeverDoublesAStep()
{
    PetInteraction p = make(Level::Global, config(false, false, true, false, false));
    p.motionGlobal(0, {200, 300});
    QVERIFY(p.takeMove());
    // The caller is slow to apply the move (more than the 220 ms cooldown): nothing new is computed from the stale rectangle.
    p.motionGlobal(300, {200, 300});
    p.tick(500);
    p.tick(1000);
    QVERIFY(!p.takeMove());
    // Applied: the window is reported and the next dodge starts from it (cooldown long over).
    p.setGeometry(kWindow.translated(0, -66), kOutput);
    p.motionGlobal(1100, {80, 134}); // 20 px left of the moved window, level with its centre
    const auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 66.0);
}

void TstPetInteraction::dodgeTowardEveryEdgeAndADiagonalCut()
{
    const PetRect w{500, 250, 200, 200}; // centre (600, 350); the step is 66
    const struct {
        PetPoint cursor;
        double dx, dy;
    } cases[] = {{{480, 350}, 66, 0}, {{720, 350}, -66, 0}, {{600, 230}, 0, 66}, {{600, 470}, 0, -66}};
    for (const auto &c : cases) {
        PetInteraction p = make(Level::Global, config(false, false, true, false, false), w);
        p.motionGlobal(0, c.cursor);
        const auto move = p.takeMove();
        QVERIFY(move);
        QCOMPARE(move->dx, c.dx);
        QCOMPARE(move->dy, c.dy);
    }
    // A diagonal step that hits the right edge keeps the part that fits on the other axis.
    PetInteraction q = make(Level::Global, config(false, false, true, false, false), {1050, 0, 200, 200});
    q.motionGlobal(0, {1040, 60}); // centre (1150, 100): the vector is (110, 40)
    const auto move = q.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 30.0);
    QCOMPARE(move->dy, 23.0);
}

void TstPetInteraction::nonFiniteInputsAreIgnored()
{
    const double nan = std::nan("");
    const double inf = std::numeric_limits<double>::infinity();
    PetInteraction g = make(Level::Global, config(true, false, true, false, false));
    g.motionGlobal(0, {nan, 150});
    g.motionGlobal(10, {150, inf});
    g.tick(32);
    QVERIFY(!g.hidden());
    QVERIFY(!g.takeMove());
    g.press(40, {nan, 5});
    QVERIFY(!g.dragging());
    PetInteraction l = make(Level::Local, config(true, false, false, false, false));
    l.enter(0, {nan, nan});
    QVERIFY(!l.hidden());
    l.motionLocal(10, {inf, 0});
    QVERIFY(!l.hidden());
    // A drag ignores a non-finite motion and keeps its state.
    PetInteraction d = make(Level::Local, config(false, false, false, false, false));
    d.press(0, {50, 50});
    d.motionLocal(100, {nan, 0});
    QVERIFY(!d.takeMove());
    QVERIFY(d.dragging());
}

void TstPetInteraction::setLevelEndsADragAndClearsPendingMoves()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(0, {50, 50});
    p.motionLocal(100, {200, 200});
    QVERIFY(p.dragging());
    p.setLevel(Level::Global, 110);
    QVERIFY(!p.dragging());
    QCOMPARE(int(p.layout()), int(PetLayout::Small));
    QVERIFY(!p.takeMove());
    // Same level: nothing happens.
    PetInteraction q = make(Level::Local, config(false, false, false, false, false));
    q.press(0, {50, 50});
    q.setLevel(Level::Local, 10);
    QVERIFY(q.dragging());
}

void TstPetInteraction::switchingBetweenHideAndFadeUnderAStillPointer()
{
    PetInteraction p = make(Level::Global, config(true, false, false, false, false));
    p.motionGlobal(0, {150, 150});
    p.tick(32);
    QCOMPARE(p.opacityTarget(), 0.0);
    p.setConfig(config(false, true, false, false, false), 40);
    QCOMPARE(p.opacityTarget(), 0.18);
    QCOMPARE(int(p.inputRegion()), int(InputRegion::Empty));
    p.setConfig(config(true, false, false, false, false), 50);
    QCOMPARE(p.opacityTarget(), 0.0);
}

void TstPetInteraction::leaveAndEnterDuringADragDoNotEndIt()
{
    PetInteraction p = make(Level::Local, config(true, false, false, false, false));
    p.press(0, {50, 50});
    QVERIFY(p.dragging());
    p.leave(10);
    p.enter(20, {500, 500});
    QVERIFY(p.dragging());
    QVERIFY(!p.hidden()); // no fade while dragging, whatever the events say
    p.motionLocal(100, {200, 200});
    QVERIFY(p.takeMove());
}

void TstPetInteraction::theSettleWindowIsExactly16ms()
{
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.press(1000, {50, 50}); // output (150, 150)
    p.motionLocal(1015, {170, 150});
    QVERIFY(!p.takeMove()); // 15 ms: dropped
    p.motionLocal(1016, {170, 150});
    const auto move = p.takeMove();
    QVERIFY(move); // 16 ms: taken
    QCOMPARE(move->dx, 20.0);
}

void TstPetInteraction::aDragEndingOutsideTheSmallWindowForgetsThePointer()
{
    // Dropped far from where the small window ends up: turning a fade on afterwards must not hide a pet the pointer is not on.
    PetInteraction p = make(Level::Local, config(false, false, false, false, false));
    p.enter(0, {50, 50});
    p.press(10, {50, 50});
    p.motionLocal(100, {5000, 5000}); // the window stops 48 px inside the output; the pointer goes on
    const auto move = p.takeMove();
    QVERIFY(move);
    p.release(120);
    p.setGeometry(kWindow.translated(move->dx, move->dy), kOutput);
    p.setConfig(config(true, false, false, false, false), 130);
    QVERIFY(!p.hidden());
    // Dropped back on itself: the pointer is on the window.
    PetInteraction q = make(Level::Local, config(false, false, false, false, false));
    q.press(10, {50, 50});
    q.motionLocal(100, {160, 160});
    q.release(120);
    q.setConfig(config(true, false, false, false, false), 130);
    QVERIFY(q.hidden());
}

void TstPetInteraction::aDegenerateDodgeTuningStillWorks()
{
    PetBehaviourTuning t;
    t.dodgeStepMin = 200;
    t.dodgeStepMax = 100; // min > max: the minimum wins, no std::clamp precondition violated
    PetInteraction p(Level::Global, t);
    p.setGeometry({500, 250, 200, 200}, kOutput);
    p.setMask(leftHalf);
    p.setConfig(config(false, false, true, false, false), 0);
    p.motionGlobal(0, {480, 350});
    const auto move = p.takeMove();
    QVERIFY(move);
    QCOMPARE(move->dx, 200.0);
    // A window that was never given a geometry (zero rectangle) does not crash or move.
    PetInteraction z(Level::Global);
    z.setConfig(config(true, false, true, false, false), 0);
    z.motionGlobal(0, {0, 0});
    z.tick(32);
    QVERIFY(!z.takeMove());
}

QTEST_APPLESS_MAIN(TstPetInteraction)
#include "tst_petinteraction.moc"
