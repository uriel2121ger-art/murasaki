#!/usr/bin/env bash
# A puppet pet on an X11 session (docs/pets/puppets.md section 22): a private Xvfb, a private D-Bus session, murasakid and murasaki-overlay from the build directory, a puppet folder
# written by this script (one Inochi2D .inp with a red square), the real engine (murasaki-puppet behind murasaki-sandbox when it works here) on whatever Vulkan device there is
# (VK_ICD_FILENAMES may point at llvmpipe). It checks:
#   - AddLocalPuppetItem makes the item and the sandboxed check answers (the canvas, the counts);
#   - the pet's window shows the puppet's picture (the red square is on the screen, read back with ffmpeg x11grab) and Windows() says the engine runs;
#   - the engine killed by a signal is restarted (puppet: engine restarted) and the picture comes back; killed three times in a row it is not started again (blocked);
#   - removing the pet stops the engine process.
# There is no window manager and no compositor: transparency is covered by the unit tests and by the nested KWin test.
#
#   tools/test-overlay-puppet-x11.sh [--build-dir DIR] [--out-dir DIR]
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when Xvfb, xwininfo, xprop or the build is missing, or MURASAKI_NESTED_TESTS is not 1.
set -u

SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")
if [[ ${1:-} == --inner ]]; then
    INNER=1
    shift
else
    INNER=0
fi
BUILD_DIR=$REPO/build/dev
OUT_DIR=${MURASAKI_NESTED_BASE:-$HOME/.cache/murasaki-tests/nested}/puppet-x11-out
while (($#)); do
    case $1 in
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --out-dir) OUT_DIR=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
BUILD_DIR=$(readlink -f "$BUILD_DIR")
mkdir -p "$OUT_DIR"
OUT_DIR=$(readlink -f "$OUT_DIR")

# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"

if ((!INNER)); then
    if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the X11 puppet test"
        exit 77
    fi
    for tool in dbus-run-session Xvfb xwininfo xprop xdpyinfo ffmpeg python3 busctl pgrep; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    for bin in src/daemon/murasakid src/ctl/murasakictl src/overlay/murasaki-overlay src/puppet/murasaki-puppet src/sandbox/murasaki-sandbox; do
        [[ -x $BUILD_DIR/$bin ]] || { echo "SKIP: $BUILD_DIR/$bin is not built"; exit 77; }
    done
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    # (under the nested-test base, on disk: /tmp is RAM)
    mkdir -p "${MURASAKI_NESTED_BASE:-$HOME/.cache/murasaki-tests/nested}/r" || { echo "FAIL: no temporary directory"; exit 1; }
    TMP_ROOT=$(mktemp -d "${MURASAKI_NESTED_BASE:-$HOME/.cache/murasaki-tests/nested}/r/mpx.XXXXXX") || { echo "FAIL: no temporary directory"; exit 1; }
    [[ -n $TMP_ROOT && -d $TMP_ROOT ]] || { echo "FAIL: no temporary directory"; exit 1; }
    mkdir -m 700 "$TMP_ROOT/rt"
    export MURASAKI_NESTED_TMP=$TMP_ROOT
    unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS KDE_SESSION_UID SWAYSOCK HYPRLAND_INSTANCE_SIGNATURE
    unset XDG_SESSION_ID XDG_SEAT XDG_VTNR QT_QPA_PLATFORMTHEME
    export HOME=$TMP_ROOT/home
    export XDG_RUNTIME_DIR=$TMP_ROOT/rt XDG_CONFIG_HOME=$TMP_ROOT/config XDG_DATA_HOME=$TMP_ROOT/data
    export XDG_CACHE_HOME=$TMP_ROOT/cache XDG_STATE_HOME=$TMP_ROOT/state
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$OUT_DIR/logs"
    export XDG_SESSION_TYPE=x11 XDG_CURRENT_DESKTOP=X-Generic QT_QPA_PLATFORM=xcb QT_ACCESSIBILITY=0
    export MURASAKI_STEAM_ROOT=$TMP_ROOT/steam MURASAKI_WORKSHOP_API_URL=http://127.0.0.1:9/
    export MURASAKI_OVERLAY_UNIT=0 MURASAKI_OVERLAY_TEST_HOOKS=1
    mkdir -p "$TMP_ROOT/services"
    cat >"$TMP_ROOT/services/io.github.uriel2121ger_art.Murasaki.Overlay.service" <<SERVICE
[D-BUS Service]
Name=io.github.uriel2121ger_art.Murasaki.Overlay
Exec=$TMP_ROOT/services/run-overlay.sh
SERVICE
    cat >"$TMP_ROOT/services/run-overlay.sh" <<RUN
#!/bin/sh
exec "$BUILD_DIR/src/overlay/murasaki-overlay" >>"$OUT_DIR/logs/overlay.log" 2>&1
RUN
    chmod +x "$TMP_ROOT/services/run-overlay.sh"
    cat >"$TMP_ROOT/bus.conf" <<CONF
<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration 1.0//EN"
 "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">
<busconfig>
  <type>session</type>
  <keep_umask/>
  <listen>unix:tmpdir=/tmp</listen>
  <auth>EXTERNAL</auth>
  <servicedir>$TMP_ROOT/services</servicedir>
  <policy context="default">
    <allow send_destination="*" eavesdrop="true"/>
    <allow eavesdrop="true"/>
    <allow own="*"/>
  </policy>
</busconfig>
CONF
    outer_cleanup() {
        trap - EXIT INT TERM
        nested_kill_marked TERM
        sleep 1
        nested_kill_marked KILL
        rm -rf "$TMP_ROOT"
    }
    trap 'outer_cleanup; exit 130' INT TERM
    trap outer_cleanup EXIT
    # Xvfb carries the mark too (it inherits the environment), so the cleanup finds it. It picks a free display itself and says which on a file descriptor, so that a
    # display of somebody else's is never taken for ours.
    Xvfb -displayfd 9 -screen 0 1280x720x24 -nolisten tcp 9>"$TMP_ROOT/display" >"$OUT_DIR/logs/xvfb.log" 2>&1 &
    XVFB_PID=$!
    for _ in $(seq 50); do
        [[ -s $TMP_ROOT/display ]] && break
        kill -0 "$XVFB_PID" 2>/dev/null || { echo "FAIL: Xvfb did not start (see $OUT_DIR/logs/xvfb.log)"; exit 1; }
        sleep 0.2
    done
    [[ -s $TMP_ROOT/display ]] || { echo "FAIL: Xvfb did not say its display"; exit 1; }
    export DISPLAY=:$(head -1 "$TMP_ROOT/display")
    kill -0 "$XVFB_PID" 2>/dev/null || { echo "FAIL: Xvfb is gone"; exit 1; }
    timeout "${MURASAKI_NESTED_TIMEOUT:-240}" dbus-run-session --config-file="$TMP_ROOT/bus.conf" -- "$SELF" --inner \
        --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR" </dev/null 2>"$OUT_DIR/logs/session.log"
    exit $?
fi

# ---- inner half -------------------------------------------------------------------------------------------------
LOG_DIR=$OUT_DIR/logs
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
[[ $HOME == "$MURASAKI_NESTED_TMP"/* ]] || { echo "FAIL: HOME is not private, refusing to continue"; exit 1; }
failed=0
fail() { echo "   FAIL: $*"; failed=1; }
OV=io.github.uriel2121ger_art.Murasaki.Overlay
OV_PATH=/io/github/uriel2121ger_art/Murasaki/Overlay
OV_IF=io.github.uriel2121ger_art.Murasaki.Overlay1
MS=io.github.uriel2121ger_art.Murasaki
MS_PATH=/io/github/uriel2121ger_art/Murasaki
MS_IF=io.github.uriel2121ger_art.Murasaki1
CTL=$BUILD_DIR/src/ctl/murasakictl
jcall() { timeout 30 busctl --user --json=short call "$@" 2>>"$LOG_DIR/busctl.err" | python3 -c 'import sys, json; print(json.load(sys.stdin)["data"][0])'; }
windows() { jcall $OV $OV_PATH $OV_IF Windows; }
wait_for() { local limit=$1 what=$2 deadline; shift 2; deadline=$((SECONDS + limit)); while ((SECONDS < deadline)); do "$@" >/dev/null 2>&1 && return 0; sleep 0.3; done; fail "timed out waiting for: $what"; return 1; }
# A field of the first window of Windows(), e.g. puppet.state.
wfield() { windows | python3 -c 'import sys, json
w = json.load(sys.stdin)
v = w[0] if w else {}
for k in sys.argv[1].split("."):
    v = v.get(k, "") if isinstance(v, dict) else ""
print(v)' "$1"; }
state_is() { [[ $(wfield puppet.state) == "$1" ]]; }
# THIS run's engines only: the processes named murasaki-puppet below the overlay of this private session (found by the run's mark in its environment). Never a host-wide pgrep: the user's own
# overlay, a benchmark or another test may have an engine too, and the signals of this test must not reach it.
overlay_pid() {
    local pid
    for pid in $(pgrep -u "$(id -u)" -x murasaki-overla); do # (the kernel keeps 15 characters of a process name)
        tr '\0' '\n' <"/proc/$pid/environ" 2>/dev/null | grep -qxF "MURASAKI_NESTED_MARK=$MURASAKI_NESTED_MARK" && { echo "$pid"; return 0; }
    done
    return 1
}
engines() {
    local root
    root=$(overlay_pid) || return 0
    ps -eo pid=,ppid=,comm= | python3 -c '
import sys
rows = [l.split(None, 2) for l in sys.stdin.read().splitlines() if l.strip()]
kids = {}
for pid, ppid, comm in rows:
    kids.setdefault(ppid, []).append((pid, comm.strip()))
todo, out = [sys.argv[1]], []
while todo:
    for pid, comm in kids.get(todo.pop(), []):
        todo.append(pid)
        if comm == "murasaki-puppet":
            out.append(pid)
print("\n".join(out))' "$root"
}
engine_pid() { engines | head -1; }
engine_count() { engines | grep -c .; }
# The most engines alive at once during `seconds` (polled): there must never be two.
max_engines() { local until=$((SECONDS + $1)) most=0 n; while ((SECONDS < until)); do n=$(engine_count); ((n > most)) && most=$n; sleep 0.05; done; echo "$most"; }

# The puppet folder: one Inochi2D .inp, a part of 100 x 100 model units with a red 2 x 2 texture.
PUP=$MURASAKI_NESTED_TMP/puppets/red
mkdir -p "$PUP"
python3 - "$PUP/model.inp" <<'PY'
import json, struct, sys, zlib
def png(w, h, rgba):
    raw = b"".join(b"\x00" + bytes(rgba) * w for _ in range(h))
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b"")
tr = {"trans": [0, 0, 0], "rot": [0, 0, 0], "scale": [1, 1]}
part = {"uuid": 2, "name": "Body", "type": "Part", "enabled": True, "zsort": 0, "transform": tr,
        "mesh": {"verts": [0, 0, 100, 0, 100, 100, 0, 100], "uvs": [0, 0, 1, 0, 1, 1, 0, 1], "indices": [0, 1, 2, 0, 2, 3], "origin": [0, 0]},
        "textures": [0, 4294967295, 4294967295], "blend_mode": "Normal", "tint": [1, 1, 1], "screenTint": [0, 0, 0], "opacity": 1, "mask_threshold": 0.5}
root = {"uuid": 1, "name": "Root", "type": "Node", "enabled": True, "zsort": 0, "transform": tr, "children": [part]}
doc = json.dumps({"meta": {"name": "Red"}, "nodes": root, "param": []}).encode()
data = b"TRNSRTS\x00" + struct.pack(">I", len(doc)) + doc + b"TEX_SECT" + struct.pack(">I", 1)
tex = png(2, 2, (255, 0, 0, 255))
data += struct.pack(">I", len(tex)) + b"\x00" + tex
open(sys.argv[1], "wb").write(data)
PY
[[ -s $PUP/model.inp ]] || { echo "FAIL: no puppet written"; exit 1; }

"$BUILD_DIR/src/daemon/murasakid" >"$LOG_DIR/murasakid.log" 2>&1 &
wait_for 30 "the daemon" bash -c "'$CTL' list >/dev/null 2>&1" || exit 1

echo "== the item"
ITEM=$(jcall $MS $MS_PATH $MS_IF AddLocalPuppetItem s "$PUP") || { echo "FAIL: AddLocalPuppetItem"; exit 1; }
echo "   item $ITEM"
[[ $ITEM == local:puppet:* ]] || { echo "FAIL: not a puppet item id: $ITEM"; exit 1; }
item_checked() { jcall $MS $MS_PATH $MS_IF GetItem s "$ITEM" | python3 -c 'import sys, json
i = json.load(sys.stdin)
sys.exit(0 if i["format"] == "puppet" and i["puppet"]["checked"] and i["status"] == "ok" and i["variants"][0]["width"] == 100 else 1)'; }
wait_for 40 "the sandboxed check to answer" item_checked || { jcall $MS $MS_PATH $MS_IF GetItem s "$ITEM"; exit 1; }

echo "== the pet"
timeout 30 "$CTL" pets add "$ITEM" --variant model --x 100 --y 80 --width 200 >"$LOG_DIR/pets-add.txt" 2>&1 || { cat "$LOG_DIR/pets-add.txt"; echo "FAIL: pets add"; exit 1; }
PET_ID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add.txt" | head -1)
pet_shown() { windows | python3 -c 'import sys, json; w = json.load(sys.stdin); sys.exit(0 if w and w[0]["shown"] else 1)'; }
drawing() { [[ $(wfield puppet.state) == running && -n $(engine_pid) ]]; }
crashes_are() { [[ $(wfield puppet.crashes) == "$1" ]]; }
no_engine() { [[ -z $(engine_pid) ]]; }
a_new_engine() { local pid; pid=$(engine_pid); [[ -n $pid && $pid != "$1" ]]; }
wait_for 40 "the overlay to show the pet" pet_shown || { tail -20 "$LOG_DIR/overlay.log"; exit 1; }
wait_for 60 "the engine to draw" drawing || { tail -30 "$LOG_DIR/overlay.log"; windows; echo "overlay pid: $(overlay_pid)"; ps -eo pid,ppid,comm | grep -E "overlay|puppet|sandbox|bwrap|murasakid"; exit 1; }
echo "   $(windows | cut -c1-300)"

# The same through murasakictl: the puppet list names the item as checked, and the status line shows the engine running.
"$CTL" pets puppet list >"$LOG_DIR/ctl-puppet-list.txt" 2>&1 || { cat "$LOG_DIR/ctl-puppet-list.txt"; fail "murasakictl pets puppet list"; }
grep -q "$ITEM" "$LOG_DIR/ctl-puppet-list.txt" || { cat "$LOG_DIR/ctl-puppet-list.txt"; fail "the puppet list does not name the item"; }
"$CTL" pets puppet status >"$LOG_DIR/ctl-puppet-status.txt" 2>&1 || { cat "$LOG_DIR/ctl-puppet-status.txt"; fail "murasakictl pets puppet status"; }
grep -q "running" "$LOG_DIR/ctl-puppet-status.txt" || { cat "$LOG_DIR/ctl-puppet-status.txt"; fail "the puppet status does not say running"; }

# The picture: the red square, read back from the X server. The pet is 200 wide and 200 high (the canvas is square): the middle of its window is red.
shot() { ffmpeg -y -v error -f x11grab -video_size 1280x720 -i "$DISPLAY" -frames:v 1 "$1" 2>>"$LOG_DIR/ffmpeg.log"; }
pixel() { python3 - "$1" "$2" "$3" <<'PY'
import subprocess, sys
png, x, y = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
raw = subprocess.run(["ffmpeg", "-v", "error", "-i", png, "-vf", f"crop=1:1:{x}:{y}", "-f", "rawvideo", "-pix_fmt", "rgb24", "-"], capture_output=True).stdout
print(raw[0], raw[1], raw[2])
PY
}
red_middle() { shot "$LOG_DIR/poll.png" && read -r R G B < <(pixel "$LOG_DIR/poll.png" 200 180) && ((R > 200 && G < 80 && B < 80)); }
wait_for 40 "the red square on the screen" red_middle
shot "$LOG_DIR/shot1.png"
read -r R G B < <(pixel "$LOG_DIR/shot1.png" 200 180)
echo "   the middle of the pet: $R $G $B"
((R > 200 && G < 80 && B < 80)) || fail "the middle of the pet is not the puppet's red (got $R $G $B)"
read -r R G B < <(pixel "$LOG_DIR/shot1.png" 20 20)
((R < 80 && G < 80)) || echo "   (outside the pet the desktop shows $R $G $B)"

echo "== the engine runs confined"
EP=$(engine_pid)
[[ -n $EP ]] || fail "no murasaki-puppet process of this run"
if [[ -n $EP ]]; then
    echo "   engine $EP: $(tr '\0' ' ' </proc/$EP/cmdline | cut -c1-200)"
    # importRequireSandbox is on (the default): an engine without its filter is a failure, not a note
    grep -q "^Seccomp:[[:space:]]*2" /proc/$EP/status || fail "the engine runs without a seccomp filter (see /proc/$EP/status)"
    grep -q "^NoNewPrivs:[[:space:]]*1" /proc/$EP/status || fail "the engine can gain privileges (NoNewPrivs is not set)"
fi

echo "== the sandbox setting restarts the engine, never two at once"
OLD=$(engine_pid)
timeout 30 busctl --user call $MS $MS_PATH $MS_IF SetSettings s '{"importRequireSandbox": false}' >/dev/null 2>>"$LOG_DIR/busctl.err" || fail "SetSettings"
MOST=$(max_engines 6)
echo "   at most $MOST engine(s) alive at once"
((MOST <= 1)) || fail "two engines were alive at once ($MOST)"
wait_for 60 "an engine to draw after the setting" drawing || windows
a_new_engine "$OLD" || fail "the setting did not restart the engine (still pid $OLD)"
timeout 30 busctl --user call $MS $MS_PATH $MS_IF SetSettings s '{"importRequireSandbox": true}' >/dev/null 2>>"$LOG_DIR/busctl.err" || fail "SetSettings back"
MOST=$(max_engines 6)
((MOST <= 1)) || fail "two engines were alive at once after the setting went back ($MOST)"
wait_for 60 "an engine to draw again" drawing || windows

echo "== a crash is restarted"
EP=$(engine_pid)
kill -SEGV "$EP" 2>/dev/null
wait_for 20 "the crash to be counted" crashes_are 1
MOST=$(max_engines 4)
((MOST <= 1)) || fail "two engines were alive at once after a crash ($MOST)"
wait_for 60 "the picture to come back" drawing
wait_for 20 "a new engine process" a_new_engine "$EP"
wait_for 40 "the red square again" red_middle
shot "$LOG_DIR/shot2.png"
read -r R G B < <(pixel "$LOG_DIR/shot2.png" 200 180)
echo "   after the restart: $R $G $B"
((R > 200 && G < 80 && B < 80)) || fail "after a restart the pet is not drawn again (got $R $G $B)"

echo "== three crashes block the model"
for n in 2 3; do
    wait_for 60 "run $n to draw" drawing || break
    EP=$(engine_pid)
    [[ -n $EP ]] || break
    kill -SEGV "$EP" 2>/dev/null
    wait_for 20 "crash $n to be counted" crashes_are "$n" || break
done
is_blocked() { [[ $(wfield puppet.state) == blocked ]]; }
wait_for 20 "the model to be blocked" is_blocked || windows
wait_for 10 "the engine to be gone" no_engine
no_engine || fail "the engine is still there after the model was blocked"
[[ $(wfield puppet.crashes) == 3 ]] || fail "Windows() does not report 3 crashes for the blocked puppet (got $(wfield puppet.crashes))"
windows | grep -q "puppet: blocked after crashes" || fail "Windows() does not say puppet: blocked after crashes"

echo "== removing the pet"
timeout 10 "$CTL" pets remove "$PET_ID" >>"$LOG_DIR/busctl.err" 2>&1 || true
wait_for 10 "no engine of this run" no_engine
no_engine || fail "an engine outlives its pet"

((failed)) && { echo "RESULT: FAIL (logs in $LOG_DIR)"; exit 1; }
echo "RESULT: PASS (logs in $LOG_DIR)"
