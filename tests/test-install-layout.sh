#!/usr/bin/env bash
# Installs the build into a temporary prefix and checks that everything which names an installed file points at it:
# units, D-Bus activation files, the Plasma wallpaper package, desktop entries, the runtime search paths of the
# binaries. The prefix is deliberately not the configured one, so anything fixed at configure time shows up.
# Usage: test-install-layout.sh <build dir>
set -u
BUILD=$(cd "${1:?usage: $0 <build dir>}" && pwd)
TMP=$(mktemp -d "$(dirname "$BUILD")/install-layout.XXXXXX") # beside the build dir, not in it, and not in RAM-backed /tmp
trap 'rm -rf "$TMP"' EXIT
P="$TMP/prefix"
fail=0
bad() { echo "FAIL: $*"; fail=1; }

cmake --install "$BUILD" --prefix "$P" >"$TMP/install.log" 2>&1 || { cat "$TMP/install.log"; echo "FAIL: cmake --install"; exit 1; }

# 1. Everything installed is text or ELF; nothing may mention the build tree or the configured prefix.
CONFIGURED=$(sed -n 's/^CMAKE_INSTALL_PREFIX:PATH=//p' "$BUILD/CMakeCache.txt")
while IFS= read -r file; do
    grep -qF "$BUILD" "$file" 2>/dev/null && bad "$file mentions the build directory"
    if [[ -n $CONFIGURED && $CONFIGURED != "$P" ]]; then
        grep -qF "$CONFIGURED/" "$file" 2>/dev/null && ! file "$file" | grep -q ELF && bad "$file mentions the configured prefix $CONFIGURED"
    fi
done < <(find "$P" -type f ! -name '*.so*' ! -name '*.qmltypes' ! -perm -u+x)

# 2. Absolute Exec lines of units and activation files name executables of this install.
shopt -s nullglob
for file in "$P"/share/dbus-1/services/*.service "$P"/share/systemd/user/*.service "$P"/lib/systemd/user/*.service; do
    while IFS= read -r program; do
        [[ $program == "$P"/bin/* && -x $program ]] || bad "$file: $program is not an executable of this install"
    done < <(sed -n 's/^\(Exec\|ExecStart\)=\([^ ]*\).*/\2/p' "$file")
done
[[ -n $(echo "$P"/share/dbus-1/services/*.service) ]] || bad "no D-Bus activation file installed"

# 3. Desktop entries name programs that exist in bin/.
for file in "$P"/share/applications/*.desktop; do
    while IFS= read -r program; do
        [[ -x $P/bin/$program ]] || bad "$file: Exec=$program is not in bin/"
    done < <(sed -n 's/^Exec=\([^ ]*\).*/\1/p' "$file")
done

# 4. The Plasma wallpaper package: it starts this install's host and carries its own Bridge module.
PKG="$P/share/plasma/wallpapers/io.github.uriel2121ger_art.Murasaki"
if [[ -d $PKG ]]; then
    grep -qF "$P/bin/murasaki-surface" "$PKG/contents/ui/main.qml" || bad "main.qml does not name $P/bin/murasaki-surface"
    grep -qF "$P/bin/murasaki" "$PKG/contents/ui/ConfigStatus.qml" || bad "ConfigStatus.qml does not name this install's murasaki"
    [[ -f $PKG/contents/ui/Murasaki/Bridge/qmldir ]] || bad "the package has no Murasaki.Bridge module"
    [[ -f $PKG/metadata.json ]] || bad "no metadata.json"
fi

# 5. Runtime search paths: only $ORIGIN or this prefix, never the build tree; and the loader finds everything with a
#    clean environment (no LD_LIBRARY_PATH, no QML_IMPORT_PATH).
while IFS= read -r elf; do
    file "$elf" | grep -q ELF || continue
    paths=$(readelf -d "$elf" 2>/dev/null | sed -n 's/.*\(RPATH\|RUNPATH\).*\[\(.*\)\]/\2/p')
    [[ $paths == *"$BUILD"* ]] && bad "$elf has a build-tree runtime path: $paths"
    missing=$(env -i PATH=/usr/bin:/bin ldd "$elf" 2>/dev/null | grep 'not found')
    [[ -z $missing ]] || bad "$elf: $missing"
done < <(find "$P" -type f \( -perm -u+x -o -name '*.so*' \))

# 6. The programs run from the install with a clean environment.
CLEAN=(env -i HOME="$TMP/home" PATH=/usr/bin:/bin QT_QPA_PLATFORM=offscreen XDG_STATE_HOME="$TMP/state")
mkdir -p "$TMP/home"
for program in murasakid murasaki-surface murasaki-overlay murasakictl murasaki; do
    [[ -x $P/bin/$program ]] || continue
    out=$("${CLEAN[@]}" timeout 20 "$P/bin/$program" --version 2>&1) || bad "$program --version failed: $out"
done
if [[ -x $P/bin/murasaki-sandbox ]]; then
    "${CLEAN[@]}" timeout 20 "$P/bin/murasaki-sandbox" --report >"$TMP/report.json" 2>&1 || bad "murasaki-sandbox --report failed"
fi
# --version must not leave state behind in a user's directory (a CLI query is not a session).
[[ -d $TMP/state/murasaki/logs ]] && bad "--version created log files"

[[ $fail -eq 0 ]] && echo "install layout ok ($P)"
exit $fail
