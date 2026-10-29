#pragma once

#include <QProcessEnvironment>
#include <QString>

namespace murasaki {

// GNOME's compositor has no layer-shell. There Murasaki's surfaces are ordinary toplevel windows that the GNOME Shell
// extension (data/gnome-shell/) finds by title, pins as the desktop background (wallpapers) or above everything (pets), and
// keeps out of the overview. The title prefixes are shared with the extension (lib/classify.js).
inline constexpr char kShellWallpaperTitlePrefix[] = "Murasaki Wallpaper | ";
inline constexpr char kShellPetTitlePrefix[] = "Murasaki Pet | ";

inline QString shellWallpaperTitle(const QString &output)
{
    return QString::fromLatin1(kShellWallpaperTitlePrefix) + output;
}

// "Murasaki Pet | <output> | <petId>", with " | below" appended while the pet must not be above the windows (keepAbove off): the extension reads
// the marker (lib/classify.js, PET_BELOW_MARKER) and arranges the window again when the title changes.
inline constexpr char kShellPetBelowMarker[] = "below";

// The application ids the shell extension trusts for those titles (lib/classify.js, SURFACE_APP_ID and OVERLAY_APP_ID): a title alone proves
// nothing, a web page can set one. Each process sets its id with QGuiApplication::setDesktopFileName.
inline constexpr char kShellSurfaceAppId[] = "io.github.uriel2121ger_art.Murasaki";
inline constexpr char kShellOverlayAppId[] = "io.github.uriel2121ger_art.Murasaki.Overlay";

// A pet in the fullscreen mode (docs/pets/stage.md) adds " | stage output" or " | stage desktop" (lib/classify.js, PET_STAGE_MARKER): the extension then keeps
// its window at the bottom of the layer above the windows, so that the pets stay in front of it.
inline constexpr char kShellPetStageMarker[] = "stage";

// stage: "" for a pet, "output" or "desktop" for a stage.
inline QString shellPetTitle(const QString &output, const QString &petId, bool below = false, const QString &stage = QString())
{
    QString title = QString::fromLatin1(kShellPetTitlePrefix) + output + QStringLiteral(" | ") + petId;
    if (below)
        title += QStringLiteral(" | ") + QLatin1String(kShellPetBelowMarker);
    if (stage == QLatin1String("output") || stage == QLatin1String("desktop"))
        title += QStringLiteral(" | ") + QLatin1String(kShellPetStageMarker) + QLatin1Char(' ') + stage;
    return title;
}

// Whether to use those windows instead of layer-shell surfaces: a GNOME session, unless MURASAKI_SHELL_WINDOWS says 0 (or 1
// to force it elsewhere, for testing).
// Whether the title of a pet window must change: it says "below" exactly while keepAbove is off, so it changes when the two agree
// (title says above/below = titleBelow false/true, keepAbove true/false). PetWindow::apply uses it; the shell extension arranges the window again.
inline constexpr bool shellPetTitleChanges(bool titleBelow, bool keepAbove)
{
    return titleBelow == keepAbove;
}

inline bool useShellWindows(const QProcessEnvironment &env = QProcessEnvironment::systemEnvironment())
{
    const QString force = env.value(QStringLiteral("MURASAKI_SHELL_WINDOWS"));
    if (force == QLatin1String("1"))
        return true;
    if (force == QLatin1String("0"))
        return false;
    return env.value(QStringLiteral("XDG_CURRENT_DESKTOP"))
        .split(QLatin1Char(':'), Qt::SkipEmptyParts)
        .contains(QStringLiteral("GNOME"), Qt::CaseInsensitive);
}

} // namespace murasaki
