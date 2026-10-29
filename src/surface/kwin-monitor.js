// Loaded into KWin by murasaki-surface (src/surface/kwinmonitor.cpp). It only reports facts about windows (including
// each one's application class and which has the focus);
// parseKWinReport() in sessionstate.cpp decides what covers an output. kwinmonitor.cpp prepends the
// declaration of surfacePid (the murasaki-surface process id).
const service = "io.github.uriel2121ger_art.Murasaki.Surface";
const path = "/io/github/uriel2121ger_art/Murasaki/Surface";
const iface = "io.github.uriel2121ger_art.Murasaki.Surface1";

function coversOutput(window) {
    const area = workspace.clientArea(KWin.MaximizeArea, window.output, workspace.currentDesktop);
    const g = window.frameGeometry;
    return g.x <= area.x && g.y <= area.y
        && g.x + g.width >= area.x + area.width && g.y + g.height >= area.y + area.height;
}

// murasaki-overlay's windows (pets, the stage, the catcher of the wallpaper interaction mode) are layer-shell
// surfaces, but KWin lists them as normal windows: a catcher covers the whole output and would pause the very
// wallpaper the user is clicking on. They are drawn above the wallpaper on purpose and never hide it.
function isOverlay(window) {
    return String(window.resourceClass || "") === "murasaki-overlay";
}

function report(removed) {
    const windows = [];
    for (const window of workspace.windowList()) {
        if (window === removed || !window.normalWindow || !window.output || window.pid === surfacePid || isOverlay(window))
            continue;
        windows.push({
            output: window.output.name,
            desktops: window.desktops.map(desktop => desktop.id),
            onAllDesktops: window.onAllDesktops,
            fullScreen: window.fullScreen,
            minimized: window.minimized,
            coversOutput: coversOutput(window),
            resourceClass: String(window.resourceClass || ""),
        });
    }
    // The window that has the focus (null when none, or when it is Murasaki's own (the surface or the overlay) or not an ordinary window): the
    // exclusion list of the pause rules matches its application class.
    const active = workspace.activeWindow;
    const focus = active && active !== removed && active.normalWindow && active.pid !== surfacePid && !isOverlay(active)
        ? { resourceClass: String(active.resourceClass || ""), fullScreen: !!active.fullScreen }
        : null;
    callDBus(service, path, iface, "ReportKWinState", JSON.stringify({
        currentDesktop: workspace.currentDesktop.id,
        desktops: workspace.desktops.map(desktop => ({ id: desktop.id, name: desktop.name })),
        activeWindow: focus,
        windows: windows,
    }));
}

function watch(window) {
    for (const signal of [window.fullScreenChanged, window.minimizedChanged, window.maximizedChanged,
                          window.outputChanged, window.desktopsChanged, window.interactiveMoveResizeFinished]) {
        if (signal)
            signal.connect(() => report());
    }
    // Borderless-fullscreen games change geometry without any of the signals above. Dragging fires this per
    // frame, so it is ignored mid-move; the C++ side coalesces whatever bursts remain.
    if (window.frameGeometryChanged) {
        window.frameGeometryChanged.connect(() => {
            if (!window.move && !window.resize)
                report();
        });
    }
}

// Every connection is guarded: a signal missing in some KWin version would throw and skip the initial report().
workspace.windowList().forEach(watch);
if (workspace.windowAdded)
    workspace.windowAdded.connect(window => { watch(window); report(); });
if (workspace.windowRemoved)
    workspace.windowRemoved.connect(window => report(window));
if (workspace.currentDesktopChanged)
    workspace.currentDesktopChanged.connect(() => report());
if (workspace.desktopsChanged)
    workspace.desktopsChanged.connect(() => report());
if (workspace.screensChanged)
    workspace.screensChanged.connect(() => report());
if (workspace.windowActivated)
    workspace.windowActivated.connect(() => report());
report();
