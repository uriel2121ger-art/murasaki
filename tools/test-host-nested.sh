#!/usr/bin/env bash
# Integration test of the Plasma host mode (murasaki-surface --host) in a fully isolated nested session.
#
# What it starts, all private: a D-Bus session (dbus-run-session), kwin_wayland --virtual, a murasakid from the
# build directory with a throw-away config, and per item kind a `bridge-view -- murasaki-surface --host OUTPUT`.
# bridge-view is the consumer: it must see non-black, animated frames for a video, a web item and a scene, Resize
# must change the picture size, and Pause must stop the frames and the unpause must resume them.
#
# The user's session is never touched: the runtime dir, the XDG dirs and the bus are private, the library is only
# read (the Steam workshop directory), and every process this script starts carries MURASAKI_NESTED_MARK, which is
# also how the leftovers are killed at the end. Nothing is installed.
#
#   tools/test-host-nested.sh [--build-dir DIR] [--out-dir DIR] [--kinds video,web,scene] [--output NAME]
#
# Item ids come from the private daemon's `murasakictl list` (the first ok item of each kind), or from
# MURASAKI_TEST_VIDEO / MURASAKI_TEST_WEB / MURASAKI_TEST_SCENE. Scenes need MURASAKI_SCENE_ENGINE (the scene
# engine binary, default ~/.local/bin/murasaki-scene) and Wallpaper Engine's assets. A kind with no item is
# skipped; no kind tested at all is a failure.
#
# Exit status: 0 all tested kinds passed, 1 a check failed, 77 (ctest skip) when the environment cannot run it.
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
OUT_DIR=${TMPDIR:-/tmp}/murasaki-host-nested-out
KINDS=video,web,scene
OUTPUT=Virtual-0   # what kwin_wayland --virtual names its one output
USE_POLICY=1
SIZE=${MURASAKI_TEST_SIZE:-960x540}
RESIZE=${MURASAKI_TEST_RESIZE:-800x450}
while (($#)); do
    case $1 in
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --out-dir) OUT_DIR=$2; shift 2 ;;
    --kinds) KINDS=$2; shift 2 ;;
    --output) OUTPUT=$2; shift 2 ;;
    --no-policy) USE_POLICY=0; shift ;;
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
    # ---- outer half: private runtime dir and bus -------------------------------------------------------
    # Opt in: the test needs the user's Wallpaper Engine library, a GPU and about a minute.
    if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the nested host-mode test"
        exit 77
    fi
    for tool in dbus-run-session kwin_wayland; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    for bin in src/daemon/murasakid src/ctl/murasakictl src/surface/murasaki-surface tools/bridge-view/bridge-view; do
        [[ -x $BUILD_DIR/$bin ]] || { echo "SKIP: $BUILD_DIR/$bin is not built"; exit 77; }
    done
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    TMP_ROOT=$(mktemp -d /tmp/mhn.XXXXXX)   # short: it holds the Wayland socket
    mkdir -m 700 "$TMP_ROOT/rt"
    export MURASAKI_NESTED_TMP=$TMP_ROOT
    unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS KDE_SESSION_UID SWAYSOCK HYPRLAND_INSTANCE_SIGNATURE
    unset XDG_SESSION_ID XDG_SEAT XDG_VTNR
    export XDG_RUNTIME_DIR=$TMP_ROOT/rt XDG_CONFIG_HOME=$TMP_ROOT/config XDG_DATA_HOME=$TMP_ROOT/data
    export XDG_CACHE_HOME=$TMP_ROOT/cache XDG_STATE_HOME=$TMP_ROOT/state
    mkdir -p "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
    export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland KDE_FULL_SESSION=true
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
    # An isolated bus: it can not activate anything (the system's portals, kglobalacceld, ...). Every service the
    # test needs is started by the test itself.
    nested_write_bus_conf "$TMP_ROOT/bus"
    timeout "${MURASAKI_NESTED_TIMEOUT:-900}" dbus-run-session --config-file="$TMP_ROOT/bus/bus.conf" -- "$SELF" --inner --build-dir "$BUILD_DIR" \
        --out-dir "$OUT_DIR" --kinds "$KINDS" --output "$OUTPUT" \
        $( ((USE_POLICY)) || echo --no-policy ) </dev/null 2>"$OUT_DIR/logs/session.log"
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

"$BUILD_DIR/src/daemon/murasakid" >"$LOG_DIR/murasakid.log" 2>&1 &
CTL=$BUILD_DIR/src/ctl/murasakictl
for _ in $(seq 100); do
    "$CTL" list >"$LOG_DIR/list.txt" 2>/dev/null && break
    sleep 0.2
done
[[ -s $LOG_DIR/list.txt ]] || { echo "SKIP: the private murasakid sees no library items (see $LOG_DIR/murasakid.log)"; exit 77; }


# The policy server: the normal-mode murasaki-surface with presenter=plasma. It must create no layer-shell window
# at all, and every host below registers with it, shows up as hosted in Outputs(), and is paused through it.
SURF=io.github.uriel2121ger_art.Murasaki.Surface
SURF_PATH=/io/github/uriel2121ger_art/Murasaki/Surface
SURF_IFACE=io.github.uriel2121ger_art.Murasaki.Surface1
# Outputs() as plain JSON text (busctl's --json=short wraps it as a string with escaped quotes).
surface_outputs() {
    timeout 5 busctl --user --json=short call "$SURF" "$SURF_PATH" "$SURF_IFACE" Outputs 2>/dev/null \
        | sed -e 's/\\"/"/g'
}
if ((USE_POLICY)); then
    timeout 10 busctl --user call io.github.uriel2121ger_art.Murasaki /io/github/uriel2121ger_art/Murasaki \
        io.github.uriel2121ger_art.Murasaki1 SetSettings s '{"presenter":"plasma"}' >"$LOG_DIR/presenter.txt" 2>&1 \
        || { echo "FAIL: cannot set presenter=plasma (see $LOG_DIR/presenter.txt)"; exit 1; }
    "$BUILD_DIR/src/surface/murasaki-surface" >"$LOG_DIR/surface.log" 2>&1 &
    POLICY_PID=$!
    for _ in $(seq 100); do
        surface_outputs | grep -qF "$OUTPUT" && break
        sleep 0.2
    done
    surface_outputs | grep -qF "$OUTPUT" || {
        surface_outputs >"$LOG_DIR/surface-outputs.txt" 2>&1
        echo "FAIL: the policy surface does not list $OUTPUT (see $LOG_DIR/surface.log, surface-outputs.txt)"
        exit 1
    }
    echo "policy server up (presenter=plasma)"
fi

MAX_TRIES=${MURASAKI_TEST_TRIES:-4}
candidates() { # kind -> up to MAX_TRIES ok item ids (an override pins the item)
    local override=$1 kind=$2
    if [[ -n $override ]]; then echo "$override"; return; fi
    awk -F'\t' -v kind="$kind" -v max="$MAX_TRIES" '$2 == kind && $3 == "ok" && n < max { print $1; n++ }' "$LOG_DIR/list.txt"
}
declare -A OVERRIDE=([video]=${MURASAKI_TEST_VIDEO:-} [web]=${MURASAKI_TEST_WEB:-} [scene]=${MURASAKI_TEST_SCENE:-})

# What the policy server saw of one host run; prints the (possibly raised) status. Failures go to stderr.
check_policy_view() {
    local tag=$1 samples=$2 status=$3
    if ! grep -qF '"hosted":true' "$samples" 2>/dev/null; then
        echo "   FAIL: Outputs() never showed $OUTPUT as hosted" >&2; status=1
    fi
    # QJsonObject sorts its keys, so "hosted" comes before "paused" in an entry.
    if ! grep -qE '"hosted":true,.*"paused":true' "$samples" 2>/dev/null; then
        echo "   FAIL: the policy never saw the hosted output paused (SetHostVisible did not arrive)" >&2; status=1
    fi
    if ! grep -q "registered $OUTPUT with the policy server" "$LOG_DIR/host-$tag.log" 2>/dev/null; then
        echo "   FAIL: the host did not register with the policy server" >&2; status=1
    fi
    # The host has exited: its registration must be gone.
    local gone=0
    for _ in $(seq 20); do
        if surface_outputs | grep -qF '"hosted":false'; then gone=1; break; fi
        sleep 0.25
    done
    ((gone)) || { echo "   FAIL: $OUTPUT is still hosted after the host exited" >&2; status=1; }
    echo "$status"
}

# One item through one host: 0 passed, 1 failed.
try_item() {
    local kind=$1 id=$2 tag=$3 extra=()
    "$CTL" set "$id" >/dev/null || { echo "   cannot assign $id"; return 1; }
    [[ $kind == scene ]] && extra=(--settle-ms "${MURASAKI_TEST_SCENE_SETTLE:-4000}")
    # With a policy server, sample its Outputs() while the host runs: the host must show up as hosted, and the
    # bridge Pause (SetHostVisible) must reach the policy as a paused output.
    local samples=$LOG_DIR/outputs-$tag.txt stop=$LOG_DIR/outputs-$tag.stop sampler=
    if ((USE_POLICY)); then
        rm -f "$samples" "$stop"
        ( while [[ ! -e $stop ]]; do surface_outputs >>"$samples"; sleep 0.25; done ) &
        sampler=$!
    fi
    "$BUILD_DIR/tools/bridge-view/bridge-view" --out-dir "$OUT_DIR" --prefix "$tag" --size "$SIZE" \
        --expect-nonblack --expect-animated --fail-fast --check-resize "$RESIZE" --check-pause "${extra[@]}" \
        --producer-log "$LOG_DIR/host-$tag.log" \
        -- ${MURASAKI_TEST_PRODUCER:-"$BUILD_DIR/src/surface/murasaki-surface" --host "$OUTPUT"} \
        2>"$LOG_DIR/view-$tag.err" | tee "$LOG_DIR/view-$tag.txt" | sed "s/^/   /"
    local status=${PIPESTATUS[0]}
    if [[ -n $sampler ]]; then
        touch "$stop"
        wait "$sampler" 2>/dev/null
        status=$(check_policy_view "$tag" "$samples" "$status") || status=1
    fi
    "$CTL" unset >/dev/null 2>&1
    # A scene must have shown frames of the engine, not the preview placeholder.
    if ((status == 0)) && [[ $kind == scene ]] && ! grep -q "murasaki.bridge: hasFrame" "$LOG_DIR/host-$tag.log"; then
        echo "   FAIL: the host never reported a frame from the scene engine (preview placeholder only)"
        status=1
    fi
    return "$status"
}

tested=0
failed=0
for kind in ${KINDS//,/ }; do
    mapfile -t ids < <(candidates "${OVERRIDE[$kind]:-}" "$kind")
    if ((${#ids[@]} == 0)); then
        echo "== $kind: no item in the library, skipped"
        continue
    fi
    if [[ $kind == scene && ! -x $MURASAKI_SCENE_ENGINE ]]; then
        echo "== $kind: no scene engine at $MURASAKI_SCENE_ENGINE, skipped"
        continue
    fi
    # An item can be legitimately dark or static (an audio visualizer in silence) or unsupported by the scene
    # engine: the kind passes when one of its first few items does.
    tested=$((tested + 1))
    passed=0
    n=0
    for id in "${ids[@]}"; do
        n=$((n + 1))
        echo "== $kind: $id (candidate $n of ${#ids[@]})"
        if try_item "$kind" "$id" "$kind"; then
            passed=1
            echo "   $kind PASSED with $id"
            break
        fi
        echo "   $kind candidate $id failed (host log: $LOG_DIR/host-$kind.log)"
        for f in "$OUT_DIR/$kind"-*.png; do [[ -e $f ]] && mv "$f" "${f%.png}.rejected-${id//[:\/]/_}.png.bak"; done
    done
    ((passed)) || { echo "FAIL: $kind: no item passed"; failed=1; }
done

# The policy server drew nothing and survived the hosts coming and going.
if ((USE_POLICY)); then
    if grep -q "^murasaki-surface: window for" "$LOG_DIR/surface.log"; then
        echo "FAIL: the policy surface (presenter=plasma) created a layer-shell window (see $LOG_DIR/surface.log)"
        failed=1
    fi
    kill -0 "$POLICY_PID" 2>/dev/null || { echo "FAIL: the policy surface died (see $LOG_DIR/surface.log)"; failed=1; }
fi

# The private audio graph must not have looked at the real machine's devices.
if nested_audio_touched_devices; then
    echo "FAIL: the private audio daemons touched devices (see $LOG_DIR/pipewire*.log, wireplumber.log)"
    failed=1
fi

if ((tested == 0)); then
    echo "FAIL: no item kind could be tested"
    exit 1
fi
((failed)) && { echo "RESULT: FAIL"; exit 1; }
echo "RESULT: PASS ($tested kinds; PNGs in $OUT_DIR)"
