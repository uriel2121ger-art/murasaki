#!/usr/bin/env bash
# Integration test of murasaki-surface's layer-shell presenter on wlroots-style compositors (sway, labwc, Hyprland, Wayfire), run
# nested and headless: WLR_BACKENDS=headless (Hyprland inside a virtual KWin), a private runtime directory and D-Bus session, screenshots with grim.
#
# What it checks, per compositor found on PATH:
#  1. the compositor profile is recognised (the surface logs its summary line);
#  2. a synthetic web wallpaper (a solid colour page, made on the fly in a private fake Steam library) is on screen: a
#     grim screenshot of the output shows that colour, which means the background layer surface is mapped and drawn;
#  3. with a fullscreen window on top (tools/overlay-backdrop) the output becomes occluded in Outputs() (the foreign
#     toplevel protocol or the compositor's IPC works) and the wallpaper pauses; closing the window resumes it.
#
# Nothing of the user's session is touched: the runtime dir, the XDG dirs and the bus are private, and nothing is
# installed. Pointer input for interactive wallpapers needs a virtual pointer tool (wlrctl, wtype, ...) and is left to
# the manual checklist in docs/wlroots.md.
#
#   tools/test-wlroots-nested.sh [--build-dir DIR] [--out-dir DIR] [--compositors sway,labwc,hyprland,wayfire]
#
# Exit status: 0 every compositor that was found passed, 1 a check failed, 77 (ctest skip) when no compositor, grim or
# build is available.
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
OUT_DIR=$BASE/wlroots-out
COMPOSITORS=sway,labwc,hyprland,wayfire
ONE=
while (($#)); do
    case $1 in
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --out-dir) OUT_DIR=$2; shift 2 ;;
    --compositors) COMPOSITORS=$2; shift 2 ;;
    --one) ONE=$2; shift 2 ;;   # used by the outer half
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
BUILD_DIR=$(readlink -f "$BUILD_DIR")
mkdir -p "$OUT_DIR"
OUT_DIR=$(readlink -f "$OUT_DIR")

# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"

SURFACE=$BUILD_DIR/src/surface/murasaki-surface
DAEMON=$BUILD_DIR/src/daemon/murasakid
CTL=$BUILD_DIR/src/ctl/murasakictl
BACKDROP=$BUILD_DIR/tools/overlay-backdrop/murasaki-overlay-backdrop

compositor_binary() {
    case $1 in
    sway) command -v sway ;;
    labwc) command -v labwc ;;
    hyprland) command -v Hyprland ;;
    wayfire) command -v wayfire ;;
    *) return 1 ;;
    esac
}

if ((!INNER)); then
    # ---- outer half: pick the compositors, give each a private runtime dir and a private bus -------------------
    command -v grim >/dev/null || { echo "SKIP: grim is not installed"; exit 77; }
    command -v dbus-run-session >/dev/null || { echo "SKIP: dbus-run-session is not installed"; exit 77; }
    command -v python3 >/dev/null || { echo "SKIP: python3 is not installed"; exit 77; }
    for bin in "$SURFACE" "$DAEMON" "$CTL"; do
        [[ -x $bin ]] || { echo "SKIP: $bin is not built (--build-dir)"; exit 77; }
    done
    found=()
    IFS=, read -ra wanted <<<"$COMPOSITORS"
    for name in "${wanted[@]}"; do
        if compositor_binary "$name" >/dev/null; then
            found+=("$name")
        else
            echo "SKIP $name: not installed"
        fi
    done
    ((${#found[@]})) || { echo "SKIP: none of $COMPOSITORS is installed"; exit 77; }

    status=0
    for name in "${found[@]}"; do
        export MURASAKI_NESTED_MARK="wlroots-$name-$$"
        mkdir -p "$BASE" || { echo "FAIL: cannot create $BASE"; exit 1; }
        MURASAKI_NESTED_TMP=$(mktemp -d "$BASE/wlroots-$name.XXXXXX") || { echo "FAIL: no temporary directory"; exit 1; }
        export MURASAKI_NESTED_TMP
        RUNTIME=$MURASAKI_NESTED_TMP/run
        # Hyprland puts its IPC sockets under $XDG_RUNTIME_DIR/hypr/<64-char signature>/, past the 108-byte limit of a
        # socket path from a directory this deep: a short private one, sockets only, removed below.
        [[ $name == hyprland ]] && RUNTIME=$(mktemp -d /tmp/murasaki-h.XXXXXX)
        mkdir -p "$RUNTIME"
        chmod 700 "$RUNTIME"
        echo "== $name (private dir $MURASAKI_NESTED_TMP)"
        env -u WAYLAND_DISPLAY -u DISPLAY -u SWAYSOCK -u HYPRLAND_INSTANCE_SIGNATURE -u XDG_CURRENT_DESKTOP \
            XDG_RUNTIME_DIR="$RUNTIME" \
            XDG_CONFIG_HOME="$MURASAKI_NESTED_TMP/config" XDG_DATA_HOME="$MURASAKI_NESTED_TMP/data" \
            XDG_CACHE_HOME="$MURASAKI_NESTED_TMP/cache" XDG_STATE_HOME="$MURASAKI_NESTED_TMP/state" \
            HOME="$MURASAKI_NESTED_TMP/home" MURASAKI_STEAM_ROOT="$MURASAKI_NESTED_TMP/steam" \
            dbus-run-session -- "$SELF" --inner --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR" --one "$name"
        rc=$?
        nested_kill_marked TERM
        sleep 1
        nested_kill_marked KILL
        [[ $RUNTIME == /tmp/murasaki-h.* ]] && rm -rf -- "$RUNTIME"
        case $rc in
        0) echo "   $name: ok" ;;
        77) echo "   $name: skipped" ;;
        *) echo "   $name: FAILED ($rc), logs in $OUT_DIR/$name"; status=1 ;;
        esac
    done
    exit $status
fi

# ---- inner half: runs inside the private bus --------------------------------------------------------------
NAME=$ONE
LOG_DIR=$OUT_DIR/$NAME
mkdir -p "$LOG_DIR"
trap 'nested_kill_marked TERM; sleep 1; nested_kill_marked KILL' EXIT
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"

# A fake Steam library with one web wallpaper: a solid colour that a screenshot can recognise.
COLOR_HEX=c01020
ITEM_DIR=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/431960/777
mkdir -p "$ITEM_DIR"
cat >"$ITEM_DIR/project.json" <<'JSON'
{"type": "web", "file": "index.html", "title": "Nested test colour"}
JSON
cat >"$ITEM_DIR/index.html" <<HTML
<!doctype html><html><body style="margin:0;background:#$COLOR_HEX"></body></html>
HTML

# ---- the compositor, headless --------------------------------------------------------------------------------
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 AQ_BACKENDS=headless
case $NAME in
sway)
    mkdir -p "$XDG_CONFIG_HOME/sway"
    echo 'output * resolution 1280x720' >"$XDG_CONFIG_HOME/sway/config"
    export XDG_CURRENT_DESKTOP=sway
    sway -c "$XDG_CONFIG_HOME/sway/config" >"$LOG_DIR/compositor.log" 2>&1 &
    for _ in $(seq 100); do
        SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
        [[ -n $SWAYSOCK ]] && break
        sleep 0.1
    done
    export SWAYSOCK
    ;;
labwc)
    mkdir -p "$XDG_CONFIG_HOME/labwc"
    : >"$XDG_CONFIG_HOME/labwc/rc.xml"
    export XDG_CURRENT_DESKTOP=labwc:wlroots
    labwc -C "$XDG_CONFIG_HOME/labwc" >"$LOG_DIR/compositor.log" 2>&1 &
    ;;
wayfire)
    # Wayfire reports windows through wlr-foreign-toplevel only when its plugin of that name is loaded.
    mkdir -p "$XDG_CONFIG_HOME/wayfire"
    printf '[core]\nplugins = foreign-toplevel\n' >"$XDG_CONFIG_HOME/wayfire/wayfire.ini"
    export XDG_CURRENT_DESKTOP=Wayfire
    wayfire -c "$XDG_CONFIG_HOME/wayfire/wayfire.ini" >"$LOG_DIR/compositor.log" 2>&1 &
    ;;
hyprland)
    # Hyprland 0.4x+ runs on aquamarine, which cannot start headless alone and needs xdg_wm_base 6 from a parent
    # compositor (sway 1.11 offers 5): it runs as a window of a virtual KWin with a socket the wayland-* search skips.
    command -v kwin_wayland >/dev/null || { echo "SKIP: Hyprland needs kwin_wayland as its nested host"; exit 77; }
    HOST_SOCKET=hyprhost-$(basename "$MURASAKI_NESTED_TMP")   # nested_kill_marked finds it by this name
    # Its own bus, so that the surface does not see org.kde.KWin and take the session for Plasma.
    KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 dbus-run-session -- kwin_wayland --virtual --socket "$HOST_SOCKET" --width 1920 --height 1080 \
        --no-lockscreen >"$LOG_DIR/host.log" 2>&1 &
    for _ in $(seq 100); do
        [[ -S $XDG_RUNTIME_DIR/$HOST_SOCKET ]] && break
        sleep 0.1
    done
    [[ -S $XDG_RUNTIME_DIR/$HOST_SOCKET ]] || { echo "FAIL: the KWin host did not come up (see $LOG_DIR/host.log)"; exit 1; }
    mkdir -p "$XDG_CONFIG_HOME/hypr"
    printf 'monitor=,1280x720@60,0x0,1\nmisc:disable_hyprland_logo = true\n' >"$XDG_CONFIG_HOME/hypr/hyprland.conf"
    export XDG_CURRENT_DESKTOP=Hyprland
    WAYLAND_DISPLAY="$HOST_SOCKET" Hyprland -c "$XDG_CONFIG_HOME/hypr/hyprland.conf" >"$LOG_DIR/compositor.log" 2>&1 &
    for _ in $(seq 100); do
        HYPRLAND_INSTANCE_SIGNATURE=$(ls "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
        [[ -n $HYPRLAND_INSTANCE_SIGNATURE ]] && break
        sleep 0.1
    done
    export HYPRLAND_INSTANCE_SIGNATURE
    ;;
esac
SOCKET=
for _ in $(seq 100); do
    SOCKET=$(cd "$XDG_RUNTIME_DIR" && ls -d wayland-* 2>/dev/null | grep -v '\.lock$' | head -1)
    [[ -n $SOCKET && -S $XDG_RUNTIME_DIR/$SOCKET ]] && break
    SOCKET=
    sleep 0.1
done
[[ -n $SOCKET ]] || { echo "FAIL: $NAME did not come up (see $LOG_DIR/compositor.log)"; exit 1; }
export WAYLAND_DISPLAY=$SOCKET QT_QPA_PLATFORM=wayland QT_ACCESSIBILITY=0

# ---- Murasaki ---------------------------------------------------------------------------------------------------
"$DAEMON" >"$LOG_DIR/murasakid.log" 2>&1 &
for _ in $(seq 100); do
    "$CTL" list 2>/dev/null | grep -q "we:777" && break
    sleep 0.2
done
"$CTL" list 2>/dev/null | grep -q "we:777" || { echo "FAIL: the private daemon does not see the test item (see $LOG_DIR/murasakid.log)"; exit 1; }
"$CTL" set we:777 >"$LOG_DIR/set.txt" 2>&1 \
    || { echo "FAIL: cannot assign the test item (see $LOG_DIR/set.txt)"; exit 1; }
timeout 10 busctl --user call io.github.uriel2121ger_art.Murasaki /io/github/uriel2121ger_art/Murasaki \
    io.github.uriel2121ger_art.Murasaki1 SetSettings s '{"presenter":"layer-shell"}' >/dev/null 2>&1

SURF=io.github.uriel2121ger_art.Murasaki.Surface
SURF_PATH=/io/github/uriel2121ger_art/Murasaki/Surface
SURF_IFACE=io.github.uriel2121ger_art.Murasaki.Surface1
surface_outputs() {
    timeout 5 busctl --user --json=short call "$SURF" "$SURF_PATH" "$SURF_IFACE" Outputs 2>/dev/null | sed -e 's/\\"/"/g'
}

"$SURFACE" >"$LOG_DIR/surface.log" 2>&1 &
for _ in $(seq 150); do
    surface_outputs | grep -q '"name"' && break
    sleep 0.2
done
surface_outputs | grep -q '"name"' || { echo "FAIL: the surface lists no output (see $LOG_DIR/surface.log)"; exit 1; }

status=0
check() { # check "description" command...
    local what=$1
    shift
    if "$@"; then echo "   ok: $what"; else echo "   FAIL: $what"; status=1; fi
}

# 1. The profile line.
case $NAME in
sway) want="Sway" ;;
labwc | wayfire) want="wlroots-style" ;;
hyprland) want="Hyprland" ;;
esac
check "profile recognised ($want, background layer)" grep -q "$want.*layer background" "$LOG_DIR/surface.log"

# 2. The wallpaper is drawn.
OUTPUT=$(surface_outputs | grep -o '"name":"[^"]*"' | head -1 | cut -d'"' -f4)
pixel_is_color() {
    python3 - "$1" "$2" <<'PY'
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
want = tuple(int(sys.argv[2][i:i + 2], 16) for i in (0, 2, 4))
got = im.getpixel((im.width // 2, im.height // 2))
sys.exit(0 if all(abs(a - b) <= 12 for a, b in zip(got, want)) else 1)
PY
}
shot_has_wallpaper() {
    for _ in $(seq 30); do
        grim -o "$OUTPUT" "$LOG_DIR/wallpaper.png" 2>>"$LOG_DIR/grim.log" && pixel_is_color "$LOG_DIR/wallpaper.png" "$COLOR_HEX" && return 0
        sleep 0.5
    done
    return 1
}
check "the wallpaper is on screen ($COLOR_HEX on $OUTPUT)" shot_has_wallpaper

# 3. A fullscreen window pauses it, closing the window resumes it.
if [[ -x $BACKDROP ]]; then
    "$BACKDROP" >"$LOG_DIR/backdrop.log" 2>&1 &
    BACKDROP_PID=$!
    occluded() { surface_outputs | grep -q '"occluded":true'; }
    wait_for() { for _ in $(seq 40); do "$@" && return 0; sleep 0.25; done; return 1; }
    check "a fullscreen window occludes the output" wait_for occluded
    kill "$BACKDROP_PID" 2>/dev/null
    uncovered() { ! occluded; }
    check "closing it uncovers the output" wait_for uncovered
else
    echo "   skip: $BACKDROP is not built (BUILD_TESTING), no occlusion check"
fi
exit $status
