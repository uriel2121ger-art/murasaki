#pragma once

// The state machine of a pet's pointer interactions: hide under cursor, focus fade, smart dodge, and the drag. Pure: no Qt
// GUI, no clock (every input carries `nowMs`), no I/O. The window code feeds it what the compositor says and applies what it
// answers; `tst_petinteraction` drives it with scripted pointer paths.
//
// What it stands on (docs/pets/pointer-facts.md):
//   - Level::Global (KWin script, Hyprland IPC, X11): the cursor position is known whether or not it is over the pet; the
//     machine is Anima's: over an opaque pixel the pet is faded and click-through, off it it is restored.
//   - Level::Local (everything else): only enter, leave and motion on the pet's own surface. The pet must hold input to hear
//     them, so a click-through pet with fade or dodge on takes clicks (the named degrade). A faded pet keeps the union
//     silhouette as its region, so the region does not change while it is invisible.
//   - F11: KWin re-picks the surface under the pointer on the next motion or press, not when a region changes; sway and
//     Hyprland only on motion. The machine never waits for enter or leave as the answer to its own region change; a press on a
//     faded pet (local, or global on a compositor that gives the press to the surface the pointer last moved onto)
//     empties the region for `releaseMs`, and `leave` / `enter` are treated as the consequence of motion. Once
//     the button is up, released() asks the window to unmap for the rest of that time: no compositor keeps the pointer focus on
//     an unmapped surface, so the next clicks pass to the window below even with the pointer still.
//   - Coordinates: `window`, `output`, every `global` position and every move are in one space (the output's, origin included,
//     so a virtual-desktop offset works); `local` is relative to the window's top-left. Clamps use `output` as a rectangle.
//   - Time: every input carries `nowMs`, one monotonic clock (a steady clock, never wall time or event timestamps mixed with a
//     timer), never going backwards. Outputs reflect the time of the last input; a release that ends is noticed by the next input.
//   - A move (dodge) is taken with takeMove() and applied by the caller, who then calls setGeometry() with the new window. No new
//     dodge starts until that call, so a slow apply (D-Bus, a store write) can not double the step.
//   - Non-finite positions are ignored.
//   - F12: after the press that grows the window, motion may still be in the old frame (the small window where the press was):
//     KWin and Hyprland switch to the grown window's frame within milliseconds, sway never does during the grab. The drag reads
//     every event both ways and takes the one nearer the last pointer position (the pointer does not jump by the window's
//     offset); once the grown frame wins it stays, no compositor goes back. It takes the absolute pointer position, with the
//     press position as the reference, and ignores motion for `dragSettleMs`.
//   - F13: a compositor may animate the window's growth (Hyprland slides a layer surface to its new place): in the middle of it
//     the frame is neither the old nor the grown one. Relative motion (zwp_relative_pointer_v1) does not depend on any frame:
//     when the local step between two events matches the relative motion in between, the frame held still and F12 decides;
//     when it does not, the frame moved, and the pointer is the last one plus the relative motion. A drag that hears no
//     relative motion (no protocol, an absolute device) is F12 alone.

#include "petbehaviours.h"
#include "petgeometry.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace murasaki {

struct PetBehaviourConfig {
    bool hoverHide = false;
    bool focusFade = false; // ignored while hoverHide is on (the placement keeps them exclusive; the machine does not trust it)
    bool smartDodge = false;
    bool clickThrough = true;
    bool locked = false;

    // What the rescue sets: nothing hides, fades, dodges, holds still or lets the pointer through.
    static PetBehaviourConfig rescued()
    {
        PetBehaviourConfig c;
        c.clickThrough = false;
        return c;
    }
    bool operator==(const PetBehaviourConfig &) const = default;
};

// The input region the window should have.
enum class InputRegion {
    Mask,  // the silhouette of the current frame
    Union, // the union of all frames' silhouettes (static: a faded pet's region does not change)
    Empty  // takes no pointer
};

// Which of the three layouts of the plan's section 3.3 (arrange is the controller's) the window should have.
enum class PetLayout { Small, Drag };

struct PetMove {
    double dx = 0;
    double dy = 0;
};

class PetInteraction
{
public:
    enum class Level { Global, Local };
    // Pet-local pixel -> is it opaque? (window coordinates of the pet's window). Without one the whole window counts.
    using MaskTest = std::function<bool(PetPoint)>;

    explicit PetInteraction(Level level = Level::Global, PetBehaviourTuning tuning = {});

    void setLevel(Level level, std::int64_t nowMs);
    void setConfig(const PetBehaviourConfig &config, std::int64_t nowMs);
    // The pet's window rectangle and the output, both in output coordinates. Called again after a move is applied.
    void setGeometry(PetRect window, PetRect output);
    void setMask(MaskTest opaque);

    // ---- inputs (positions: `local` is window coordinates, `global` output coordinates) ----
    void enter(std::int64_t nowMs, PetPoint local);
    void leave(std::int64_t nowMs);
    void motionLocal(std::int64_t nowMs, PetPoint local);
    // Relative pointer motion (accelerated, as the cursor moves); only a drag uses it (F13).
    void motionRelative(std::int64_t nowMs, double dx, double dy);
    void motionGlobal(std::int64_t nowMs, PetPoint global);
    void press(std::int64_t nowMs, PetPoint local);
    void release(std::int64_t nowMs);
    void tick(std::int64_t nowMs);

    // ---- outputs ----
    // Whether a pointer source has to run: any of the three behaviours on.
    bool needsPointer() const;
    // The factor on the placement's opacity: 1, the hide opacity or the fade opacity.
    double opacityTarget() const;
    InputRegion inputRegion() const;
    PetLayout layout() const;
    bool hidden() const;   // faded by hover or focus (what Windows() reports)
    bool released() const; // a click went through a faded pet; the window unmaps until the release ends (F11)
    bool dragging() const;
    // The step the window should take now; reading it clears it.
    std::optional<PetMove> takeMove();
    // Whether the last release ended a click: a press on an opaque pixel of a pet that takes the pointer (not click-through, not faded), released without
    // a drag (a locked pet can not be dragged: its press is a click). Reading it clears it.
    bool takeClick();
    // Names of section 5 that apply to this pet right now (see petbehaviours.h).
    std::vector<std::string_view> degraded() const;

private:
    bool fadeOn() const;
    bool hit(PetPoint local) const;
    bool dodgeReady(std::int64_t nowMs) const;
    void dodgeFrom(std::int64_t nowMs, PetPoint cursor);
    void evaluateGlobal(std::int64_t nowMs);
    void endDrag();
    void expireRelease(std::int64_t nowMs);

    Level m_level;
    PetBehaviourTuning m_tuning;
    PetBehaviourConfig m_config;
    PetRect m_window;
    PetRect m_output;
    MaskTest m_mask;

    std::optional<PetPoint> m_cursor; // global level: last known cursor, output coordinates
    bool m_inside = false;            // local level: the compositor says the pointer is on the surface
    bool m_faded = false;
    std::optional<std::int64_t> m_releaseUntil; // local level: the region is empty until this time
    bool m_releasePressed = false;              // the button of the press that started the release is still down
    std::optional<std::int64_t> m_lastDodge;
    bool m_dodgePending = false; // a dodge was emitted and setGeometry() has not followed
    std::optional<PetMove> m_move;

    bool m_dragging = false;
    bool m_clickCandidate = false; // a press that may become a click
    bool m_clicked = false;
    PetPoint m_pressGlobal;
    PetRect m_dragStart;
    std::int64_t m_pressAt = 0;
    PetMove m_dragReported;
    PetPoint m_dragPointer; // last accepted pointer position of the drag, output coordinates
    bool m_dragGrownFrame = false; // the drag's motion comes in the grown window's frame (F12)
    void moveDragTo(PetPoint pointer); // the drag's pointer is there now: the window follows
    PetPoint m_dragLastLocal;      // the local position of the last event of the drag (the press, then each motion)
    PetMove m_relativePending;     // relative motion heard since that event (F13)
    bool m_relativeSeen = false;   // this drag hears relative motion
};

} // namespace murasaki
