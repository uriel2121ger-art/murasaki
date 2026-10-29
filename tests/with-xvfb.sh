#!/bin/sh
# Runs a command against a private Xvfb (never the user's X or Wayland session) and removes the server afterwards.
# Exit status 77 (skipped, for ctest) when Xvfb is missing or does not come up.
# Usage: with-xvfb.sh <command> [arguments...]
command -v Xvfb >/dev/null 2>&1 || { echo "skip: Xvfb is not installed" >&2; exit 77; }
dir=$(mktemp -d "${TMPDIR:-/tmp}/murasaki-xvfb.XXXXXX") || exit 77
Xvfb -displayfd 3 -screen 0 1280x1024x24 -nolisten tcp 3>"$dir/display" >"$dir/log" 2>&1 &
server=$!
i=0
while [ ! -s "$dir/display" ] && [ $i -lt 100 ]; do
    sleep 0.1
    i=$((i + 1))
done
if [ ! -s "$dir/display" ]; then
    echo "skip: Xvfb did not start" >&2
    kill "$server" 2>/dev/null
    rm -rf "$dir"
    exit 77
fi
DISPLAY=":$(head -n 1 "$dir/display")"
export DISPLAY
unset WAYLAND_DISPLAY WAYLAND_SOCKET
export QT_QPA_PLATFORM=xcb QT_ACCESSIBILITY=0 XDG_SESSION_TYPE=x11
"$@"
status=$?
kill "$server" 2>/dev/null
wait "$server" 2>/dev/null
rm -rf "$dir"
exit $status
