# Packaging

Murasaki ships as a `.deb` (CPack), an Arch `PKGBUILD` and a Flatpak manifest. All three run the same CMake install rules
(`cmake --install`), so a new program or data file added to a `CMakeLists.txt` reaches every format; what differs is the
dependency list and what the format can not carry. The design notes of the first version are in
`docs/design/specs/2026-10-17-murasaki-packaging-design.md`.

## What `cmake --install` installs (prefix `/usr`)

| Part | Files |
|---|---|
| Programs (`bin/`) | `murasaki` (GUI), `murasakid`, `murasakictl`, `murasaki-surface`, `murasaki-overlay` (pets), `murasaki-puppet` (Inochi2D/Live2D engine), `murasaki-petimport`, `murasaki-petaudio`, `murasaki-screenmatch` (screen-trigger matching helper; the daemon runs it through `murasaki-sandbox --profile screenmatch`, whose profile is inside the sandbox binary), `murasaki-scene-vk` (Vulkan scene engine), `murasaki-sandbox` |
| Libraries | `lib/<multiarch>/murasaki/libmurasaki-scene-vk-video.so` (libav plugin, loaded with dlopen) |
| systemd user units | `lib/systemd/user/{murasakid,murasaki-surface,murasaki-overlay}.service` (with `-DMURASAKI_SYSTEMD_USER_UNIT_DIR=lib/systemd/user`) |
| D-Bus activation | `share/dbus-1/services/io.github.uriel2121ger_art.Murasaki{,.Overlay}.service` |
| Desktop | launcher and hidden overlay entry in `share/applications/`, icon, AppStream metainfo |
| Plasma | `share/plasma/wallpapers/io.github.uriel2121ger_art.Murasaki/` (its `contents/ui/Murasaki/Bridge/qmldir` loads the `Murasaki.Bridge` module from Qt's QML directory (absolute plugin path, so that a copied package still works), `lib/<multiarch>/qt6/qml/Murasaki/Bridge/`, set with `-DMURASAKI_INSTALL_QMLDIR`) |
| GNOME Shell | `share/gnome-shell/extensions/murasaki@uriel2121ger-art.github.io/` |
| Docs and licences | `share/doc/murasaki/`: `copyright`, `changelog.gz`, `README.md`, `LICENSING.md`, `third-party.md`; `licenses/`: the notices of the statically linked code (QuickJS-ng, volk, VMA, stb, dr_libs, and glslang when the build fetched it) |

Not installed on purpose: `murasaki-petprobe` and the `scene-vk-*` tools (development tools of the compat sweeps and the
engine), the tests. Translations (Spanish) and the KWin scripts are Qt resources inside the binaries.

## Licences in a package

Murasaki is dual-licensed (`LICENSING.md`); every package here is the GPL-3.0-or-later one. Vendored code is permissive and listed
in `third-party.md`. The Live2D Cubism Core is proprietary and is in no package. Wallpaper
Engine assets are never packaged. A commercial redistribution must ship LGPL builds of libmpv and FFmpeg (Flatpak does).

## Debian / Ubuntu (`.deb`)

**Which release a `.deb` targets is decided by where it is built**: `dpkg-shlibdeps` pins the libraries of the build machine
(glibc, Qt 6.x and its private ABI, FFmpeg). A `.deb` built on the development machine (Ubuntu 26.04: Qt 6.10, FFmpeg 8)
installs on 26.04 only. To build for another release, use the container script, which builds inside that release's image
(needs docker without sudo; nothing is installed on the host):

```bash
tools/packaging/build-deb-in-container.sh ubuntu:25.10   # -> build/deb-ubuntu-25.10/murasaki_0.1.0_amd64.deb
tools/packaging/build-deb-in-container.sh ubuntu:26.04
```

The GUI needs Qt >= 6.8 to build (`MURASAKI_BUILD_APP`), so Ubuntu 24.04 (Qt 6.4) can not build the full package; the
build also works against FFmpeg 7.x (25.10) and 8.x. Ubuntu 25.10 is end of life: its image is served from
old-releases.ubuntu.com, which the Dockerfile handles. The run-time Qt of a package is the Qt of the image it was built in
(25.10: Qt >= 6.9.1; 26.04: >= 6.10.2). Dependencies for a build outside a container: those of `tools/packaging/Dockerfile.deb`
(glslang is the system one when installed and otherwise fetched, pinned and hash-checked).

With `--no-install-recommends`, install `qt6-wayland` yourself (the GUI and the layer-shell surface are Wayland-first);
`qml6-module-org-kde-desktop` (suggested) gives the GUI its native KDE style, otherwise a plain Qt style is used.

Manual build on the host:

```bash
cmake -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr \
      -DMURASAKI_SYSTEMD_USER_UNIT_DIR=lib/systemd/user -DBUILD_TESTING=OFF \
      -DMURASAKI_BUILD_SURFACE=ON -DMURASAKI_BUILD_APP=ON
cmake --build build/pkg
(cd build/pkg && cpack -G DEB)         # build/pkg/murasaki_0.1.0_amd64.deb
dpkg-deb -I build/pkg/murasaki_*.deb   # control; dpkg-deb -c lists the files
```

- Library dependencies come from `dpkg-shlibdeps` (`CPACK_DEBIAN_..._SHLIBDEPS`); the QML modules the QML files import are
  listed by hand in `cmake/Packaging.cmake` (they appear in no ELF header). Re-check that list when a QML file gets a new `import`.
- `postinst` only prints how to enable the user units; it enables and starts nothing and touches no home directory.
- Check without installing: `desktop-file-validate`, `appstreamcli validate --no-net` on the files of the staged install
  (`DESTDIR=$PWD/build/stage cmake --install build/pkg`).

## Arch Linux (`packaging/arch/PKGBUILD`)

Builds from the tag `v<pkgver>` of the repository (replace the `SKIP` checksum with `updpkgsums`). It uses the system `glslang`, so
the build downloads nothing besides the source.

```bash
cd packaging/arch && makepkg -si      # on an Arch system; namcap PKGBUILD murasaki-*.pkg.tar.zst afterwards
```


## Flatpak (`packaging/flatpak/`)

See `packaging/flatpak/README.md` for the build and for what the sandbox can not carry (Plasma plugin, GNOME extension, systemd
units). `python3 tools/lint-flatpak.py` checks the manifest without flatpak-builder.

## Releases (`.github/workflows/release.yml`)

Pushing a tag `v<version>` builds the `.deb` (Ubuntu 26.04, with `tools/packaging/build-deb-in-container.sh`) and the
Flatpak bundle, and attaches both to the GitHub release of that tag. The tag must match `VERSION` in `CMakeLists.txt`; a
tag with a dash (`v0.1.0-rc1`) makes a pre-release. Run by hand, the workflow only builds the packages (workflow artifacts).

```bash
git tag v0.1.0 && git push origin v0.1.0
```

## Keeping the packages current

When you add a program: give it an `install(TARGETS ...)` (or a deliberate omission here), add its libraries to the build
dependencies of each format, and, if it is started by a unit or by D-Bus, check that the Flatpak post-install step still gives it a way to
start.
