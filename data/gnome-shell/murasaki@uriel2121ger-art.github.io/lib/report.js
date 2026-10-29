// Turns plain descriptions of the windows into the JSON murasaki-surface reads (the format of kwin-monitor.js, see
// parseKWinReport in src/surface/sessionstate.cpp). No GNOME API in here: extension.js describes the windows, this
// decides, and the node tests run it.
import {isTrustedTitle} from './classify.js';

// A window description:
//   {title, appId, pid, output: 'DP-1', workspace: 0 | -1 (on all workspaces), fullscreen, minimized, maximized,
//    isNormal, rect: {x, y, width, height}}
// A monitor description: {name, rect: {x, y, width, height}}

function covers(rect, area) {
    return rect.x <= area.x && rect.y <= area.y &&
        rect.x + rect.width >= area.x + area.width && rect.y + rect.height >= area.y + area.height;
}

export function buildReport({windows, monitors, workspaces, currentWorkspace, focus, ownPid}) {
    const areas = new Map(monitors.map(monitor => [monitor.name, monitor.rect]));
    const reported = [];
    for (const window of windows) {
        if (!window.isNormal || !window.output || window.pid === ownPid || isTrustedTitle(window.title, window.appId))
            continue;
        const area = areas.get(window.output);
        reported.push({
            output: window.output,
            desktops: window.workspace >= 0 ? [String(window.workspace)] : [],
            onAllDesktops: window.workspace < 0,
            fullScreen: !!window.fullscreen,
            minimized: !!window.minimized,
            // A maximized window covers the work area, which is smaller than the monitor: the shell's panel is
            // still above the wallpaper there, but nothing of the wallpaper shows. Treat it as covering.
            coversOutput: !!window.maximized || (area ? covers(window.rect, area) : false),
            resourceClass: String(window.appId || ''),
        });
    }
    const focusReported = focus && focus.isNormal && focus.pid !== ownPid && !isTrustedTitle(focus.title, focus.appId)
        ? {resourceClass: String(focus.appId || ''), fullScreen: !!focus.fullscreen}
        : null;
    return {
        currentDesktop: String(currentWorkspace),
        desktops: workspaces.map((name, index) => ({id: String(index), name})),
        activeWindow: focusReported,
        windows: reported,
    };
}

// The surface reads at most a bounded report; a pathological number of windows is cut off, the front ones first.
export const MAX_WINDOWS = 400;

export function serialize(report) {
    const bounded = report.windows.length > MAX_WINDOWS ? {...report, windows: report.windows.slice(0, MAX_WINDOWS)} : report;
    return JSON.stringify(bounded);
}
