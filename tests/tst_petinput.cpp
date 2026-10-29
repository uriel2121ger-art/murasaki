// PetInput: the pointer input of one pet window without the window (src/overlay/petinput.h).

#include "petinput.h"

#include <QSignalSpy>
#include <QTest>

#include <memory>

using namespace murasaki;
using Level = PointerSource::Level;

namespace {

constexpr int kPicW = 200;
constexpr int kPicH = 100;

// Three frames of a 200 x 100 picture; frame i has an opaque block 40 x 40 at x = 20 + 60 i, y = 30.
std::shared_ptr<const MaskSet> blockMasks(int frames = 3)
{
    MaskBuilder builder(kPicW, kPicH, frames);
    for (int i = 0; i < frames; ++i) {
        std::vector<std::uint8_t> alpha(kPicW * kPicH, 0);
        for (int y = 30; y < 70; ++y) {
            for (int x = 20 + 60 * i; x < 60 + 60 * i; ++x)
                alpha[std::size_t(y) * kPicW + std::size_t(x)] = 255;
        }
        builder.addFrame(alpha.data(), kPicW, 1, kPicW, kPicH);
    }
    return std::make_shared<const MaskSet>(builder.finish());
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

// The pet shown at half size: window 100 x 50 at (300, 200) in a 1280 x 720 output.
void place(PetInput &in, PetRect window = {300, 200, 100, 50})
{
    in.setGeometry(window, {1280, 720});
}

bool covers(const std::vector<MaskRect> &rects, int x, int y)
{
    for (const MaskRect &r : rects) {
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h)
            return true;
    }
    return false;
}

} // namespace

class TstPetInput : public QObject
{
    Q_OBJECT

private slots:
    void aClickThroughPetTakesNothing();
    void aPetWithoutMasksTakesItsWholeRectangle();
    void theRegionIsTheMaskScaledToTheWindow();
    void theRegionFollowsTheFrameAtMost30TimesASecond();
    void theVideoPathUsesTheUnion();
    void globalHoverHideFadesAndLetsThroughAndRestores();
    void localHoverKeepsTheUnionAndReleasesAfterAPress();
    void aDragGrowsTheWindowAndMovesThePet();
    void aLockedPetDoesNotDrag();
    void dodgeAsksForAStepAndWaitsForTheGeometry();
    void shellWindowsAreOutputSized();
    void aDisabledInputIsNeutral();
    void statusNamesWhatIsDegraded();
    void turnedMasksFollowTheTransform();
    void theTickRunsOnlyWhileABehaviourIsOn();
    void pointerEventsTellTheStatesWhatTheDragAndTheDodgeDid();
    void aSwitchOfThePictureDuringADragNeitherEndsItNorMovesThePet();
};

void TstPetInput::aClickThroughPetTakesNothing()
{
    PetInput in;
    place(in);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, true, false));
    QCOMPARE(int(in.region()), int(InputRegion::Empty));
    QVERIFY(in.regionRects().empty());
    QCOMPARE(in.opacityFactor(), 1.0);
    QVERIFY(!in.needsPointer());
    QVERIFY(!in.wantsGlobalPointer());
    in.press({50, 25}, false);
    QCOMPARE(int(in.layout()), int(PetLayout::Small)); // click-through pets never receive
}

void TstPetInput::aPetWithoutMasksTakesItsWholeRectangle()
{
    PetInput in;
    place(in);
    in.setBehaviour(config(false, false, false, false, false));
    QCOMPARE(int(in.region()), int(InputRegion::Mask));
    const auto &rects = in.regionRects();
    QCOMPARE(int(rects.size()), 1);
    QCOMPARE(rects[0], (MaskRect{0, 0, 100, 50}));
    QCOMPARE(in.status().hitMask, QStringLiteral("none"));
    QVERIFY(std::find(in.status().degraded.begin(), in.status().degraded.end(), degrade::kHitMaskNone) != in.status().degraded.end());
}

void TstPetInput::theRegionIsTheMaskScaledToTheWindow()
{
    PetInput in;
    place(in);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, false, false));
    const auto &rects = in.regionRects();
    QVERIFY(!rects.empty());
    QVERIFY(int(rects.size()) <= kMaxFrameRects);
    // Frame 0: the block at (20, 30) 40 x 40 of a 200 x 100 picture, shown at half size: (10, 15) 20 x 20.
    QVERIFY(covers(rects, 20, 25));
    QVERIFY(covers(rects, 10, 15));
    QVERIFY(!covers(rects, 50, 25));
    QVERIFY(!covers(rects, 2, 2));
    for (const MaskRect &r : rects)
        QVERIFY(r.x >= 0 && r.y >= 0 && r.x + r.w <= 100 && r.y + r.h <= 50);
}

void TstPetInput::theRegionFollowsTheFrameAtMost30TimesASecond()
{
    PetInput in;
    place(in);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, false, false));
    QVERIFY(covers(in.regionRects(), 20, 25));
    QSignalSpy changed(&in, &PetInput::changed);
    in.setFrame(1);
    QTRY_COMPARE(changed.count(), 1);
    QVERIFY(covers(in.regionRects(), 20 + 30, 25));
    QVERIFY(!covers(in.regionRects(), 20, 25));
    // Frames changing faster than 30 Hz are coalesced into region changes at most that often.
    QElapsedTimer clock;
    clock.start();
    changed.clear();
    for (int i = 0; i < 40; ++i) {
        in.setFrame(i % 3 == 0 ? 2 : (i % 3 == 1 ? 0 : 1));
        QTest::qWait(5);
    }
    QTest::qWait(60);
    QVERIFY2(changed.count() <= clock.elapsed() / PetInput::kRegionMinIntervalMs + 2, qPrintable(QString::number(changed.count())));
    QVERIFY(changed.count() >= 2);
}

void TstPetInput::theVideoPathUsesTheUnion()
{
    PetInput in;
    place(in);
    in.setMasks(blockMasks(), false); // the frame on screen is not known
    in.setBehaviour(config(false, false, false, false, false));
    const auto &rects = in.regionRects();
    // The union of the three blocks: x 20..180 of the picture, 10..90 in the window.
    QVERIFY(covers(rects, 12, 25));
    QVERIFY(covers(rects, 40, 25));
    QVERIFY(covers(rects, 80, 25));
    QCOMPARE(in.status().hitMask, QStringLiteral("silhouette"));
    QSignalSpy changed(&in, &PetInput::changed);
    in.setFrame(2); // frame changes mean nothing without tracking
    QTest::qWait(80);
    QCOMPARE(changed.count(), 0);
}

void TstPetInput::globalHoverHideFadesAndLetsThroughAndRestores()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Global);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(true, false, false, false, false));
    QVERIFY(in.wantsGlobalPointer());
    QCOMPARE(int(in.region()), int(InputRegion::Mask));
    QSignalSpy changed(&in, &PetInput::changed);
    in.globalPointer({320, 225}); // window-local (20, 25): on the block of frame 0
    QCOMPARE(in.opacityFactor(), 0.0);
    QCOMPARE(int(in.region()), int(InputRegion::Empty)); // click-through while hidden
    QVERIFY(in.regionRects().empty());
    QVERIFY(changed.count() >= 1);
    QVERIFY(in.status().hidden);
    in.globalPointer({395, 205}); // inside the window, on a transparent pixel
    QCOMPARE(in.opacityFactor(), 1.0);
    QCOMPARE(int(in.region()), int(InputRegion::Mask));
    QVERIFY(!in.regionRects().empty());
    QVERIFY(!in.status().hidden);
}

void TstPetInput::localHoverKeepsTheUnionAndReleasesAfterAPress()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(true, false, false, false, false));
    QVERIFY(!in.wantsGlobalPointer());
    QVERIFY(in.needsPointer());
    in.enter({20, 25}, false);
    QCOMPARE(in.opacityFactor(), 0.0);
    QCOMPARE(int(in.region()), int(InputRegion::Union)); // the region does not change while it is invisible
    const auto union_ = in.regionRects();
    QVERIFY(!union_.empty());
    QVERIFY(covers(union_, 12, 25) && covers(union_, 80, 25));
    in.press({20, 25}, false); // the first click is swallowed; the region is empty for a while
    QCOMPARE(int(in.region()), int(InputRegion::Empty));
    QVERIFY(in.regionRects().empty());
    QVERIFY(std::find(in.status().degraded.begin(), in.status().degraded.end(), degrade::kHideClicksSwallowed) != in.status().degraded.end());
    QTRY_COMPARE_WITH_TIMEOUT(int(in.region()), int(InputRegion::Union), 3000); // 1.2 s, noticed by the tick
    in.leave();
    QCOMPARE(in.opacityFactor(), 1.0);
    QCOMPARE(int(in.region()), int(InputRegion::Mask));
}

void TstPetInput::aDragGrowsTheWindowAndMovesThePet()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, false, false));
    QSignalSpy moves(&in, &PetInput::moveRequested);
    in.enter({20, 25}, false);
    in.press({20, 25}, false); // output (320, 225)
    QCOMPARE(int(in.layout()), int(PetLayout::Drag));
    QCOMPARE(int(in.region()), int(InputRegion::Mask));
    QTest::qWait(30); // the first events after the press may be in the old frame
    in.motion({420, 245}, true); // the grown window: output coordinates
    QCOMPARE(moves.count(), 1);
    QCOMPARE(moves.at(0).at(0).toDouble(), 100.0);
    QCOMPARE(moves.at(0).at(1).toDouble(), 20.0);
    place(in, {400, 220, 100, 50}); // the owner applied it
    in.motion({430, 245}, true);
    QCOMPARE(moves.count(), 2);
    QCOMPARE(moves.at(1).at(0).toDouble(), 10.0);
    QCOMPARE(moves.at(1).at(1).toDouble(), 0.0);
    in.release();
    QCOMPARE(int(in.layout()), int(PetLayout::Small));
}

void TstPetInput::aLockedPetDoesNotDrag()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, false, true));
    QSignalSpy moves(&in, &PetInput::moveRequested);
    in.enter({20, 25}, false);
    in.press({20, 25}, false);
    QCOMPARE(int(in.layout()), int(PetLayout::Small));
    QTest::qWait(30);
    in.motion({60, 30}, false);
    QCOMPARE(moves.count(), 0);
}

void TstPetInput::dodgeAsksForAStepAndWaitsForTheGeometry()
{
    PetInput in;
    place(in, {600, 300, 90, 90});
    in.setLevel(Level::Global);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, true, true, false)); // click-through pets dodge as well
    QVERIFY(in.wantsGlobalPointer());
    QCOMPARE(int(in.region()), int(InputRegion::Empty));
    QSignalSpy moves(&in, &PetInput::moveRequested);
    in.globalPointer({645, 400}); // 10 px below the pet
    QCOMPARE(moves.count(), 1);
    QCOMPARE(moves.at(0).at(0).toDouble(), 0.0);
    QCOMPARE(moves.at(0).at(1).toDouble(), -54.0);
    QTest::qWait(300);
    in.globalPointer({645, 401}); // the owner has not applied the step yet: no second one from the stale rectangle
    QCOMPARE(moves.count(), 1);
    place(in, {600, 246, 90, 90});
    in.globalPointer({645, 340});
    QCOMPARE(moves.count(), 2);
}

void TstPetInput::pointerEventsTellTheStatesWhatTheDragAndTheDodgeDid()
{
    using Kind = PetPointerEvent::Kind;
    // a drag: Grab at the press, the steps, Release (before the click, if it was one)
    {
        PetInput in;
        place(in);
        in.setLevel(Level::Local);
        in.setMasks(blockMasks(), true);
        in.setBehaviour(config(false, false, false, false, false));
        QSignalSpy events(&in, &PetInput::pointerEvent);
        in.enter({20, 25}, false);
        in.press({20, 25}, false);
        QCOMPARE(events.count(), 1);
        QCOMPARE(int(events.at(0).at(0).value<PetPointerEvent>().kind), int(Kind::Grab));
        QTest::qWait(30);
        in.motion({420, 245}, true);
        QCOMPARE(events.count(), 2);
        const auto step = events.at(1).at(0).value<PetPointerEvent>();
        QCOMPARE(int(step.kind), int(Kind::DragMove));
        QCOMPARE(step.dx, 100.0);
        QCOMPARE(step.dy, 20.0);
        in.release();
        QCOMPARE(events.count(), 3);
        QCOMPARE(int(events.at(2).at(0).value<PetPointerEvent>().kind), int(Kind::Release));
    }
    // a click that did not move it: Grab, Release, then clicked()
    {
        PetInput in;
        place(in);
        in.setLevel(Level::Local);
        in.setMasks(blockMasks(), true);
        in.setBehaviour(config(false, false, false, false, false));
        QList<int> order;
        connect(&in, &PetInput::pointerEvent, this, [&](const PetPointerEvent &event) { order << int(event.kind); });
        connect(&in, &PetInput::clicked, this, [&] { order << 100; });
        in.enter({20, 25}, false);
        in.press({20, 25}, false);
        in.release();
        QCOMPARE(order, (QList<int>{int(Kind::Grab), int(Kind::Release), 100}));
    }
    // a pet that cannot be grabbed says nothing: locked, click-through, a transparent pixel
    for (int variant = 0; variant < 3; ++variant) {
        PetInput in;
        place(in);
        in.setLevel(Level::Local);
        in.setMasks(blockMasks(), true);
        in.setBehaviour(variant == 0 ? config(false, false, false, false, true) : variant == 1 ? config(false, false, false, true, false) : config(false, false, false, false, false));
        QSignalSpy events(&in, &PetInput::pointerEvent);
        const QPointF at = variant == 2 ? QPointF(90, 5) : QPointF(20, 25); // (90, 5) is a transparent pixel
        in.enter(at, false);
        in.press(at, false);
        in.release();
        QCOMPARE(events.count(), 0);
    }
    // a dodge step is a Dodge with the length of the cooldown, and it is not a DragMove
    {
        PetInput in;
        place(in, {600, 300, 90, 90});
        in.setLevel(Level::Global);
        in.setMasks(blockMasks(), true);
        in.setBehaviour(config(false, false, true, true, false));
        QSignalSpy events(&in, &PetInput::pointerEvent);
        in.globalPointer({645, 400});
        QCOMPARE(events.count(), 1);
        const auto step = events.at(0).at(0).value<PetPointerEvent>();
        QCOMPARE(int(step.kind), int(Kind::Dodge));
        QCOMPARE(step.dy, -54.0);
        QCOMPARE(step.durationMs, PetBehaviourTuning{}.dodgeCooldownMs);
    }
    // letting go because the window is being edited or hidden ends the grab too
    {
        PetInput in;
        place(in);
        in.setLevel(Level::Local);
        in.setMasks(blockMasks(), true);
        in.setBehaviour(config(false, false, false, false, false));
        QSignalSpy events(&in, &PetInput::pointerEvent);
        in.enter({20, 25}, false);
        in.press({20, 25}, false);
        in.setEnabled(false);
        QCOMPARE(events.count(), 2);
        QCOMPARE(int(events.at(1).at(0).value<PetPointerEvent>().kind), int(Kind::Release));
    }
}

void TstPetInput::aSwitchOfThePictureDuringADragNeitherEndsItNorMovesThePet()
{
    // The picture of a pet changes with its state (another file: other masks, another height, the same top-left corner and width). The drag is the pointer's.
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, false, false));
    QSignalSpy moves(&in, &PetInput::moveRequested);
    in.enter({20, 25}, false);
    in.press({20, 25}, false);
    QCOMPARE(int(in.layout()), int(PetLayout::Drag));
    QTest::qWait(30);
    in.motion({420, 245}, true);
    QCOMPARE(moves.count(), 1);
    place(in, {400, 220, 100, 50});
    // the state changes: other masks (one frame, the whole picture), a taller window with the same corner
    MaskBuilder builder(kPicW, kPicH, 1);
    std::vector<std::uint8_t> opaque(kPicW * kPicH, 255);
    builder.addFrame(opaque.data(), kPicW, 1, kPicW, kPicH);
    in.setMasks(std::make_shared<const MaskSet>(builder.finish()), true);
    in.setFrame(0);
    place(in, {400, 220, 100, 80});
    QCOMPARE(moves.count(), 1);                            // the switch moved nothing
    QCOMPARE(int(in.layout()), int(PetLayout::Drag));      // and the drag goes on
    QCOMPARE(int(in.region()), int(InputRegion::Mask));
    in.motion({440, 255}, true);
    QCOMPARE(moves.count(), 2);
    QCOMPARE(moves.at(1).at(0).toDouble(), 20.0);          // the pointer moved 20 px since the pet was last placed at x 400 and grabbed at 420
    QCOMPARE(moves.at(1).at(1).toDouble(), 10.0);
    in.release();
    QCOMPARE(int(in.layout()), int(PetLayout::Small));
}

void TstPetInput::shellWindowsAreOutputSized()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(true, false, false, false, false));
    // The window covers the output: positions are output positions, the pet is at (300, 200).
    in.enter({320, 225}, true);
    QCOMPARE(in.opacityFactor(), 0.0);
    in.leave();
    in.enter({390, 205}, true); // inside the window rectangle, off the block
    QCOMPARE(in.opacityFactor(), 1.0);
    in.enter({10, 10}, true);   // nowhere near the pet
    QCOMPARE(in.opacityFactor(), 1.0);
}

void TstPetInput::aDisabledInputIsNeutral()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(true, false, false, false, false));
    in.setEnabled(false);
    QCOMPARE(in.opacityFactor(), 1.0);
    QCOMPARE(int(in.layout()), int(PetLayout::Small));
    QVERIFY(!in.needsPointer());
    in.enter({20, 25}, false);
    QCOMPARE(in.opacityFactor(), 1.0);
    in.setEnabled(true);
    QCOMPARE(in.opacityFactor(), 1.0); // (the enter was ignored)
}

void TstPetInput::statusNamesWhatIsDegraded()
{
    PetInput in;
    place(in);
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, true, false));
    auto status = in.status();
    QCOMPARE(status.pointer, QStringLiteral("local"));
    QCOMPARE(status.hitMask, QStringLiteral("frame"));
    QVERIFY(status.maskBytes > 0);
    QVERIFY(std::find(status.degraded.begin(), status.degraded.end(), degrade::kPointerLocal) != status.degraded.end());
    QVERIFY(std::find(status.degraded.begin(), status.degraded.end(), degrade::kHitMaskNone) == status.degraded.end());
    in.setLevel(Level::Global);
    in.setBehaviour(config(true, false, false, false, false));
    status = in.status();
    QCOMPARE(status.pointer, QStringLiteral("global"));
    QVERIFY(status.degraded.empty());
    // A mask set of the union alone.
    MaskBuilder b(kPicW, kPicH, 20000, 100);
    std::vector<std::uint8_t> alpha(kPicW * kPicH, 255);
    b.addFrame(alpha.data(), kPicW, 1, kPicW, kPicH);
    in.setMasks(std::make_shared<const MaskSet>(b.finish()), true);
    status = in.status();
    QCOMPARE(status.hitMask, QStringLiteral("silhouette"));
    QVERIFY(std::find(status.degraded.begin(), status.degraded.end(), degrade::kHitMaskSilhouette) != status.degraded.end());
}

void TstPetInput::turnedMasksFollowTheTransform()
{
    PetInput in;
    in.setLevel(Level::Local);
    in.setMasks(blockMasks(), true);
    in.setBehaviour(config(false, false, false, false, false));
    // Turned 90 degrees the 200 x 100 picture is 100 x 200: the window is the footprint, shown at half size: 50 x 100.
    in.setTransform(90, false, false);
    place(in, {300, 200, 50, 100});
    const auto &rects = in.regionRects();
    QVERIFY(!rects.empty());
    // The block of frame 0 at picture (20..60, 30..70) lands at (30..70, 20..60) of the turned 100 x 200 picture, halved: (15..35, 10..30).
    QVERIFY(covers(rects, 25, 20));
    QVERIFY(!covers(rects, 25, 60));
    for (const MaskRect &r : rects)
        QVERIFY(r.x + r.w <= 50 && r.y + r.h <= 100);
}

void TstPetInput::theTickRunsOnlyWhileABehaviourIsOn()
{
    // (observable through the dodge: it needs the tick to see a pointer that rests)
    PetInput in;
    place(in, {600, 300, 90, 90});
    in.setLevel(Level::Global);
    in.setBehaviour(config(false, false, false, true, false));
    QVERIFY(!in.needsPointer());
    in.setBehaviour(config(false, true, false, true, false));
    QVERIFY(in.needsPointer());
    in.setBehaviour(config(false, false, false, true, false));
    QVERIFY(!in.needsPointer());
}

QTEST_MAIN(TstPetInput)
#include "tst_petinput.moc"
