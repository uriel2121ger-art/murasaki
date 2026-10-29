// Loaded into KWin by murasaki-overlay (KWinPointerSource) only while some pet needs the global cursor: hide under cursor, focus fade or
// smart dodge on a Plasma Wayland session. KWin always knows where the cursor is; a client of the compositor does not (docs/pets/pointer-facts.md,
// F3/F5). This reports workspace.cursorPos (global logical pixels) to the overlay's own D-Bus method ReportPointer, at most once per
// `intervalMs`: the first movement goes out at once, the rest of a burst is coalesced into one trailing report, and an idle cursor sends
// nothing. The overlay accepts the call only from org.kde.KWin.
// The script must not outlive the overlay: every `ownerCheckMs` it asks the bus whether the overlay's name still has an owner and, after two
// answers of "no" in a row, stops reporting and drops its cursor connection (the overlay unloads it on start and on exit as well; this covers
// a SIGKILL, where nobody is left to do it).
const service = "io.github.uriel2121ger_art.Murasaki.Overlay";
const path = "/io/github/uriel2121ger_art/Murasaki/Overlay/Pointer";
const iface = "io.github.uriel2121ger_art.Murasaki.Overlay.Pointer1";
const intervalMs = 34; // just under 30 reports per second
const ownerCheckMs = 10000;

let waiting = false; // the throttle window is open
let dirty = false;   // the cursor moved inside it
let stopped = false; // the overlay is gone: nothing is reported any more
let missing = 0;     // consecutive checks that found no owner of the overlay's name
const watchdog = new QTimer();

function send() {
    const pos = workspace.cursorPos;
    callDBus(service, path, iface, "ReportPointer", Math.round(pos.x), Math.round(pos.y));
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
    if (stopped)
        return;
    if (waiting) {
        dirty = true;
        return;
    }
    waiting = true;
    send();
    gate.start();
}

function stop() {
    stopped = true;
    if (workspace.cursorPosChanged && workspace.cursorPosChanged.disconnect)
        workspace.cursorPosChanged.disconnect(onMoved);
    watchdog.stop();
}

watchdog.singleShot = false;
watchdog.interval = ownerCheckMs;
watchdog.timeout.connect(() => {
    if (stopped)
        return;
    callDBus("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner", service, (hasOwner) => {
        missing = hasOwner ? 0 : missing + 1;
        if (missing >= 2)
            stop();
    });
});

if (workspace.cursorPosChanged)
    workspace.cursorPosChanged.connect(onMoved);
watchdog.start();
send(); // where the cursor is now, for a pet that starts while it rests
