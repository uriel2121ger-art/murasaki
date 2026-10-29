#pragma once

// The numbers and the names of the pet interactions, in one place. A fact that changes (Anima's, or a measurement of
// docs/pets/pointer-facts.md) changes a field here, not the logic of PetInteraction.

#include <array>
#include <string_view>

namespace murasaki {

struct PetBehaviourTuning {
    // Anima: opacity 0 under the cursor for hover_hide, 0.18 for focus_fade, both applied instantly (no animation).
    double hideOpacity = 0.0;
    double fadeOpacity = 0.18;
    // Anima's interaction tick, and the pointer rate Murasaki asks of a PointerSource.
    int tickMs = 32;
    // smart_dodge: the cursor within 26 px of the pet's rectangle; the step clamp(max(w, h) // 3, 54, 140) px away from it;
    // 0.22 s between steps.
    double dodgeMargin = 26;
    int dodgeStepDivisor = 3;
    int dodgeStepMin = 54;
    int dodgeStepMax = 140;
    int dodgeCooldownMs = 220;
    // The local-level hide: after a press on a faded pet its input region is empty for this long (the next clicks pass), then
    // it comes back (task 0, F11: KWin re-picks the surface under the pointer on the next press or motion).
    int releaseMs = 1200;
    // Grow on press (task 0, F12): motion events in this window after the press may still be in the old small-surface frame.
    int dragSettleMs = 16;
    // F13: a local step within this many pixels of the relative motion in between means the window's frame held still.
    double frameStillPx = 1.5;
    // A press and release that move the pet by no more than this many pixels are a click (the sound mode `click`); more is a drag.
    double clickSlopPx = 4;
};

// The names a pet's `degraded` list can carry (docs/pets/behaviours.md, section 5 of the phase 2 plan).
namespace degrade {
inline constexpr std::string_view kHideClicksSwallowed = "hide under cursor: clicks swallowed";
inline constexpr std::string_view kDodgeOnlyWhenTouched = "smart dodge: only when touched";
inline constexpr std::string_view kHitMaskCoarse = "hit mask: coarse";
inline constexpr std::string_view kHitMaskSilhouette = "hit mask: silhouette";
inline constexpr std::string_view kHitMaskNone = "hit mask: none";
inline constexpr std::string_view kKeepAboveBottom = "keep above off: always below windows";
inline constexpr std::string_view kKeepAboveUnsupported = "keep above off: unsupported";
inline constexpr std::string_view kPointerLocal = "pointer: local"; // informational, not a gap by itself

inline constexpr std::array<std::string_view, 8> kAll = {kHideClicksSwallowed, kDodgeOnlyWhenTouched, kHitMaskCoarse,
                                                         kHitMaskSilhouette, kHitMaskNone, kKeepAboveBottom,
                                                         kKeepAboveUnsupported, kPointerLocal};

// Who draws the pet: the layer-shell protocol (Plasma, wlroots, ...), an X11 override-redirect window, or a plain window the GNOME extension pins.
enum class PetPresenter { LayerShell, X11, Shell };

// What keepAbove off costs on each presenter, or nothing (an empty name). Layer-shell can only reach the bottom layer, which is below every
// window; X11 does it exactly; GNOME stays above until the extension's marker is checked on a real shell.
constexpr std::string_view keepAboveDegrade(bool keepAbove, PetPresenter presenter)
{
    if (keepAbove)
        return {};
    switch (presenter) {
    case PetPresenter::LayerShell:
        return kKeepAboveBottom;
    case PetPresenter::Shell:
        return kKeepAboveUnsupported;
    case PetPresenter::X11:
        break;
    }
    return {};
}
} // namespace degrade

} // namespace murasaki
