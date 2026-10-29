#!/usr/bin/env bash
# The pets' sound player (murasaki-petaudio) against a REAL PipeWire, in a private session: its own runtime dir, a private PipeWire with one null sink (and
# WirePlumber's "policy" profile only, which probes no hardware: see nested_start_audio in tools/nested-common.sh), nothing of the user's audio session. It
# checks that a sound reaches the sink (captured from the sink's monitor: level and pitch), that a sound scheduled with a time starts at it, that the player
# works inside murasaki-sandbox (a Landlock policy of its own: the system and the sounds folder read-only, no network), and that it leaves with status 4 when
# PipeWire goes away, so that the overlay starts a new one.
#
#   tools/test-petaudio-pipewire.sh [--build-dir DIR] [--out-dir DIR]
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when it cannot run. Needs MURASAKI_NESTED_TESTS=1, like the other nested tests.
set -u

SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")
INNER=
if [[ ${1:-} == --inner ]]; then
    INNER=1
    shift
fi
BUILD_DIR=$REPO/build/dev
OUT_DIR=${TMPDIR:-/tmp}/murasaki-petaudio-pipewire-out
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
LOG_DIR=$OUT_DIR/logs
mkdir -p "$LOG_DIR"
PLAYER=$BUILD_DIR/src/audio/murasaki-petaudio
SANDBOX=$BUILD_DIR/src/sandbox/murasaki-sandbox

# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"

if [[ -z $INNER ]]; then
    if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the PipeWire player test"
        exit 77
    fi
    for tool in pipewire pipewire-pulse wireplumber pw-record ffmpeg python3; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    [[ -x $PLAYER ]] || { echo "SKIP: $PLAYER is not built"; exit 77; }
    export MURASAKI_NESTED_MARK="$$-$RANDOM-$RANDOM"
    TMP_ROOT=$(mktemp -d /tmp/mpw.XXXXXX)
    mkdir -m 700 "$TMP_ROOT/rt"
    export MURASAKI_NESTED_TMP=$TMP_ROOT
    unset WAYLAND_DISPLAY DISPLAY DBUS_SESSION_BUS_ADDRESS PULSE_SERVER PIPEWIRE_REMOTE PIPEWIRE_RUNTIME_DIR
    export HOME=$TMP_ROOT/home XDG_RUNTIME_DIR=$TMP_ROOT/rt XDG_CONFIG_HOME=$TMP_ROOT/config XDG_DATA_HOME=$TMP_ROOT/data XDG_CACHE_HOME=$TMP_ROOT/cache XDG_STATE_HOME=$TMP_ROOT/state
    mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$XDG_DATA_HOME" "$XDG_CACHE_HOME" "$XDG_STATE_HOME"
    cleanup() {
        trap - EXIT INT TERM
        nested_kill_marked TERM
        sleep 1
        nested_kill_marked KILL
        rm -rf "$TMP_ROOT"
    }
    trap 'cleanup; exit 130' INT TERM
    trap cleanup EXIT
    timeout "${MURASAKI_NESTED_TIMEOUT:-240}" "$SELF" --inner --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR" </dev/null
    exit $?
fi

# ---- inner half ---------------------------------------------------------------------------------------------------------
[[ $XDG_RUNTIME_DIR == /tmp/mpw.*/rt ]] || { echo "FAIL: the runtime dir is not the private one, refusing to continue"; exit 1; }
failed=0
fail() { echo "   FAIL: $*"; failed=1; }
nested_start_audio
if nested_audio_touched_devices; then
    echo "FAIL: the private audio daemons looked at the real machine's devices (see $LOG_DIR)"
    exit 1
fi
[[ -S $XDG_RUNTIME_DIR/pipewire-0 ]] || { echo "FAIL: no private PipeWire"; exit 1; }

ASSETS=$MURASAKI_NESTED_TMP/assets
mkdir -p "$ASSETS"
timeout 60 ffmpeg -y -v error -f lavfi -i "sine=frequency=440:duration=1.5:sample_rate=48000" -ac 2 "$MURASAKI_NESTED_TMP/tone.wav" || { echo "FAIL: ffmpeg"; exit 1; }
ASSET=$("$PLAYER" --import "$MURASAKI_NESTED_TMP/tone.wav" "$ASSETS" | python3 -c 'import sys, json; print(json.loads(sys.stdin.readline())["asset"])')
[[ ${#ASSET} == 64 ]] || { echo "FAIL: the import made no asset"; exit 1; }

# capture CAPTURE_SECONDS LABEL: records the sink's monitor into $OUT_DIR/LABEL.wav in the background
capture() {
    timeout "$1" pw-record --target murasaki-null-sink -P stream.capture.sink=true --rate 48000 --channels 2 --format s16 "$OUT_DIR/$2.wav" >"$LOG_DIR/record-$2.log" 2>&1 &
    CAPTURE_PID=$!
}
# analyse LABEL: prints "rms=.. hz=.. first_ms=.." of the recording (the level, the pitch by zero crossings of the loud part, and when it became loud)
analyse() {
    python3 - "$OUT_DIR/$1.wav" <<'PY'
import sys, wave, array
w = wave.open(sys.argv[1])
n, rate = w.getnframes(), w.getframerate()
d = array.array("h"); d.frombytes(w.readframes(n))
left = d[0::2]
loud = [i for i, v in enumerate(left) if abs(v) > 800]
if not loud:
    print("rms=0 hz=0 first_ms=-1"); sys.exit()
a, b = loud[0], loud[-1]
seg = left[a:b]
rms = (sum(v * v for v in seg) / max(1, len(seg))) ** 0.5 / 32768
crossings = sum(1 for i in range(1, len(seg)) if seg[i - 1] < 0 <= seg[i])
hz = crossings * rate / max(1, len(seg))
print("rms=%.4f hz=%.0f first_ms=%.0f" % (rms, hz, a * 1000 / rate))
PY
}
field() { sed -n "s/.*$1=\([-0-9.]*\).*/\1/p"; }
num_between() { awk -v v="$1" -v lo="$2" -v hi="$3" 'BEGIN { exit !(v >= lo && v <= hi) }'; }

now_ns() { python3 -c 'import time; print(time.monotonic_ns())'; }

# run_player NAME [sandbox]: starts the player with stdin from a fifo; PLAYER_PID, the fifo and the log are $MURASAKI_NESTED_TMP/NAME.*
run_player() {
    local name=$1 mode=${2:-} fifo=$MURASAKI_NESTED_TMP/$1.fifo
    mkfifo "$fifo"
    if [[ $mode == sandbox ]]; then
        "$SANDBOX" --ro /usr --ro /lib --ro /lib64 --ro /etc --ro "$ASSETS" --ro "$PLAYER" --no-net --require --verbose -- "$PLAYER" --assets "$ASSETS" --idle-exit 4 <>"$fifo" >"$LOG_DIR/$name.out" 2>"$LOG_DIR/$name.err" &
    else
        "$PLAYER" --assets "$ASSETS" --idle-exit 4 <>"$fifo" >"$LOG_DIR/$name.out" 2>"$LOG_DIR/$name.err" &
    fi
    PLAYER_PID=$!
    for _ in $(seq 50); do
        grep -qE "^(ready|unavailable)" "$LOG_DIR/$name.out" && break
        sleep 0.1
    done
}

for mode in plain sandbox; do
    echo "== $mode"
    [[ $mode == sandbox && ! -x $SANDBOX ]] && { echo "   (no murasaki-sandbox built: skipped)"; continue; }
    run_player "player-$mode" "$([[ $mode == sandbox ]] && echo sandbox)"
    FIFO=$MURASAKI_NESTED_TMP/player-$mode.fifo
    grep -q "^ready" "$LOG_DIR/player-$mode.out" || { fail "$mode: the player did not come up: $(cat "$LOG_DIR/player-$mode.out" "$LOG_DIR/player-$mode.err" | tail -3)"; kill "$PLAYER_PID" 2>/dev/null; continue; }
    # a sound now
    capture 4 "$mode-now"
    sleep 0.6
    echo "play 1 1 $ASSET 0 800 0 0" >"$FIFO"
    wait "$CAPTURE_PID" 2>/dev/null
    stats=$(analyse "$mode-now")
    echo "   now: $stats"
    num_between "$(echo "$stats" | field rms)" 0.03 0.5 || fail "$mode: the sound is not at the sink (level)"
    num_between "$(echo "$stats" | field hz)" 420 460 || fail "$mode: the pitch is not 440 Hz"
    grep -q "^started 1 1" "$LOG_DIR/player-$mode.out" || fail "$mode: the player never said it started"
    # a sound scheduled 1.2 s ahead on the player's clock: it must not be heard before
    capture 5 "$mode-later"
    sleep 0.8
    at=$(python3 -c "import time; print(time.monotonic_ns() + 1200_000_000)")
    echo "play 2 1 $ASSET 0 800 0 $at" >"$FIFO"
    wait "$CAPTURE_PID" 2>/dev/null
    stats=$(analyse "$mode-later")
    echo "   scheduled 1.2 s after the recording's 0.8 s mark: $stats"
    # the recording started at about 0 and the sound was scheduled for about 2.0 s after it (measured 2012 to 2018 ms in six runs; the window leaves for a loaded machine)
    num_between "$(echo "$stats" | field first_ms)" 1900 2150 || fail "$mode: the scheduled sound did not start near its time"
    echo "quit" >"$FIFO"
    wait "$PLAYER_PID" 2>/dev/null
    rc=$?
    [[ $rc == 0 ]] || fail "$mode: the player left with status $rc"
done

echo "== PipeWire goes away"
run_player "player-dead"
FIFO=$MURASAKI_NESTED_TMP/player-dead.fifo
echo "play 3 1 $ASSET 0 500 1 0" >"$FIFO"
sleep 1
for pid in $(ls /proc | grep -E '^[0-9]+$'); do
    if { tr '\0' '\n' <"/proc/$pid/environ" | grep -qxF "MURASAKI_NESTED_MARK=$MURASAKI_NESTED_MARK"; } 2>/dev/null && [[ $(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null) == "pipewire "* || $(cat "/proc/$pid/comm" 2>/dev/null) == pipewire ]]; then
        kill -TERM "$pid" 2>/dev/null
    fi
done
for _ in $(seq 40); do
    kill -0 "$PLAYER_PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$PLAYER_PID" 2>/dev/null; then
    fail "the player is still running after PipeWire ended"
    kill "$PLAYER_PID" 2>/dev/null
else
    wait "$PLAYER_PID" 2>/dev/null
    rc=$?
    echo "   the player left with status $rc"
    [[ $rc == 4 ]] || fail "the player left with status $rc, not 4"
    grep -q "^dead" "$LOG_DIR/player-dead.out" || fail "the player did not say it was dead"
fi

((failed)) && { echo "RESULT: FAIL (recordings and logs in $OUT_DIR)"; exit 1; }
echo "RESULT: PASS (recordings and logs in $OUT_DIR)"
