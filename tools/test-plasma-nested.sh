#!/usr/bin/env bash
# Integration test of the Plasma wallpaper package in a fully isolated nested session.
#
# What it starts, all private: a D-Bus session (dbus-run-session), kwin_wayland --virtual, a real plasmashell, a
# murasakid and the policy murasaki-surface from a throw-away install of the build (cmake --install into a temp
# prefix; the package goes into the nested XDG_DATA_HOME), and a private PipeWire with one null sink.
#
# Then, for a video, a web item and a scene: assign it, `murasakictl plasma enable` (first time), wait, take a
# screenshot with spectacle. The wallpaper has to show the item with the panel on top. Finally
# `murasakictl plasma disable` must give back exactly the wallpaper plugins the desktops had before.
#
# Cursor phase (MURASAKI_TEST_CURSOR=0 skips it): the desktop above the wallpaper takes the pointer, so the host gets
# the mouse from KWin through the policy server (HostCursor). The nested KWin is virtual and has no way to move its
# pointer (the fake-input protocol is not available to scripts and workspace.cursorPos is read-only), so:
#   - the real kwin-cursor.js is loaded by the policy server when the host registers and reports the pointer where
#     it rests (the middle of the output): the host must log that position;
#   - movement is driven by tiny KWin scripts in the nested KWin that call Surface1.ReportKWinCursor exactly like
#     kwin-cursor.js does (the sender is KWin, so the policy server accepts it). That covers the policy server's
#     mapping, its throttle and the host's hover; the script's own throttle is covered by tst_kwincursorscript.
#   - a burst of reports must reach the host at no more than ~30 per second;
#   - with MURASAKI_TEST_PARALLAX (default we:2182253741, the mouse-parallax scene "Nino (X-Ray)") in the library, the
#     frames of that scene must change when the pointer goes from one corner to the other.
#
# The user's session is never touched: the runtime dir, the XDG dirs and the bus are private, nothing is installed
# outside the temp prefix, and every process carries MURASAKI_NESTED_MARK, which is also how the leftovers are killed
# at the end. The Wallpaper Engine library is only read.
#
#   tools/test-plasma-nested.sh [--build-dir DIR] [--out-dir DIR] [--kinds video,web,scene] [--output NAME]
#
# Item ids come from the private daemon's `murasakictl list` (the first ok item of each kind), or from
# MURASAKI_TEST_VIDEO / MURASAKI_TEST_WEB / MURASAKI_TEST_SCENE. Scenes need MURASAKI_SCENE_ENGINE (default
# ~/.local/bin/murasaki-scene). A kind with no item is skipped; no kind tested at all is a failure.
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when the environment cannot run it.
set -u

SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")
# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"
kill_marked() { nested_kill_marked "$@"; }

if [[ ${1:-} == --inner ]]; then
    shift
    INNER=1
else
    INNER=0
fi

BUILD_DIR=$REPO/build/dev
OUT_DIR=${TMPDIR:-/tmp}/murasaki-plasma-nested-out
KINDS=video,web,scene
OUTPUT=Virtual-0   # what kwin_wayland --virtual names its one output
while (($#)); do
    case $1 in
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --out-dir) OUT_DIR=$2; shift 2 ;;
    --kinds) KINDS=$2; shift 2 ;;
    --output) OUTPUT=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
BUILD_DIR=$(readlink -f "$BUILD_DIR")
mkdir -p "$OUT_DIR"
OUT_DIR=$(readlink -f "$OUT_DIR")

if ((!INNER)); then
    # ---- outer half: private runtime dir, install prefix and bus ------------------------------------------
    if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the nested Plasma test"
        exit 77
    fi
    for tool in dbus-run-session kwin_wayland plasmashell spectacle python3; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    for bin in src/daemon/murasakid src/ctl/murasakictl src/surface/murasaki-surface; do
        [[ -x $BUILD_DIR/$bin ]] || { echo "SKIP: $BUILD_DIR/$bin is not built"; exit 77; }
    done
    [[ -d $BUILD_DIR/data ]] || { echo "SKIP: $BUILD_DIR is not a build directory"; exit 77; }
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    TMP_ROOT=$(mktemp -d /tmp/mpn.XXXXXX)   # short: it holds the Wayland socket
    mkdir -m 700 "$TMP_ROOT/rt"
    export MURASAKI_NESTED_TMP=$TMP_ROOT
    mkdir -p "$OUT_DIR/logs"
    outer_cleanup() {
        trap - EXIT INT TERM
        kill_marked TERM
        sleep 1
        kill_marked KILL
        rm -rf "$TMP_ROOT"
    }
    trap 'outer_cleanup; exit 130' INT TERM
    trap outer_cleanup EXIT
    # The install is what a user gets: the binaries, the package (main.qml and the config page learn the prefix
    # here) and the module inside it. Nothing outside the temp prefix is written.
    timeout 120 cmake --install "$BUILD_DIR" --prefix "$TMP_ROOT/prefix" >"$OUT_DIR/logs/install.log" 2>&1 \
        || { echo "FAIL: cmake --install failed (see $OUT_DIR/logs/install.log)"; exit 1; }
    [[ -f $TMP_ROOT/prefix/share/plasma/wallpapers/io.github.uriel2121ger_art.Murasaki/metadata.json ]] \
        || { echo "SKIP: this build has no Plasma package (murasaki-surface not built)"; exit 77; }
    unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS KDE_SESSION_UID SWAYSOCK HYPRLAND_INSTANCE_SIGNATURE
    unset XDG_SESSION_ID XDG_SEAT XDG_VTNR
    export XDG_RUNTIME_DIR=$TMP_ROOT/rt XDG_CONFIG_HOME=$TMP_ROOT/config XDG_DATA_HOME=$TMP_ROOT/data
    export XDG_CACHE_HOME=$TMP_ROOT/cache XDG_STATE_HOME=$TMP_ROOT/state
    export XDG_DATA_DIRS=$TMP_ROOT/prefix/share:/usr/share XDG_CONFIG_DIRS=/etc/xdg
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
    export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland KDE_FULL_SESSION=true
    # An isolated bus: it can start only the services listed here (the activation file of the install in the temp
    # prefix, and the KDE session services plasmashell asks for), never the whole /usr/share/dbus-1/services.
    activatable=("$TMP_ROOT"/prefix/share/dbus-1/services/*.service)
    for name in org.kde.kded6 org.kde.kglobalaccel org.kde.plasma.Notifications org.kde.StatusNotifierWatcher \
        org.freedesktop.Notifications org.kde.KSplash org.kde.klauncher6 org.kde.ksmserver; do
        [[ -f /usr/share/dbus-1/services/$name.service ]] && activatable+=("/usr/share/dbus-1/services/$name.service")
    done
    nested_write_bus_conf "$TMP_ROOT/bus" "${activatable[@]}"
    timeout "${MURASAKI_NESTED_TIMEOUT:-900}" dbus-run-session --config-file="$TMP_ROOT/bus/bus.conf" -- "$SELF" --inner --build-dir "$BUILD_DIR" \
        --out-dir "$OUT_DIR" --kinds "$KINDS" --output "$OUTPUT" </dev/null 2>"$OUT_DIR/logs/session.log"
    exit $?
fi

# ---- inner half: runs inside the private bus --------------------------------------------------------------
LOG_DIR=$OUT_DIR/logs
mkdir -p "$LOG_DIR"
trap 'kill_marked TERM; sleep 1; kill_marked KILL' EXIT
echo "private bus: $DBUS_SESSION_BUS_ADDRESS"
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
case $XDG_RUNTIME_DIR in
/tmp/mpn.*/rt) ;;
*) echo "FAIL: XDG_RUNTIME_DIR is not the private one, refusing to continue"; exit 1 ;;
esac

PREFIX=$MURASAKI_NESTED_TMP/prefix
CTL=$PREFIX/bin/murasakictl
PLUGIN=io.github.uriel2121ger_art.Murasaki
export QT_QPA_PLATFORM=wayland QT_ACCESSIBILITY=0 MURASAKI_BRIDGE_LOG=1
export MURASAKI_SCENE_ENGINE=${MURASAKI_SCENE_ENGINE:-$HOME/.local/bin/murasaki-scene}
SOCKET=wayland-$(basename "$MURASAKI_NESTED_TMP")

KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 kwin_wayland --virtual --socket "$SOCKET" --width 1280 --height 720 \
    --no-lockscreen >"$LOG_DIR/kwin.log" 2>&1 &
for _ in $(seq 100); do
    [[ -S $XDG_RUNTIME_DIR/$SOCKET ]] && break
    sleep 0.1
done
[[ -S $XDG_RUNTIME_DIR/$SOCKET ]] || { echo "FAIL: kwin_wayland did not come up (see $LOG_DIR/kwin.log)"; exit 1; }
export WAYLAND_DISPLAY=$SOCKET

nested_start_audio

# The package as a user's ~/.local/share would hold it. (XDG_DATA_DIRS also reaches the prefix; this is the
# location Plasma searches first.)
mkdir -p "$XDG_DATA_HOME/plasma"
cp -r "$PREFIX/share/plasma/wallpapers" "$XDG_DATA_HOME/plasma/"

"$PREFIX/bin/murasakid" >"$LOG_DIR/murasakid.log" 2>&1 &
for _ in $(seq 100); do
    timeout 5 "$CTL" list >"$LOG_DIR/list.txt" 2>/dev/null && break
    sleep 0.2
done
[[ -s $LOG_DIR/list.txt ]] || { echo "SKIP: the private murasakid sees no library items (see $LOG_DIR/murasakid.log)"; exit 77; }

# plasmashell refuses to load without the activity manager, and D-Bus activation of it goes through systemd --user,
# which this session does not have. It is started by hand, in the private session.
ACTIVITYD=$(sed -n 's/^Exec=//p' /usr/share/dbus-1/services/org.kde.ActivityManager.service 2>/dev/null | head -1)
[[ -x $ACTIVITYD ]] || ACTIVITYD=$(command -v kactivitymanagerd)
[[ -x $ACTIVITYD ]] || { echo "SKIP: kactivitymanagerd not found"; exit 77; }
"$ACTIVITYD" >"$LOG_DIR/kactivitymanagerd.log" 2>&1 &
for _ in $(seq 50); do
    timeout 5 busctl --user status org.kde.ActivityManager >/dev/null 2>&1 && break
    sleep 0.2
done
status_lines() { timeout 20 "$CTL" plasma status 2>/dev/null | grep '^desktop '; }
# plasmashell dies when it starts before kwin has its (virtual) output ready, so it gets a few tries.
sleep 3
for attempt in 1 2 3; do
    plasmashell >"$LOG_DIR/plasmashell.log" 2>&1 &
    PLASMA_PID=$!
    for _ in $(seq 150); do
        [[ -n $(status_lines) ]] && break
        kill -0 "$PLASMA_PID" 2>/dev/null || break
        sleep 0.5
    done
    [[ -n $(status_lines) ]] && kill -0 "$PLASMA_PID" 2>/dev/null && break
    echo "plasmashell did not come up (attempt $attempt, see $LOG_DIR/plasmashell.log)"
    kill "$PLASMA_PID" 2>/dev/null
    sleep 2
    cp "$LOG_DIR/plasmashell.log" "$LOG_DIR/plasmashell-attempt$attempt.log"
done
[[ -n $(status_lines) ]] || { echo "FAIL: plasmashell has no desktop (see $LOG_DIR/plasmashell.log)"; exit 1; }
echo "plasmashell up"
sleep 3 # let the panel and the desktop finish painting

# The policy server, in its default mode: it shows layer-shell windows until the Plasma presenter takes over.
"$PREFIX/bin/murasaki-surface" >"$LOG_DIR/surface.log" 2>&1 &
POLICY_PID=$!
SURF=io.github.uriel2121ger_art.Murasaki.Surface
SURF_PATH=/io/github/uriel2121ger_art/Murasaki/Surface
SURF_IFACE=io.github.uriel2121ger_art.Murasaki.Surface1
surface_outputs() {
    timeout 5 busctl --user --json=short call "$SURF" "$SURF_PATH" "$SURF_IFACE" Outputs 2>/dev/null | sed -e 's/\\"/"/g'
}
for _ in $(seq 100); do
    surface_outputs | grep -qF "$OUTPUT" && break
    sleep 0.2
done
surface_outputs | grep -qF "$OUTPUT" || { echo "FAIL: the policy surface does not list $OUTPUT (see $LOG_DIR/surface.log)"; exit 1; }

shot() { # name; spectacle does not overwrite a file that exists
    rm -f "$OUT_DIR/$1.png"
    timeout 60 spectacle -b -n -f -o "$OUT_DIR/$1.png" >>"$LOG_DIR/spectacle.log" 2>&1
    [[ -s $OUT_DIR/$1.png ]] || { echo "FAIL: spectacle took no screenshot ($1)"; return 1; }
}

# stats FILE [OTHER]: prints "key=value" lines about the wallpaper area (everything above the panel) of a screenshot.
stats() {
    python3 - "$@" <<'PY'
import sys
from PIL import Image, ImageChops, ImageStat
img = Image.open(sys.argv[1]).convert("RGB")
w, h = img.size
area = img.crop((0, 0, w, int(h * 0.88)))
st = ImageStat.Stat(area)
mean = st.mean
# Share of pixels that are the probe's magenta (r and b high, g low).
small = area.resize((160, 90))
px = list(small.getdata())
magenta = sum(1 for r, g, b in px if r > 180 and b > 180 and g < 90) / len(px)
# The panel is the bottom strip; it is lit even when the wallpaper is black.
panel = ImageStat.Stat(img.crop((0, int(h * 0.93), w, h))).mean
print("panel=%.1f" % (sum(panel) / 3))
print("mean=%.1f" % (sum(mean) / 3))
print("stddev=%.1f" % (sum(st.stddev) / 3))
print("magenta=%.2f" % magenta)
if len(sys.argv) > 2:
    other = Image.open(sys.argv[2]).convert("RGB").crop((0, 0, w, int(h * 0.88)))
    diff = ImageStat.Stat(ImageChops.difference(area, other)).mean
    print("diff=%.1f" % (sum(diff) / 3))
PY
}
field() { sed -n "s/^$1=//p"; }
fnum_gt() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'; }

failed=0
fail() { echo "   FAIL: $*"; failed=1; }

# The first screenshot of a fresh session is black: take them until the panel shows. (The default wallpaper of a
# nested session with no theme images is black, so the panel is the sign of life.)
for _ in $(seq 30); do
    shot before || exit 1
    fnum_gt "$(stats "$OUT_DIR/before.png" | field panel)" 40 && break
    sleep 1
done
fnum_gt "$(stats "$OUT_DIR/before.png" | field panel)" 40 || { echo "FAIL: the nested Plasma desktop never painted (see before.png, plasmashell.log)"; exit 1; }
status_lines >"$LOG_DIR/status-before.txt"
echo "before: $(tr '\n' ';' <"$LOG_DIR/status-before.txt")"
grep -q "$PLUGIN" "$LOG_DIR/status-before.txt" && { echo "FAIL: the desktop already shows the Murasaki plugin"; exit 1; }

# The dry run changes nothing.
timeout 30 "$CTL" plasma enable --dry-run >"$LOG_DIR/dry-run.txt" 2>&1 || fail "enable --dry-run failed (see $LOG_DIR/dry-run.txt)"
[[ $(status_lines) == "$(cat "$LOG_DIR/status-before.txt")" ]] || fail "enable --dry-run changed the desktop"
[[ -e $XDG_STATE_HOME/murasaki/plasma-previous.json ]] && fail "enable --dry-run wrote the state file"

declare -A OVERRIDE=([video]=${MURASAKI_TEST_VIDEO:-} [web]=${MURASAKI_TEST_WEB:-} [scene]=${MURASAKI_TEST_SCENE:-})
candidates() { # kind -> up to 4 ok item ids (an override pins the item)
    if [[ -n ${OVERRIDE[$1]:-} ]]; then echo "${OVERRIDE[$1]}"; return; fi
    awk -F'\t' -v kind="$1" '$2 == kind && $3 == "ok" && n < 4 { print $1; n++ }' "$LOG_DIR/list.txt"
}

enabled=0
# One item on the wallpaper: 0 when it shows (not black, not flat, changed from the default, panel on top,
# animated unless it is a still item, hosted at the policy server), 1 otherwise. Messages go to stdout.
try_item() {
    local kind=$1 id=$2 settle problems=0
    timeout 10 "$CTL" set "$id" >/dev/null || { echo "   cannot assign $id"; return 1; }
    if ((!enabled)); then
        timeout 60 "$CTL" plasma enable >"$LOG_DIR/enable.txt" 2>&1 || { cat "$LOG_DIR/enable.txt"; fail "plasma enable failed"; return 2; }
        enabled=1
        status_lines >"$LOG_DIR/status-enabled.txt"
        [[ $(grep -c "$PLUGIN" "$LOG_DIR/status-enabled.txt") == "$(wc -l <"$LOG_DIR/status-enabled.txt")" ]] \
            || fail "not every desktop shows the Murasaki plugin: $(tr '\n' ';' <"$LOG_DIR/status-enabled.txt")"
        [[ -s $XDG_STATE_HOME/murasaki/plasma-previous.json ]] || fail "no state file after enable"
        [[ -s $XDG_STATE_HOME/murasaki/plasma-org.kde.plasma.desktop-appletsrc.bak ]] \
            || echo "   (no config backup: the nested session had no appletsrc yet)"
    fi
    case $kind in scene) settle=${MURASAKI_TEST_SCENE_SETTLE:-12} ;; *) settle=${MURASAKI_TEST_SETTLE:-8} ;; esac
    sleep "$settle"
    surface_outputs >"$LOG_DIR/outputs-$kind.txt"
    shot "$kind-a" && sleep 2 && shot "$kind-b" || return 1
    stats "$OUT_DIR/$kind-a.png" "$OUT_DIR/before.png" >"$LOG_DIR/stats-$kind.txt"
    stats "$OUT_DIR/$kind-b.png" "$OUT_DIR/$kind-a.png" >"$LOG_DIR/stats-$kind-anim.txt"
    echo "   $(tr '\n' ' ' <"$LOG_DIR/stats-$kind.txt") animation-$(field diff <"$LOG_DIR/stats-$kind-anim.txt")"
    bad() { echo "   problem: $*"; problems=1; }
    fnum_gt "$(field panel <"$LOG_DIR/stats-$kind.txt")" 40 || bad "the panel is missing (the wallpaper covers it?)"
    fnum_gt "$(field mean <"$LOG_DIR/stats-$kind.txt")" 8 || bad "the wallpaper is black"
    fnum_gt "$(field stddev <"$LOG_DIR/stats-$kind.txt")" 5 || bad "the wallpaper is flat"
    fnum_gt "0.3" "$(field magenta <"$LOG_DIR/stats-$kind.txt")" || bad "the wallpaper is magenta (the probe?)"
    fnum_gt "$(field diff <"$LOG_DIR/stats-$kind.txt")" 3 || bad "the wallpaper did not change from the default one"
    fnum_gt "$(field diff <"$LOG_DIR/stats-$kind-anim.txt")" 0.2 || bad "the wallpaper does not move (paused, stuck or a still item)"
    grep -qF '"hosted":true' "$LOG_DIR/outputs-$kind.txt" || bad "Outputs() does not show $OUTPUT as hosted"
    grep -qF '"paused":true' "$LOG_DIR/outputs-$kind.txt" && bad "the policy has the hosted output paused"
    ((problems)) && return 1
    cp "$OUT_DIR/$kind-a.png" "$OUT_DIR/$kind.png"
    return 0
}

# ---- cursor phase ---------------------------------------------------------------------------------------------
# KWin scripts of the nested session, loaded and unloaded over its Scripting interface.
kwin_script() { # name file
    timeout 10 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$2" "$1" >/dev/null 2>&1 \
        && timeout 10 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting start >/dev/null 2>&1
}
kwin_unload() { timeout 10 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s "$1" >/dev/null 2>&1; }
# Reports the pointer at logical X,Y the way kwin-cursor.js does.
cursor_report() {
    local file=$LOG_DIR/cursor-report-$1-$2.js
    cat >"$file" <<JS
callDBus("$SURF", "$SURF_PATH", "$SURF_IFACE", "ReportKWinCursor", $1, $2);
JS
    kwin_script "murasaki-test-cursor" "$file"
    sleep 0.4
    kwin_unload "murasaki-test-cursor"
}
# host_cursors FROM_LINE: "x y w h" of every "cursor" line the host logged after line FROM_LINE of plasmashell.log.
host_cursors() {
    tail -n "+$(($1 + 1))" "$LOG_DIR/plasmashell.log" | sed -n 's/.*murasaki\.host: cursor \([0-9]*\),\([0-9]*\) of \([0-9]*\)x\([0-9]*\).*/\1 \2 \3 \4/p'
}
# expect_cursor FROM_LINE FX FY: the host logged a hover at fraction FX,FY (+-0.02) of its render size.
expect_cursor() {
    host_cursors "$1" | awk -v fx="$2" -v fy="$3" 'function abs(v) { return v < 0 ? -v : v }
        { if (abs($1 / $3 - fx) < 0.02 && abs($2 / $4 - fy) < 0.02) found = 1 } END { exit !found }'
}

cursor_phase() {
    local mark n
    echo "== cursor: the host follows the compositor's pointer"
    # The real script, loaded when the host registered, reported the resting pointer (the middle of the output).
    host_cursors 0 >"$LOG_DIR/cursor-initial.txt"
    if [[ -s $LOG_DIR/cursor-initial.txt ]]; then
        expect_cursor 0 0.5 0.5 && echo "   the real kwin-cursor.js reported the resting pointer: $(head -1 "$LOG_DIR/cursor-initial.txt")" \
            || fail "the host's first cursor is not the middle of the output: $(head -1 "$LOG_DIR/cursor-initial.txt")"
    else
        fail "the host logged no cursor position from kwin-cursor.js (is the script loaded? see surface.log, kwin.log)"
    fi
    timeout 10 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting isScriptLoaded s murasaki-cursor 2>/dev/null \
        | grep -q true || fail "the policy server did not load kwin-cursor.js into KWin"

    # Corners and the middle, in logical pixels of the 1280x720 nested output.
    mark=$(wc -l <"$LOG_DIR/plasmashell.log")
    cursor_report 128 72
    cursor_report 1152 648
    cursor_report 640 360
    for target in "0.1 0.1" "0.9 0.9" "0.5 0.5"; do
        # shellcheck disable=SC2086
        expect_cursor "$mark" $target || fail "the host got no hover at fraction $target (see plasmashell.log)"
    done
    # The pointer leaving the output keeps the last position: the host logs the departure and sends nothing more.
    mark=$(wc -l <"$LOG_DIR/plasmashell.log")
    cursor_report 5000 5000
    sleep 0.5
    grep -q "cursor left $OUTPUT" <(tail -n "+$((mark + 1))" "$LOG_DIR/plasmashell.log") || fail "the host was not told the pointer left $OUTPUT"
    n=$(host_cursors "$mark" | wc -l)
    ((n == 0)) || fail "the host got $n positions for a pointer outside the output"

    # Rate: 250 reports in about a second reach the host at no more than ~30 per second.
    cat >"$LOG_DIR/cursor-burst.js" <<JS
let i = 0;
const t = new QTimer();
t.interval = 4;
t.timeout.connect(() => {
    callDBus("$SURF", "$SURF_PATH", "$SURF_IFACE", "ReportKWinCursor", 100 + (i % 1000), 100 + (i % 500));
    if (++i >= 250) t.stop();
});
t.start();
JS
    sleep 1
    mark=$(wc -l <"$LOG_DIR/plasmashell.log")
    local t0=$SECONDS
    kwin_script "murasaki-test-burst" "$LOG_DIR/cursor-burst.js"
    sleep 3
    kwin_unload "murasaki-test-burst"
    n=$(host_cursors "$mark" | wc -l)
    echo "   burst: $n hover updates reached the host for 250 reports ($((SECONDS - t0)) s window)"
    ((n >= 5)) || fail "the burst produced only $n hover updates"
    ((n <= 100)) || fail "the burst produced $n hover updates; the throttle should keep it near 30 per second"
}

# The frames of a mouse-parallax scene have to follow the pointer.
parallax_phase() {
    local id=${MURASAKI_TEST_PARALLAX:-we:2182253741}
    [[ -x $MURASAKI_SCENE_ENGINE ]] || { echo "== parallax: no scene engine, skipped"; return; }
    awk -F'\t' -v id="$id" '$1 == id && $3 == "ok" { found = 1 } END { exit !found }' "$LOG_DIR/list.txt" \
        || { echo "== parallax: $id is not in the library, skipped"; return; }
    echo "== parallax: $id follows the pointer"
    timeout 10 "$CTL" set "$id" >/dev/null || { fail "cannot assign $id"; return; }
    cursor_report 128 72
    sleep "${MURASAKI_TEST_SCENE_SETTLE:-12}"
    cursor_report 128 72
    sleep 2
    shot parallax-a1 && sleep 2 && shot parallax-a2 || return
    # A machine that is busy (the whole test suite runs beside this) can make the scene's own animation as big as the pointer's effect for a moment: the pointer is
    # moved across the screen and back up to three times, and the scene has to follow it clearly at least once.
    local still moved attempt followed=0
    for attempt in 1 2 3; do
        cursor_report 1152 648
        sleep 3
        shot parallax-b1 || return
        stats "$OUT_DIR/parallax-a2.png" "$OUT_DIR/parallax-a1.png" >"$LOG_DIR/stats-parallax-still.txt"
        stats "$OUT_DIR/parallax-b1.png" "$OUT_DIR/parallax-a2.png" >"$LOG_DIR/stats-parallax-moved.txt"
        still=$(field diff <"$LOG_DIR/stats-parallax-still.txt")
        moved=$(field diff <"$LOG_DIR/stats-parallax-moved.txt")
        echo "   attempt $attempt: diff with the pointer at rest: $still; after moving it across the screen: $moved"
        # Moving the pointer has to change the picture clearly more than the scene's own animation does.
        if awk -v a="$moved" -v b="$still" 'BEGIN { exit !(a > 1.0 && a > 2 * b) }'; then
            followed=1
            break
        fi
        cursor_report 128 72
        sleep 3
        shot parallax-a1 && sleep 2 && shot parallax-a2 || return
    done
    ((followed)) \
        || fail "the parallax scene did not follow the pointer (see parallax-*.png)"
}

tested=0
for kind in ${KINDS//,/ }; do
    mapfile -t ids < <(candidates "$kind")
    if ((${#ids[@]} == 0)); then
        echo "== $kind: no item in the library, skipped"
        continue
    fi
    if [[ $kind == scene && ! -x $MURASAKI_SCENE_ENGINE ]]; then
        echo "== $kind: no scene engine at $MURASAKI_SCENE_ENGINE, skipped"
        continue
    fi
    # An item can be legitimately static or unsupported by the scene engine: the kind passes when one of its first
    # few items does.
    tested=$((tested + 1))
    passed=0
    for id in "${ids[@]}"; do
        echo "== $kind: $id"
        try_item "$kind" "$id"
        rc=$?
        ((rc == 2)) && break 2
        if ((rc == 0)); then passed=1; echo "   $kind PASSED with $id"; break; fi
        for f in "$OUT_DIR/$kind"-?.png; do [[ -e $f ]] && mv "$f" "${f%.png}.rejected-${id//[:\/]/_}.png.bak"; done
    done
    ((passed)) || fail "$kind: no item passed"
done

if ((enabled)) && [[ ${MURASAKI_TEST_CURSOR:-1} != 0 ]]; then
    cursor_phase
    parallax_phase
fi

if ((enabled)); then
    # The plugin found the connector name of its screen, and the host runs from the install.
    grep -q "murasaki wallpaper: output $OUTPUT" "$LOG_DIR/plasmashell.log" || fail "the plugin did not report output $OUTPUT (see plasmashell.log)"
    grep -q "registered $OUTPUT with the policy server" "$LOG_DIR/plasmashell.log" || fail "the host did not register $OUTPUT"
    grep -q "^murasaki-surface: window for" "$LOG_DIR/surface.log" && echo "(the policy surface had drawn a layer-shell window before the switch: expected)"
    surface_mark=$(wc -l <"$LOG_DIR/surface.log")

    timeout 60 "$CTL" plasma disable >"$LOG_DIR/disable.txt" 2>&1 || { cat "$LOG_DIR/disable.txt"; fail "plasma disable failed"; }
    sleep 4
    status_lines >"$LOG_DIR/status-after.txt"
    if cmp -s "$LOG_DIR/status-before.txt" "$LOG_DIR/status-after.txt"; then
        echo "disable restored: $(tr '\n' ';' <"$LOG_DIR/status-after.txt")"
    else
        fail "disable did not restore the previous wallpaper plugins (see status-before.txt, status-after.txt)"
    fi
    [[ -e $XDG_STATE_HOME/murasaki/plasma-previous.json ]] && fail "the state file survived disable"
    # Presenter auto again: the policy surface draws layer-shell windows once more (this nested desktop has no
    # wallpaper image, so its desktop window lets them show through; a real one covers them).
    tail -n "+$((surface_mark + 1))" "$LOG_DIR/surface.log" | grep -q "^murasaki-surface: window for" \
        || fail "the policy surface did not create its layer-shell windows again after disable"
    shot after-layer-shell
    timeout 10 "$CTL" unset >/dev/null
    sleep 3
    shot after
    stats "$OUT_DIR/after.png" "$OUT_DIR/before.png" >"$LOG_DIR/stats-after.txt"
    echo "   after vs before: $(tr '\n' ' ' <"$LOG_DIR/stats-after.txt")"
    fnum_gt 6 "$(field diff <"$LOG_DIR/stats-after.txt")" || fail "after disable the desktop does not look like before"
    # The host ends with its wallpaper item.
    for _ in $(seq 20); do
        pgrep -f -- "$PREFIX/bin/murasaki-surface --host" >/dev/null || break
        sleep 0.5
    done
    pgrep -f -- "$PREFIX/bin/murasaki-surface --host" >/dev/null && fail "a host process is still running after disable"
    # Back to the layer-shell presenter: presenter=auto and nothing is hosted.
    surface_outputs | grep -qF '"hosted":true' && fail "an output is still hosted after disable"
fi

kill -0 "$POLICY_PID" 2>/dev/null || fail "the policy surface died (see $LOG_DIR/surface.log)"
kill -0 "$PLASMA_PID" 2>/dev/null || fail "plasmashell died (see $LOG_DIR/plasmashell.log)"
nested_audio_touched_devices && fail "the private audio daemons touched devices (see $LOG_DIR/pipewire*.log)"

if ((tested == 0)); then
    echo "FAIL: no item kind could be tested"
    exit 1
fi
((failed)) && { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS ($tested kinds; screenshots in $OUT_DIR)"
