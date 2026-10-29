# Murasaki subproject 9: GNOME support

Date: 2026-10-17
Status: approved by the lead. Written without gnome-shell installed: everything that does not need a running shell is
tested (node tests for the extension's pure modules, C++ tests for the surface side); the shell glue is written against the
documented GNOME Shell 46-50 extension API and has to be checked on a real session (docs/gnome.md has the checklist).

**Why:** GNOME's compositor (Mutter) has no wlr-layer-shell, so `murasaki-surface` can not place wallpaper windows the way it
does on Plasma and wlroots compositors, and Mutter gives no foreign-toplevel protocol. Both gaps are closed from inside the shell:
a GNOME Shell extension.

## 1. Wallpaper: how the frames reach the desktop background

Options considered:

1. **Import the bridge's DMA-BUFs in the extension** (a Clutter actor textured from the engine's buffers). Not feasible from JS:
   GNOME Shell's GI bindings expose no Cogl/Mutter call to create a texture from a dma-buf (`Cogl.Texture`/`CoglDmaBufHandle`
   are private to Mutter's native backend), and GJS can not receive the file descriptors over the bridge socket anyway. It
   would need a native companion library loaded into gnome-shell, which is the thing extensions must not do.
2. **A Wayland window pinned as the background (the Hanabi approach). Chosen.** `murasaki-surface` creates one ordinary
   xdg-toplevel per output, frameless, sized to the output, titled `Murasaki Wallpaper | <output>`. The extension finds those
   windows, moves each to its monitor, reparents its window actor into the shell's background group (under every window and
   the desktop icons, above the wallpaper actor, which it hides) and keeps it out of the overview, the window list and
   Alt-Tab.

Trade-offs of 2: the compositor composes a client window (one extra copy path compared to a layer surface), the window
exists as far as Mutter is concerned (it needs the patches above to be invisible), and the shell's own background is covered,
not replaced. In return: it is the only approach that works with JS-only extension code, with GPU buffers staying in the
client (Murasaki's renderers already draw to windows), and it keeps working across GNOME 46-50 because only stable
`Meta.WindowActor`, `Meta.Window` and `Main.layoutManager` surfaces are used.

When the wallpaper is not interactive the window has an empty input region, so clicks reach the shell's own desktop (its context
menu). When it is interactive (web, scenes) the window takes pointer events natively, as on the other compositors (4).

## 2. Pets

`murasaki-overlay` has no layer-shell on GNOME either. Each pet window is an output-sized, transparent toplevel titled
`Murasaki Pet | <output> | <id>` whose input region is the pet's rectangle (arrange mode) or empty (normal mode, click-through).
The extension keeps those windows above everything (`make_above`), on all workspaces (`stick`), on the right monitor, out
of the overview, the window list and Alt-Tab, and never gives them the focus.

## 3. Window state for the pause rules

The extension reports to `murasaki-surface` over D-Bus (`ReportShellState`) in the same JSON `kwin-monitor.js` uses
(`parseKWinReport`): per window its output, workspace, fullscreen/minimized/covers-output and application class, the focused
window and the workspaces. The surface accepts the call only from the owner of `org.gnome.Shell`. The report is built by a pure
module (`lib/report.js`) and sent when windows, focus, workspaces or monitors change, and again when `murasaki-surface`
appears on the bus. Application ids are the Wayland `app_id` (`Meta.Window.get_gtk_application_id()` / `get_wm_class()`).

## 4. Pointer

Native: the wallpaper window receives Mutter's pointer events like any client. The extension keeps wallpaper windows from taking
the keyboard focus (`Meta.Window` `focus` is never given, and the surface sets `WindowDoesNotAcceptFocus`).

## 5. Layout of the work

- `data/gnome-shell/murasaki@uriel2121ger-art.github.io/`: `metadata.json` (shell-version 46-50), `extension.js` (glue), `lib/*.js`
  (pure: `classify.js`, `report.js`, `placement.js`), installed to `share/gnome-shell/extensions/<uuid>/`.
- Surface: `ReportShellState` (checked sender), `ShellMonitor`, `MonitorKind::GnomeShell`, wallpaper windows as plain toplevels
  when the session is GNOME. Overlay: pet windows as above.
- `tools/test-gnome-nested.sh`: `gnome-shell --devkit` (GNOME 49+) or `--nested` (46-48) with a private bus, skipping when
  gnome-shell is missing.
- `node --test data/gnome-shell/.../test` (ctest `gnome_extension_node` when node is installed).

## Not done / risks

Everything shell-side is unverified until a GNOME session is available; the places most likely to need a fix are called out
in docs/gnome.md. No settings UI in the extension (the Murasaki GUI is the UI). X11 sessions: the same extension works (windows
are X11 windows), but Murasaki's X11 support is a separate subproject.
