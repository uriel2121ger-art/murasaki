# Murasaki on wlroots-based compositors and COSMIC

Murasaki draws wallpapers as `wlr-layer-shell` surfaces, so it works on Sway, labwc, Hyprland, river, wayfire, niri and COSMIC,
and in Plasma next to the Plasma wallpaper plugin. It does not work on GNOME (no layer-shell): there `murasaki-surface
--windowed` shows ordinary windows, which is only good for trying it.

## What differs from Plasma

- **Layer.** Wallpapers go on the *background* layer (under the compositor's own colour or image and under the desktop-icon
  and panel layers); Plasma keeps the *bottom* layer. The desktop pets are on the *overlay* layer everywhere.
- **Pausing when something covers the screen.** Sway and Hyprland report their windows over their IPC; every compositor that
  offers `wlr-foreign-toplevel-management` (labwc, river, wayfire, niri, ...) reports them through that protocol. A window
  covers an output when it is fullscreen or maximized on it and not minimized. On a tiling compositor any tiled window covers
  the background, which is why Sway and Hyprland use their IPC for this. COSMIC has no supported source yet: wallpapers there never
  pause (use `pauseWhenCovered` off and the pause rules for games instead).
- **Pause rules** (games, applications): the application classes come from the foreign-toplevel protocol when the compositor
  has it (`app_id`, the same names you would put in `pauseApps`: `steam_app_620`, `mpv`, ...). The GameMode and battery rules
  work everywhere.
- **Desktops (workspaces).** Sway and Hyprland workspaces work as in the docs of playlists/assignments; labwc, river and the
  rest have no workspace protocol Murasaki reads, so every output is on desktop `*`.
- **Pointer.** Interactive wallpapers (web pages, scenes) receive the compositor's own pointer events; no helper is needed.
  Elements above the wallpaper (panels, icons) take the clicks that land on them.

## Choosing and checking

The surface logs what it found, for example `murasaki-surface: wlroots-style: wlr-layer-shell, layer background, windows from
wlr-foreign-toplevel-management`. `journalctl --user -u murasaki-surface | grep murasaki-surface:` shows it.
The state of every output is in the surface's D-Bus `Outputs()` call (`occluded`, `paused`, `pauseReason`).

`tools/test-wlroots-nested.sh` starts sway, labwc or Hyprland headless in a private session and checks that the wallpaper is
drawn and that a fullscreen window pauses it (needs `grim`; it skips what is not installed). Hyprland cannot start
headless on its own, so it runs as a window of a virtual `kwin_wayland` with its own bus; it needs `kwin_wayland` too. Last
run: sway 1.11, labwc 0.9.3, Hyprland 0.53.3 and Wayfire 0.10.0 on Ubuntu 26.04, all four checks. Wayfire needs its
`foreign-toplevel` plugin loaded for the pause.

## Manual checklist (needs a real session)

1. Start the compositor, then `systemctl --user enable --now murasakid murasaki-surface`; set a wallpaper with `murasakictl`.
2. A web wallpaper that reacts to the mouse follows the pointer; panels and icons above it still work.
3. Fullscreen a video: the wallpaper pauses (`Outputs()` shows `occluded`); leave fullscreen: it plays.
4. `pauseApps` with a window class pauses while that application has the focus.
5. Plug a second output: it gets a wallpaper window without restarting.
