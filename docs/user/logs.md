# Where Murasaki writes its logs

Every Murasaki process and every wallpaper engine it starts writes to its own file under

    ~/.local/state/murasaki/logs/        (or $XDG_STATE_HOME/murasaki/logs/)

The directory is private to you (mode 0700) and so are the files (0600): the lines name items and paths.

| File | Written by | Contents |
|---|---|---|
| `murasakid.log` | the daemon | its `qDebug/qInfo/qWarning` messages: library scans, settings, D-Bus errors |
| `surface.log` | `murasaki-surface` on KDE, GNOME, wlroots and X11 | the same, for the surface and its monitors |
| `overlay.log` | `murasaki-overlay` | the same, for the pets |
| `scene-<output>.log` | the surface (or the Plasma host) that runs a scene on `<output>` | everything the engine prints on stdout and stderr: the Vulkan engine's diagnostics, the sandbox helper's `murasaki-sandbox:` lines |
| `murasaki-surface-<output>.log` | the Plasma wallpaper plugin | what the per-output host process (`murasaki-surface --host <output>`) prints. plasmashell's own stderr pipe is shared by every plugin and easily lost, so the plugin writes it here |

The same messages still go to stderr, so `journalctl --user -u murasaki-surface` keeps working.

## Rotation

A file is rotated when the next line would take it past 2 MiB: `x.log` becomes `x.log.1`, `x.log.1` becomes `x.log.2`, and the old `x.log.2` is deleted. At most three files per component, 6 MiB, are ever kept. Lines are never split between files.

## Reading them

    tail -f ~/.local/state/murasaki/logs/scene-DP-1.log
    grep -h 'murasaki-sandbox' ~/.local/state/murasaki/logs/*.log

Engine lines start with an ISO timestamp. Process lines also carry the level: `[D]` debug, `[I]` info, `[W]` warning, `[E]` critical, `[F]` fatal.

Setting `MURASAKI_SCENE_LOG=1` (or `MURASAKI_BRIDGE_LOG=1`) additionally prints the engine's lines through the process's own log, as before.

## Resumen en espanol

Cada proceso de Murasaki y cada motor de wallpaper escribe en `~/.local/state/murasaki/logs/<componente>[-<salida>].log` (permisos 0600). Cada archivo rota a los 2 MiB y se conservan tres (`.log`, `.log.1`, `.log.2`). El stderr de los motores de escena queda en `scene-<salida>.log`, y el del host de Plasma en `murasaki-surface-<salida>.log`, para no perderlo en el pipe de plasmashell.
