#!/usr/bin/env bash
# Measures what a Wayland client can know and do about the pointer, per compositor, with a test layer surface (not the
# overlay) in a nested, headless compositor. docs/pets/pointer-facts.md has the numbers.
#
#   U5  pointer motion can be injected (KWin: org_kde_kwin_fake_input; sway and Hyprland: zwlr_virtual_pointer_v1, which
#       is also what gives their headless seat a pointer: without one, swaymsg's seat cursor reaches no client)
#   U1  enter / leave when the input region changes with the pointer inside, without motion
#   U2  CPU of the compositor and of the client with a 512-rectangle region replaced at 30 Hz
#   U3  Qt's setMask with 512 and 2048 rectangles reaches the compositor intact (hit tests at every cell centre)
#   U4  setLayer on a mapped surface: stacking order afterwards, unmaps, configures, the gaps of a frame log
#   3.3 the drag layout: the small surface grows to the whole output on a press (what the events say, what is drawn)
#   KWin only: src/overlay/kwin-pet-cursor.js, loaded into the nested KWin over org.kde.kwin.Scripting, reports the injected pointer to a sink
#       that owns the overlay's D-Bus name (rate, last position, nothing once unloaded)
#
# Everything is private: runtime dir, HOME, XDG dirs, D-Bus session, the Wayland socket (wayland-mon.*). The fake-input
# client refuses any other socket. Every process carries MURASAKI_NESTED_MARK, which is also how leftovers are killed.
#
#   tools/test-overlay-pointer-nested.sh [--build-dir DIR] [--out-dir DIR] [--compositors kwin,sway,hyprland]
#
# Exit status: 0 passed, 1 a check failed, 77 (ctest skip) when nothing could run. Needs MURASAKI_NESTED_TESTS=1, like
# the other nested tests. A compositor that is not installed is skipped (its cells stay "unverified" in the document);
# labwc is not run (it offers the same virtual pointer; not tried).
set -u

SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")

INNER=
if [[ ${1:-} == --inner ]]; then
    INNER=$2
    shift 2
fi

BUILD_DIR=$REPO/build/dev
OUT_DIR=${TMPDIR:-/tmp}/murasaki-overlay-pointer-nested-out
COMPOSITORS=kwin,sway,hyprland
while (($#)); do
    case $1 in
    --build-dir) BUILD_DIR=$2; shift 2 ;;
    --out-dir) OUT_DIR=$2; shift 2 ;;
    --compositors) COMPOSITORS=$2; shift 2 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
BUILD_DIR=$(readlink -f "$BUILD_DIR")
mkdir -p "$OUT_DIR"
OUT_DIR=$(readlink -f "$OUT_DIR")
PROBE=$BUILD_DIR/tools/overlay-pointer-probe/murasaki-pointer-probe
SINK=$BUILD_DIR/tools/overlay-pointer-probe/murasaki-pointer-sink
FAKEINPUT=$BUILD_DIR/tools/overlay-pointer-probe/murasaki-pointer-fakeinput

# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"
kill_marked() { nested_kill_marked "$@"; }

compositor_binary() {
    case $1 in
    kwin) echo kwin_wayland ;;
    sway) echo sway ;;
    hyprland) echo Hyprland ;;
    *) echo "" ;;
    esac
}

if [[ -z $INNER ]]; then
    # ---- outer half: one private session per compositor -----------------------------------------------------
    if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then
        echo "SKIP: set MURASAKI_NESTED_TESTS=1 to run the nested pointer test"
        exit 77
    fi
    for tool in dbus-run-session python3 flock; do
        command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }
    done
    for bin in "$PROBE" "$FAKEINPUT"; do
        [[ -x $bin ]] || { echo "SKIP: $bin is not built"; exit 77; }
    done
    mkdir -p "$OUT_DIR/logs"
    ran=0 failed=0
    IFS=, read -ra LIST <<<"$COMPOSITORS"
    for comp in "${LIST[@]}"; do
        binary=$(compositor_binary "$comp")
        if [[ -z $binary ]] || ! command -v "$binary" >/dev/null; then
            echo "== $comp: not installed, skipped (unverified)"
            continue
        fi
        if [[ $comp == kwin ]] && ! command -v spectacle >/dev/null; then
            echo "== kwin: spectacle not found, skipped"
            continue
        fi
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
        export QT_QPA_PLATFORM=wayland QT_ACCESSIBILITY=0
        cleanup() {
            trap - EXIT INT TERM
            kill_marked TERM
            sleep 1
            kill_marked KILL
            rm -rf "$TMP_ROOT"
        }
        trap 'cleanup; exit 130' INT TERM
        trap cleanup EXIT
        echo "== $comp"
        timeout "${MURASAKI_NESTED_TIMEOUT:-420}" dbus-run-session -- "$SELF" --inner "$comp" \
            --build-dir "$BUILD_DIR" --out-dir "$OUT_DIR" </dev/null 2>"$OUT_DIR/logs/session-$comp.log"
        rc=$?
        cleanup
        trap - EXIT INT TERM
        ran=$((ran + 1))
        ((rc != 0)) && { echo "== $comp: FAILED (rc=$rc, see $OUT_DIR/logs)"; failed=1; }
    done
    ((ran == 0)) && { echo "SKIP: no compositor to run"; exit 77; }
    ((failed)) && { echo "RESULT: FAIL"; exit 1; }
    echo "RESULT: PASS (facts in $OUT_DIR/facts-*.txt)"
    exit 0
fi

# ---- inner half: runs inside the private bus, for one compositor -------------------------------------------------
COMP=$INNER
LOG_DIR=$OUT_DIR/logs
FACTS=$OUT_DIR/facts-$COMP.txt
: >"$FACTS"
trap 'kill_marked TERM; sleep 1; kill_marked KILL' EXIT
case $DBUS_SESSION_BUS_ADDRESS in
unix:path=/tmp/dbus-* | unix:abstract=/tmp/dbus-*) ;;
*) echo "FAIL: the session bus is not a private /tmp/dbus-* bus, refusing to continue"; exit 1 ;;
esac
[[ $HOME == "$MURASAKI_NESTED_TMP"/* ]] || { echo "FAIL: HOME is not private, refusing to continue"; exit 1; }

failed=0
fail() { echo "   FAIL: $*"; failed=1; }
# The facts are recorded for every compositor; only KWin's are pinned (they are what the design stands on), the others
# are measured for the document and may legitimately differ.
pinned() { [[ $COMP == kwin ]] && fail "$*"; return 0; }
fact() { echo "   FACT $*"; echo "$*" >>"$FACTS"; }

SOCKET=wayland-$(basename "$MURASAKI_NESTED_TMP")
OUTW=1280 OUTH=720
export WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 AQ_BACKENDS=headless
case $COMP in
kwin)
    export XDG_CURRENT_DESKTOP=KDE XDG_SESSION_TYPE=wayland KDE_FULL_SESSION=true
    KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 kwin_wayland --virtual --socket "$SOCKET" --width $OUTW --height $OUTH \
        --no-lockscreen --no-global-shortcuts >"$LOG_DIR/compositor-kwin.log" 2>&1 &
    ;;
sway)
    export XDG_CURRENT_DESKTOP=sway
    mkdir -p "$XDG_CONFIG_HOME/sway"
    echo "output * resolution ${OUTW}x${OUTH}" >"$XDG_CONFIG_HOME/sway/config"
    sway -c "$XDG_CONFIG_HOME/sway/config" >"$LOG_DIR/compositor-sway.log" 2>&1 &
    for _ in $(seq 100); do
        SWAYSOCK=$(ls "$XDG_RUNTIME_DIR"/sway-ipc.*.sock 2>/dev/null | head -1)
        [[ -n $SWAYSOCK ]] && break
        sleep 0.1
    done
    export SWAYSOCK
    SOCKET=$(ls "$XDG_RUNTIME_DIR" | grep -E '^wayland-[0-9]+$' | head -1)
    ;;
hyprland)
    # Hyprland (aquamarine) cannot start headless alone: it runs as a window of a virtual KWin, on its own bus so that
    # nothing here sees org.kde.KWin (as in tools/test-wlroots-nested.sh). The host socket is not a wayland-N name.
    command -v kwin_wayland >/dev/null || { echo "FAIL: Hyprland needs kwin_wayland as its nested host"; exit 1; }
    HOST_SOCKET=hyprhost-$(basename "$MURASAKI_NESTED_TMP")   # nested_kill_marked finds it by this name
    KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 dbus-run-session -- kwin_wayland --virtual --socket "$HOST_SOCKET" --width 1920 \
        --height 1080 --no-lockscreen >"$LOG_DIR/host-hyprland.log" 2>&1 &
    for _ in $(seq 100); do
        [[ -S $XDG_RUNTIME_DIR/$HOST_SOCKET ]] && break
        sleep 0.1
    done
    [[ -S $XDG_RUNTIME_DIR/$HOST_SOCKET ]] || { echo "FAIL: the KWin host did not come up (see $LOG_DIR/host-hyprland.log)"; exit 1; }
    export XDG_CURRENT_DESKTOP=Hyprland
    mkdir -p "$XDG_CONFIG_HOME/hypr"
    printf 'monitor=,%sx%s@60,0x0,1\nmisc:disable_hyprland_logo = true\n' $OUTW $OUTH >"$XDG_CONFIG_HOME/hypr/hyprland.conf"
    WAYLAND_DISPLAY="$HOST_SOCKET" Hyprland -c "$XDG_CONFIG_HOME/hypr/hyprland.conf" >"$LOG_DIR/compositor-hyprland.log" 2>&1 &
    for _ in $(seq 100); do
        SOCKET=$(ls "$XDG_RUNTIME_DIR" 2>/dev/null | grep -E '^wayland-[0-9]+$' | head -1)
        [[ -n $SOCKET ]] && break
        sleep 0.1
    done
    sig=$(ls "$XDG_RUNTIME_DIR/hypr" 2>/dev/null | head -1)
    export HYPRLAND_INSTANCE_SIGNATURE=$sig
    ;;
esac
for _ in $(seq 100); do
    [[ -n $SOCKET && -S $XDG_RUNTIME_DIR/$SOCKET ]] && break
    sleep 0.1
done
[[ -n $SOCKET && -S $XDG_RUNTIME_DIR/$SOCKET ]] || { echo "FAIL: $COMP did not come up (see $LOG_DIR/compositor-$COMP.log)"; exit 1; }
export WAYLAND_DISPLAY=$SOCKET

comp_pid() {
    case $COMP in
    kwin) pgrep -f -- "--socket $SOCKET " | head -1 ;;
    sway) pgrep -n -x sway ;;
    hyprland) pgrep -n -x Hyprland ;;
    esac
}
CPID=$(comp_pid)
[[ -n $CPID ]] || { echo "FAIL: cannot find the $COMP process"; exit 1; }

# ---- driving the probe and the pointer ---------------------------------------------------------------------------
FIFO=$MURASAKI_NESTED_TMP/probe.fifo
mkfifo "$FIFO"
PLOG=$LOG_DIR/probe-$COMP.log
: >"$PLOG"
SX=200 SY=150 SW=200 SH=200          # the small window of the probe, in output coordinates
CX=$((SX + SW / 2)) CY=$((SY + SH / 2))
PARK_X=900 PARK_Y=620                # where the pointer waits, far from every probe

# One injector connection for the whole run: every new connection adds an input device, the compositor announces the seat
# again and Qt recreates its wl_pointer, which drops the pointer focus (and the leave event a test is waiting for). It
# starts before the probe: on sway and Hyprland its virtual pointer is the seat's only pointer.
IFIFO=$MURASAKI_NESTED_TMP/inject.fifo
ILOG=$LOG_DIR/inject-$COMP.log
mkfifo "$IFIFO"
: >"$ILOG"
"$FAKEINPUT" --extent ${OUTW}x${OUTH} --stdin <>"$IFIFO" >>"$ILOG" 2>&1 &

sleep 1
"$PROBE" --fifo "$FIFO" --x $SX --y $SY --w $SW --h $SH >>"$PLOG" 2>&1 &
PROBE_PID=$!
P() { echo "$*" >"$FIFO"; }   # the FIFO is opened read-write by the probe, so a writer never blocks for long
log_lines() { wc -l <"$PLOG"; }
since() { tail -n +$(($1 + 1)) "$PLOG"; }

# inject TOKENS...: abs X Y | down | up | sleep MS (the same words the fake-input client takes)
inject() {
    local id deadline=$((SECONDS + 120))
    id=$(date +%s%N)   # unique, also from the subshell of a background call
    echo "$* mark $id" >"$IFIFO"
    while ((SECONDS < deadline)); do
        grep -qx "mark $id" "$ILOG" && return 0
        grep -q "fakeinput:" "$ILOG" && return 1
        sleep 0.05
    done
    return 1
}
can_shoot() { [[ $COMP == kwin ]]; }   # spectacle talks to KWin only (grim would do for wlroots: not run here)

wait_log() { # PATTERN [SECONDS]: waits until the probe log has the pattern
    local deadline=$((SECONDS + ${2:-10}))
    while ((SECONDS < deadline)); do
        grep -q -- "$1" "$PLOG" && return 0
        sleep 0.1
    done
    return 1
}
wait_log "expose exposed=1" 20 || { echo "FAIL: the probe did not map (see $PLOG)"; tail -5 "$PLOG"; exit 1; }
sleep 1

# lag_report FROM_LINE CMD_PATTERN: prints "kind@ms" for every enter/leave/move event after the command line
events_after() { # FROM_LINE PATTERN -> lines "ms event ..." relative to the time of the command line
    python3 - "$PLOG" "$1" "$2" <<'PY'
import re, sys
log, start, pat = sys.argv[1], int(sys.argv[2]), sys.argv[3]
lines = open(log).read().splitlines()[start:]
t0 = None
out = []
for l in lines:
    m = re.match(r"t=(\d+) (.*)", l)
    if not m:
        continue
    t, rest = int(m.group(1)), m.group(2)
    if t0 is None:
        if re.search(pat, rest):
            t0 = t
        continue
    if rest.split()[0] in ("enter", "leave", "move", "press", "release"):
        out.append("+%dms %s" % (t - t0, rest))
print("; ".join(out) if out else "no event")
PY
}

# ---- U5: can the test move the pointer? --------------------------------------------------------------------------
echo "== U5 pointer injection"
P "region disc"
sleep 0.5
m=$(log_lines)
inject abs $((CX + 10)) $((CY + 5)) sleep 300 || fail "the injector failed"
sleep 0.3
got=$(since "$m" | grep -E '^t=[0-9]+ enter' | head -1)
if [[ -n $got ]]; then
    fact "U5 $COMP: injected motion reaches the probe: $(echo "$got" | sed 's/^t=[0-9]* //') (expected enter 110 105)"
else
    fact "U5 $COMP: injected motion did NOT reach the probe"
    fail "no pointer injection on $COMP"
    exit 1
fi
inject abs $PARK_X $PARK_Y sleep 300

# ---- U1: region changes with the pointer inside ------------------------------------------------------------------
echo "== U1 enter/leave on a region change, no motion"
NUDGE=0
nudge() { NUDGE=$((1 - NUDGE)); inject abs $((CX + NUDGE)) $CY sleep 300; }   # one pixel, to the other side each time
u1() { # NAME REGION-COMMAND EXPECTED-EVENT-AFTER-MOTION: the region changes, 0.8 s of nothing, one pixel of motion
    local name=$1 cmd=$2 want=$3 m quiet moved
    m=$(log_lines)
    P "$cmd"
    sleep 0.8
    quiet=$(events_after "$m" "cmd region")
    fact "U1 $COMP: '$cmd' ($name): no motion: $quiet"
    m=$(log_lines)
    nudge
    moved=$(since "$m" | grep -E 'enter|leave|move' | sed 's/^t=[0-9]* //' | tr '\n' ';')
    fact "U1 $COMP: '$cmd' ($name): then one pixel of motion: $moved"
    [[ $quiet == "no event" ]] || pinned "U1 '$cmd': KWin sent an event without motion ($quiet)"
    [[ $moved == "$want"* ]] || pinned "U1 '$cmd': expected '$want' after the motion, got '$moved'"
}
P "region disc"; sleep 0.5
m=$(log_lines)
inject abs $CX $CY sleep 400
fact "U1 $COMP: pointer moved onto the disc: $(since "$m" | grep -E 'enter|leave' | sed 's/^t=[0-9]* //' | tr '\n' ';')"
u1 "pointer inside, region now excludes it" "region rect 0 0 20 20" leave
u1 "pointer outside the region, region grows to contain it" "region disc" "enter"
u1 "pointer inside, region emptied (transparent for input)" "region empty" leave
u1 "pointer outside, region back to infinite" "region none" "enter"
# The press that follows a region change without motion: does it still reach the (stale) focus?
P "region disc"; sleep 0.5
nudge
P "region rect 0 0 20 20"; sleep 0.8
m=$(log_lines)
inject down sleep 100 up sleep 400
got=$(since "$m" | grep -E 'enter|leave|press|release' | sed 's/^t=[0-9]* //' | tr '\n' ';')
fact "U1 $COMP: pointer was inside; region now excludes it; a click with no motion: $got"
[[ $got == "leave;" ]] || pinned "U1 click with the pointer outside the region: expected 'leave;' only, got '$got'"
P "region disc"; sleep 0.8
m=$(log_lines)
inject down sleep 100 up sleep 400
got=$(since "$m" | grep -E 'enter|leave|press|release' | sed 's/^t=[0-9]* //' | tr '\n' ';')
fact "U1 $COMP: region then grows back over the pointer; a click with no motion: $got"
[[ $got == enter*";press"*";release"* ]] || pinned "U1 click over a region that grew back: expected enter, press, release, got '$got'"
# The same release by unmapping instead of emptying the region: an unmapped surface can not keep the pointer focus on any
# compositor, so the click with no motion must go below; mapped again, the next click with no motion reaches it again.
nudge
P "unmap"; sleep 0.8
m=$(log_lines)
inject down sleep 100 up sleep 400
got=$(since "$m" | grep -E 'enter|leave|press|release' | sed 's/^t=[0-9]* //' | tr '\n' ';')
fact "U1 $COMP: pointer was inside; surface unmapped; a click with no motion: ${got:-no event}"
[[ $got != *press* ]] || fail "U1 click on an unmapped surface reached it: '$got'"
P "map"; sleep 0.8
m=$(log_lines)
inject down sleep 100 up sleep 400
got=$(since "$m" | grep -E 'enter|leave|press|release' | sed 's/^t=[0-9]* //' | tr '\n' ';')
fact "U1 $COMP: mapped again under the still pointer; a click with no motion: ${got:-no event}"
P "region none"; sleep 0.5
inject abs $PARK_X $PARK_Y sleep 300

# ---- U3: Qt's setMask with many rectangles ---------------------------------------------------------------------
echo "== U3 setMask with many rectangles"
u3() { # COLS ROWS SAMPLES
    local cols=$1 rows=$2 samples=$3 m args
    P "region none"; sleep 0.3
    m=$(log_lines)
    P "region grid $cols $rows 0"
    sleep 1
    args=$(python3 - "$cols" "$rows" "$samples" "$SX" "$SY" "$SW" "$SH" "$MURASAKI_NESTED_TMP/u3-expected.txt" <<'PY'
import random, sys
cols, rows, samples, sx, sy, sw, sh = (int(v) for v in sys.argv[1:8])
cw, ch = sw / cols, sh / rows
cells = [(c, r) for r in range(rows) for c in range(cols)]
random.Random(7).shuffle(cells)
cells = sorted(cells[:samples], key=lambda cr: (cr[1], cr[0]))   # row by row: short moves
toks, exp = [], []
for c, r in cells:
    lx = int(c * cw) + max(1, int(cw)) // 2
    ly = int(r * ch) + max(1, int(ch)) // 2
    toks += ["abs", str(sx + lx), str(sy + ly), "sleep", "6"]
    exp.append("%d %d %d" % (lx, ly, 1 if (c + r) % 2 == 0 else 0))
open(sys.argv[8], "w").write("\n".join(exp) + "\n")
print(" ".join(toks))
PY
)
    inject abs $PARK_X $PARK_Y sleep 100
    # shellcheck disable=SC2086
    inject $args sleep 300
    local rects
    rects=$(since "$m" | grep -m1 'cmd region grid' | sed 's/.*rects=//')
    python3 - "$PLOG" "$m" "$MURASAKI_NESTED_TMP/u3-expected.txt" "$rects" "$cols" "$rows" "$COMP" "$FACTS" <<'PY'
import re, sys
log, start, expf, rects, cols, rows, comp, facts = sys.argv[1:9]
moves = set()
for l in open(log).read().splitlines()[int(start):]:
    m = re.match(r"t=\d+ (?:move|enter) (-?[\d.]+) (-?[\d.]+)", l)
    if m:
        moves.add((round(float(m.group(1))), round(float(m.group(2)))))
exp = [tuple(int(v) for v in l.split()) for l in open(expf).read().splitlines()]
on = sum(1 for _, _, e in exp if e)
hit_on = sum(1 for x, y, e in exp if e and (x, y) in moves)
hit_off = sum(1 for x, y, e in exp if not e and (x, y) in moves)
msg = "U3 %s: grid %sx%s, probe committed %s rectangles; %d sample points on rectangles, %d reached the client (%.1f%%); %d of %d points off the rectangles reached it (must be 0)" % (
    comp, cols, rows, rects, on, hit_on, 100.0 * hit_on / max(1, on), hit_off, len(exp) - on)
print("   FACT " + msg)
open(facts, "a").write(msg + "\n")
sys.exit(0 if hit_on == on and hit_off == 0 else 1)
PY
    [[ $? -eq 0 ]] || pinned "U3 grid $cols x $rows: the compositor's region differs from what the probe committed"
    inject abs $PARK_X $PARK_Y sleep 200
}
u3 32 32 400   # 512 rectangles
u3 64 64 600   # 2048 rectangles
P "region none"; sleep 0.3

# ---- U2: CPU of a region replaced at 30 Hz ---------------------------------------------------------------------
echo "== U2 cost of a region replaced at 30 Hz (20 s each)"
ticks=$(getconf CLK_TCK)
cpu() { awk '{print $14 + $15}' "/proc/$1/stat"; }   # utime + stime, in clock ticks
u2() { # NAME COMMAND
    local name=$1 cmd=$2 c0 p0 c1 p1
    P "$cmd"
    sleep 2
    c0=$(cpu "$CPID"); p0=$(cpu "$PROBE_PID")
    sleep 20
    c1=$(cpu "$CPID"); p1=$(cpu "$PROBE_PID")
    ((c1 - c0 < 20 * ticks / 2)) || pinned "U2 '$name': the compositor used more than half a core"
    fact "U2 $COMP: $name: compositor $(awk -v a="$c0" -v b="$c1" -v t="$ticks" 'BEGIN { printf "%.1f%%", (b - a) / t / 20 * 100 }') of one core, client $(awk -v a="$p0" -v b="$p1" -v t="$ticks" 'BEGIN { printf "%.1f%%", (b - a) / t / 20 * 100 }') of one core (20 s)"
}
P "region disc"
P "tick 0 2 1"
# Two rounds in opposite orders: the machine is shared, one sample is noise.
u2 "idle, static region" "region disc"
u2 "30 Hz, 1 rectangle replaced by another (2x1 cells)" "tick 30 2 1"
u2 "30 Hz, 128 rectangles (16x16 checkerboard, two parities)" "tick 30 16 16"
u2 "30 Hz, 512 rectangles (32x32 checkerboard, two parities)" "tick 30 32 32"
u2 "30 Hz, 512 rectangles again" "tick 30 32 32"
u2 "30 Hz, 1 rectangle again" "tick 30 2 1"
P "tick 0 2 1"; sleep 0.5

# ---- U4: setLayer at runtime -----------------------------------------------------------------------------------------
echo "== U4 setLayer on a mapped surface"
if can_shoot; then
    mkfifo "$MURASAKI_NESTED_TMP/b.fifo"
    "$PROBE" --fifo "$MURASAKI_NESTED_TMP/b.fifo" --x $((SX + 50)) --y $((SY + 50)) --w $SW --h $SH --layer top --color 0050ff \
        >"$LOG_DIR/probe-b-$COMP.log" 2>&1 &
    sleep 2
    OUT_SHOT=$OUT_DIR
    shot() { # NAME: spectacle does not overwrite a file that exists
        rm -f "$OUT_SHOT/$1.png"
        timeout 60 spectacle -b -n -f -o "$OUT_SHOT/$1.png" >>"$LOG_DIR/spectacle.log" 2>&1
        [[ -s $OUT_SHOT/$1.png ]]
    }
    pixel() { # NAME X Y -> "r g b"
        python3 -c 'import sys; from PIL import Image; print(*Image.open(sys.argv[1]).convert("RGB").getpixel((int(sys.argv[2]), int(sys.argv[3]))))' "$OUT_SHOT/$1.png" "$2" "$3"
    }
    colour_name() { # r g b -> orange | blue | other
        awk '{ if ($1 > 200 && $2 > 90 && $2 < 190 && $3 < 80) print "orange(A)"; else if ($3 > 200 && $1 < 60) print "blue(B)"; else print "other(" $1 "," $2 "," $3 ")" }'
    }
    P "region none"
    OX=$((SX + 125)) OY=$((SY + 125))   # inside both discs
    first=
    for _ in $(seq 20); do
        shot u4-start && [[ $(pixel u4-start $OX $OY) != "0 0 0" ]] && { first=1; break; }
        sleep 0.5
    done
    if [[ -z $first ]]; then
        fact "U4 $COMP: unverified: the compositor never showed the probes in a screenshot"
    else
        fact "U4 $COMP: overlay(A) over top(B), pixel in both: $(pixel u4-start $OX $OY | colour_name)"
        for layer in bottom overlay top overlay; do
            P "layer $layer"
            sleep 0.7
            shot "u4-$layer" || { fail "no screenshot (u4-$layer)"; continue; }
            got=$(pixel "u4-$layer" $OX $OY | colour_name)
            fact "U4 $COMP: A moved to '$layer' while B stays on 'top': pixel in both is $got"
            case $layer in
            bottom | top) [[ $got == "blue(B)" ]] || pinned "U4 A on $layer should be under B (got $got)" ;;
            overlay) [[ $got == "orange(A)" ]] || pinned "U4 A on overlay should be over B (got $got)" ;;
            esac
        done
        # The frame log of A repainting at 60 Hz, once idle and once while its layer changes 20 times at 10 Hz. No
        # screenshots in between: taking one stalls the compositor.
        m=$(log_lines)
        P "animate on"; sleep 4
        P "ping base-end"; sleep 0.2
        for _ in $(seq 10); do P "layer bottom"; sleep 0.1; P "layer overlay"; sleep 0.1; done
        sleep 0.3
        P "ping layer-end"; sleep 0.3
        P "animate off"; sleep 0.3
        python3 - "$PLOG" "$m" "$COMP" "$FACTS" <<'PY'
import re, sys
log, start, comp, facts = sys.argv[1], int(sys.argv[2]), sys.argv[3], sys.argv[4]
seg, phase = {"idle": [], "layer": []}, None
unmaps = resizes = calls = 0
for l in open(log).read().splitlines()[start:]:
    m = re.match(r"t=(\d+) (.*)", l)
    if not m:
        continue
    t, rest = int(m.group(1)), m.group(2)
    if rest.startswith("cmd animate on"):
        phase = "idle"
    elif rest.startswith("pong base-end"):
        phase = "layer"
    elif rest.startswith("pong layer-end"):
        phase = None
    if phase is None:
        continue
    if rest.startswith("paint"):
        seg[phase].append(t)
    elif rest.startswith("expose exposed=0"):
        unmaps += 1
    elif rest.startswith("resize"):
        resizes += 1
    elif rest.startswith("cmd layer"):
        calls += 1
def stats(ts):
    gaps = [b - a for a, b in zip(ts, ts[1:])]
    med = sorted(gaps)[len(gaps) // 2] if gaps else 0
    return "%d paints, median gap %d ms, max gap %d ms, gaps over 40 ms: %d" % (len(ts), med, max(gaps) if gaps else 0, sum(1 for g in gaps if g > 40))
msg = "U4 %s: frame log of a 60 Hz repaint: no layer change: %s; during %d setLayer calls on the mapped surface: %s; unmaps (expose exposed=0): %d; resize events: %d" % (
    comp, stats(seg["idle"]), calls, stats(seg["layer"]), unmaps, resizes)
print("   FACT " + msg)
open(facts, "a").write(msg + "\n")
sys.exit(0 if unmaps == 0 and resizes == 0 and max(seg["layer"][i + 1] - seg["layer"][i] for i in range(len(seg["layer"]) - 1)) < 100 else 1)
PY
        [[ $? -eq 0 ]] || pinned "U4 setLayer unmapped, resized or stalled the surface (see the frame log fact)"
    fi
    echo "quit" >"$MURASAKI_NESTED_TMP/b.fifo"
else
    fact "U4 $COMP: stacking order unverified (no screenshot tool for this compositor here); the layer request itself: $( m=$(log_lines); P 'layer top'; sleep 0.5; P 'layer overlay'; sleep 0.5; since "$m" | grep -cE 'expose exposed=0') unmaps"
fi

# ---- 3.3: grow on press -------------------------------------------------------------------------------------------------
echo "== 3.3 the small surface grows to the whole output on a press"
P "region disc"
P "layer overlay"
P "expandonpress on"
sleep 0.5
if [[ $COMP == kwin ]]; then
    # A KWin script that logs every geometry change of every window with the wall clock: the compositor's side.
    cat >"$MURASAKI_NESTED_TMP/geom.js" <<'JS'
function watch(w) {
    if (w.resourceClass === undefined)
        return;
    w.frameGeometryChanged.connect(function () {
        var g = w.frameGeometry;
        print("GEOM " + Date.now() + " " + w.caption + " " + g.x + "," + g.y + " " + g.width + "x" + g.height);
    });
}
workspace.windowList().forEach(watch);
workspace.windowAdded.connect(watch);
JS
    sid=$(timeout 20 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$MURASAKI_NESTED_TMP/geom.js" u33geom 2>/dev/null | awk '{print $2}')
    [[ -n $sid ]] && timeout 20 busctl --user call org.kde.KWin "/Scripting/Script$sid" org.kde.kwin.Script run >/dev/null 2>&1
    echo "   geometry script id: ${sid:-none}"
fi
inject abs $PARK_X $PARK_Y sleep 200
# (a) slow: the motion after the press comes 400 ms later
m=$(log_lines)
inject abs $((CX + 20)) $((CY + 10)) sleep 100 down sleep 400 abs 500 400 sleep 100 abs 700 500 sleep 100 up sleep 500
got=$(since "$m" | grep -vE '^t=[0-9]+ (expose|resize)' | sed 's/^t=\([0-9]*\) /\1ms /' | tr '\n' ';')
fact "3.3 $COMP slow drag (press at output (320,260), 400 ms, moves to output (500,400) and (700,500), release): $got"
[[ $got == *"move 500 400 buttons=1"*"move 700 500 buttons=1"*"release 700 500"* ]] || pinned "3.3 slow drag: the moves or the release did not arrive in output coordinates"
# (b) fast: 40 one-pixel moves, 3 ms apart, start right after the press, before the compositor can have reconfigured
# the surface. Every event is classified by the frame its coordinates are in (the old small surface, or the output).
inject abs $PARK_X $PARK_Y sleep 200
m=$(log_lines)
gx=$((CX + 20)) gy=$((CY + 10))
toks=
for i in $(seq 1 40); do toks+=" abs $((gx + i)) $((gy + i)) sleep 3"; done
inject abs $gx $gy sleep 100 down $toks sleep 300 abs 500 400 sleep 100 up sleep 500
python3 - "$PLOG" "$m" "$SX" "$SY" "$gx" "$gy" "$COMP" "$FACTS" <<'PY'
import re, sys
log, start, sx, sy, gx, gy, comp, facts = sys.argv[1:9]
start, sx, sy, gx, gy = int(start), int(sx), int(sy), int(gx), int(gy)
press = None
old = new = other = 0
first_new = None
old_at = []
first_big_paint = None
leaves = enters = 0
for l in open(log).read().splitlines()[start:]:
    m = re.match(r"t=(\d+) (.*)", l)
    if not m:
        continue
    t, rest = int(m.group(1)), m.group(2)
    if rest.startswith("press"):
        press = t
    if press is None:
        continue
    if rest.startswith("paint 1280") and first_big_paint is None:
        first_big_paint = t - press
    if rest.startswith("leave") and first_big_paint is None:
        leaves += 1
    if rest.startswith("enter") and first_big_paint is None:
        enters += 1
    mm = re.match(r"move (-?[\d.]+) (-?[\d.]+) buttons=1", rest)
    if mm and t - press < 400:
        x, y = round(float(mm.group(1))), round(float(mm.group(2)))
        if (x - (gx - sx)) == (y - (gy - sy)) and (x - (gx - sx)) >= 1 and x < 260:
            old += 1          # local to the small surface
            old_at.append(t - press)
        elif (x - gx) == (y - gy) and 1 <= x - gx <= 40:
            new += 1          # in the output frame
            if first_new is None:
                first_new = t - press
        else:
            other += 1
msg = "3.3 %s fast drag: after the press, 40 moves 3 ms apart (120 ms): %d arrived in the old small-surface frame (%s ms after the press), %d in the output frame (first one %s ms after the press), %d unclassified; first paint at 1280x720 %s ms after the press; leave/enter around the grow: %d/%d" % (
    comp, old, old_at, new, first_new, other, first_big_paint, leaves, enters)
print("   FACT " + msg)
open(facts, "a").write(msg + "\n")
sys.exit(0 if old <= 6 and new >= 30 and other == 0 and not leaves and not enters else 1)
PY
[[ $? -eq 0 ]] || pinned "3.3 the events around the grow are not what the drag layout assumes (see the fast drag fact)"
if [[ $COMP == kwin && -n ${sid:-} ]]; then
    fact "3.3 $COMP: compositor-side geometry of the probe windows (wall clock ms, KWin script): $(grep -a 'GEOM' "$LOG_DIR/compositor-kwin.log" | sed 's/.*GEOM //' | tail -30 | tr '\n' ';')"
fi
# what is drawn while the button is down: the disc must not jump
if can_shoot; then
    inject abs $PARK_X $PARK_Y sleep 200
    shot u33-before || true
    (inject abs $((CX + 20)) $((CY + 10)) sleep 100 down sleep 6000 abs $((CX + 40)) $((CY + 30)) sleep 100 up sleep 300) &
    INJ=$!
    sleep 2.5
    shot u33-during || true
    wait "$INJ"
    sleep 0.5
    shot u33-after || true
    python3 - "$OUT_DIR" "$COMP" "$FACTS" <<'PY'
import sys
from PIL import Image
d, comp, facts = sys.argv[1:4]
def centroid(name):
    try:
        img = Image.open("%s/%s.png" % (d, name)).convert("RGB")
    except OSError:
        return None
    xs = ys = n = 0
    w, h = img.size
    px = img.load()
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            r, g, b = px[x, y]
            if r > 200 and 90 < g < 190 and b < 80:
                xs += x; ys += y; n += 1
    return (xs / n, ys / n, n) if n else None
c = [centroid(n) for n in ("u33-before", "u33-during", "u33-after")]
msg = "3.3 %s: orange disc centroid before the press / while the button is down (surface grown) / after the release: %s" % (
    comp, " / ".join("(%.1f, %.1f) %d samples" % v if v else "not found" for v in c))
print("   FACT " + msg)
open(facts, "a").write(msg + "\n")
PY
else
    fact "3.3 $COMP: the drawn result is unverified (no screenshot tool for this compositor here)"
fi

# ---- the KWin pointer source script ------------------------------------------------------------------------------------
if [[ $COMP == kwin && -x $SINK ]]; then
    echo "== kwin-pet-cursor.js in the nested KWin"
    SINK_NAME=test.murasaki.PointerSink
    "$SINK" "$SINK_NAME" >"$LOG_DIR/sink.log" 2>&1 &
    SINK_PID=$!
    wait_log_file() { local deadline=$((SECONDS + ${3:-10})); while ((SECONDS < deadline)); do grep -q -- "$1" "$2" && return 0; sleep 0.1; done; return 1; }
    if ! wait_log_file ready "$LOG_DIR/sink.log" 10; then
        fail "the pointer sink did not start (see $LOG_DIR/sink.log)"
    else
        # The shipped script with the overlay's name replaced by the sink's.
        SCRIPT=$MURASAKI_NESTED_TMP/kwin-pet-cursor.js
        sed "s/^const service = \"[^\"]*\";/const service = \"$SINK_NAME\";/" "$REPO/src/overlay/kwin-pet-cursor.js" >"$SCRIPT"
        sed -i 's/^const ownerCheckMs = [0-9]*;/const ownerCheckMs = 400;/' "$SCRIPT"   # (10 s in the shipped script)
        grep -q "$SINK_NAME" "$SCRIPT" || fail "the script copy was not retargeted"
        grep -q '^const ownerCheckMs = 400;' "$SCRIPT" || fail "the owner check interval was not shortened"
        reports() { grep -c '^report ' "$LOG_DIR/sink.log"; }
        last_report() { grep '^report ' "$LOG_DIR/sink.log" | tail -1 | awk '{print $3, $4}'; }
        inject abs $PARK_X $PARK_Y sleep 300
        n0=$(reports)
        sid=$(timeout 20 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$SCRIPT" murasaki-pet-cursor 2>>"$LOG_DIR/busctl.err" | awk '{print $2}')
        timeout 20 busctl --user call org.kde.KWin "/Scripting/Script$sid" org.kde.kwin.Script run >>"$LOG_DIR/busctl.err" 2>&1
        sleep 1
        n1=$(reports)
        fact "KWinSource $COMP: script id ${sid:-none}; reports at load (where the cursor rests): $((n1 - n0)), last '$(last_report)' (expected '$PARK_X $PARK_Y')"
        [[ $(last_report) == "$PARK_X $PARK_Y" ]] || pinned "KWinSource: the script did not report the resting cursor"
        # 100 one-pixel moves, 5 ms apart: at most ~30 reports a second, and the last position is never lost.
        toks=
        for i in $(seq 1 100); do toks+=" abs $((PARK_X - i)) $((PARK_Y - i)) sleep 5"; done
        n2=$(reports)
        start_ms=$(date +%s%3N)
        inject $toks sleep 200
        took=$(( $(date +%s%3N) - start_ms ))
        sleep 0.3
        n3=$(reports)
        # The rate is read from the sink's own clock (each report carries its arrival time): what the injector's call took is not the
        # time the compositor spent on the moves (one run measured 128 ms for a burst whose reports spanned 557 ms, 34 ms apart).
        span=$(grep '^report ' "$LOG_DIR/sink.log" | tail -n +$((n2 + 1)) | head -n $((n3 - n2)) | awk 'NR == 1 { a = $2 } { b = $2 } END { print b - a }')
        fact "KWinSource $COMP: 100 moves in ${took} ms gave $((n3 - n2)) reports over ${span:-0} ms (<= ~$((${span:-0} / 34 + 2))), last '$(last_report)' (expected '$((PARK_X - 100)) $((PARK_Y - 100))')"
        [[ $(last_report) == "$((PARK_X - 100)) $((PARK_Y - 100))" ]] || pinned "KWinSource: the last position was lost"
        (((n3 - n2) >= 2 && (n3 - n2) <= ${span:-0} / 34 + 2)) || pinned "KWinSource: $((n3 - n2)) reports over ${span:-0} ms is not <= 30 Hz"
        sleep 1
        n4=$(reports)
        (( n4 == n3 )) || pinned "KWinSource: reports while the cursor was idle"
        # Unloaded: nothing more, however the pointer moves.
        timeout 20 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s murasaki-pet-cursor >>"$LOG_DIR/busctl.err" 2>&1
        sleep 0.3
        inject abs 111 222 sleep 100 abs 333 444 sleep 300
        n5=$(reports)
        fact "KWinSource $COMP: reports after unloadScript: $((n5 - n4))"
        (( n5 == n4 )) || pinned "KWinSource: the script kept reporting after it was unloaded"
        # A script that outlives its overlay (the overlay was killed): it notices that the name has no owner and goes quiet by itself.
        sid=$(timeout 20 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting loadScript ss "$SCRIPT" murasaki-pet-cursor 2>>"$LOG_DIR/busctl.err" | awk '{print $2}')
        timeout 20 busctl --user call org.kde.KWin "/Scripting/Script$sid" org.kde.kwin.Script run >>"$LOG_DIR/busctl.err" 2>&1
        sleep 0.5
        inject abs 120 130 sleep 100
        n6=$(reports)
        (( n6 > n5 )) || pinned "KWinSource: the reloaded script does not report"
        kill -9 "$SINK_PID" 2>/dev/null
        wait "$SINK_PID" 2>/dev/null
        sleep 1.8   # more than two owner checks of 400 ms
        "$SINK" "$SINK_NAME" >"$LOG_DIR/sink2.log" 2>&1 &
        SINK_PID=$!
        wait_log_file ready "$LOG_DIR/sink2.log" 10 || fail "the second sink did not start"
        inject abs 500 400 sleep 100 abs 520 420 sleep 400
        n7=$(grep -c '^report ' "$LOG_DIR/sink2.log")
        fact "KWinSource $COMP: reports reaching a new owner of the overlay's name after the first owner was killed: $n7"
        (( n7 == 0 )) || pinned "KWinSource: a script left behind by a killed overlay reported to the next owner of the name"
        timeout 20 busctl --user call org.kde.KWin /Scripting org.kde.kwin.Scripting unloadScript s murasaki-pet-cursor >>"$LOG_DIR/busctl.err" 2>&1
    fi
fi

P quit
sleep 0.5
((failed)) && { echo "RESULT $COMP: FAIL"; exit 1; }
echo "RESULT $COMP: PASS"
exit 0
