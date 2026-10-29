#pragma once

#include "sessionstate.h"

#include <QProcessEnvironment>
#include <QSet>
#include <QString>

namespace murasaki {

enum class CompositorFamily { Unknown, Kde, Sway, Hyprland, Wlroots, Cosmic, Gnome, X11 };

// Which wallpaper layer of wlr-layer-shell the windows use.
enum class WallpaperLayer { Bottom, Background };

// What Murasaki does on a given compositor, decided from the environment and the Wayland globals the compositor offers
// (the names of the interfaces the registry lists). Pure: the tests give it fake environments and registries.
struct CompositorProfile {
    CompositorFamily family = CompositorFamily::Unknown;
    bool layerShell = false;           // zwlr_layer_shell_v1 is there: the surface can be a wallpaper
    WallpaperLayer layer = WallpaperLayer::Bottom;
    MonitorKind monitor = MonitorKind::None; // where occlusion and desktops come from
    // The application classes of the pause rules come from the foreign-toplevel protocol on top of `monitor` (Sway and
    // Hyprland know occlusion but not app ids; KWin brings its own).
    bool appsFromToplevels = false;
    QString summary;                   // one line for the log and the docs
};

// The Wayland globals that matter.
inline constexpr char kLayerShellGlobal[] = "zwlr_layer_shell_v1";
inline constexpr char kWlrToplevelGlobal[] = "zwlr_foreign_toplevel_manager_v1";
inline constexpr char kExtToplevelListGlobal[] = "ext_foreign_toplevel_list_v1";

// `kwinOnBus`: org.kde.KWin is on the session bus; `gnomeShellOnBus`: org.gnome.Shell is (the extension reports through it). `x11Session`: the Qt platform is xcb;
// whatever desktop it is, the window manager's EWMH state is the source then. `globals`: interface names of the registry (empty when unknown).
CompositorProfile selectCompositorProfile(const QProcessEnvironment &env, bool kwinOnBus, const QSet<QString> &globals,
                                          bool gnomeShellOnBus = false, bool x11Session = false);

} // namespace murasaki
