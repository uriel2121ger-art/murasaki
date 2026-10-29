# Murasaki subproject 12: packaging

Date: 2026-10-17
Status: approved by the lead.

**Why:** until now Murasaki is installed with `cmake --install` into `~/.local`. People need a `.deb` for Debian-family
systems and a Flatpak for everyone else, both with the desktop integration (launcher, AppStream metadata, user services,
the Plasma wallpaper) and without surprises (no system services, nothing enabled behind the user's back).

## 1. Debian package (CPack, no debhelper)

- `include(CPack)` driven by `cmake/Packaging.cmake`. The package is built from a Release configuration with prefix `/usr`:
  `cmake -B build/pkg -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DMURASAKI_SYSTEMD_USER_UNIT_DIR=lib/systemd/user`,
  then `cpack -G DEB` in that directory.
- **Layout (all under /usr):** `bin/` (`murasaki`, `murasakid`, `murasaki-surface`, `murasaki-overlay`, `murasakictl`,
  `murasaki-scene-vk`, `murasaki-sandbox`); `lib/systemd/user/` (user units only); `share/dbus-1/services/` (activation);
  `share/applications/` (launcher, hidden overlay entry); `share/icons/hicolor/scalable/apps/`; `share/metainfo/`
  (AppStream); `share/plasma/wallpapers/io.github.uriel2121ger_art.Murasaki/` (the Plasma wallpaper package, with its
  generated files written at install time from the real prefix); `share/doc/murasaki/` (copyright, README).
  The KWin scripts and the Spanish translation are compiled into the binaries (Qt resources), so they have no files.
- **Dependencies:** `dpkg-shlibdeps` fills in the libraries; the QML modules that are imported at run time, which no
  library link shows, are listed by hand (Qt Quick, Controls, Layouts, Dialogs, Templates, Window, WebEngine, Kirigami).
  Recommends: `bubblewrap` (the mount-namespace layer of the sandbox), `ffmpeg`, `mesa-vulkan-drivers`. Suggests:
  `plasma-desktop` (the wallpaper plugin), `steam`. No service is enabled or started: the maintainer script only prints how
  to enable the user units (`systemctl --user enable --now murasakid murasaki-surface`).
- Verified without installing: `dpkg-deb -I/-c`, extraction into a temporary directory, `desktop-file-validate`,
  `appstreamcli validate`, `lintian` when present.

## 2. AppStream and desktop file

`data/io.github.uriel2121ger_art.Murasaki.metainfo.xml` (component type `desktop-application`, GPL-3.0-or-later project
license, OARS content rating, release entry, Spanish summary). Installed to `share/metainfo/`. Validated with
`appstreamcli validate --no-net`; the desktop files with `desktop-file-validate`.

## 3. Flatpak

`packaging/flatpak/io.github.uriel2121ger_art.Murasaki.yml`, written for Flathub conventions, runtime `org.kde.Platform//6.9`.
- Modules: glslang is fetched by the build as before (pinned release and hash); libmpv (with its FFmpeg dependency taken
  from the runtime's ffmpeg-full extension is not available, so mpv is built as a module with `-Dgpl=false`); LayerShellQt as a module
  (it is not in the KDE runtime); Murasaki itself.
- `finish-args` are the minimum: `--share=ipc`, `--socket=wayland`, `--socket=fallback-x11`, `--device=dri`,
  `--socket=pulseaudio` (PipeWire clients use the PulseAudio socket in the sandbox), `--share=network` (item pages and the
  Workshop browser), `--talk-name` for the portals and the session services Murasaki calls (`org.kde.KWin`,
  `org.freedesktop.ScreenSaver`, `org.freedesktop.portal.*` come with the portal sockets), read-only access to the Steam
  libraries (`--filesystem=~/.steam:ro`, `--filesystem=~/.local/share/Steam:ro`).
- **Sandbox decisions.** Murasaki's own sandbox (Landlock, and bubblewrap on top) runs inside Flatpak's. Bubblewrap needs
  user namespaces, which Flatpak's sandbox denies unless `--allow=devel`; so inside Flatpak the helper finds no working bwrap,
  says so once, and runs Landlock only (which needs no privileges and no namespace). That keeps the filesystem, TCP and
  scope confinement; it loses the removal of path-based unix sockets, but Flatpak's own sandbox already does not expose the
  host session bus to the app (only the names granted by `--talk-name`). We do **not** use `flatpak-spawn --host` or
  `--talk-name=org.freedesktop.Flatpak`: both would break the sandbox.
- The Vulkan engine is the Flatpak's scene engine.
- `flatpak-builder` is not installed on the development machine, so the manifest is linted as YAML only; the build
  instructions are in `packaging/flatpak/README.md`.

## Out of scope

Signing, a repository, an AppImage, automatic updates.
