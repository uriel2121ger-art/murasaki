#!/usr/bin/env bash
# Integration test of murasaki-overlay (the desktop pets) in a fully isolated nested session.
#
# What it starts, all private: a D-Bus session (dbus-run-session with a config that can activate only the files
# generated here), kwin_wayland --virtual, a fullscreen window of colored stripes (the backdrop), a murasakid from
# the build directory with a throw-away config and a generated pet (an APNG with alpha in a Steam-like directory),
# and murasaki-overlay, which D-Bus activation starts. It checks:
#   - a pet window exists on the overlay layer and the pet is drawn with real transparency: the stripes of the
#     backdrop show through its transparent pixels, nothing leaks outside its rectangle, and it animates;
#   - pets are above a fullscreen window;
#   - arrange mode (D-Bus, the Overlay ToggleArrange call and the global shortcut through a real kglobalacceld)
#     switches input on and off and draws the outline;
#   - UpdatePet moves and resizes the window, and an edit made by the window (SimulateEdit, a test hook) reaches
#     the daemon through UpdatePet and comes back;
#   - a finished transcode (VP9 with alpha) keeps the transparency;
#   - the lock screen signal pauses the pets;
#   - removing the pet removes the window;
#   - the pointer behaviours, with pointer motion injected into the nested KWin (murasaki-pointer-fakeinput): a click-through pet lets the
#     pointer through, a pet that takes input is dragged by its opaque pixels and not by its transparent ones, a locked one stays, hide
#     under the cursor and focus fade follow the cursor, smart dodge steps away from it (the daemon hears about each step).
#
# The user's session is never touched: the runtime dir, HOME, the XDG dirs and the bus are private, the bus can
# not activate anything installed on the system (a normal session bus searches /usr/share/dbus-1/services whatever
# XDG_DATA_DIRS says), no network is used, and every process this script starts carries MURASAKI_NESTED_MARK,
# which is also how the leftovers are killed at the end. Nothing is installed.
#
#   tools/test-overlay-nested.sh [--build-dir DIR] [--out-dir DIR] [--compositor kwin|sway|hyprland]
#
# The compositor is kwin_wayland --virtual by default. sway (headless) and Hyprland (a window of a virtual KWin: aquamarine
# cannot start headless alone) run the same checks through the same overlay; only what the compositor itself offers differs
# (screenshots with grim, pointer injection with zwlr_virtual_pointer_v1, no kwin script or KWin support information), and
# the level the pet reads the pointer at (global on KWin and Hyprland, local on sway).
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when the environment cannot run it. Needs
# MURASAKI_NESTED_TESTS=1, like the other nested tests.
set -u

SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")

if [[ ${1:-} == --inner ]]; then
    shift
    INNER=1
else
    INNER=0
fi

BUILD_DIR=$REPO/build/dev
OUT_DIR=${TMPDIR:-/tmp}/murasaki-overlay-nested-out
COMP=kwin
while (($#)); do
    case $1 in
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --out-dir) OUT_DIR=$2; shift 2 ;;
    --compositor) COMP=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
BUILD_DIR=$(readlink -f "$BUILD_DIR")
mkdir -p "$OUT_DIR"
OUT_DIR=$(readlink -f "$OUT_DIR")

# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"
kill_marked() { nested_kill_marked "$@"; }

if ((!INNER)); then
    # ---- outer half: private runtime dir, HOME and bus ------------------------------------------------------
    if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the nested overlay test"
        exit 77
    fi
    case $COMP in
    kwin) tools="kwin_wayland spectacle" ;;
    sway) tools="sway grim" ;;
    hyprland) tools="Hyprland kwin_wayland grim" ;;   # (the virtual KWin is Hyprland's host)
    *) echo "unknown compositor: $COMP" >&2; exit 2 ;;
    esac
    for tool in dbus-run-session ffmpeg python3 busctl $tools; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    python3 -c 'import PIL' 2>/dev/null || { echo "SKIP: python3 PIL not found"; exit 77; }
    for bin in src/daemon/murasakid src/ctl/murasakictl src/overlay/murasaki-overlay tools/overlay-backdrop/murasaki-overlay-backdrop; do
        [[ -x $BUILD_DIR/$bin ]] || { echo "SKIP: $BUILD_DIR/$bin is not built"; exit 77; }
    done
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    TMP_ROOT=$(mktemp -d /tmp/mon.XXXXXX)   # short: it holds the Wayland socket
    mkdir -m 700 "$TMP_ROOT/rt"
    export MURASAKI_NESTED_TMP=$TMP_ROOT
    unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS KDE_SESSION_UID SWAYSOCK HYPRLAND_INSTANCE_SIGNATURE
    unset XDG_SESSION_ID XDG_SEAT XDG_VTNR QT_QPA_PLATFORMTHEME
    export HOME=$TMP_ROOT/home
    export XDG_RUNTIME_DIR=$TMP_ROOT/rt XDG_CONFIG_HOME=$TMP_ROOT/config XDG_DATA_HOME=$TMP_ROOT/data
    export XDG_CACHE_HOME=$TMP_ROOT/cache XDG_STATE_HOME=$TMP_ROOT/state
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
    export XDG_SESSION_TYPE=wayland
    case $COMP in
    kwin) export XDG_CURRENT_DESKTOP=KDE KDE_FULL_SESSION=true ;;
    sway) export XDG_CURRENT_DESKTOP=sway ;;
    hyprland) export XDG_CURRENT_DESKTOP=Hyprland ;;
    esac
    # What D-Bus-activated processes (the overlay, kglobalacceld) inherit: the bus daemon's own environment. sway and Hyprland
    # pick their socket name themselves; the inner half hands it (and their IPC sockets) to the bus once they are up.
    export WAYLAND_DISPLAY=wayland-$(basename "$TMP_ROOT") QT_QPA_PLATFORM=wayland QT_ACCESSIBILITY=0
    export MURASAKI_STEAM_ROOT=$TMP_ROOT/steam MURASAKI_WORKSHOP_API_URL=http://127.0.0.1:9/
    export MURASAKI_OVERLAY_UNIT=0 MURASAKI_OVERLAY_TEST_HOOKS=1
    # The pets' sound: the player renders on a timer and throws the samples away, and logs where each sound started (no audio device is touched).
    export MURASAKI_AUDIO_BACKEND=paced MURASAKI_AUDIO_LOG=$TMP_ROOT/audio.log MURASAKI_PETAUDIO=$BUILD_DIR/src/audio/murasaki-petaudio
    : >"$TMP_ROOT/audio.log" # (it exists before the confined player starts: the sandbox lets it write this one file)
    mkdir -p "$OUT_DIR/logs"

    # The only services this bus can start: the overlay of this build, and kglobalacceld if installed (the
    # overlay's fallback shortcut backend talks to it; its config goes to the private XDG_CONFIG_HOME).
    mkdir -p "$TMP_ROOT/services"
    cat >"$TMP_ROOT/services/io.github.uriel2121ger_art.Murasaki.Overlay.service" <<SERVICE
[D-BUS Service]
Name=io.github.uriel2121ger_art.Murasaki.Overlay
Exec=$TMP_ROOT/services/run-overlay.sh
SERVICE
    # The activated process inherits the bus daemon's streams; its output belongs in a log of its own.
    cat >"$TMP_ROOT/services/run-overlay.sh" <<RUN
#!/bin/sh
exec "$BUILD_DIR/src/overlay/murasaki-overlay" >>"$OUT_DIR/logs/overlay.log" 2>&1
RUN
    chmod +x "$TMP_ROOT/services/run-overlay.sh"
    KGA=/usr/lib/x86_64-linux-gnu/libexec/kglobalacceld
    if [[ -x $KGA ]]; then
        cat >"$TMP_ROOT/services/org.kde.kglobalaccel.service" <<SERVICE
[D-BUS Service]
Name=org.kde.kglobalaccel
Exec=$KGA
SERVICE
    fi
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
        kill_marked TERM
        sleep 1
        kill_marked KILL
        rm -rf "$TMP_ROOT"
    }
    trap 'outer_cleanup; exit 130' INT TERM
    trap outer_cleanup EXIT
    timeout "${MURASAKI_NESTED_TIMEOUT:-640}" dbus-run-session --config-file="$TMP_ROOT/bus.conf" -- "$SELF" --inner \
        --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR" --compositor "$COMP" </dev/null 2>"$OUT_DIR/logs/session.log"
    exit $?
fi

# ---- inner half: runs inside the private bus ------------------------------------------------------------------
LOG_DIR=$OUT_DIR/logs
mkdir -p "$LOG_DIR"
trap 'kill_marked TERM; sleep 1; kill_marked KILL' EXIT
echo "private bus: $DBUS_SESSION_BUS_ADDRESS"
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
[[ $HOME == "$MURASAKI_NESTED_TMP"/* ]] || { echo "FAIL: HOME is not private, refusing to continue"; exit 1; }

failed=0
fail() { echo "   FAIL: $*"; failed=1; }

MD=io.github.uriel2121ger_art.Murasaki
MD_PATH=/io/github/uriel2121ger_art/Murasaki
MD_IF=io.github.uriel2121ger_art.Murasaki1
OV=io.github.uriel2121ger_art.Murasaki.Overlay
OV_PATH=/io/github/uriel2121ger_art/Murasaki/Overlay
OV_IF=io.github.uriel2121ger_art.Murasaki.Overlay1
CTL=$BUILD_DIR/src/ctl/murasakictl

jcall() { # busctl call returning the first string argument as plain text
    timeout 30 busctl --user --json=short call "$@" 2>>"$LOG_DIR/busctl.err" \
        | python3 -c 'import sys, json; print(json.load(sys.stdin)["data"][0])'
}
windows() { jcall $OV $OV_PATH $OV_IF Windows; }
jq_py() { # EXPR: evaluates a Python expression on `w` (the JSON read from stdin); a tuple prints space separated
    python3 -c 'import sys, json; w = json.load(sys.stdin); r = eval(sys.argv[1]); print(" ".join(map(str, r)) if isinstance(r, tuple) else r)' "$1" 2>/dev/null
}
arrange_state() {
    timeout 10 busctl --user get-property $MD $MD_PATH $MD_IF PetsArrange 2>/dev/null | awk '{print $2}'
}
wait_for() { # SECONDS DESCRIPTION COMMAND...: polls until the command succeeds
    local limit=$1 what=$2 deadline
    shift 2
    deadline=$((SECONDS + limit))
    while ((SECONDS < deadline)); do
        "$@" >/dev/null 2>&1 && return 0
        sleep 0.3
    done
    fail "timed out waiting for: $what"
    return 1
}

# ---- the nested compositor and the backdrop ----------------------------------------------------------------
SOCKET=$WAYLAND_DISPLAY
OUTPUT=Virtual-0
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 AQ_BACKENDS=headless
case $COMP in
kwin)
    KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 kwin_wayland --virtual --socket "$SOCKET" --width 1280 --height 720 ${MURASAKI_NESTED_OUTPUTS:+--output-count "$MURASAKI_NESTED_OUTPUTS"} \
        --no-lockscreen --no-global-shortcuts >"$LOG_DIR/kwin.log" 2>&1 &
    ;;
sway)
    mkdir -p "$XDG_CONFIG_HOME/sway"
    echo "output * resolution 1280x720" >"$XDG_CONFIG_HOME/sway/config"
    sway -c "$XDG_CONFIG_HOME/sway/config" >"$LOG_DIR/kwin.log" 2>&1 &
    for _ in $(seq 100); do
        SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
        SOCKET=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
        [[ -n $SWAYSOCK && -n $SOCKET ]] && break
        sleep 0.1
    done
    export SWAYSOCK
    ;;
hyprland)
    # On a bus of its own, so that nothing here sees org.kde.KWin; the host socket is not a wayland-N name.
    HOST_SOCKET=hyprhost-$(basename "$MURASAKI_NESTED_TMP")   # nested_kill_marked finds it by this name
    KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 dbus-run-session -- kwin_wayland --virtual --socket "$HOST_SOCKET" --width 1920 \
        --height 1080 --no-lockscreen >"$LOG_DIR/host-hyprland.log" 2>&1 &
    for _ in $(seq 100); do
        [[ -S $XDG_RUNTIME_DIR/$HOST_SOCKET ]] && break
        sleep 0.1
    done
    [[ -S $XDG_RUNTIME_DIR/$HOST_SOCKET ]] || { echo "FAIL: the KWin host did not come up (see $LOG_DIR/host-hyprland.log)"; exit 1; }
    mkdir -p "$XDG_CONFIG_HOME/hypr"
    # (Hyprland's own animations stay on, as a user has them: the grown window of a drag slides to its place, PetInteraction F13)
    printf 'monitor=,1280x720@60,0x0,1\nmisc:disable_hyprland_logo = true\n' >"$XDG_CONFIG_HOME/hypr/hyprland.conf"
    WAYLAND_DISPLAY="$HOST_SOCKET" Hyprland -c "$XDG_CONFIG_HOME/hypr/hyprland.conf" >"$LOG_DIR/kwin.log" 2>&1 &
    for _ in $(seq 100); do
        SOCKET=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
        [[ -n $SOCKET ]] && break
        sleep 0.1
    done
    # (its IPC socket comes after the Wayland one)
    for _ in $(seq 100); do
        HYPRLAND_INSTANCE_SIGNATURE=$(ls "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
        [[ -n $HYPRLAND_INSTANCE_SIGNATURE && -S $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE/.socket.sock ]] && break
        sleep 0.1
    done
    export HYPRLAND_INSTANCE_SIGNATURE
    ;;
esac
for _ in $(seq 100); do
    [[ -n $SOCKET && -S $XDG_RUNTIME_DIR/$SOCKET ]] && break
    sleep 0.1
done
[[ -n $SOCKET && -S $XDG_RUNTIME_DIR/$SOCKET ]] || { echo "FAIL: $COMP did not come up (see $LOG_DIR/kwin.log)"; exit 1; }
export WAYLAND_DISPLAY=$SOCKET
if [[ $COMP != kwin ]]; then
    # The overlay is started by D-Bus activation: it gets the bus daemon's environment, which has to learn the sockets.
    timeout 10 busctl --user call org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus UpdateActivationEnvironment 'a{ss}' 3 \
        WAYLAND_DISPLAY "$SOCKET" SWAYSOCK "${SWAYSOCK:-}" HYPRLAND_INSTANCE_SIGNATURE "${HYPRLAND_INSTANCE_SIGNATURE:-}" \
        >>"$LOG_DIR/busctl.err" 2>&1 || { echo "FAIL: cannot hand the sockets to the bus"; exit 1; }
    case $COMP in
    sway) OUTPUT=$(swaymsg -t get_outputs -r | python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["name"])') ;;
    hyprland)
        for _ in $(seq 50); do
            OUTPUT=$(hyprctl -j monitors 2>/dev/null | python3 -c 'import sys, json; print(json.load(sys.stdin)[0]["name"])' 2>/dev/null)
            [[ -n $OUTPUT ]] && break
            sleep 0.2
        done
        ;;
    esac
    [[ -n $OUTPUT ]] || { echo "FAIL: $COMP names no output"; exit 1; }
fi
echo "compositor $COMP up, output $OUTPUT"

# One pointer injector for the whole run (a new connection adds an input device and makes Qt drop its pointer). It starts before
# any client: on sway and Hyprland its virtual pointer is the seat's only pointer.
FAKEINPUT=$BUILD_DIR/tools/overlay-pointer-probe/murasaki-pointer-fakeinput
IFIFO=$MURASAKI_NESTED_TMP/inject.fifo
ILOG=$LOG_DIR/inject.log
if [[ -x $FAKEINPUT ]]; then
    mkfifo "$IFIFO"
    : >"$ILOG"
    "$FAKEINPUT" --extent 1280x720 --stdin <>"$IFIFO" >>"$ILOG" 2>&1 &
    sleep 1
fi

"$BUILD_DIR/tools/overlay-backdrop/murasaki-overlay-backdrop" >"$LOG_DIR/backdrop.log" 2>&1 &

shot() { # NAME: spectacle does not overwrite a file that exists
    rm -f "$OUT_DIR/$1.png"
    if [[ $COMP == kwin ]]; then
        timeout 60 spectacle -b -n -f -o "$OUT_DIR/$1.png" >>"$LOG_DIR/spectacle.log" 2>&1
    else
        timeout 60 grim -o "$OUTPUT" "$OUT_DIR/$1.png" >>"$LOG_DIR/spectacle.log" 2>&1
    fi
    [[ -s $OUT_DIR/$1.png ]] || { fail "no screenshot ($1)"; return 1; }
}
backdrop_visible() { # the first screenshot of a fresh session is black
    python3 - "$OUT_DIR/$1.png" <<'PY'
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB")
# every corner of the first stripe: a window still being mapped (scaled in, offset by a pixel) fails this
w, h = img.size
red = lambda p: p[0] > 150 and p[1] < 100 and p[2] < 100
sys.exit(0 if all(red(img.getpixel(p)) for p in ((0, 0), (2, 2), (0, h - 1), (39, h // 2))) else 1)
PY
}
backdrop_settled() { # two screenshots in a row that agree: the backdrop is not animating any more
    python3 - "$OUT_DIR/backdrop.png" "$OUT_DIR/backdrop-check.png" <<'PY'
import sys
from PIL import Image, ImageChops
a, b = (Image.open(p).convert("RGB") for p in sys.argv[1:3])
sys.exit(0 if a.size == b.size and ImageChops.difference(a, b).getbbox() is None else 1)
PY
}
for _ in $(seq 40); do
    if shot backdrop && backdrop_visible backdrop; then
        sleep 0.5
        shot backdrop-check && backdrop_settled && break
    fi
    sleep 0.5
done
rm -f "$OUT_DIR/backdrop-check.png"
backdrop_visible backdrop || { echo "FAIL: the backdrop never painted (see backdrop.png, $LOG_DIR/kwin.log)"; exit 1; }
echo "backdrop up"

# ---- a generated pet in a Steam-like library ------------------------------------------------------------------
PET_DIR=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/3474900/9000001
mkdir -p "$PET_DIR"
# 320x200, 15 fps, 2 s; an orange disc moving over a transparent background. Nothing is copied from Workshop.
timeout 120 ffmpeg -y -v error -f lavfi \
    -i "color=c=black:s=320x200:r=15:d=2,format=gbrap,geq=r='255':g='160+60*sin(X/20)':b='40':a='if(lt(hypot(X-160-80*sin(N*0.4),Y-100),50),255,0)',format=rgba" \
    -plays 0 -f apng "$PET_DIR/disc.apng" 2>"$LOG_DIR/ffmpeg.log" || { echo "FAIL: ffmpeg cannot generate the pet (see $LOG_DIR/ffmpeg.log)"; exit 1; }
# A second item for the pointer checks: the same disc, standing still (every frame the same), so that a point is on it or off it for good.
PET_DIR2=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/3474900/9000002
mkdir -p "$PET_DIR2"
timeout 120 ffmpeg -y -v error -f lavfi \
    -i "color=c=black:s=320x200:r=15:d=2,format=gbrap,geq=r='255':g='160+60*sin(X/20)':b='40':a='if(lt(hypot(X-160,Y-100),50),255,0)',format=rgba" \
    -plays 0 -f apng "$PET_DIR2/still.apng" 2>>"$LOG_DIR/ffmpeg.log" || { echo "FAIL: ffmpeg cannot generate the second pet (see $LOG_DIR/ffmpeg.log)"; exit 1; }
# A third item for the rotated pets: the disc stands on the left of the picture (centre (80, 100), radius 40), so that turning or flipping the
# pet moves it somewhere else.
PET_DIR3=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/3474900/9000003
mkdir -p "$PET_DIR3"
timeout 120 ffmpeg -y -v error -f lavfi \
    -i "color=c=black:s=320x200:r=15:d=2,format=gbrap,geq=r='255':g='160+60*sin(X/20)':b='40':a='if(lt(hypot(X-80,Y-100),40),255,0)',format=rgba" \
    -plays 0 -f apng "$PET_DIR3/left.apng" 2>>"$LOG_DIR/ffmpeg.log" || { echo "FAIL: ffmpeg cannot generate the third pet (see $LOG_DIR/ffmpeg.log)"; exit 1; }

# A fourth item for the states: the same still disc in three colours, one file each (rest: orange, held: blue, dropped: green).
PET_DIR4=$MURASAKI_STEAM_ROOT/steamapps/workshop/content/3474900/9000004
mkdir -p "$PET_DIR4"
for spec in "rest 255 140 40" "held 40 120 255" "dropped 40 220 80"; do
    read -r name cr cg cb <<<"$spec"
    timeout 120 ffmpeg -y -v error -f lavfi \
        -i "color=c=black:s=320x200:r=15:d=2,format=gbrap,geq=r='$cr':g='$cg':b='$cb':a='if(lt(hypot(X-160,Y-100),50),255,0)',format=rgba" \
        -plays 0 -f apng "$PET_DIR4/$name.apng" 2>>"$LOG_DIR/ffmpeg.log" || { echo "FAIL: ffmpeg cannot generate the states pet (see $LOG_DIR/ffmpeg.log)"; exit 1; }
done

"$BUILD_DIR/src/daemon/murasakid" >"$LOG_DIR/murasakid.log" 2>&1 &
wait_for 30 "the daemon to list the pet" bash -c "'$CTL' list 2>/dev/null | grep -q '^anima:9000001'" || exit 1
ITEM=anima:9000001
echo "daemon up, pet item $ITEM"

# ---- the pet ---------------------------------------------------------------------------------------------------
PX=200 PY=150 PW=256            # position and displayed width; the height is 160 (320x200 native)
PH=160
timeout 30 "$CTL" pets add "$ITEM" --variant disc --output "$OUTPUT" --x "$PX" --y "$PY" --width "$PW" \
    >"$LOG_DIR/pets-add.txt" 2>&1 || { echo "FAIL: pets add (see $LOG_DIR/pets-add.txt)"; exit 1; }
PET_ID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add.txt" | head -1)
[[ -n $PET_ID ]] || { echo "FAIL: no pet id (pets list)"; exit 1; }
echo "pet $PET_ID added"

# The first call activates the overlay through D-Bus activation.
wait_for 40 "the overlay to show the pet" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"shown\"]' | grep -qx True" || { tail -20 "$LOG_DIR/busctl.err"; exit 1; }
echo "overlay activated, pet shown"
W=$(windows)
echo "windows: $W" >"$LOG_DIR/windows-initial.txt"
[[ $(echo "$W" | jq_py 'w[0]["interactive"]') == False ]] || fail "the pet takes input outside the arrange mode"
[[ $(echo "$W" | jq_py 'round(w[0]["width"]), round(w[0]["height"])') == "256 160" ]] || fail "the window is not 256x160: $W"

# Layer: the compositor knows a layer-shell window of the pet scope.
if [[ $COMP == kwin ]]; then
    timeout 20 busctl --user call org.kde.KWin /KWin org.kde.KWin supportInformation >"$LOG_DIR/kwin-support.txt" 2>&1
    grep -q "murasaki-pet" "$LOG_DIR/kwin-support.txt" || echo "   note: kwin's support information does not name the pet scope (not a failure)"
fi

# rect_stats BASE SHOT X Y W H: share of the rect that differs from the backdrop, share that is the backdrop, and
# the share of pixels outside the rect (4 px margin) that differ.
rect_stats() {
    python3 - "$@" <<'PY'
import sys
from PIL import Image, ImageChops
base = Image.open(sys.argv[1]).convert("RGB")
shot = Image.open(sys.argv[2]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[3:7])
diff = ImageChops.difference(base, shot).convert("L").point(lambda v: 255 if v > 24 else 0)
def share(img, box):
    c = img.crop(box)
    px = c.getdata()
    return sum(1 for p in px if p) / max(1, len(px))
inside = share(diff, (x, y, x + w, y + h))
print("changed=%.3f" % inside)
print("same=%.3f" % (1 - inside))
# Everything outside the rectangle, minus a margin: mask the rectangle out.
mask = Image.new("L", diff.size, 255)
mask.paste(0, (max(0, x - 4), max(0, y - 4), x + w + 4, y + h + 4))
outside = ImageChops.multiply(diff, mask)
px = outside.getdata()
print("outside=%.5f" % (sum(1 for p in px if p) / len(px)))
PY
}
field() { sed -n "s/^$1=//p"; }
num_between() { awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }'; }

check_pet_drawn() { # NAME X Y W H: the pet is drawn transparently at the rect
    local name=$1 stats changed same outside
    shot "$name" || return 1
    stats=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/$name.png" "$2" "$3" "$4" "$5")
    changed=$(echo "$stats" | field changed); same=$(echo "$stats" | field same); outside=$(echo "$stats" | field outside)
    echo "   $name: changed=$changed same=$same outside=$outside"
    num_between "$changed" 0.03 0.45 || fail "$name: the pet is not drawn in its rectangle (changed=$changed)"
    num_between "$same" 0.55 1 || fail "$name: the pet is not transparent, the backdrop does not show through (same=$same)"
    num_between "$outside" 0 0.001 || fail "$name: something changed outside the pet rectangle (outside=$outside)"
}

if [[ ${MURASAKI_NESTED_OUTPUTS:-1} -ge 2 ]]; then
    # Two outputs side by side (1280x720 each, 2560x720 together): a `desktop` stage is one picture across both. This mode checks that and nothing else.
    echo "== a desktop stage across two outputs"
    timeout 10 "$CTL" pets stage "$PET_ID" desktop >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets stage desktop"
    wait_for 15 "two windows for the desktop stage" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'len(w[0].get(\"stageOutputs\", []))' | grep -qx 2"
    sleep 2
    W=$(windows)
    echo "   stage window: $W" >"$LOG_DIR/windows-desktop.txt"
    # 320x200 contained in 2560x720 is 1152x720, centred: x from 704 to 1856, across the bezel at 1280.
    # The disc moves (about 290 px either side of the middle of the picture, where the bezel is): over a few screenshots each half of the picture shows it.
    for n in 1 2 3 4 5 6; do shot desktop-stage-$n; sleep 0.25; done
    cp "$OUT_DIR/desktop-stage-1.png" "$OUT_DIR/desktop-stage.png"
    for part in "704 0 576 720:left" "1280 0 576 720:right"; do
        rect=${part%%:*}
        python3 - "$OUT_DIR" ${rect//:/ } <<'PY' || fail "the ${part##*:} half of the desktop stage never shows anything"
import sys
from PIL import Image, ImageChops
out = sys.argv[1]
x, y, w, h = (int(v) for v in sys.argv[2:6])
base = Image.open(out + "/backdrop.png").convert("RGB").crop((x, y, x + w, y + h))
best = 0.0
for n in range(1, 7):
    shot = Image.open("%s/desktop-stage-%d.png" % (out, n)).convert("RGB").crop((x, y, x + w, y + h))
    d = ImageChops.difference(base, shot).convert("L")
    best = max(best, sum(1 for p in d.getdata() if p > 24) / (w * h))
print("   most changed share of the half over six screenshots: %.3f" % best)
sys.exit(0 if best > 0.01 else 1)
PY
    done
    stats=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/desktop-stage.png" 704 0 1152 720)
    num_between "$(echo "$stats" | field outside)" 0 0.001 || fail "something changed outside the desktop stage's picture (outside=$(echo "$stats" | field outside))"
    [[ $(echo "$W" | jq_py 'w[0]["interactive"]') == False ]] || fail "a desktop stage takes input"
    ((failed)) && { echo "RESULT: FAIL (screenshots in $OUT_DIR)"; exit 1; }
    echo "RESULT: PASS (screenshots in $OUT_DIR)"
    exit 0
fi
echo "== transparency over a fullscreen window"
sleep 1
check_pet_drawn pet-idle "$PX" "$PY" "$PW" "$PH"
# The disc moves with the frame, and two screenshots may land on the same phase (or on a loaded machine on neighbouring ones): the best of up to six pairs
# against the first one is what counts, with the threshold unchanged.
d=0
for n in 2 3 4 5 6 7; do
    shot "pet-idle$n" || continue
    d=$(python3 - "$OUT_DIR/pet-idle.png" "$OUT_DIR/pet-idle$n.png" "$PX" "$PY" "$PW" "$PH" "$d" <<'PY'
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[3:7])
d = ImageChops.difference(a.crop((x, y, x + w, y + h)), b.crop((x, y, x + w, y + h))).convert("L")
print("%.4f" % max(float(sys.argv[7]), sum(1 for p in d.getdata() if p > 24) / (w * h)))
PY
)
    num_between "$d" 0.004 1 && break
    sleep 0.4
done
echo "   animation: $d of the pet rectangle changed between the first screenshot and the best of up to six others"
num_between "$d" 0.004 1 || fail "the pet does not animate"

echo "== arrange mode"
timeout 10 "$CTL" pets arrange on >/dev/null 2>&1 || fail "pets arrange on"
wait_for 10 "the pet window to take input" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"interactive\"]' | grep -qx True"
sleep 1
shot pet-arrange && {
    outline=$(python3 - "$OUT_DIR/pet-arrange.png" "$PX" "$PY" "$PW" "$PH" <<'PY'
import sys
from PIL import Image
img = Image.open(sys.argv[1]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[2:6])
# The outline is a 2 px violet border (#7c4dff, mostly opaque) along the top edge, inside the window.
hits = sum(1 for i in range(x + 30, x + w - 30) if (lambda p: p[2] > 200 and p[0] < 200 and p[1] < 160)(img.getpixel((i, y + 1))))
print("%.3f" % (hits / (w - 60)))
PY
)
    echo "   outline along the top edge: $outline"
    num_between "$outline" 0.6 1 || fail "the arrange outline is not drawn"
    # The window covers the whole output while arranging; nothing may be drawn outside the pet.
    outside=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/pet-arrange.png" "$PX" "$PY" "$PW" "$PH" | field outside)
    echo "   outside the pet while arranging: $outside"
    num_between "$outside" 0 0.001 || fail "the arrange mode draws outside the pet (outside=$outside)"
}
timeout 10 "$CTL" pets arrange off >/dev/null 2>&1
wait_for 10 "the pet window to let input through" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"interactive\"]' | grep -qx False"

# ToggleArrange (the call behind the GUI toggle when there is no shortcut) flips the daemon's state.
timeout 10 busctl --user call $OV $OV_PATH $OV_IF ToggleArrange >>"$LOG_DIR/busctl.err" 2>&1
wait_for 10 "ToggleArrange to switch the arrange mode on" bash -c "[[ \$(timeout 10 busctl --user get-property $MD $MD_PATH $MD_IF PetsArrange 2>/dev/null | awk '{print \$2}') == true ]]"
timeout 10 busctl --user call $OV $OV_PATH $OV_IF ToggleArrange >>"$LOG_DIR/busctl.err" 2>&1
wait_for 10 "ToggleArrange to switch it off" bash -c "[[ \$(timeout 10 busctl --user get-property $MD $MD_PATH $MD_IF PetsArrange 2>/dev/null | awk '{print \$2}') == false ]]"

echo "== global shortcut"
backend=$(timeout 10 busctl --user get-property $OV $OV_PATH $OV_IF ShortcutBackend 2>/dev/null | sed 's/^s "\(.*\)"$/\1/')
echo "   backend: ${backend:-none}"
if [[ $COMP != kwin ]]; then
    echo "   note: kglobalaccel serves KWin only, and the nested session has no portal: no shortcut on $COMP (the overlay says so and goes on)"
elif [[ -x /usr/lib/x86_64-linux-gnu/libexec/kglobalacceld ]]; then
    [[ $backend == kglobalaccel ]] || fail "the shortcut was not bound through kglobalaccel (backend=${backend:-none}; see $LOG_DIR/overlay.log)"
    if [[ $backend == kglobalaccel ]]; then
        # What a key press does in kglobalacceld: its component emits globalShortcutPressed.
        timeout 10 busctl --user call org.kde.kglobalaccel /component/murasaki org.kde.kglobalaccel.Component \
            invokeShortcut s toggle-arrange >>"$LOG_DIR/shortcut.log" 2>&1 || fail "invokeShortcut failed (see $LOG_DIR/shortcut.log)"
        wait_for 10 "the shortcut to switch the arrange mode on" bash -c "[[ \$(timeout 10 busctl --user get-property $MD $MD_PATH $MD_IF PetsArrange 2>/dev/null | awk '{print \$2}') == true ]]"
        timeout 10 busctl --user call org.kde.kglobalaccel /component/murasaki org.kde.kglobalaccel.Component \
            invokeShortcut s toggle-arrange >>"$LOG_DIR/shortcut.log" 2>&1
        wait_for 10 "the shortcut to switch it off again" bash -c "[[ \$(timeout 10 busctl --user get-property $MD $MD_PATH $MD_IF PetsArrange 2>/dev/null | awk '{print \$2}') == false ]]"
    fi
else
    echo "   note: kglobalacceld is not installed, the shortcut was not exercised"
fi

echo "== UpdatePet round trip"
NX=600 NY=300 NW=128 NH=80
timeout 10 busctl --user call $MD $MD_PATH $MD_IF UpdatePet ss "$PET_ID" "{\"x\": $NX, \"y\": $NY, \"width\": $NW}" >>"$LOG_DIR/busctl.err" 2>&1 || fail "UpdatePet failed"
wait_for 10 "the window to move" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py '(round(w[0][\"x\"]), round(w[0][\"width\"]))' | grep -qx '$NX $NW'"
sleep 1
check_pet_drawn pet-moved "$NX" "$NY" "$NW" "$NH"
# The old place is the backdrop again.
python3 - "$OUT_DIR/backdrop.png" "$OUT_DIR/pet-moved.png" "$PX" "$PY" "$PW" "$PH" <<'PY' || fail "the old place still shows the pet"
import sys
from PIL import Image, ImageChops
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
x, y, w, h = (int(v) for v in sys.argv[3:7])
d = ImageChops.difference(a.crop((x, y, x + w, y + h)), b.crop((x, y, x + w, y + h))).convert("L")
sys.exit(0 if sum(1 for p in d.getdata() if p > 24) / (w * h) < 0.001 else 1)
PY

echo "== an edit made by the window reaches the daemon"
timeout 10 busctl --user call $OV $OV_PATH $OV_IF SimulateEdit ssdd "$PET_ID" move 40 25 >>"$LOG_DIR/busctl.err" 2>&1 || fail "SimulateEdit failed"
timeout 10 busctl --user call $OV $OV_PATH $OV_IF SimulateEdit ssdd "$PET_ID" scale 1.5 0 >>"$LOG_DIR/busctl.err" 2>&1
wait_for 10 "the committed edit to reach the daemon" bash -c "'$CTL' pets list 2>/dev/null | grep -E '640|325'"
listing=$("$CTL" pets list 2>/dev/null)
echo "   daemon now: $listing"
echo "$listing" | grep -qE '(^|[^0-9])640([^0-9]|$)' || fail "the moved x (640) did not reach the daemon"
echo "$listing" | grep -qE '(^|[^0-9])325([^0-9]|$)' || fail "the moved y (325) did not reach the daemon"
echo "$listing" | grep -qE '(^|[^0-9])192([^0-9]|$)' || fail "the scaled width (192) did not reach the daemon"
W=$(windows)
[[ $(echo "$W" | jq_py 'round(w[0]["x"]), round(w[0]["y"]), round(w[0]["width"])') == "640 325 192" ]] || fail "the window lost its edit: $W"

echo "== transcode keeps the transparency"
timeout 10 busctl --user call $MD $MD_PATH $MD_IF RequestTranscode ss "$ITEM" disc >>"$LOG_DIR/busctl.err" 2>&1 || fail "RequestTranscode"
wait_for 90 "the transcode to finish and the pet to play it" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"file\"]' | grep -q '\\.webm\$'"
W=$(windows)
echo "   playing: $(echo "$W" | jq_py 'w[0]["file"]')"
sleep 1.5
WX=$(echo "$W" | jq_py 'round(w[0]["x"])'); WY=$(echo "$W" | jq_py 'round(w[0]["y"])')
WW=$(echo "$W" | jq_py 'round(w[0]["width"])'); WH=$(echo "$W" | jq_py 'round(w[0]["height"])')
check_pet_drawn pet-webm "$WX" "$WY" "$WW" "$WH"

echo "== the lock screen pauses the pets"
timeout 10 busctl --user emit /org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver ActiveChanged b true >>"$LOG_DIR/busctl.err" 2>&1
wait_for 10 "the pet to pause when the session locks" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"paused\"]' | grep -qx True"
timeout 10 busctl --user emit /org/freedesktop/ScreenSaver org.freedesktop.ScreenSaver ActiveChanged b false >>"$LOG_DIR/busctl.err" 2>&1
wait_for 10 "the pet to play again after the unlock" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"paused\"]' | grep -qx False"

echo "== the fullscreen mode (stage)"
# The 320x200 pet contained in the 1280x720 output is 1152x720, centred: the stage window covers the output and draws it there. The nested session has no
# murasaki-surface, so the session cannot say what is fullscreen: the stage is shown, and says so.
timeout 10 "$CTL" pets stage "$PET_ID" output >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets stage output"
wait_for 15 "the window to become a stage" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0].get(\"stage\")' | grep -qx output"
sleep 1.5
W=$(windows)
echo "   stage window: $W" >"$LOG_DIR/windows-stage.txt"
[[ $(echo "$W" | jq_py 'round(w[0]["x"]), round(w[0]["y"]), round(w[0]["width"]), round(w[0]["height"])') == "64 0 1152 720" ]] || fail "the stage rectangle is not 64 0 1152 720: $W"
[[ $(echo "$W" | jq_py 'w[0]["interactive"]') == False ]] || fail "a stage takes input"
[[ $(echo "$W" | jq_py '"stage: fullscreen apps unknown" in w[0]["degraded"]') == True ]] || fail "a session that cannot say what is fullscreen did not say so: $W"
[[ $(echo "$W" | jq_py 'w[0]["stageHidden"] == ""') == True ]] || fail "the stage is hidden without a fullscreen window: $W"
check_pet_drawn stage-output 64 0 1152 720
# The band the picture does not cover (64 px on each side) stays the backdrop, and the stage is a layer surface, not a fullscreen window.
if [[ $COMP == kwin ]]; then
    timeout 20 busctl --user call org.kde.KWin /KWin org.kde.KWin supportInformation >"$LOG_DIR/kwin-support-stage.txt" 2>&1
    grep -qi "fullscreen.*murasaki-pet\|murasaki-pet.*fullscreen" "$LOG_DIR/kwin-support-stage.txt" && fail "the stage window is a fullscreen window for the compositor"
fi
# With the policy server running, the backdrop (a real fullscreen window) is seen: the stage hides on that output, nothing is drawn there, and it comes back when the
# policy server goes (the session can not say again, so it is shown).
SF=io.github.uriel2121ger_art.Murasaki.Surface
SF_PATH=/io/github/uriel2121ger_art/Murasaki/Surface
SF_IF=io.github.uriel2121ger_art.Murasaki.Surface1
"$BUILD_DIR/src/surface/murasaki-surface" >"$LOG_DIR/surface.log" 2>&1 &
SURFACE_PID=$!
fullscreen_seen() { jcall $SF $SF_PATH $SF_IF OutputFullscreen | python3 -c 'import sys, json; sys.exit(0 if json.load(sys.stdin)["'$OUTPUT'"]["fullscreen"] else 1)'; }
if wait_for 40 "the surface to see the backdrop as fullscreen" fullscreen_seen; then
    wait_for 15 "the stage to hide over the fullscreen window" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"stageHidden\"]' | grep -qx fullscreen"
    sleep 1
    W=$(windows)
    [[ $(echo "$W" | jq_py 'w[0]["paused"]') == True ]] || fail "the hidden stage is not paused: $W"
    [[ $(echo "$W" | jq_py '"stage: fullscreen apps unknown" in w[0]["degraded"]') == False ]] || fail "the session says it knows and the stage says it does not: $W"
    shot stage-hidden && {
        stats=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/stage-hidden.png" 64 0 1152 720)
        num_between "$(echo "$stats" | field changed)" 0 0.001 || fail "the stage is still drawn over the fullscreen window (changed=$(echo "$stats" | field changed))"
    }
    kill "$SURFACE_PID" 2>/dev/null
    wait_for 15 "the stage to come back when the policy server is gone" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0][\"stageHidden\"] == \"\"' | grep -qx True"
    sleep 1.5
    check_pet_drawn stage-back 64 0 1152 720
else
    kill "$SURFACE_PID" 2>/dev/null
    echo "   note: murasaki-surface did not report the fullscreen window (see $LOG_DIR/surface.log)"
fi
# Back to a pet: it returns to its old place and size.
timeout 10 "$CTL" pets stage "$PET_ID" off >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets stage off"
wait_for 15 "the stage to end" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py '\"stage\" in w[0]' | grep -qx False"
sleep 1.5
W=$(windows)
[[ $(echo "$W" | jq_py 'round(w[0]["x"]), round(w[0]["y"]), round(w[0]["width"])') == "$WX $WY $WW" ]] || fail "the pet did not come back to its place: $W"
check_pet_drawn stage-off "$WX" "$WY" "$WW" "$WH"

echo "== automation: a real timeline hides, moves and restores the pet; a trigger, a profile and the rescue"
pw() { windows | jq_py "w[0]$1"; } # a field of the first window
TL_DIR=$MURASAKI_NESTED_TMP/automation
mkdir -p "$TL_DIR"
cat >"$TL_DIR/nap.json" <<JSON
{"restore": true, "tracks": [{"pet": {"petId": "$PET_ID"}, "startVisible": true, "actions": [{"kind": "hide"}, {"kind": "wait", "ms": 2500}]}]}
JSON
cat >"$TL_DIR/walk.json" <<JSON
{"restore": false, "tracks": [{"pet": {"petId": "$PET_ID"}, "actions": [{"kind": "move", "x": 120, "y": 90, "mode": "smooth", "durationMs": 700}]}]}
JSON
timeout 20 "$CTL" timeline set nap "$TL_DIR/nap.json" >/dev/null 2>&1 || fail "timeline set nap"
timeout 20 "$CTL" timeline set walk "$TL_DIR/walk.json" >/dev/null 2>&1 || fail "timeline set walk"
# a timeline hides the pet in the runtime layer: the window is unmapped, the stored placement is untouched, and it comes back by itself
timeout 20 "$CTL" timeline run nap >/dev/null 2>&1 || fail "timeline run nap"
wait_for 10 "the overlay to report the pet hidden by the timeline" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0].get(\"runtimeHidden\", False)' | grep -qx True"
sleep 0.7
shot automation-hidden && {
    stats=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/automation-hidden.png" "$WX" "$WY" "$WW" "$WH")
    num_between "$(echo "$stats" | field changed)" 0 0.001 || fail "the pet is still drawn while a timeline hides it"
}
"$CTL" pets list 2>/dev/null | grep -qE '(^|[^0-9])640([^0-9]|$)' || fail "the timeline changed the stored placement"
wait_for 15 "the timeline to end and the pet to come back" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0].get(\"runtimeHidden\", False)' | grep -qx False"
sleep 1
check_pet_drawn automation-back "$WX" "$WY" "$WW" "$WH"
# a glide: the window goes there by itself (the daemon sent the motion once), and the stored placement stays
timeout 20 "$CTL" timeline run walk >/dev/null 2>&1 || fail "timeline run walk"
wait_for 10 "the window to glide to (120, 90)" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; [[ \$(windows | jq_py 'round(w[0][\"x\"]), round(w[0][\"y\"])') == '120 90' ]]"
"$CTL" pets list 2>/dev/null | grep -qE '(^|[^0-9])640([^0-9]|$)' || fail "the glide changed the stored placement"
sleep 1
check_pet_drawn automation-walked 120 90 "$WW" "$WH"
timeout 20 "$CTL" pets runtime clear >/dev/null 2>&1 || fail "pets runtime clear"
wait_for 10 "the pet to be back at its stored place" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; [[ \$(windows | jq_py 'round(w[0][\"x\"]), round(w[0][\"y\"])') == '640 325' ]]"
# a signal-file trigger (the daemon watches the runtime directory) plays the timeline
timeout 20 "$CTL" triggers add napping signal-file timeline source.name=nap-now "action.id=$(timeout 10 "$CTL" timeline list 2>/dev/null | awk -F'\t' '$2 == "nap" {print $1}')" >/dev/null 2>&1 || fail "triggers add"
mkdir -p "$XDG_RUNTIME_DIR/murasaki/triggers"
wait_for 10 "the daemon to watch the signal directory" bash -c "[[ -d '$XDG_RUNTIME_DIR/murasaki/triggers' ]]"
sleep 0.5
: >"$XDG_RUNTIME_DIR/murasaki/triggers/nap-now"
wait_for 10 "the signal file to hide the pet through the trigger" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0].get(\"runtimeHidden\", False)' | grep -qx True"
# the rescue: stops the timeline and puts the pet back at once
timeout 20 "$CTL" pets rescue >/dev/null 2>&1 || fail "pets rescue"
wait_for 10 "the rescue to bring the pet back" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py 'w[0].get(\"runtimeHidden\", False)' | grep -qx False"
[[ -z $(timeout 10 busctl --user call $MD $MD_PATH $MD_IF RunningTimelines 2>/dev/null | awk '{ if ($2 > 0) print "x" }') ]] || fail "a timeline still runs after the rescue"
# profiles: save the setup, change it, switch back; the pet keeps its id (so its window)
timeout 20 "$CTL" profile save base >/dev/null 2>&1 || fail "profile save"
timeout 10 busctl --user call $MD $MD_PATH $MD_IF UpdatePet ss "$PET_ID" '{"x": 300, "y": 200}' >>"$LOG_DIR/busctl.err" 2>&1
wait_for 10 "the window to follow the edit" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; [[ \$(windows | jq_py 'round(w[0][\"x\"])') == 300 ]]"
timeout 20 "$CTL" profile switch base >/dev/null 2>&1 || fail "profile switch"
wait_for 10 "the profile to put the pet back" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; [[ \$(windows | jq_py 'round(w[0][\"x\"]), w[0][\"petId\"]') == '640 $PET_ID' ]]"
timeout 20 "$CTL" triggers list 2>/dev/null | grep -q napping || fail "the trigger is not listed"
timeout 20 "$CTL" triggers remove napping >/dev/null 2>&1

echo "== removal"
timeout 10 "$CTL" pets remove "$PET_ID" >/dev/null 2>&1 || fail "pets remove"
wait_for 10 "the window to go" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; [[ \$(windows) == '[]' ]]"
sleep 1
shot pet-removed && {
    stats=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/pet-removed.png" "$WX" "$WY" "$WW" "$WH")
    num_between "$(echo "$stats" | field changed)" 0 0.001 || fail "the removed pet is still drawn"
}


# ---- the pointer behaviours ----------------------------------------------------------------------------------------------
if [[ -x $FAKEINPUT ]]; then
    echo "== pointer behaviours"
    inject() { # TOKENS...: abs X Y | down | up | sleep MS, one connection for the whole run (a new one makes Qt drop its pointer)
        local id deadline=$((SECONDS + 60))
        id=$(date +%s%N)
        echo "$* mark $id" >"$IFIFO"
        while ((SECONDS < deadline)); do
            grep -qx "mark $id" "$ILOG" && return 0
            sleep 0.05
        done
        return 1
    }
    presses_at() { # X Y: presses the backdrop (the window below every pet) has logged within a pixel of the point
        awk -v x="$1" -v y="$2" '$1 == "press" && ($2 - x) ^ 2 <= 1 && ($3 - y) ^ 2 <= 1 { n++ } END { print n + 0 }' "$LOG_DIR/backdrop.log"
    }
    SX0=900 SY0=500 SW0=256 SH0=160     # the still pet: its rectangle
    CXP=$((SX0 + 128)) CYP=$((SY0 + 80)) # the middle of the disc (radius 40 px)
    FAR_X=1270 FAR_Y=710
    timeout 30 "$CTL" pets add anima:9000002 --variant still --output "$OUTPUT" --x $SX0 --y $SY0 --width $SW0 >"$LOG_DIR/pets-add2.txt" 2>&1 \
        || fail "pets add (the still pet): $(cat "$LOG_DIR/pets-add2.txt")"
    SID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add2.txt" | head -1)
    spw() { windows | jq_py "[p for p in w if p['petId'] == '$SID'][0].get('$1')"; }
    setp() { timeout 10 busctl --user call $MD $MD_PATH $MD_IF UpdatePet ss "$SID" "$1" >>"$LOG_DIR/busctl.err" 2>&1 || fail "UpdatePet $1"; }
    pos() { windows | jq_py "(round([p for p in w if p['petId'] == '$SID'][0]['x']), round([p for p in w if p['petId'] == '$SID'][0]['y']))"; }
    wait_pw() { # FIELD VALUE SECONDS: waits until the field has the value
        local deadline=$((SECONDS + ${3:-10}))
        while ((SECONDS < deadline)); do
            [[ $(spw "$1") == "$2" ]] && return 0
            sleep 0.2
        done
        return 1
    }
    if [[ -z $SID ]] || ! wait_for 40 "the still pet to be shown" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"[p for p in w if p['petId'] == '$SID'][0]['shown']\" | grep -qx True"; then
        fail "the still pet did not appear"
    else
        inject abs $FAR_X $FAR_Y sleep 200
        # -- a click-through pet (the default) lets the pointer through: a drag over it moves nothing
        inject abs $CXP $CYP sleep 150 down sleep 100 abs $((CXP + 60)) $((CYP + 30)) sleep 150 up sleep 300
        [[ $(pos) == "$SX0 $SY0" ]] || fail "a click-through pet was dragged (now at $(pos))"
        echo "   click-through: the pet stayed at $(pos), the window below got $(presses_at $CXP $CYP) press(es) there"
        [[ $(presses_at $CXP $CYP) -ge 1 ]] || fail "the press on a click-through pet did not reach the window below it"
        # -- it takes input on its opaque pixels
        setp '{"clickThrough": false}'
        wait_pw hitMask frame 40 || echo "   note: hitMask is '$(spw hitMask)' (frames not in the GPU cache yet?)"
        echo "   pointer: $(spw pointer), hit mask: $(spw hitMask), mask bytes: $(spw maskBytes)"
        case $COMP in sway) want_level=local ;; *) want_level=global ;; esac
        LEVEL=$(spw pointer)
        [[ $LEVEL == "$want_level" ]] || fail "the pointer level on $COMP is '$(spw pointer)', not $want_level"
        # a press on a transparent corner does not start a drag
        inject abs $((SX0 + 10)) $((SY0 + 10)) sleep 150 down sleep 100 abs $((SX0 + 70)) $((SY0 + 40)) sleep 150 up sleep 300
        [[ $(pos) == "$SX0 $SY0" ]] || fail "a press on a transparent pixel dragged the pet (now at $(pos))"
        # a press on the disc does: the pet follows the pointer by the same amount. The drag moves the pointer as a mouse does
        # (relative motion): a compositor that animates the window's growth is followed by it (PetInteraction, F13).
        inject abs $CXP $CYP sleep 200 down sleep 100 rel 40 20 sleep 100 rel 60 30 sleep 200 up sleep 600
        got=$(pos)
        echo "   drag by (100, 50): the pet is at $got (from $SX0 $SY0)"
        case $got in "$((SX0 + 100)) $((SY0 + 50))" | "$((SX0 + 99)) $((SY0 + 50))" | "$((SX0 + 101)) $((SY0 + 50))" | "$((SX0 + 100)) $((SY0 + 49))" | "$((SX0 + 100)) $((SY0 + 51))") ;; *) fail "the drag did not move the pet by (100, 50): it is at $got" ;; esac
        wait_for 10 "the daemon to hear of the drop" bash -c "'$CTL' pets list 2>/dev/null | grep -qE '$((SX0 + 100))|$((SX0 + 99))|$((SX0 + 101))'" || fail "the daemon never heard the new place"
        [[ $(spw interactive) == False ]] || fail "the window is still grown after the drop"
        # back to the start (after the hold the window keeps its own place for, 1.5 s: an outside edit within it is not applied)
        sleep 2
        setp "{\"x\": $SX0, \"y\": $SY0}"
        wait_for 10 "the pet to go back" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"round([p for p in w if p['petId'] == '$SID'][0]['x'])\" | grep -qx $SX0"
        sleep 2   # (the hold after our own commit)
        # -- locked: a drag moves nothing
        setp '{"locked": true}'
        sleep 1
        inject abs $CXP $CYP sleep 200 down sleep 100 abs $((CXP + 80)) $((CYP + 40)) sleep 200 up sleep 400
        [[ $(pos) == "$SX0 $SY0" ]] || fail "a locked pet was dragged (now at $(pos))"
        setp '{"locked": false}'
        # -- hide under the cursor
        setp '{"hoverHide": true}'
        wait_pw hidden False 10
        inject abs $FAR_X $FAR_Y sleep 300
        shot ptr-away && base_stats=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/ptr-away.png" $SX0 $SY0 $SW0 $SH0 | field changed)
        inject abs $CXP $CYP sleep 300
        wait_pw hidden True 5 || fail "hide under the cursor did not hide the pet"
        shot ptr-hidden && hid=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/ptr-hidden.png" $SX0 $SY0 $SW0 $SH0 | field changed)
        # two clicks on the hidden pet, without moving: they are for the window below. The pet may get the first press (a local pet hears
        # it that way; sway and Hyprland give a press to the surface the pointer last moved onto, whatever its region is now); it then lets
        # go of the place (unmaps) and the second one gets there. KWin re-picks the surface on a press: there a global pet gets neither.
        before=$(presses_at $CXP $CYP)
        inject down sleep 80 up sleep 300 down sleep 80 up sleep 300
        through=$(($(presses_at $CXP $CYP) - before))
        echo "   two clicks on the hidden pet ($(spw pointer)): $through reached the window below"
        ((through >= 1)) || fail "no click on a hidden pet reached the window below it"
        [[ $COMP == kwin && $(spw pointer) == global ]] && ((through != 2)) && fail "on KWin only $through of the two clicks on a hidden global pet reached the window below it"
        [[ $(pos) == "$SX0 $SY0" ]] || fail "a click on the hidden pet moved it (now at $(pos))"
        inject abs $FAR_X $FAR_Y sleep 400
        wait_pw hidden False 5 || fail "the pet did not come back when the cursor left"
        shot ptr-back && back=$(rect_stats "$OUT_DIR/backdrop.png" "$OUT_DIR/ptr-back.png" $SX0 $SY0 $SW0 $SH0 | field changed)
        echo "   hide under the cursor: changed share of the pet rectangle, away ${base_stats:-?}, under the cursor ${hid:-?}, away again ${back:-?}"
        num_between "${hid:-1}" 0 0.002 || fail "the pet is still drawn under the cursor (changed=$hid)"
        num_between "${base_stats:-0}" 0.03 0.6 || fail "the pet is not drawn while the cursor is away (changed=$base_stats)"
        num_between "${back:-0}" 0.03 0.6 || fail "the pet did not come back (changed=$back)"
        # the cursor on a transparent pixel of the rectangle does not hide it
        inject abs $((SX0 + 8)) $((SY0 + 8)) sleep 400
        [[ $(spw hidden) == False ]] || fail "the cursor on a transparent pixel hid the pet"
        # -- focus fade: faded and click-through, also by the opaque pixels only
        setp '{"focusFade": true}'
        sleep 1
        [[ $(spw hidden) == False ]] || true
        inject abs $FAR_X $FAR_Y sleep 300 abs $CXP $CYP sleep 400
        wait_pw hidden True 5 || fail "focus fade did not fade the pet"
        inject abs $FAR_X $FAR_Y sleep 400
        wait_pw hidden False 5 || fail "the faded pet did not come back"
        setp '{"hoverHide": false, "focusFade": false}'
        sleep 1
        # -- smart dodge: the cursor 10 px to the left of the pet makes it step right, by clamp(256 // 3, 54, 140) = 85 px. A pet that reads
        # the pointer locally hears it only on its opaque pixels, and says so: there the cursor comes onto the disc, left of its middle.
        setp '{"smartDodge": true, "clickThrough": true}'
        sleep 1
        if [[ $LEVEL == local ]]; then
            [[ $(spw degraded) == *"smart dodge: only when touched"* ]] || fail "a local pet does not say its dodge needs a touch (degraded: $(spw degraded))"
            inject abs $FAR_X $FAR_Y sleep 300 abs $((CXP - 30)) $CYP sleep 700
        else
            inject abs $FAR_X $FAR_Y sleep 300 abs $((SX0 - 10)) $CYP sleep 700
        fi
        got=$(pos)
        echo "   dodge: the pet went from $SX0 $SY0 to $got"
        case $got in "$((SX0 + 85)) $SY0" | "$((SX0 + 84)) $SY0" | "$((SX0 + 86)) $SY0") ;; *) fail "the dodge was not one step of 85 px to the right: the pet is at $got" ;; esac
        wait_for 10 "the daemon to hear of the dodge" bash -c "'$CTL' pets list 2>/dev/null | grep -qE '$((SX0 + 85))|$((SX0 + 84))|$((SX0 + 86))'" || fail "the daemon never heard the dodge"
        setp '{"smartDodge": false}'
        inject abs $FAR_X $FAR_Y sleep 300
        timeout 10 "$CTL" pets remove "$SID" >/dev/null 2>&1
    fi

    # ---- rotated and flipped pets: the input follows what is drawn
    timeout 10 "$CTL" pets list >/dev/null 2>&1
    RX=800 RY=300
    timeout 30 "$CTL" pets add anima:9000003 --variant left --output "$OUTPUT" --x $RX --y $RY --width 256 >"$LOG_DIR/pets-add3.txt" 2>&1 \
        || fail "pets add (the left pet): $(cat "$LOG_DIR/pets-add3.txt")"
    SID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add3.txt" | head -1)
    if [[ -z $SID ]] || ! wait_for 40 "the left pet to be shown" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"[p for p in w if p['petId'] == '$SID'][0]['shown']\" | grep -qx True"; then
        fail "the left pet did not appear"
    else
        setp '{"clickThrough": false}'
        wait_pw hitMask frame 40 || echo "   note: hitMask is '$(spw hitMask)'"
        pixel_changed() { # SHOT X Y: does the screenshot differ from the backdrop at the point?
            python3 - "$OUT_DIR/backdrop.png" "$OUT_DIR/$1.png" "$2" "$3" <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
x, y = int(sys.argv[3]), int(sys.argv[4])
pa, pb = a.getpixel((x, y)), b.getpixel((x, y))
print(1 if max(abs(pa[i] - pb[i]) for i in range(3)) > 24 else 0)
PY
        }
        rot_case() { # NAME ROTATION FLIPH FLIPV HITX HITY PASSX PASSY
            local name=$1 rot=$2 fh=$3 fv=$4 hx=$5 hy=$6 px=$7 py=$8 got
            setp "{\"rotation\": $rot, \"flipH\": $fh, \"flipV\": $fv}"
            sleep 2.5
            inject abs $FAR_X $FAR_Y sleep 300
            shot "rot-$name" || return
            [[ $(pixel_changed "rot-$name" $hx $hy) == 1 ]] || fail "$name: the drawing is not at the point the input expects ($hx,$hy): the picture and the mask disagree"
            [[ $(pixel_changed "rot-$name" $px $py) == 0 ]] || fail "$name: something is drawn at the pass-through point ($px,$py)"
            # a press where the turned drawing is transparent (the unturned one was opaque, or the turned pet is elsewhere): through
            inject abs $px $py sleep 200 down sleep 100 abs $((px + 60)) $((py + 30)) sleep 150 up sleep 400
            got=$(pos)
            [[ $got == "$RX $RY" ]] || fail "$name: a press on a transparent pixel of the turned drawing dragged the pet (now at $got)"
            # a press on the drawing drags it
            inject abs $hx $hy sleep 200 down sleep 100 rel 50 20 sleep 100 rel 50 20 sleep 200 up sleep 600
            got=$(pos)
            echo "   $name: drag by (100, 40) from the drawing: the pet went from $RX $RY to $got"
            case $got in "$((RX + 100)) $((RY + 40))" | "$((RX + 99)) $((RY + 40))" | "$((RX + 101)) $((RY + 40))" | "$((RX + 100)) $((RY + 39))" | "$((RX + 100)) $((RY + 41))") ;; *) fail "$name: the drag from the drawing did not move the pet by (100, 40): it is at $got" ;; esac
            sleep 2
            setp "{\"x\": $RX, \"y\": $RY}"
            sleep 2.5
        }
        #            name       rot  flipH  flipV   hit point         pass point
        rot_case     r0flipH      0  true   false   $((RX + 192)) $((RY + 80))  $((RX + 64)) $((RY + 80))
        rot_case     r90         90  false  false   $((RX + 128)) $((RY + 16))  $((RX + 64)) $((RY + 80))
        rot_case     r37         37  false  false   $((RX + 77))  $((RY + 41))  $((RX + 64)) $((RY + 80))
        rot_case     r90flipH    90  true   false   $((RX + 128)) $((RY + 144)) $((RX + 128)) $((RY + 16))
        timeout 10 "$CTL" pets remove "$SID" >/dev/null 2>&1
    fi

    # ---- the sound of a pet: the exact wraps, the click, click-through
    echo "== pet sound"
    AUDIO_LOG=$MURASAKI_NESTED_TMP/audio.log
    AUDIO_HELPER=$BUILD_DIR/src/audio/murasaki-petaudio
    if [[ -x $AUDIO_HELPER ]]; then
        : >"$AUDIO_LOG"
        timeout 60 ffmpeg -y -v error -f lavfi -i "sine=frequency=660:duration=0.4" "$MURASAKI_NESTED_TMP/ding.wav" 2>>"$LOG_DIR/ffmpeg.log" || fail "ffmpeg cannot make the sound"
        timeout 30 "$CTL" pets add anima:9000002 --variant still --output "$OUTPUT" --x $SX0 --y $SY0 --width $SW0 >"$LOG_DIR/pets-add4.txt" 2>&1 \
            || fail "pets add (the sound pet): $(cat "$LOG_DIR/pets-add4.txt")"
        SID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add4.txt" | head -1)
        starts() { grep -c '^start ' "$AUDIO_LOG" 2>/dev/null; }
        if [[ -z $SID ]] || ! wait_for 40 "the sound pet to be shown" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"[p for p in w if p['petId'] == '$SID'][0]['shown']\" | grep -qx True"; then
            fail "the sound pet did not appear"
        else
            # -- importing: the same file twice is one sound
            timeout 60 "$CTL" pets audio import "$MURASAKI_NESTED_TMP/ding.wav" >"$LOG_DIR/audio-import.txt" 2>&1 || fail "pets audio import: $(cat "$LOG_DIR/audio-import.txt")"
            ASSET=$(head -1 "$LOG_DIR/audio-import.txt")
            [[ ${#ASSET} == 64 ]] || fail "the sound id is not 64 characters: '$ASSET'"
            [[ $(timeout 10 "$CTL" pets audio list | wc -l) == 1 ]] || fail "the imported sound is not listed once"
            # -- off by default: nothing runs (the PLAYER: the import helper of a second ago may still be leaving, and its command line has no --assets)
            sleep 1
            [[ $(starts) == 0 ]] || fail "a pet with no sound made one"
            pgrep -f -- "murasaki-petaudio .*--assets $XDG_DATA_HOME/" >/dev/null && fail "the sound player runs although no pet has a sound"
            # -- once: starts when chosen
            timeout 20 "$CTL" pets audio set "$SID" --asset "$ASSET" --mode once --volume 60 >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets audio set once"
            wait_for 10 "the sound of mode once to start" bash -c "[[ \$(grep -c '^start ' '$AUDIO_LOG') -ge 1 ]]"
            echo "   once: $(starts) start(s) logged; player running: $(pgrep -f -- "murasaki-petaudio .*--assets $XDG_DATA_HOME/" | wc -l) process(es)"
            [[ $(spw audio | python3 -c 'import sys, ast; print(ast.literal_eval(sys.stdin.read())["mode"])' 2>/dev/null) == once ]] || echo "   note: Windows() audio is '$(spw audio)'"
            # -- animation_loop: one start per wrap of the animation, at the exact loop length
            : >"$AUDIO_LOG"
            timeout 20 "$CTL" pets audio set "$SID" --mode animation_loop >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets audio set animation_loop"
            wait_for 20 "four wraps" bash -c "[[ \$(grep -c '^start ' '$AUDIO_LOG') -ge 4 ]]"
            cp "$AUDIO_LOG" "$LOG_DIR/audio.log"
            python3 - "$AUDIO_LOG" <<'PY' || fail "the animation_loop starts are not one loop apart (see audio.log)"
import sys
rows = [l.split() for l in open(sys.argv[1]) if l.startswith("start ")]
intended = [int(r[3]) for r in rows]
actual = [int(r[4]) for r in rows]
gaps = [(b - a) / 1e6 for a, b in zip(intended, intended[1:])]
lag = [(a - i) / 1e6 for i, a in zip(intended, actual)]
print("   animation_loop: %d starts, gaps between the wraps (ms) %s, placed %.1f..%.1f ms after the wrap" % (len(rows), ["%.2f" % g for g in gaps], min(lag), max(lag)))
# 30 frames at the default 24 fps, 42 ms each (round(1000 / 24)): a 1260 ms loop; every wrap exactly one loop after the last, whatever the timer did.
# The wrap is announced ahead and the start scheduled for it, so the sound is placed on the wrap itself (to the sample), whatever the overlay's timer did.
ok = len(gaps) >= 3 and all(abs(g - 1260) < 3 for g in gaps) and min(lag) > -2 and max(lag) < 2
sys.exit(0 if ok else 1)
PY
            # -- click: plays on a click of a pet that takes the pointer, not on a click-through one
            timeout 20 "$CTL" pets audio set "$SID" --mode click >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets audio set click"
            setp '{"clickThrough": false}'
            sleep 1
            inject abs $FAR_X $FAR_Y sleep 200
            before=$(starts)
            sleep 1.5
            [[ $(starts) == "$before" ]] || fail "the click mode played without a click"
            inject abs $CXP $CYP sleep 250 down sleep 80 up sleep 300
            wait_for 5 "the click sound" bash -c "[[ \$(grep -c '^start ' '$AUDIO_LOG') -gt $before ]]"
            after=$(starts)
            # a press that drags is not a click
            inject abs $CXP $CYP sleep 250 down sleep 100 abs $((CXP + 50)) $((CYP + 30)) sleep 150 up sleep 600
            [[ $(starts) == "$after" ]] || fail "a drag played the click sound"
            setp "{\"x\": $SX0, \"y\": $SY0}"; sleep 3
            setp '{"clickThrough": true}'
            sleep 1
            inject abs $CXP $CYP sleep 250 down sleep 80 up sleep 600
            [[ $(starts) == "$after" ]] || fail "a click on a click-through pet played a sound"
            echo "   click: $before -> $after starts after a click, none for a drag or on a click-through pet"
            # -- taking the sound off ends the player after its idle time (not waited for) and frees the asset
            timeout 20 "$CTL" pets audio off "$SID" >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets audio off"
            timeout 20 "$CTL" pets remove "$SID" >/dev/null 2>&1
            timeout 20 "$CTL" pets audio remove "$ASSET" >>"$LOG_DIR/busctl.err" 2>&1 || fail "pets audio remove"
        fi
    else
        echo "   note: murasaki-petaudio is not built: the sound was not exercised"
    fi

    # ---- the states of a pet and its mouse tracking (docs/pets/states.md): a press shows the grabbed picture, the release the dropped one and then rest; look turns
    # the picture as the cursor crosses; follow walks to its distance from the cursor without writing the store
    echo "== pet states and tracking"
    SX0=900 SY0=500
    CXP=$((SX0 + 128)) CYP=$((SY0 + 80))
    timeout 30 "$CTL" pets add anima:9000004 --variant rest --output "$OUTPUT" --x $SX0 --y $SY0 --width 256 >"$LOG_DIR/pets-add4s.txt" 2>&1 \
        || fail "pets add (the states pet): $(cat "$LOG_DIR/pets-add4s.txt")"
    TID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add4s.txt" | head -1)
    tw() { windows | jq_py "[p for p in w if p['petId'] == '$TID'][0].get('$1')"; }
    tset() { timeout 10 busctl --user call $MD $MD_PATH $MD_IF UpdatePet ss "$TID" "$1" >>"$LOG_DIR/busctl.err" 2>&1 || fail "UpdatePet $1"; }
    tpos() { windows | jq_py "(round([p for p in w if p['petId'] == '$TID'][0]['x']), round([p for p in w if p['petId'] == '$TID'][0]['y']))"; }
    disc_kind() { # the colour of the disc at its centre in a fresh screenshot: orange, blue, green or other
        shot "states-$1" || { echo none; return; }
        python3 - "$OUT_DIR/states-$1.png" "$2" "$3" <<'PY'
import sys
from PIL import Image
r, g, b = Image.open(sys.argv[1]).convert("RGB").getpixel((int(sys.argv[2]), int(sys.argv[3])))
near = lambda t: all(abs(a - c) < 40 for a, c in zip((r, g, b), t))
print("orange" if near((255, 140, 40)) else "blue" if near((40, 120, 255)) else "green" if near((40, 220, 80)) else "other")
PY
    }
    await_kind() { # NAME KIND SECONDS: until the disc has that colour
        local deadline=$((SECONDS + $3)) got=
        while ((SECONDS < deadline)); do
            got=$(disc_kind "$1" $CXP $CYP)
            [[ $got == "$2" ]] && return 0
        done
        echo "   (the disc was $got, wanted $2)"
        return 1
    }
    if [[ -z $TID ]] || ! wait_for 40 "the states pet to be shown" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"[p for p in w if p['petId'] == '$TID'][0]['shown']\" | grep -qx True"; then
        fail "the states pet did not appear"
    else
        tset '{"clickThrough": false, "states": {"grabbed": {"variant": "held"}, "released": {"variant": "dropped"}}}'
        sleep 3
        inject abs $FAR_X $FAR_Y sleep 300
        await_kind rest orange 10 || fail "the states pet at rest is not orange"
        [[ $(tw state) == idle ]] || fail "the state at rest is '$(tw state)'"
        # a press on the disc: the grabbed picture
        inject abs $CXP $CYP sleep 300 down sleep 700
        await_kind held blue 8 || fail "the grabbed picture (blue) is not shown while the pet is held"
        [[ $(tw state) == grabbed ]] || fail "the state while held is '$(tw state)'"
        # the release: the dropped picture, then rest again by itself
        inject up sleep 100
        got=$(disc_kind dropped $CXP $CYP)
        echo "   release: the disc is $got right after it, state $(tw state)"
        [[ $got == green || $(tw state) == released ]] || fail "the dropped picture was not shown after the release (disc $got)"
        await_kind rest2 orange 10 || fail "the pet did not go back to rest after the dropped picture"
        [[ $(tw state) == idle ]] || fail "the state after the release is '$(tw state)', not idle"
        [[ $(tpos) == "$SX0 $SY0" ]] || fail "a press and release without moving moved the pet (now at $(tpos))"
        timeout 10 "$CTL" pets remove "$TID" >/dev/null 2>&1
    fi

    # look: the left pet (the disc on the left of its picture) turns to the side the cursor is on; the item faces right
    LX=800 LY=300
    timeout 30 "$CTL" pets add anima:9000003 --variant left --output "$OUTPUT" --x $LX --y $LY --width 256 >"$LOG_DIR/pets-add5s.txt" 2>&1 \
        || fail "pets add (the look pet): $(cat "$LOG_DIR/pets-add5s.txt")"
    TID=$(grep -oE '[0-9a-f]{8}-[0-9a-f-]{27}' "$LOG_DIR/pets-add5s.txt" | head -1)
    if [[ -z $TID ]] || ! wait_for 40 "the look pet to be shown" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"[p for p in w if p['petId'] == '$TID'][0]['shown']\" | grep -qx True"; then
        fail "the look pet did not appear"
    else
        disc_side() { # NAME: left or right half of the pet rectangle holds the disc (the left disc is at 64 px of 256, the mirrored one at 192)
            shot "states-$1" || { echo none; return; }
            python3 - "$OUT_DIR/backdrop.png" "$OUT_DIR/states-$1.png" $LX $LY <<'PY'
import sys
from PIL import Image
a = Image.open(sys.argv[1]).convert("RGB"); b = Image.open(sys.argv[2]).convert("RGB")
x0, y0 = int(sys.argv[3]), int(sys.argv[4])
def changed(cx):
    pa, pb = a.getpixel((x0 + cx, y0 + 80)), b.getpixel((x0 + cx, y0 + 80))
    return max(abs(pa[i] - pb[i]) for i in range(3)) > 24
l, r = changed(64), changed(192)
print("left" if l and not r else "right" if r and not l else "other")
PY
        }
        tset '{"tracking": {"mode": "look"}}'
        sleep 2
        if [[ $LEVEL == local ]]; then
            # look and follow need the cursor away from the pet: a pet that reads the pointer locally does neither, and says so
            [[ $(tw trackingActive) == False ]] || fail "a local pet says its tracking is active"
            [[ $(tw degraded) == *"tracking: needs the global pointer"* ]] || fail "a local pet does not say tracking needs the global pointer (degraded: $(tw degraded))"
            echo "   look and follow: not on a local pointer, and the pet says so"
            tset '{"tracking": {"mode": "off"}}'
        else
            # (the first look is at the cursor on the left: a pet that did nothing would show its disc on the left and pass a test that began on the right)
            inject abs $((LX - 600)) $((LY + 80)) sleep 800
            [[ $(tw trackingActive) == True ]] || fail "look is not active (degraded: $(tw degraded))"
            got=$(disc_side look-left)
            echo "   look: cursor on the left: the disc is on the $got"
            [[ $got == right ]] || fail "with the cursor on its left the pet is not turned (disc on the $got)"
            inject abs $((LX + 900)) $((LY + 80)) sleep 800
            got=$(disc_side look-right)
            echo "   look: cursor on the right: the disc is on the $got"
            [[ $got == left ]] || fail "with the cursor on its right the pet is not turned back (disc on the $got)"
            # follow: it walks to 100 px from the cursor and stops; nothing is written; off sends it home
            tset '{"tracking": {"mode": "follow", "distance": 100}}'
            sleep 2
            inject abs 1100 400 sleep 4000
            got=$(tpos)
            echo "   follow: the pet is at $got (centre expected near 1000,388)"
            python3 -c 'import sys; x, y = map(float, sys.argv[1:3]); sys.exit(0 if abs(x + 128 - 1000.7) < 6 and abs(y + 80 - 388.5) < 6 else 1)' $got || fail "the follower did not stop 100 px from the cursor (it is at $got)"
            [[ $(tw trackingActive) == True ]] || fail "follow is not active (degraded: $(tw degraded))"
            sleep 1
            [[ $(tpos) == "$got" ]] || fail "the follower is still walking after it stopped ($got, then $(tpos))"
            timeout 10 "$CTL" pets list 2>/dev/null | grep "^$TID" | cut -f5,6 | grep -qx "$LX.$LY" || fail "the follower wrote its walk to the store: $(timeout 10 "$CTL" pets list 2>/dev/null | grep "^$TID" | cut -f5,6)"
            tset '{"tracking": {"mode": "off"}}'
            wait_for 12 "the follower to go back home" bash -c "$(declare -f jcall windows jq_py); LOG_DIR='$LOG_DIR' OV=$OV OV_PATH=$OV_PATH OV_IF=$OV_IF; windows | jq_py \"(round([p for p in w if p['petId'] == '$TID'][0]['x']), round([p for p in w if p['petId'] == '$TID'][0]['y']))\" | grep -qx '$LX $LY'" || fail "the follower did not go home (it is at $(tpos))"
        fi
        inject abs $FAR_X $FAR_Y sleep 300
        timeout 10 "$CTL" pets remove "$TID" >/dev/null 2>&1
    fi
    echo quit >"$IFIFO"
else
    echo "   note: murasaki-pointer-fakeinput is not built: the pointer behaviours were not exercised"
fi

# Nothing in the logs that means a broken QML file or a dead process.
if grep -qiE "unavailable|is not a type|cannot assign|ReferenceError|TypeError" "$LOG_DIR/overlay.log" 2>/dev/null; then
    fail "the overlay logged QML errors (see $LOG_DIR/overlay.log)"
fi
pgrep -f "murasaki-overlay$" >/dev/null || echo "   note: the overlay is not running at the end"

((failed)) && { echo "RESULT: FAIL (screenshots in $OUT_DIR)"; exit 1; }
echo "RESULT: PASS (screenshots in $OUT_DIR)"
