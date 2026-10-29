# Playlists, spanning and pause rules

Three features of Murasaki 11 (D-Bus API revision 7). Everything here works from the GUI and from `murasakictl`; the
daemon's side is in `docs/dbus-api.md`.

## Playlists

A playlist changes the wallpaper of a screen every few minutes, from a list you choose.

**In the GUI.** Open **Playlists** in the sidebar and press **New playlist…**. On the playlist's page:

- **Add wallpapers…** picks from your library. Pets cannot be in a playlist. You can also use **Add to playlist** on a
  wallpaper's page, or right-click a wallpaper in the library.
- The arrows move an item up or down (the order of the list is the order of play), the red cross removes it.
- **Order**: in order, or shuffle (every wallpaper plays once, in a random order, before any repeats; the same
  wallpaper never plays twice in a row across the reshuffle).
- **Change every**: 1 to 1440 minutes (15 by default).
- **On login**: start with the next wallpaper when the daemon starts, instead of resuming where it was.
- **Apply** puts the playlist on a screen and desktop, like a single wallpaper (fill mode and mute apply to every
  wallpaper of the list). While it plays, the page shows what is showing and has **Previous**, **Next** and **Stop**.

**From a terminal.**

```bash
murasakictl playlist create "Evening"            # prints its id; an id or the name works everywhere below
murasakictl playlist add Evening we:123 we:456
murasakictl playlist set Evening interval=30 order=shuffle changeOnLogin=on
murasakictl playlist apply Evening --output DP-1
murasakictl playlist next                         # or prev; --output/--desktop narrow it down
murasakictl playlist show Evening
murasakictl playlist remove Evening we:123
murasakictl playlist delete Evening              # not while a screen still plays it
```

**Good to know.**

- One playlist assigned to "All screens" shows the same wallpaper on every screen, in step.
- Wallpapers that leave the library (or break) are skipped and come back with the library. A playlist with nothing
  playable shows black.
- Nothing rotates while the screen is locked; after unlocking, a rotation that came due happens once. A long suspend
  never makes it rotate several times.
- Restarting the daemon keeps the position and the time of the next change; it does not catch up on missed changes.
- Playlists live in `~/.config/murasaki/playlists.json`, positions in `~/.local/state/murasaki/playlist-state.json`.

## Spanning

**Span across all screens** (a switch on a wallpaper's or playlist's page when the target is "All screens" and you
have more than one screen; `--span` in `murasakictl set` and `playlist apply`) draws one wallpaper as a single
picture over all your screens, instead of a copy on each.

- The picture covers the bounding box of your screens as laid out in your desktop settings. The fill mode applies to
  the whole picture: with **Fit**, a screen that falls entirely in a letterbox band shows nothing.
- Videos on the screens are kept within 50 ms of each other (checked every four seconds).
- Scenes need Murasaki's own Vulkan engine (`murasaki-scene-vk`); without it the screen shows the scene's preview and
  the log says why.
- Web wallpapers see a page as wide as all screens together.
- Every screen runs its own renderer, so the cost is that of one wallpaper per screen: fine for two or three
  screens. A spanned wallpaper in the Plasma plugin follows the layout but its video players are not kept in step.

## Pause rules

Wallpapers already pause when a window covers the screen. **Settings → Pause** adds:

| Rule | Default | What it does |
|---|---|---|
| Games | on | Pause everything while GameMode has clients: games started through `gamemoderun` (in Steam, the launch option `gamemoderun %command%`). Nothing happens without GameMode. |
| On battery | keep playing | Pause the wallpaper, or limit it to 15 frames per second (videos included). Needs UPower; without it the computer counts as plugged in. |
| Applications | none | Pause while a window of one of these applications has the focus or is fullscreen. Use the window class (what KDE shows in its window rules, e.g. `obs`, `steam_app_620`); `*` and `?` match any characters; separate with commas. KDE Plasma only. |

The **Screens** page says why an output is paused ("a game is running", "on battery", "<app> has the focus or is
fullscreen", or "limited to 15 fps on battery"). When several rules apply the reason is the first of: game, battery,
application. A frame rate limit only applies while nothing is paused.

Settings are stored in `~/.config/murasaki/settings.json` (`pauseOnGame`, `batteryMode`: `off`/`pause`/`lowfps`,
`pauseApps`).

## Resumen en espanol

- **Listas**: el apartado Listas crea listas de fondos que rotan cada N minutos (1 a 1440), en orden o aleatorio sin
  repetir hasta agotarlos. Se aplican a una pantalla y escritorio como un fondo normal. No rotan con la sesion
  bloqueada y no recuperan cambios perdidos. `murasakictl playlist ...` hace lo mismo desde la terminal.
- **Extender**: el interruptor "Extender por todas las pantallas" dibuja un solo fondo sobre todas las pantallas
  (`--span` en la terminal). Los videos se mantienen sincronizados a 50 ms; las escenas necesitan el motor Vulkan; el
  coste es el de un fondo por pantalla.
- **Pausa**: en Ajustes, seccion Pausa: pausar con un juego en marcha (GameMode), con bateria (pausar o limitar a 15
  fps) y para una lista de aplicaciones (clase de ventana, solo KDE Plasma). La pagina Pantallas muestra el motivo.
