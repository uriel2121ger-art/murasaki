# Flatpak

`io.github.uriel2121ger_art.Murasaki.yml` is the manifest, in the style Flathub expects (runtime `org.kde.Platform//6.11`, the
Qt WebEngine base app, every source pinned by hash or commit).

## Build and run

```bash
flatpak install flathub org.kde.Platform//6.11 org.kde.Sdk//6.11 io.qt.qtwebengine.BaseApp//6.11
flatpak-builder --user --install --force-clean build-dir packaging/flatpak/io.github.uriel2121ger_art.Murasaki.yml
flatpak run io.github.uriel2121ger_art.Murasaki
```

`python3 tools/lint-flatpak.py` checks the manifest without flatpak-builder (valid YAML, required keys, no broad
permissions, every source pinned). The manifest builds with flatpak-builder in the `ghcr.io/flathub-infra/flatpak-github-actions:kde-6.11` image (cold build about
30 minutes, the bundle is about 110 MB); the headless checks (`murasakid --version`, `murasaki --smoke-test` offscreen) pass. The sandboxed
behaviour (D-Bus activation of the surface and the overlay, KWin access) has not been tested on a real desktop.

## Decisions

- **Permissions are the minimum**: Wayland (X11 as fallback), DRI, the PulseAudio socket (PipeWire clients use it inside the
  sandbox), network, the portals' and KWin's/ScreenSaver's bus names, and read-only access to the Steam libraries. No
  `--filesystem=home`, no `--talk-name=org.freedesktop.Flatpak`, no `flatpak-spawn --host`: any of them would void the sandbox.
- **Two sandboxes**: Murasaki's own scene sandbox (docs/security.md) runs inside Flatpak's. Bubblewrap needs user namespaces,
  which Flatpak's sandbox does not give the app, so `murasaki-sandbox` finds no working bwrap, says so once, and applies
  Landlock alone (filesystem, TCP and scope confinement, no privileges needed). What is lost is the removal of path-based unix
  sockets; Flatpak's own sandbox already keeps the host session bus away from the app (only the names granted above).
- **Network gate**: the proxy that keeps web wallpapers off the network is inside the app and works unchanged.
- **Scene engines**: the Vulkan engine (`murasaki-scene-vk`) is built in.
- **Plasma wallpaper plugin**: not exported (plasmashell on the host can not load a plugin from inside a sandbox). Flatpak
  users get the layer-shell presenter, which works on Plasma and wlroots compositors; use the `.deb` for the plugin.
- **Services**: the systemd user units are not used in Flatpak (the post-install step of the manifest removes them). D-Bus
  activation starts the programs: `murasakid` and `murasaki-overlay` (their service files are exported with the
  `SystemdService=` line removed) and `murasaki-surface` (a service file the manifest writes: the repository has none for it, because
  on a normal install the unit starts it). When the daemon finds no systemd on the bus (always the case in the sandbox) it
  starts the pets' overlay with `StartServiceByName` on the overlay's own name. Not available in Flatpak: starting at login
  (`systemctl --user enable`, the GUI's autostart switch); use the desktop environment's autostart for `flatpak run`.
- **Desktop pets**: `murasaki-overlay`, `murasaki-puppet` (Inochi2D/Live2D engine), `murasaki-petimport` and
  `murasaki-petaudio` are built and installed in `/app/bin` with the rest, and the overlay starts by the activation above
  when the first pet is added. Stopping it with the last pet is not done (there is no unit to stop): it keeps running
  until the Flatpak session ends or you quit it. Untested in a real sandbox.

## What the Flatpak does and does not cover

| Component | In the Flatpak | Notes |
|---|---|---|
| `murasaki` GUI, `murasakid`, `murasakictl`, `murasaki-surface` | yes | the surface is the layer-shell presenter |
| `murasaki-scene-vk` and its libav video plugin | yes | libav is the LGPL FFmpeg of the manifest |
| `murasaki-overlay`, `murasaki-puppet`, `murasaki-petimport`, `murasaki-petaudio`, `murasaki-screenmatch` | yes | see Desktop pets above; screen triggers capture through the ScreenCast portal, untested here |
| `murasaki-sandbox` | yes | Landlock only (see Two sandboxes) |
| Plasma wallpaper package (`share/plasma`, `share/murasaki/plasma`) | no | removed in post-install; use the `.deb` or the Arch package |
| GNOME Shell extension (`share/gnome-shell`) | no | an extension must be installed on the host |
| systemd user units | no | removed; D-Bus activation instead |
| Live2D Cubism Core | no | proprietary; never bundled |
| `steamcmd`, host `ffmpeg` | no | not in the sandbox; the Workshop download and transcode features that call them find nothing |
| libdeflate, libjpeg-turbo | no | optional speed-ups of the scene engine; it falls back to stb_image |

Licences: the manifest builds mpv and FFmpeg as LGPL, so the Flatpak contains no GPL code besides Murasaki itself
(GPL-3.0-or-later, or the commercial licence for a different distribution; LICENSING.md).
- **mpv and FFmpeg** are built as LGPL (`-Dgpl=false`, `--disable-gpl`), which keeps the commercial license of Murasaki
  possible (LICENSING.md).
