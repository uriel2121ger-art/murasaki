# Murasaki on GNOME

GNOME's compositor (Mutter) has no `wlr-layer-shell`, so Murasaki can not put its wallpapers and pets where it does on
Plasma and wlroots compositors. On GNOME it uses ordinary windows and a **GNOME Shell extension** that puts them in place.
The extension supports GNOME Shell 46 to 50 and is **experimental: it was written without a running GNOME Shell and has to
be checked on a real session** (the checklist is below).

## Install and enable

```bash
cmake --install build/pkg          # or the .deb: the extension goes to share/gnome-shell/extensions/
gnome-extensions enable murasaki@uriel2121ger-art.github.io
```

On Wayland a newly installed extension is seen after logging out and in. Then start the services as usual
(`systemctl --user enable --now murasakid murasaki-surface`). The surface notices the GNOME session by `XDG_CURRENT_DESKTOP`
(`MURASAKI_SHELL_WINDOWS=1` forces the window mode elsewhere, `=0` turns it off).

## How it works

- **Wallpaper.** `murasaki-surface` opens one frameless window per output titled `Murasaki Wallpaper | <output>`. The extension
  finds them, moves each to its monitor, reparents its window actor into the shell's background group (under every window and
  the desktop icons) and hides it from the overview, Alt-Tab and the window list. Why not import the engine's frames directly
  into the shell: GNOME Shell's JavaScript has no way to build a texture from a DMA-BUF, so a pinned client window is the
  only option that needs no native code in the shell (design notes in the spec).
- **Pets.** `murasaki-overlay` opens one transparent window per pet, as big as its output, titled `Murasaki Pet | <output> | <id>`;
  the window's input region is the pet (arrange mode) or empty (click-through). The extension keeps these windows above
  everything, on all workspaces, and out of the overview and Alt-Tab. While a pet has `keepAbove` off, the overlay appends ` | below` to the
  title (`Murasaki Pet | <output> | <id> | below`); the extension reads the marker when the title changes, takes the window out of the
  above layer and pushes it down (`unmake_above`, `make_below`, `lower`, each only if this GNOME Shell has it). **Unverified on a real
  shell**: Mutter has no layer, so the pet becomes an ordinary stacked window that other windows can cover and that comes up when
  clicked. Until it is checked, treat `keepAbove: false` on GNOME as experimental.
  A pet in the fullscreen mode (docs/pets/stage.md) adds ` | stage output` or ` | stage desktop` after the id (and after `| below`): the extension places
  its window on its output like any pet window, but keeps it at the bottom of the layer above the windows (`make_above`, then `lower`) so that the pets stay in
  front of it. The actions are decided by `petActions` in `lib/placement.js`, which the node tests cover; **the effect on Mutter is unverified on a real
  shell**. The stage window is Murasaki's own (it is trusted by title and application id, never reported as a window that covers the output), takes no input
  (its input region is empty) and cannot take the focus.
- **Pausing.** The extension reports the shell's windows to `murasaki-surface` (`ReportShellState`, accepted only from
  `org.gnome.Shell`): which windows are fullscreen, maximized or covering an output, their application ids (`pauseApps` uses
  them, for example `org.gnome.Totem`) and the workspaces, so wallpapers pause under a fullscreen window and follow
  workspaces like on Plasma.
- **Pointer.** An interactive wallpaper (web page, scene) receives the pointer natively, as a window of the background.
  A non-interactive one has an empty input region, so the shell's own desktop menu keeps working.

## Which windows are Murasaki's

The titles are not trusted by themselves: a web page can set its title to `Murasaki Pet | DP-1`. The extension treats a window as the
overlay's pet only when its application id is `io.github.uriel2121ger_art.Murasaki.Overlay`, and as the surface's wallpaper only
when it is `io.github.uriel2121ger_art.Murasaki` (both processes set it with `setDesktopFileName`, and Mutter reports it as the Wayland
app id). A window with the title but another id is an ordinary window: it is not pinned, resized, lowered or hidden from the overview,
Alt-Tab and the pause report. (The application id is a claim of the client, not a proof: a local program that sets both can still be
mistaken for Murasaki. The pid of the owner of the overlay's D-Bus name would be a stronger check; it is not done.)

## Trade-offs

The compositor composes a client window instead of a layer surface, so GPU memory and one extra composition step are spent; the
window is a window as far as Mutter is concerned, so a GNOME version that changes how the overview, Alt-Tab or the window
tracker list windows can show it until the extension's patches (`InjectionManager` overrides in `extension.js`) are updated.
Every patch is guarded: a failure disables that piece and logs `Murasaki: ...` to `journalctl /usr/bin/gnome-shell`.

## Tests

- `node --test data/gnome-shell/murasaki@uriel2121ger-art.github.io/test/lib.test.js` (ctest `gnome_extension_node`): the pure
  modules (window titles, the report, placement, connector names).
- `tools/test-gnome-nested.sh` (ctest `gnome_nested`): a headless `gnome-shell` in a private session; skips without it.
- C++: `tst_shellwindows`, `tst_sessionstate`, `tst_surfaceservice` (the sender check).

## Checklist for the first real session

1. `gnome-extensions enable ...` and no error in `journalctl -f /usr/bin/gnome-shell`.
2. A wallpaper is behind all windows and the desktop icons (if you use an icons extension), on every monitor, and not in the
   overview, Alt-Tab or the dash. If it shows in them, the patch for that list needs the name of the function of your GNOME version.
3. The wallpaper window never takes the keyboard focus when it is mapped (type into another window while it loads).
4. A pet is above everything, even fullscreen windows, follows the monitor it is on, and is click-through except in arrange mode.
   With `murasakictl pets set <id> --keep-above off` it goes below the windows (and back above with `on`); report what the shell did.
5. Fullscreen a video: the wallpaper pauses (`busctl --user call io.github.uriel2121ger_art.Murasaki.Surface /io/github/uriel2121ger_art/Murasaki/Surface io.github.uriel2121ger_art.Murasaki.Surface1 Outputs` shows `occluded`).
6. Unplug and plug a monitor: wallpapers and pets follow.
7. Disable the extension: the shell's own background is back and no Murasaki window stays in the background group.
