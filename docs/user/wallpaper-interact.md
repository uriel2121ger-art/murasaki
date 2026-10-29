# Clicking on the wallpaper

Some wallpapers are meant to be used: a web page with a music player, a scene with buttons. On KDE Plasma the desktop
(the icons view) sits above the wallpaper and takes every click, so those wallpapers see the mouse move but never a
click. **Wallpaper interaction** is a mode you turn on when you want to use the wallpaper; it is off by default and
after every restart. D-Bus API revision 13; the daemon's side is in `docs/dbus-api.md` ("Interacting with the
wallpaper").

## Turning it on

- **Shortcut:** `Meta+Shift+W` (set by `murasaki-overlay`; change it in System Settings, Shortcuts, Murasaki). If the key
  is taken by another application the overlay says so in its log and the shortcut does nothing; use the command below.
- **Terminal:** `murasakictl wallpaper interact on`, `off` or `toggle`.

While it is on, every screen with an interactive wallpaper (web pages and scenes, shown through the Plasma plugin) gets
a thin purple border and a small badge in the top right corner, and left, right and middle clicks, drags and the mouse
wheel go to the wallpaper. Everything else on that screen, the panel and the windows included, cannot be clicked
until the mode ends: the catcher covers the whole screen, so windows and the panel do not get clicks (they go to the
wallpaper) and the pets cannot be grabbed either.

## Turning it off

It ends by itself, so you cannot get stuck:

- **Esc** (after a click on the screen, which is when the compositor gives the keyboard to the window),
- the **same shortcut** again, or `murasakictl wallpaper interact off`,
- **60 seconds** without moving or clicking the mouse, and in any case **5 minutes** after it began (the daemon also
  ends it after 10 minutes if nothing else did),
- when the session is **locked**,
- when no screen has an interactive wallpaper any more (you changed the wallpaper to a video, for example).

Turning on the **arrange mode of the pets** (`Meta+Shift+P`) also ends it, and it cannot be turned on while pets are
being arranged: arranging wins. The pets keep working as they are while you click on the wallpaper.

## Limits

- It is for wallpapers shown through the Plasma plugin (the "plasma" presenter). With the layer-shell presenter the
  wallpaper is drawn below the windows of your compositor and clicks are not covered by this mode.
- On GNOME (shell windows) and on X11 it is not available (an X11 window cannot take the keyboard, so Esc could not
  end it).
- Esc needs the keyboard: click on the screen first (the badge says so). The shortcut works without a click.
- The shortcut needs `murasaki-overlay` to be running, which it is while at least one pet exists. Without pets use
  `murasakictl wallpaper interact on`: the daemon starts the overlay for as long as the mode lasts.
- A click on a dock shortcut of a wallpaper (the icons that start an application) counts as the real click it is: it
  starts the application you chose, with the same limits as always.

## Resumen en espanol

El escritorio de Plasma esta por encima del fondo y se queda con todos los clics, asi que los fondos interactivos
(paginas web con botones, escenas) solo ven el movimiento del raton. **Interaccion con el fondo** es un modo que
activas cuando quieres usar el fondo: `Meta+Shift+W` o `murasakictl wallpaper interact on|off|toggle`. Mientras esta
activo, cada pantalla con un fondo interactivo muestra un borde fino y una etiqueta, y los clics, arrastres y la rueda
llegan al fondo; el panel y las ventanas de esa pantalla no reciben clics. Termina con Esc (tras un clic), con el
mismo atajo, a los 60 segundos sin usar el raton, al bloquear la sesion o cuando ya no hay un fondo interactivo. El
modo de colocar mascotas (`Meta+Shift+P`) lo desactiva y tiene prioridad. Sin mascotas el atajo no existe (el overlay no
corre); usa el comando.
