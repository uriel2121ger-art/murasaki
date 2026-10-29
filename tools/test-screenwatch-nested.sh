#!/usr/bin/env bash
# The screen triggers as far as a nested session lets them go: a private bus, kwin_wayland --virtual, the
# desktop's screen cast portal (xdg-desktop-portal with the KDE backend) and a murasakid from the build directory with a throw-away config and one screen trigger.
#
# What it checks: before the user asks, nothing exists (no helper, no portal call, state `off`, the daemon has no child); StartScreenWatch refused while the setting is off; with the setting on
# it reaches the portal and the desktop's own dialog (state `asking`). The dialog needs a person (there is no input in a virtual KWin that a test may press), so the run STOPS there: either
# the state becomes `watching` (a portal that answers by itself), or stays `asking` for the whole wait, which is the expected result here. Then StopScreenWatch ends it at once: state `off`,
# no helper, no child. What happens after a real yes (frames, scores, a real trigger) is in docs/pets/automation.md, "First real session".
#
#   tools/test-screenwatch-nested.sh [--build-dir DIR] [--out-dir DIR]
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when the environment cannot run it (or MURASAKI_NESTED_TESTS is not 1).
set -u

SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")
if [[ ${1:-} == --inner ]]; then
    shift
    INNER=1
else
    INNER=0
fi
BUILD_DIR=$REPO/build/ci
OUT_DIR=${TMPDIR:-/tmp}/murasaki-screenwatch-nested-out
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
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the nested screen watch test"
        exit 77
    fi
    for tool in dbus-run-session kwin_wayland busctl; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    PORTAL=/usr/libexec/xdg-desktop-portal
    PORTAL_KDE=/usr/lib/x86_64-linux-gnu/libexec/xdg-desktop-portal-kde
    [[ -x $PORTAL && -x $PORTAL_KDE ]] || { echo "SKIP: xdg-desktop-portal and its KDE backend are not installed"; exit 77; }
    for bin in src/daemon/murasakid src/ctl/murasakictl; do
        [[ -x $BUILD_DIR/$bin ]] || { echo "SKIP: $BUILD_DIR/$bin is not built"; exit 77; }
    done
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    TMP_ROOT=$(mktemp -d /tmp/msn.XXXXXX)
    mkdir -m 700 "$TMP_ROOT/rt"
    export MURASAKI_NESTED_TMP=$TMP_ROOT
    unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS KDE_SESSION_UID SWAYSOCK HYPRLAND_INSTANCE_SIGNATURE XDG_SESSION_ID XDG_SEAT XDG_VTNR
    export XDG_RUNTIME_DIR=$TMP_ROOT/rt XDG_CONFIG_HOME=$TMP_ROOT/config XDG_DATA_HOME=$TMP_ROOT/data XDG_CACHE_HOME=$TMP_ROOT/cache XDG_STATE_HOME=$TMP_ROOT/state
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME" "$OUT_DIR/logs"
    export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland KDE_FULL_SESSION=true
    outer_cleanup() {
        trap - EXIT INT TERM
        nested_kill_marked TERM
        sleep 1
        nested_kill_marked KILL
        rm -rf "$TMP_ROOT"
    }
    trap 'outer_cleanup; exit 130' INT TERM
    trap outer_cleanup EXIT
    # An isolated bus: it activates nothing by itself. The two portal services are started by the test, on this bus only.
    mkdir -p "$TMP_ROOT/svc"
    cat >"$TMP_ROOT/svc/portal.service" <<EOF
[D-BUS Service]
Name=org.freedesktop.portal.Desktop
Exec=$PORTAL
EOF
    cat >"$TMP_ROOT/svc/portal-kde.service" <<EOF
[D-BUS Service]
Name=org.freedesktop.impl.portal.desktop.kde
Exec=$PORTAL_KDE
EOF
    nested_write_bus_conf "$TMP_ROOT/bus" "$TMP_ROOT/svc/portal.service" "$TMP_ROOT/svc/portal-kde.service"
    timeout "${MURASAKI_NESTED_TIMEOUT:-300}" dbus-run-session --config-file="$TMP_ROOT/bus/bus.conf" -- "$SELF" --inner --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR" </dev/null 2>"$OUT_DIR/logs/session.log"
    exit $?
fi

# ---- inner half -------------------------------------------------------------------------------------------------
LOG_DIR=$OUT_DIR/logs
mkdir -p "$LOG_DIR"
trap 'nested_kill_marked TERM; sleep 1; nested_kill_marked KILL' EXIT
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
fail() { echo "FAIL: $*"; exit 1; }
SOCKET=wayland-$(basename "$MURASAKI_NESTED_TMP")
KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 kwin_wayland --virtual --socket "$SOCKET" --width 1280 --height 720 --no-lockscreen >"$LOG_DIR/kwin.log" 2>&1 &
for _ in $(seq 100); do [[ -S $XDG_RUNTIME_DIR/$SOCKET ]] && break; sleep 0.1; done
[[ -S $XDG_RUNTIME_DIR/$SOCKET ]] || fail "kwin_wayland did not come up (see $LOG_DIR/kwin.log)"
export WAYLAND_DISPLAY=$SOCKET QT_QPA_PLATFORM=wayland QT_ACCESSIBILITY=0

MD=io.github.uriel2121ger_art.Murasaki
MDP=/io/github/uriel2121ger_art/Murasaki
MDI=io.github.uriel2121ger_art.Murasaki1
export MURASAKI_SCREENMATCH=$BUILD_DIR/src/screenmatch/murasaki-screenmatch
"$BUILD_DIR/src/daemon/murasakid" >"$LOG_DIR/murasakid.log" 2>&1 &
DAEMON=$!
for _ in $(seq 100); do busctl --user get-property "$MD" "$MDP" "$MDI" ApiVersion >/dev/null 2>&1 && break; sleep 0.2; done
busctl --user get-property "$MD" "$MDP" "$MDI" ApiVersion >/dev/null 2>&1 || fail "murasakid did not come up (see $LOG_DIR/murasakid.log)"
state() { busctl --user --json=short get-property "$MD" "$MDP" "$MDI" ScreenWatchState | python3 -I -c 'import json,sys; print(json.loads(json.load(sys.stdin)["data"])["state"])'; }
children() { pgrep -P "$DAEMON" | wc -l; }
helpers() { pgrep -P "$DAEMON" -f murasaki-screenmatch | wc -l; }

echo "1. a fresh daemon: state off, no helper, no child"
[[ $(state) == off ]] || fail "state is not off"
[[ $(helpers) == 0 && $(children) == 0 ]] || fail "a fresh daemon has a helper or a child"

echo "2. the setting is off: StartScreenWatch is refused, nothing is opened"
if busctl --user call "$MD" "$MDP" "$MDI" StartScreenWatch >/dev/null 2>"$LOG_DIR/refused.txt"; then fail "StartScreenWatch worked with the setting off"; fi
grep -q screenTriggersEnabled "$LOG_DIR/refused.txt" || fail "the refusal does not name the setting: $(cat "$LOG_DIR/refused.txt")"
[[ $(state) == off && $(helpers) == 0 ]] || fail "something was opened by a refused start"

echo "3. a screen trigger and the setting on: the portal's dialog is asked for"
TRIGGER='{"name":"nested screen","enabled":true,"source":{"kind":"screen","data":{"regions":[{"kind":"rect","x":10,"y":10,"w":200,"h":100}],"match":{"mode":"hash","threshold":0.8}}},"action":{"type":"hide-all"}}'
busctl --user call "$MD" "$MDP" "$MDI" SetTrigger ss "" "$TRIGGER" >/dev/null || fail "SetTrigger"
busctl --user call "$MD" "$MDP" "$MDI" SetSettings s '{"screenTriggersEnabled":true}' >/dev/null || fail "SetSettings"
if ! busctl --user call "$MD" "$MDP" "$MDI" StartScreenWatch >/dev/null 2>"$LOG_DIR/start.txt"; then
    # no helper is installed in the build, say
    fail "StartScreenWatch refused: $(cat "$LOG_DIR/start.txt")"
fi
seen=
for _ in $(seq 60); do
    s=$(state)
    seen=$s
    [[ $s == asking || $s == watching || $s == failed || $s == refused ]] && break
    sleep 0.5
done
echo "   state after the call: $seen"
case $seen in
asking | watching) ;;
*) busctl --user --json=short get-property "$MD" "$MDP" "$MDI" ScreenWatchState; echo "SKIP: the portal did not get as far as its dialog in this nested session"; exit 77 ;;
esac
if [[ $seen == asking ]]; then
    # the dialog is a person's: wait a little to show that nothing starts by itself, then stop
    sleep 5
    [[ $(state) == asking ]] || echo "   (the state moved on by itself: $(state))"
    [[ $(helpers) == 0 ]] || fail "a helper runs although the dialog has not been answered"
fi

echo "4. StopScreenWatch ends it at once"
busctl --user call "$MD" "$MDP" "$MDI" StopScreenWatch >/dev/null || fail "StopScreenWatch"
[[ $(state) == off ]] || fail "state is $(state) after Stop"
sleep 1
[[ $(helpers) == 0 && $(children) == 0 ]] || fail "a helper or a child is left after Stop"
echo "PASS (the dialog needs a person: the live part is in the first real session checklist)"
