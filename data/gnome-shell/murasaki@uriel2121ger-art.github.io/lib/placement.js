// Where Murasaki's windows belong. Pure: extension.js applies the result with Meta.Window calls.
import {isTrustedTitle, petInfo, wallpaperOutput} from './classify.js';

// Wallpaper windows -> the rectangle of their output. A window for an output that does not exist (any more) gets no
// placement; the extension leaves it alone and the surface closes it when the screen goes.
export function wallpaperPlacements(windows, monitors) {
    const byName = new Map(monitors.map(monitor => [monitor.name, monitor.rect]));
    const placements = [];
    for (const window of windows) {
        if (!isTrustedTitle(window.title, window.appId))
            continue;
        const output = wallpaperOutput(window.title);
        const rect = output ? byName.get(output) : undefined;
        if (rect)
            placements.push({id: window.id, output, rect});
    }
    return placements;
}

// Pet windows are output-sized and transparent (their input region is the pet): they sit at the origin of their output.
export function petPlacements(windows, monitors) {
    const byName = new Map(monitors.map(monitor => [monitor.name, monitor.rect]));
    const placements = [];
    for (const window of windows) {
        if (!isTrustedTitle(window.title, window.appId))
            continue;
        const info = petInfo(window.title);
        const rect = info ? byName.get(info.output) : undefined;
        if (rect)
            placements.push({id: window.id, output: info.output, petId: info.petId, below: info.below, stage: info.stage, rect});
    }
    return placements;
}

// What the extension does to a pet window, in order, as names of Meta.Window calls (extension.js maps each to the call, optional ones guarded):
//   a pet above the windows:  unmake_below, make_above
//   a pet below (keepAbove off): unmake_above, make_below, lower
//   a stage above: unmake_below, make_above, lower  -- a stage is a whole screen of transparency: it goes to the bottom of the layer above the windows, so that the pets
//                                                       (small windows in the same layer) stay in front of it
//   a stage below: as a pet below
// then stick (all workspaces) and the move to the output's rectangle. Mutter has no layer-shell: this is experimental and unverified on a real shell.
export function petActions(placement, applied = null) {
    // What a window was last arranged for: the same again changes nothing but, if it moved, its place. Every call below touches the stacking, which Mutter answers with
    // its own signals; re-arranging on each of them would be a loop.
    if (applied !== null && applied === petSignature(placement))
        return ['place'];
    const actions = placement.below ? ['unmake_above', 'make_below', 'lower'] : ['unmake_below', 'make_above'];
    if (placement.stage && !placement.below)
        actions.push('lower');
    actions.push('stick', 'place');
    return actions;
}

// What petActions arranges for: whether the window is below and whether it is a stage. The extension keeps it per window.
export function petSignature(placement) {
    return `${placement.below ? 'below' : 'above'}|${placement.stage ?? ''}`;
}

// Whether a window already is where it should be (to avoid moving it every time anything changes).
export function sameRect(a, b) {
    return a.x === b.x && a.y === b.y && a.width === b.width && a.height === b.height;
}
