#include "compositorprofile.h"

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

bool desktopIs(const QProcessEnvironment &env, const QString &name)
{
    return env.value(u"XDG_CURRENT_DESKTOP"_s).split(u':', Qt::SkipEmptyParts).contains(name, Qt::CaseInsensitive);
}

} // namespace

CompositorProfile selectCompositorProfile(const QProcessEnvironment &env, bool kwinOnBus, const QSet<QString> &globals,
                                          bool gnomeShellOnBus, bool x11Session)
{
    CompositorProfile profile;
    profile.layerShell = globals.contains(QLatin1String(kLayerShellGlobal));
    const bool wlrToplevels = globals.contains(QLatin1String(kWlrToplevelGlobal));

    const bool kde = !env.value(u"KDE_FULL_SESSION"_s).isEmpty() || desktopIs(env, u"KDE"_s);
    if (x11Session) {
        profile.family = CompositorFamily::X11;
    } else if (kde && kwinOnBus) {
        profile.family = CompositorFamily::Kde;
    } else if (!env.value(u"SWAYSOCK"_s).isEmpty() || desktopIs(env, u"sway"_s)) {
        profile.family = CompositorFamily::Sway;
    } else if (!env.value(u"HYPRLAND_INSTANCE_SIGNATURE"_s).isEmpty() || desktopIs(env, u"Hyprland"_s)) {
        profile.family = CompositorFamily::Hyprland;
    } else if (desktopIs(env, u"COSMIC"_s)) {
        profile.family = CompositorFamily::Cosmic;
    } else if (desktopIs(env, u"GNOME"_s)) {
        profile.family = CompositorFamily::Gnome;
    } else if (profile.layerShell) {
        profile.family = CompositorFamily::Wlroots; // labwc, river, wayfire, niri, ...: anything that speaks layer-shell
    }

    // The wallpaper goes under everything on compositors that have a background layer of their own to cover; KWin draws
    // the desktop above the background layer, so Plasma keeps the bottom one.
    profile.layer = profile.family == CompositorFamily::Kde ? WallpaperLayer::Bottom : WallpaperLayer::Background;

    switch (profile.family) {
    case CompositorFamily::X11:
        profile.layerShell = false; // no layer-shell on X11 (the window is a desktop-type window, see x11.h)
        profile.monitor = MonitorKind::X11Ewmh;
        break;
    case CompositorFamily::Kde:
        profile.monitor = MonitorKind::KWin;
        break;
    case CompositorFamily::Gnome:
        // No layer-shell and no foreign-toplevel protocol: the Murasaki extension of the shell reports the windows.
        profile.monitor = gnomeShellOnBus ? MonitorKind::GnomeShell : MonitorKind::None;
        break;
    case CompositorFamily::Sway:
        profile.monitor = env.value(u"SWAYSOCK"_s).isEmpty() ? MonitorKind::None : MonitorKind::Sway;
        profile.appsFromToplevels = wlrToplevels;
        break;
    case CompositorFamily::Hyprland:
        profile.monitor = env.value(u"HYPRLAND_INSTANCE_SIGNATURE"_s).isEmpty() ? MonitorKind::None : MonitorKind::Hyprland;
        profile.appsFromToplevels = wlrToplevels;
        break;
    default:
        break;
    }
    if (profile.monitor == MonitorKind::None && wlrToplevels) {
        profile.monitor = MonitorKind::ForeignToplevel; // stacking compositors: the protocol says what covers an output
        profile.appsFromToplevels = false;              // the same source already brings the app ids
    }

    static const char *const names[] = {"unknown", "KDE Plasma", "Sway", "Hyprland", "wlroots-style", "COSMIC", "GNOME", "X11"};
    QString how;
    switch (profile.monitor) {
    case MonitorKind::KWin: how = u"KWin script"_s; break;
    case MonitorKind::Sway: how = u"sway IPC"_s; break;
    case MonitorKind::Hyprland: how = u"Hyprland IPC"_s; break;
    case MonitorKind::ForeignToplevel: how = u"wlr-foreign-toplevel-management"_s; break;
    case MonitorKind::GnomeShell: how = u"the GNOME Shell extension"_s; break;
    case MonitorKind::X11Ewmh: how = u"the window manager (EWMH)"_s; break;
    case MonitorKind::None: how = u"none (wallpapers never pause)"_s; break;
    }
    if (profile.appsFromToplevels)
        how += u" + app ids from wlr-foreign-toplevel-management"_s;
    profile.summary = u"%1: %2, layer %3, windows from %4"_s
        .arg(QLatin1String(names[int(profile.family)]),
             profile.layerShell ? u"wlr-layer-shell"_s : u"no layer-shell (wallpaper windows can not be placed)"_s,
             profile.layer == WallpaperLayer::Background ? u"background"_s : u"bottom"_s, how);
    return profile;
}

} // namespace murasaki
