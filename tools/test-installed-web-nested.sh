#!/usr/bin/env bash
# A web wallpaper on an installed host, headless: installs the build into a temporary prefix, then runs that
# prefix's murasakid and `murasaki-surface --host` (behind bridge-view) in a private bus and a virtual kwin_wayland,
# with a clean environment, a throw-away library and a loopback "internet" of its own. The item must load its local
# files (fetch of its own data.json) and must not reach the network: neither its fetch nor an <img> may arrive.
# The user's session, library and ~/.local are never touched.
#
#   tools/test-installed-web-nested.sh <build dir>
#
# Exit status: 0 passed, 1 failed, 77 (ctest skip) unless MURASAKI_NESTED_TESTS=1 and kwin_wayland exist.
set -u
SELF=$(readlink -f "$0")
REPO=$(dirname "$(dirname "$SELF")")
BUILD=$(readlink -f "${1:?usage: $0 <build dir>}")
if [[ ${MURASAKI_NESTED_TESTS:-} != 1 ]]; then echo "SKIP: set MURASAKI_NESTED_TESTS=1"; exit 77; fi
for tool in dbus-run-session kwin_wayland python3; do command -v "$tool" >/dev/null || { echo "SKIP: $tool not found"; exit 77; }; done
[[ -x $BUILD/tools/bridge-view/bridge-view ]] || { echo "SKIP: bridge-view is not built"; exit 77; }
# shellcheck source=tools/nested-common.sh
source "$REPO/tools/nested-common.sh"

if [[ ${2:-} != --inner ]]; then
    W=$(mktemp -d "$(dirname "$BUILD")/installed-web.XXXXXX")
    SOCKDIR=$(mktemp -d /tmp/iw.XXXXXX) # short: it holds the Wayland socket
    trap 'rm -rf "$W" "$SOCKDIR"' EXIT
    cmake --install "$BUILD" --prefix "$W/pfx" >"$W/install.log" 2>&1 || { echo "FAIL: cmake --install"; exit 1; }
    nested_write_bus_conf "$W/bus" # no activation: the live install's services can not start here
    export MURASAKI_IW_W=$W MURASAKI_IW_SOCKDIR=$SOCKDIR
    timeout 240 dbus-run-session --config-file="$W/bus/bus.conf" -- "$SELF" "$BUILD" --inner </dev/null
    exit $?
fi

W=$MURASAKI_IW_W P=$W/pfx PORT=$((20000 + RANDOM % 20000)) ITEM=$W/root/steamapps/workshop/content/431960/9001
mkdir -p "$ITEM" "$W"/{home,state,cfg,data,cache,frames}
export XDG_RUNTIME_DIR=$MURASAKI_IW_SOCKDIR HOME=$W/home XDG_CONFIG_HOME=$W/cfg XDG_DATA_HOME=$W/data XDG_CACHE_HOME=$W/cache XDG_STATE_HOME=$W/state
export MURASAKI_STEAM_ROOT=$W/root PATH=/usr/bin:/bin QT_ACCESSIBILITY=0 MURASAKI_NESTED_MARK="$$-$RANDOM"
unset WAYLAND_DISPLAY DISPLAY
chmod 700 "$XDG_RUNTIME_DIR"
echo '{"title":"webcheck","type":"web","file":"index.html"}' >"$ITEM/project.json"
echo '{"ok":1}' >"$ITEM/data.json"
cat >"$ITEM/index.html" <<HTML
<html><body style="background:#f0f"><script>
fetch("data.json").then(r=>r.json()).then(j=>console.error("LOCAL_OK "+j.ok)).catch(e=>console.error("LOCAL_FAIL "+e));
fetch("http://127.0.0.1:$PORT/evil").then(()=>console.error("NET_REACHED")).catch(e=>console.error("NET_BLOCKED"));
var i=document.createElement("img");i.src="http://127.0.0.1:$PORT/img";document.body.appendChild(i);
</script></body></html>
HTML
cat >"$W/evil.py" <<'PY'
import http.server, sys
class H(http.server.BaseHTTPRequestHandler):
    def do_GET(s):
        open(sys.argv[2], 'a').write(s.path + '\n')
        s.send_response(200); s.send_header('Access-Control-Allow-Origin', '*'); s.end_headers(); s.wfile.write(b'x')
    def log_message(*a): pass
http.server.HTTPServer(('127.0.0.1', int(sys.argv[1])), H).serve_forever()
PY
cleanup() { kill "${PIDS[@]}" 2>/dev/null; wait 2>/dev/null; }
PIDS=()
trap cleanup EXIT
python3 "$W/evil.py" "$PORT" "$W/hits.txt" & PIDS+=($!)
S=iw-$$
KWIN_WAYLAND_NO_PERMISSION_CHECKS=1 kwin_wayland --virtual --socket "$S" --width 1280 --height 720 --no-lockscreen >"$W/kwin.log" 2>&1 & PIDS+=($!)
for _ in $(seq 100); do [[ -S $XDG_RUNTIME_DIR/$S ]] && break; sleep 0.1; done
[[ -S $XDG_RUNTIME_DIR/$S ]] || { echo "SKIP: kwin_wayland did not come up"; exit 77; }
export WAYLAND_DISPLAY=$S QT_QPA_PLATFORM=wayland
"$P/bin/murasakid" >"$W/daemon.log" 2>&1 & PIDS+=($!)
for _ in $(seq 50); do "$P/bin/murasakictl" list 2>/dev/null | grep -q 9001 && break; sleep 0.2; done
"$P/bin/murasakictl" set we:9001 >/dev/null || { echo "FAIL: cannot assign the item"; exit 1; }
timeout 120 "$BUILD/tools/bridge-view/bridge-view" --out-dir "$W/frames" --size 640x360 --producer-log "$W/host.log" \
    -- "$P/bin/murasaki-surface" --host Virtual-0 >"$W/view.txt" 2>&1
fail=0
grep -q 'js: LOCAL_OK 1' "$W/host.log" || { echo "FAIL: the item did not load its local file"; fail=1; }
grep -q 'js: NET_BLOCKED' "$W/host.log" || { echo "FAIL: the page's fetch to the network was not refused"; fail=1; }
grep -q 'NET_REACHED' "$W/host.log" && { echo "FAIL: the page reached the network"; fail=1; }
[[ -s $W/hits.txt ]] && { echo "FAIL: the loopback server was hit: $(tr '\n' ' ' <"$W/hits.txt")"; fail=1; }
((fail)) && { tail -20 "$W/host.log"; exit 1; }
echo "installed web host ok: local content loaded, network blocked"
