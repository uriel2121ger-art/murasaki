// Which windows are Murasaki's. The titles are set by murasaki-surface and murasaki-overlay (OutputSurface and PetWindow
// in shell-window mode); keep the prefixes in step with src/surface/outputsurface.cpp and src/overlay/petwindow.cpp.

export const WALLPAPER_TITLE_PREFIX = 'Murasaki Wallpaper | ';
export const PET_TITLE_PREFIX = 'Murasaki Pet | ';
export const SURFACE_BUS_NAME = 'io.github.uriel2121ger_art.Murasaki.Surface';
export const SURFACE_PATH = '/io/github/uriel2121ger_art/Murasaki/Surface';
export const SURFACE_IFACE = 'io.github.uriel2121ger_art.Murasaki.Surface1';

// `title` may be null or empty while a window is still being created.
export function wallpaperOutput(title) {
    if (typeof title !== 'string' || !title.startsWith(WALLPAPER_TITLE_PREFIX))
        return null;
    const output = title.slice(WALLPAPER_TITLE_PREFIX.length).trim();
    return output === '' ? null : output;
}

// The marker a pet window's title ends with while the pet must not be above the windows (keepAbove off).
export const PET_BELOW_MARKER = 'below';

// The marker of a pet window that is a stage (the fullscreen mode, docs/pets/stage.md): 'stage output' or 'stage desktop'.
export const PET_STAGE_MARKER = 'stage';
export const STAGE_MODES = ['output', 'desktop'];

// 'Murasaki Pet | DP-1 | <petId>' -> {output: 'DP-1', petId: '<petId>', below: false, stage: null}; the markers after the pet id are separated by ' | ':
// 'below' (keepAbove off) and 'stage output' / 'stage desktop'. A pet window changes its title when either changes; the extension arranges again on the title change.
// Anything else after the id is ignored, and a stage word with a mode that is not one of ours is no stage.
export function petInfo(title) {
    if (typeof title !== 'string' || !title.startsWith(PET_TITLE_PREFIX))
        return null;
    const [output, petId, ...markers] = title.slice(PET_TITLE_PREFIX.length).split(' | ');
    if (!output)
        return null;
    let below = false;
    let stage = null;
    for (const marker of markers.map(m => m.trim())) {
        if (marker === PET_BELOW_MARKER)
            below = true;
        else if (marker.startsWith(PET_STAGE_MARKER + ' ') && STAGE_MODES.includes(marker.slice(PET_STAGE_MARKER.length + 1)))
            stage = marker.slice(PET_STAGE_MARKER.length + 1);
    }
    return {output: output.trim(), petId: (petId ?? '').trim(), below, stage};
}

export function isWallpaperWindow(title) {
    return wallpaperOutput(title) !== null;
}

export function isPetWindow(title) {
    return petInfo(title) !== null;
}

export function isMurasakiWindow(title) {
    return isWallpaperWindow(title) || isPetWindow(title);
}

// The application ids the windows come with (QGuiApplication::setDesktopFileName in murasaki-surface and murasaki-overlay; on Wayland the
// compositor reports it as the window's app id). A title alone is not enough: a web page can set its title to "Murasaki Pet | DP-1", and
// such a window must never be pinned, lowered, resized or hidden from the lists.
export const SURFACE_APP_ID = 'io.github.uriel2121ger_art.Murasaki';
export const OVERLAY_APP_ID = 'io.github.uriel2121ger_art.Murasaki.Overlay';

// A window is Murasaki's wallpaper only when the surface made it, and a pet only when the overlay did.
export function isTrustedTitle(title, appId) {
    if (isPetWindow(title))
        return appId === OVERLAY_APP_ID;
    if (isWallpaperWindow(title))
        return appId === SURFACE_APP_ID;
    return false;
}
