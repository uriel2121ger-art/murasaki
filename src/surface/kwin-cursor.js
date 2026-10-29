// Loaded into KWin by murasaki-surface (src/surface/kwinmonitor.cpp) only while a Plasma host is registered.
// Plasma's desktop containment sits above the wallpaper and takes every pointer event, so the wallpaper never
// sees the mouse; KWin always knows where it is. This reports the global cursor position to the policy server
// (SurfaceService::ReportKWinCursor), at most once per `intervalMs`: the first movement goes out at once, the
// rest of a burst is coalesced into one trailing report, and an idle cursor sends nothing.
const service = "io.github.uriel2121ger_art.Murasaki.Surface";
const path = "/io/github/uriel2121ger_art/Murasaki/Surface";
const iface = "io.github.uriel2121ger_art.Murasaki.Surface1";
const intervalMs = 34; // just under 30 reports per second

let waiting = false; // the throttle window is open
let dirty = false;   // the cursor moved inside it

function send() {
    const pos = workspace.cursorPos;
    callDBus(service, path, iface, "ReportKWinCursor", Math.round(pos.x), Math.round(pos.y));
}

const gate = new QTimer();
gate.singleShot = true;
gate.interval = intervalMs;
gate.timeout.connect(() => {
    if (!dirty) {
        waiting = false;
        return;
    }
    dirty = false;
    send();
    gate.start();
});

function onMoved() {
    if (waiting) {
        dirty = true;
        return;
    }
    waiting = true;
    send();
    gate.start();
}

if (workspace.cursorPosChanged)
    workspace.cursorPosChanged.connect(onMoved);
send(); // where the cursor is now, for a host that registers while it rests
