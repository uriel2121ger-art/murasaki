#!/usr/bin/env bash
# The desktop pets on an X11 session, with the fullscreen mode (stage): a private Xvfb, a private D-Bus session, murasakid and murasaki-overlay from the build
# directory (QT_QPA_PLATFORM=xcb) and one generated pet. It checks, by reading the windows back from the X server:
#   - the pet's window is an override-redirect window of the pet's size, with the notification type and the above state;
#   - as a stage (`pets stage <id> output`) the same window covers the whole screen, is still override-redirect, has the above state and NEVER the fullscreen or maximized
#     state (a window manager and every monitor would take it for an application), and is not in the window manager's client list;
#   - turned off, it is the pet's window again at its old size.
# There is no window manager and no compositor: transparency and the stacking against ordinary windows are covered by tst_x11hints (unit level) and the nested KWin test.
#
#   tools/test-overlay-x11.sh [--build-dir DIR] [--out-dir DIR]
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
OUT_DIR=${MURASAKI_NESTED_BASE:-$HOME/.cache/murasaki-tests/nested}/x11-out
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
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the X11 overlay test"
        exit 77
    fi
    for tool in dbus-run-session Xvfb xwininfo xprop xdpyinfo ffmpeg python3 busctl; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    for bin in src/daemon/murasakid src/ctl/murasakictl src/overlay/murasaki-overlay tools/overlay-backdrop/murasaki-overlay-backdrop tools/stage-probe/murasaki-fakecm; do
        [[ -x $BUILD_DIR/$bin ]] || { echo "SKIP: $BUILD_DIR/$bin is not built"; exit 77; }
    done
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    TMP_ROOT=$(mktemp -d /tmp/mox.XXXXXX) || { echo "FAIL: no temporary directory"; exit 1; }
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
CTL=$BUILD_DIR/src/ctl/murasakictl
jcall() { timeout 30 busctl --user --json=short call "$@" 2>>"$LOG_DIR/busctl.err" | python3 -c 'import sys, json; print(json.load(sys.stdin)["data"][0])'; }
windows() { jcall $OV $OV_PATH $OV_IF Windows; }
wait_for() { local limit=$1 what=$2 deadline; shift 2; deadline=$((SECONDS + limit)); while ((SECONDS < deadline)); do "$@" >/dev/null 2>&1 && return 0; sleep 0.3; done; fail "timed out waiting for: $what"; return 1; }

# An ordinary window (the backdrop of the nested tests, fullscreen on this X server) to tell the stacking against: the pet stays above it, and a stage with keep above off goes under.
"$BUILD_DIR/tools/overlay-backdrop/murasaki-overlay-backdrop" >"$LOG_DIR/backdrop.log" 2>&1 &
sleep 2

PET_DIR=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/3474900/9000001
mkdir -p "$PET_DIR"
timeout 120 ffmpeg -y -v error -f lavfi -i "color=c=black:s=320x200:r=15:d=2,format=gbrap,geq=r='255':g='160':b='40':a='if(lt(hypot(X-160,Y-100),50),255,0)',format=rgba" \
    -plays 0 -f apng "$PET_DIR/disc.apng" 2>"$LOG_DIR/ffmpeg.log" || { echo "FAIL: ffmpeg"; exit 1; }
"$BUILD_DIR/src/daemon/murasakid" >"$LOG_DIR/murasakid.log" 2>&1 &
wait_for 30 "the daemon to list the pet" bash -c "'$CTL' list 2>/dev/null | grep -q '^anima:9000001'" || exit 1
timeout 30 "$CTL" pets add anima:9000001 --variant disc --x 100 --y 80 --width 256 >"$LOG_DIR/pets-add.txt" 2>&1 || { echo "FAIL: pets add"; exit 1; }
PET_ID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add.txt" | head -1)
wait_for 40 "the overlay to show the pet" bash -c "$(declare -f jcall windows); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | python3 -c 'import sys, json; w = json.load(sys.stdin); sys.exit(0 if w and w[0][\"shown\"] else 1)'" || { tail -20 "$LOG_DIR/overlay.log"; exit 1; }

# The windows of the X server that belong to the overlay: the children of the root with a notification type (the pet hints).
pet_window() { # prints "<id> <width> <height> <x> <y> <override>" of the first override-redirect child of the root that is not the root
    python3 - <<'PY'
import re, subprocess, sys
out = subprocess.run(["xwininfo", "-root", "-children"], capture_output=True, text=True).stdout
for m in re.finditer(r"^\s+(0x[0-9a-f]+) .*?: \(.*?\)\s+(\d+)x(\d+)\+(-?\d+)\+(-?\d+)", out, re.M):
    wid, w, h, x, y = m.groups()
    info = subprocess.run(["xwininfo", "-id", wid, "-stats"], capture_output=True, text=True).stdout
    if "Override Redirect State: yes" in info and "Map State: IsViewable" in info:
        print(wid, w, h, x, y)
        sys.exit(0)
sys.exit(1)
PY
}
# The children of the root window, topmost first, as xwininfo lists them; the position of a window in it (0 is the topmost).
stack_pos() {
    xwininfo -root -children | python3 -c '
import re, sys
ids = re.findall(r"^\s+(0x[0-9a-f]+) ", sys.stdin.read(), re.M)
want = sys.argv[1].lower()
print(next((i for i, w in enumerate(ids) if int(w, 16) == int(want, 16)), -1))' "$1"
}
backdrop_pos() { # the position of the ordinary window (the backdrop: with no window manager it is not resized to the screen, it only has to exist and be viewable)
    python3 - <<'PY'
import re, subprocess
out = subprocess.run(["xwininfo", "-root", "-children"], capture_output=True, text=True).stdout
ids = re.findall(r"^\s+(0x[0-9a-f]+) ", out, re.M)
for i, wid in enumerate(ids):
    info = subprocess.run(["xwininfo", "-id", wid, "-stats"], capture_output=True, text=True).stdout
    if "Murasaki overlay test backdrop" in info and "Override Redirect State: no" in info and "Map State: IsViewable" in info:
        print(i, wid)
        break
else:
    print("-1 0")
PY
}
state_of() { xprop -id "$1" _NET_WM_STATE 2>/dev/null; }

echo "== the pet"
sleep 1
read -r BPOS BID < <(backdrop_pos)
echo "   the ordinary window $BID is at $BPOS in the stack"
[[ $BPOS != -1 ]] || fail "the ordinary window is not on the X server (see $LOG_DIR/backdrop.log)"
read -r WID W H X Y < <(pet_window) || { echo "FAIL: no override-redirect window for the pet"; exit 1; }
echo "   pet window $WID ${W}x${H}+$X+$Y"
[[ $W == 256 && $H == 160 ]] || fail "the pet's window is not 256x160: ${W}x${H}"
(( $(stack_pos "$WID") < BPOS )) || fail "the pet is not above the ordinary window"
state_of "$WID" | grep -q "_NET_WM_STATE_ABOVE" || fail "the pet has no above state: $(state_of "$WID")"
xprop -id "$WID" _NET_WM_WINDOW_TYPE | grep -q "NOTIFICATION" || fail "the pet is not a notification-type window"

echo "== the stage without a compositing manager"
# Xvfb has none: a screen-sized translucent window would be an opaque rectangle over the desktop, so the stage is shown as a pet and says why.
timeout 10 "$CTL" pets stage "$PET_ID" output >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets stage output"
sleep 3
W2=$(windows)
python3 - "$W2" <<'PY' || fail "without a compositor the pet is not a pet that says so: $W2"
import json, sys
w = json.loads(sys.argv[1])[0]
sys.exit(0 if "stage" not in w and "stage: needs a compositor" in w["degraded"] and w["shown"] else 1)
PY
read -r WID W H X Y < <(pet_window) || { echo "FAIL: no window for the pet"; exit 1; }
echo "   window $WID ${W}x${H}+$X+$Y"
[[ $W == 256 && $H == 160 ]] || fail "without a compositor the window is not the pet's 256x160: ${W}x${H}"

echo "== a compositing manager starts: the stage"
"$BUILD_DIR/tools/stage-probe/murasaki-fakecm" >"$LOG_DIR/fakecm.log" 2>&1 &
FAKECM_PID=$!
wait_for 8 "the stage to appear once a compositor owns the selection" bash -c "$(declare -f jcall windows); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | python3 -c 'import sys, json; w = json.load(sys.stdin); sys.exit(0 if w and w[0].get(\"stage\") == \"output\" else 1)'"
sleep 1.5
read -r WID W H X Y < <(pet_window) || { echo "FAIL: no window for the stage"; exit 1; }
echo "   stage window $WID ${W}x${H}+$X+$Y"
[[ $W == 1280 && $H == 720 && $X == 0 && $Y == 0 ]] || fail "the stage does not cover the screen: ${W}x${H}+$X+$Y"
S=$(state_of "$WID")
echo "   state: $S"
grep -q "_NET_WM_STATE_ABOVE" <<<"$S" || fail "the stage has no above state"
grep -q "FULLSCREEN" <<<"$S" && fail "the stage has the fullscreen state: a monitor would take it for an application"
grep -qE "MAXIMIZED" <<<"$S" && fail "the stage has a maximized state"
CLIENTS=$(xprop -root _NET_CLIENT_LIST 2>/dev/null)
grep -q "$WID" <<<"$CLIENTS" && fail "the stage is in the window manager's client list"
W2=$(windows)
python3 - "$W2" <<'PY' || fail "Windows() does not describe a stage on a covering rectangle, shown, not interactive"
import json, sys
w = json.loads(sys.argv[1])[0]
sys.exit(0 if w["stage"] == "output" and w["shown"] and not w["interactive"] and round(w["width"]) > 0 else 1)
PY

echo "== keep above off"
timeout 10 "$CTL" pets set "$PET_ID" --keep-above off >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets set --keep-above off"
sleep 1.5
S=$(state_of "$WID")
echo "   state: $S"
# (with no window manager the state of a mapped window can not change; what keep above off does here is the stacking)
grep -q "FULLSCREEN" <<<"$S" && fail "the stage has the fullscreen state with keep above off"
read -r BPOS BID < <(backdrop_pos)
(( $(stack_pos "$WID") > BPOS )) || fail "the stage with keep above off is still above the ordinary window (stage at $(stack_pos "$WID"), window at $BPOS)"
timeout 10 "$CTL" pets set "$PET_ID" --keep-above on >>"$LOG_DIR/busctl.err" 2>&1
sleep 3.5   # (an above pet is raised every two seconds)
read -r BPOS BID < <(backdrop_pos)
(( $(stack_pos "$WID") < BPOS )) || fail "the stage is not back above the ordinary window with keep above on"
echo "== the compositing manager goes: back to a pet that says so"
kill "$FAKECM_PID" 2>/dev/null
wait_for 8 "the stage to become a pet when the compositor is gone" bash -c "$(declare -f jcall windows); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | python3 -c 'import sys, json; w = json.load(sys.stdin); sys.exit(0 if w and \"stage\" not in w[0] and \"stage: needs a compositor\" in w[0][\"degraded\"] else 1)'"
sleep 1
read -r WID W H X Y < <(pet_window) || { echo "FAIL: no window for the pet"; exit 1; }
[[ $W == 256 && $H == 160 ]] || fail "after the compositor went the window is not the pet's: ${W}x${H}"

echo "== back to a pet"
timeout 10 "$CTL" pets stage "$PET_ID" off >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets stage off"
sleep 2
read -r WID W H X Y < <(pet_window) || { echo "FAIL: no pet window after the stage"; exit 1; }
echo "   pet window $WID ${W}x${H}+$X+$Y"
[[ $W == 256 && $H == 160 ]] || fail "the pet did not come back to 256x160: ${W}x${H}"

((failed)) && { echo "RESULT: FAIL (logs in $LOG_DIR)"; exit 1; }
echo "RESULT: PASS (logs in $LOG_DIR)"
