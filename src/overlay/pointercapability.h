#pragma once

#include "compositorprofile.h"
#include "pointersource.h"

#include <QString>

#include <string_view>
#include <vector>

namespace murasaki {

enum class PointerBackend {
    KWinScript,  // KWinPointerSource: Plasma on Wayland
    HyprlandIpc, // HyprlandPointerSource
    X11Poll,     // PollingPointerSource with QCursor::pos(): any X11 session
    Local        // LocalPointerSource: only the pet's own enter / leave / motion
};

// What the session offers, from facts the caller has gathered (so that this stays pure and the tests give it any combination).
struct PointerEnvironment {
    bool kwinOnBus = false;       // org.kde.KWin is on the session bus
    bool x11Session = false;      // the Qt platform is xcb
    QString hyprlandSocketDir;    // HyprlandPointerSource::socketDir(env); empty when there is none
    QString override;             // $MURASAKI_POINTER: "local" forces the local level, "auto" or empty lets the session decide
};

struct PointerCapability {
    PointerBackend backend = PointerBackend::Local;
    PointerSource::Level level = PointerSource::Level::Local;
    QString reason;                              // one line for the log
    std::vector<std::string_view> degraded;      // the names of petbehaviours.h that follow from the level (`pointer: local`)
};

// Picks the backend:
//   X11 session (whatever the desktop)                      -> X11Poll        (never the KWin script: its D-Bus call per movement buys nothing there)
//   KDE with org.kde.KWin on the bus, Wayland               -> KWinScript
//   Hyprland with its sockets                               -> HyprlandIpc
//   everything else (sway, labwc, river, wayfire, niri, COSMIC, GNOME, unknown, KDE without KWin on the bus) -> Local
// MURASAKI_POINTER=local forces Local; any other value is ignored (a global source can not be forced into a session that has none).
PointerCapability choosePointerBackend(const CompositorProfile &profile, const PointerEnvironment &environment);

} // namespace murasaki
