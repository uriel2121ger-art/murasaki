#!/usr/bin/env bash
# Integration test of the GNOME support: a headless gnome-shell (its own Mutter, virtual 1280x720 monitor) in a private
# session with the Murasaki extension enabled, murasakid and murasaki-surface from the build directory.
#
# What it checks:
#  1. the extension reports: Desktops() lists the shell's workspaces (that list only comes from the extension's report,
#     through ReportShellState, which the surface accepts only from org.gnome.Shell);
#  2. the extension is enabled in the private session;
#  3. a fullscreen window (tools/overlay-backdrop) makes the output `occluded` in Outputs(), and closing it uncovers it.
# That the wallpaper window sits behind the windows is not checked here (the headless shell has no screenshot tool for
# us): the manual checklist in docs/gnome.md covers it.
#
# The user's session is never touched: private runtime dir, XDG dirs, dconf (GSETTINGS_BACKEND=keyfile) and bus.
# Nothing is installed (the extension is copied into the private XDG_DATA_HOME).
#
#   tools/test-gnome-nested.sh [--build-dir DIR] [--out-dir DIR]
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when gnome-shell, gnome-extensions, the build or a bus tool is
# missing.
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
BASE=${MURASAKI_NESTED_BASE:-$HOME/.cache/murasaki-tests/nested}   # never /tmp: it is RAM
OUT_DIR=$BASE/gnome-out
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

UUID=murasaki@uriel2121ger-art.github.io
EXT_SRC=$REPO/data/gnome-shell/$UUID
SURFACE=$BUILD_DIR/src/surface/murasaki-surface
DAEMON=$BUILD_DIR/src/daemon/murasakid
CTL=$BUILD_DIR/src/ctl/murasakictl
BACKDROP=$BUILD_DIR/tools/overlay-backdrop/murasaki-overlay-backdrop

if ((!INNER)); then
    command -v gnome-shell >/dev/null || { echo "SKIP: gnome-shell is not installed"; exit 77; }
    command -v gnome-extensions >/dev/null || { echo "SKIP: gnome-extensions is not installed"; exit 77; }
    command -v gsettings >/dev/null || { echo "SKIP: gsettings is not installed"; exit 77; }
    command -v dbus-run-session >/dev/null || { echo "SKIP: dbus-run-session is not installed"; exit 77; }
    for bin in "$SURFACE" "$DAEMON" "$CTL"; do
        [[ -x $bin ]] || { echo "SKIP: $bin is not built (--build-dir)"; exit 77; }
    done
    [[ -f $EXT_SRC/metadata.json ]] || { echo "FAIL: $EXT_SRC is missing"; exit 1; }

    export MURASAKI_NESTED_MARK="gnome-$$"
    mkdir -p "$BASE" || { echo "FAIL: cannot create $BASE"; exit 1; }
    MURASAKI_NESTED_TMP=$(mktemp -d "$BASE/gnome.XXXXXX") || { echo "FAIL: no temporary directory"; exit 1; }
    export MURASAKI_NESTED_TMP
    mkdir -p "$MURASAKI_NESTED_TMP/run"
    chmod 700 "$MURASAKI_NESTED_TMP/run"
    env -u WAYLAND_DISPLAY -u DISPLAY \
        XDG_RUNTIME_DIR="$MURASAKI_NESTED_TMP/run" XDG_CONFIG_HOME="$MURASAKI_NESTED_TMP/config" \
        XDG_DATA_HOME="$MURASAKI_NESTED_TMP/data" XDG_CACHE_HOME="$MURASAKI_NESTED_TMP/cache" \
        XDG_STATE_HOME="$MURASAKI_NESTED_TMP/state" HOME="$MURASAKI_NESTED_TMP/home" \
        MURASAKI_STEAM_ROOT="$MURASAKI_NESTED_TMP/steam" GSETTINGS_BACKEND=keyfile \
        dbus-run-session -- "$SELF" --inner --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR"
    rc=$?
    nested_kill_marked TERM
    sleep 1
    nested_kill_marked KILL
    exit $rc
fi

# ---- inner half: inside the private bus -------------------------------------------------------------------------
LOG_DIR=$OUT_DIR
trap 'nested_kill_marked TERM; sleep 1; nested_kill_marked KILL' EXIT
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME/gnome-shell/extensions" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
cp -r "$EXT_SRC" "$XDG_DATA_HOME/gnome-shell/extensions/$UUID"
gsettings set org.gnome.shell disable-user-extensions false
gsettings set org.gnome.shell enabled-extensions "['$UUID']"

# A fake Steam library with one web wallpaper.
ITEM_DIR=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/431960/777
mkdir -p "$ITEM_DIR"
echo '{"type": "web", "file": "index.html", "title": "Nested test colour"}' >"$ITEM_DIR/project.json"
echo '<!doctype html><html><body style="margin:0;background:#c01020"></body></html>' >"$ITEM_DIR/index.html"

export XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_TYPE=wayland
gnome-shell --wayland --headless --virtual-monitor 1280x720 --wayland-display wayland-murasaki-test \
    >"$LOG_DIR/gnome-shell.log" 2>&1 &
for _ in $(seq 150); do
    [[ -S $XDG_RUNTIME_DIR/wayland-murasaki-test ]] && break
    sleep 0.2
done
[[ -S $XDG_RUNTIME_DIR/wayland-murasaki-test ]] || { echo "FAIL: gnome-shell did not come up (see $LOG_DIR/gnome-shell.log)"; exit 1; }
export WAYLAND_DISPLAY=wayland-murasaki-test QT_QPA_PLATFORM=wayland QT_ACCESSIBILITY=0
for _ in $(seq 100); do
    busctl --user status org.gnome.Shell >/dev/null 2>&1 && break
    sleep 0.2
done
busctl --user status org.gnome.Shell >/dev/null 2>&1 || { echo "FAIL: org.gnome.Shell is not on the private bus"; exit 1; }

"$DAEMON" >"$LOG_DIR/murasakid.log" 2>&1 &
for _ in $(seq 100); do
    "$CTL" list 2>/dev/null | grep -q "we:777" && break
    sleep 0.2
done
"$CTL" list 2>/dev/null | grep -q "we:777" || { echo "FAIL: the private daemon does not see the test item (see $LOG_DIR/murasakid.log)"; exit 1; }
"$CTL" set we:777 >"$LOG_DIR/set.txt" 2>&1 || { echo "FAIL: cannot assign the test item"; exit 1; }

SURF=io.github.uriel2121ger_art.Murasaki.Surface
SURF_PATH=/io/github/uriel2121ger_art/Murasaki/Surface
SURF_IFACE=io.github.uriel2121ger_art.Murasaki.Surface1
surface_call() {
    timeout 5 busctl --user --json=short call "$SURF" "$SURF_PATH" "$SURF_IFACE" "$1" 2>/dev/null | sed -e 's/\\"/"/g'
}
"$SURFACE" >"$LOG_DIR/surface.log" 2>&1 &
for _ in $(seq 150); do
    surface_call Outputs | grep -q '"name"' && break
    sleep 0.2
done
surface_call Outputs | grep -q '"name"' || { echo "FAIL: the surface lists no output (see $LOG_DIR/surface.log)"; exit 1; }

status=0
check() {
    local what=$1
    shift
    if "$@"; then echo "   ok: $what"; else echo "   FAIL: $what"; status=1; fi
}
wait_for() { for _ in $(seq 60); do "$@" && return 0; sleep 0.25; done; return 1; }

# 1. The extension is running and reports: the workspaces in Desktops() come from its report.
has_workspaces() { surface_call Desktops | grep -q '"name"'; }
check "the extension reports (Desktops() lists the shell's workspaces)" wait_for has_workspaces
check "the extension is enabled" bash -c "gnome-extensions list --enabled | grep -qF '$UUID'"

# 3. A fullscreen window occludes the output; closing it uncovers it.
if [[ -x $BACKDROP ]]; then
    "$BACKDROP" >"$LOG_DIR/backdrop.log" 2>&1 &
    BACKDROP_PID=$!
    occluded() { surface_call Outputs | grep -q '"occluded":true'; }
    uncovered() { ! occluded; }
    check "a fullscreen window occludes the output" wait_for occluded
    kill "$BACKDROP_PID" 2>/dev/null
    check "closing it uncovers the output" wait_for uncovered
else
    echo "   skip: $BACKDROP is not built (BUILD_TESTING), no occlusion check"
fi
exit $status
