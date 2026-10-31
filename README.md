# Murasaki

**Español** · [English](README.en.md)

![Mascotas de escritorio animadas sobre un fondo de Wallpaper Engine](docs/images/mascotas.gif)

Fondos de pantalla animados y mascotas de escritorio para Linux. Murasaki reproduce el contenido de Wallpaper Engine que ya tienes en Steam: escenas, videos, fondos web y elementos del Steam Workshop, además de las mascotas de Anima Engine. Lo lee todo desde tu propia instalación de Steam y nunca lo redistribuye.

> [!WARNING]
> **Beta (v0.1).** Murasaki se usa a diario, pero todavía puede haber fallos y algunas escenas se ven incompletas. Si algo no funciona, [abre un issue](../../issues/new/choose) con tus logs ([cómo sacarlos](docs/user/logs.md)).

Cómo está hecho: `murasakid` indexa tu biblioteca del Workshop de Wallpaper Engine y la ofrece por D-Bus (ver `docs/dbus-api.md`). `murasaki-surface` muestra fondos de video, web y escena detrás de tus ventanas en sesiones de KDE Plasma (Wayland), Sway, Hyprland y X11 (`docs/user/x11.md`). Las escenas necesitan el motor de escenas (abajo); sin él muestran su imagen de vista previa. En Plasma, los iconos del escritorio se ocultan mientras Murasaki está activo, salvo que uses el plugin de fondo de Plasma (abajo).

## Capturas

| | |
|---|---|
| ![Biblioteca](docs/images/biblioteca.png) | ![Mascotas en la biblioteca](docs/images/mascotas-biblioteca.png) |
| **Biblioteca:** escenas, videos, fondos web y mascotas | **Mascotas** de Anima Engine |
| ![Página de un fondo](docs/images/fondo.png) | ![Automatizaciones](docs/images/automatizaciones.png) |
| **Cada fondo** con sus propiedades y en qué pantalla va | **Automatizaciones:** pausar en juegos, ahorrar batería, ocultar mascotas... |

La app sigue el idioma del sistema (español e inglés).

## Guía de uso

La [guía de uso](docs/guia/README.md) explica cada página de la app, qué hace cada ajuste y qué hacer cuando algo falla.

## Requisitos

- **Steam con Wallpaper Engine comprado e instalado.** Murasaki no trae fondos: reproduce los que tienes en tu cuenta. Instala Wallpaper Engine en el Steam de Linux (con Proton basta para que se descargue; Murasaki no lo ejecuta). Las escenas necesitan su carpeta `assets`: sin ella se quedan en su imagen de vista previa.
- **Anima Engine** en la misma cuenta, si quieres las mascotas.
- **Sin el cliente de Steam abierto:** Murasaki solo lee los archivos que Steam ya descargó. Los elementos del Workshop también se pueden bajar con [SteamCMD](docs/user/steamcmd.md) y tu propia cuenta, pero Wallpaper Engine en sí se instala desde Steam.
- **Linux con KDE Plasma 6, GNOME, Sway, Hyprland o X11**, y un driver Vulkan 1.3 (Mesa o NVIDIA) para las escenas.

## Instalación

Descarga el paquete de la última versión en la sección [Releases](../../releases):

- **Ubuntu 26.04 y derivadas (.deb):** `sudo apt install ./murasaki_*_amd64.deb`. Necesita glibc 2.43 y FFmpeg 8, que solo trae Ubuntu 26.04: en Ubuntu 24.04, KDE neon (que todavía usa la base 24.04) y las demás distribuciones usa el Flatpak.
- **Cualquier distribución (Flatpak):** `flatpak install --user ./murasaki-*-x86_64.flatpak` (necesita el remoto de Flathub para el runtime de KDE).
- **Arch Linux:** compila el paquete con el PKGBUILD de la repo:

  ```sh
  git clone https://github.com/uriel2121ger-art/murasaki
  cd murasaki/packaging/arch && makepkg -si
  ```

El plugin de fondo de Plasma y la extensión de GNOME vienen en el `.deb` y en el paquete de Arch, no en el Flatpak.

## Compilar

Requisitos: CMake ≥ 3.25, Ninja, un compilador de C++20 y Qt ≥ 6.8 (Core, DBus, Test). En Debian/Ubuntu: `qt6-base-dev`.

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev   # la prueba de D-Bus corre sola bajo dbus-run-session si está instalado
./build/dev/src/daemon/murasakid --list
```

Todo necesita Qt ≥ 6.8 (Ubuntu 26.04 LTS, Fedora 41+, Arch, KDE neon; en Ubuntu 24.04 usa el Flatpak). Para la superficie del fondo instala también `qt6-declarative-dev qt6-webengine-dev libmpv-dev libavformat-dev libavcodec-dev libswscale-dev libavutil-dev pkg-config` y, para la capa del escritorio, `liblayershellqtinterface-dev`.

La interfaz gráfica (`murasaki`) necesita para compilarse Qt ≥ 6.8 con Qt Quick Controls 2, Widgets y WebEngine, y en ejecución Kirigami (KF6, `kirigami` / `qml6-module-org-kde-kirigami`) y QtWebEngine. Con `MURASAKI_BUILD_APP=AUTO` (el valor por defecto) solo se compila si encuentra Qt ≥ 6.8 y esos módulos; con `ON` la configuración falla si faltan y con `OFF` se omite.

## Uso

```bash
cmake --install build/dev                 # instala en ~/.local
murasakictl list                          # busca el id de un elemento
murasakictl set we:1081733658             # todas las pantallas, todos los escritorios
murasakictl set we:123 --output DP-1 --desktop 2 --fill fit
murasakictl mute on                       # silencia la asignación por defecto
murasakictl autostart on                  # inicia la superficie con tu sesión (unidad de usuario de systemd)
murasakictl status
```

Los fondos se pausan y se silencian mientras una ventana a pantalla completa o maximizada cubre su pantalla (y, si lo pides, mientras corre un juego, con batería o con las aplicaciones que elijas).

Los elementos del Workshop también se pueden descargar sin el cliente de Steam, con tu propia cuenta, mediante SteamCMD (`murasakictl steamcmd ...`, la página Workshop): ver `docs/user/steamcmd.md`.

Las listas de reproducción rotan fondos con un temporizador (`murasakictl playlist ...`, la página Listas) y un fondo puede extenderse por todas las pantallas (`--span`): ver `docs/user/playlists-spanning-pause.md`.

## Mascotas de escritorio

Los elementos de Anima Engine del Workshop aparecen en la biblioteca como mascotas. El daemon guarda dónde se muestra cada una (`pets.json`), obtiene sus títulos y vistas previas del endpoint público del Workshop de Steam (sin clave, con caché) y convierte una sola vez las animaciones pesadas en una copia VP9 ligera con `ffmpeg`:

```bash
murasakictl pets add anima:3123456789 --output DP-1 --x 200 --y 300 --width 300 --fps 24
murasakictl pets list
murasakictl pets arrange toggle           # mover y redimensionar mascotas con el ratón
murasakictl pets check <item>            # ¿puede reproducirla el overlay? una línea JSON (ver docs/pets/check.md)
murasakictl pets remove <pet id>
```

Anima Engine ignora los retardos de fotograma guardados en el APNG o GIF de una mascota y anima todas a un ritmo fijo propio (15 fps). Murasaki también usa un ritmo fijo, de 24 fps por defecto: `murasakictl pets timing <pet id> fixed 15` da el de Anima, y `murasakictl pets timing <pet id> file` ("Tiempo original" en la interfaz) reproduce el archivo con sus propios retardos.

`murasaki-overlay` las dibuja: una ventana layer-shell transparente por mascota en la capa overlay, por encima de todas las ventanas (también las de pantalla completa) y sin capturar clics, reproduciendo el archivo original o la copia convertida con libmpv, siempre sin sonido. El daemon inicia la unidad de usuario `murasaki-overlay.service` al añadir la primera mascota y la detiene al quitar la última; la activación por D-Bus de `io.github.uriel2121ger_art.Murasaki.Overlay` también la inicia. Las mascotas se pausan mientras la sesión está bloqueada y nunca se pausan por ventanas a pantalla completa.

El modo de acomodo hace que las mascotas reciban el puntero: arrastra para moverlas, usa la rueda para escalarlas (de 0.1 a 2 veces su tamaño nativo), arrastra la esquina para redimensionarlas y la cruz quita la mascota. Se activa con `murasakictl pets arrange toggle`, desde la interfaz o con el atajo global Meta+Shift+P (el portal XDG GlobalShortcuts o kglobalaccel de KDE; puedes cambiarlo en la Configuración del sistema).

`murasakictl pets set <id> --click-through off --lock on --keep-above off --hover-hide on --smart-dodge on --rotation 90 --flip-h on` cambia lo que hace una mascota (se guarda con ella; la página de mascotas de la interfaz tiene los mismos controles y el overlay los aplica todos):

- una mascota que no deja pasar los clics recibe el puntero en sus píxeles opacos y se puede arrastrar;
- ocultar bajo el cursor (`--hover-hide`) y desvanecer con el foco (`--focus-fade`) siguen al cursor y se excluyen entre sí; el esquive inteligente se aparta de él;
- la rotación y los volteos se dibujan sin volver a decodificar;
- con "mantener encima" desactivado, las ventanas pueden tapar la mascota.

Cuando un compositor no puede hacer alguna de estas cosas exactamente, la mascota lo indica por nombre en `Windows()` del overlay (`degraded`); `docs/pets/behaviours.md` explica qué hace cada escritorio, los ajustes (`MURASAKI_MASK_CACHE_MIB`, `MURASAKI_POINTER`) y la lista de comprobación para el primer uso en un escritorio nuevo. Las mascotas con los valores por defecto cuestan lo mismo de siempre.

Meta+Shift+H oculta o muestra todas las mascotas (`murasakictl pets hide|show|toggle`) y Meta+Shift+R es el rescate (`murasakictl pets rescue`): vuelve a dejar todas las mascotas visibles, sólidas y agarrables tras una opción olvidada. La API está en `docs/dbus-api.md`.

## Plugin de fondo de Plasma

En KDE Plasma 6, Murasaki puede ser el propio plugin de fondo del escritorio en lugar de una ventana layer-shell, así los iconos, widgets y paneles siguen funcionando. `cmake --install` instala el plugin; para activarlo:

```bash
murasakictl plasma enable    # Murasaki pasa a ser el fondo de todos los escritorios (containments)
murasakictl plasma status    # qué escritorios lo usan y si el paquete está instalado
murasakictl plasma disable   # devuelve a cada escritorio su fondo anterior
murasakictl plasma enable --dry-run   # muestra qué cambiaría sin cambiar nada
```

`enable` pone el ajuste `presenter` del daemon en `plasma`: `murasaki-surface` deja de abrir ventanas propias y dibuja para el plugin (`auto`, el valor por defecto, usa una ventana solo en las pantallas sin plugin; `layer-shell` nunca usa el plugin). `disable` restaura el presenter guardado.

Qué se instala (bajo el prefijo de instalación, `~/.local` por defecto): el paquete `share/plasma/wallpapers/io.github.uriel2121ger_art.Murasaki/` con `metadata.json`, el QML y el módulo `Murasaki.Bridge` dentro, así plasmashell no necesita variables de entorno extra. El paquete ejecuta `murasaki-surface --host <output>` una vez por pantalla. Lo que `enable` cambia fuera de él: el plugin de fondo de tus containments de Plasma (mediante la interfaz de scripting de plasmashell), una copia de seguridad de `plasma-org.kde.plasma.desktop-appletsrc` y un registro de los plugins anteriores en `~/.local/state/murasaki/`, y el ajuste del daemon.

Para deshacerlo: `murasakictl plasma disable`. Si plasmashell no responde, elige otro tipo de fondo en la configuración del escritorio, o restaura la copia de `plasma-org.kde.plasma.desktop-appletsrc` desde `~/.local/state/murasaki/` y reinicia plasmashell; borrar el directorio del paquete instalado quita el plugin. El plugin necesita `murasakid` de esta versión o posterior (revisión de API 4); si el daemon en ejecución es más antiguo, se avisa y el fondo muestra una nota hasta que se reinicie.

Tras una actualización (`cmake --install`, una actualización de paquete) no hace falta reiniciar plasmashell: el plugin vigila el `murasaki-surface` instalado (y el motor de escenas) y, en cuanto el archivo nuevo está en su sitio, es ejecutable y no cambia durante un par de segundos, vuelve a lanzar sus procesos con él, manteniendo la última imagen en pantalla hasta que se dibuje la nueva. Un binario nuevo que falle pasa por la espera de reinicio habitual y aparece en el log de plasmashell (`BridgeItem: ... was replaced on disk`). Solo la biblioteca propia del plugin, que plasmashell tiene cargada, necesita `systemctl --user restart plasma-plasmashell` para reemplazarse (cambia poco; las notas de la versión lo indican). Reinicia `murasakid` como siempre tras instalar.

## Motor de escenas

Los fondos de escena los dibuja el motor Vulkan propio de Murasaki, `murasaki-scene-vk`, que envía sus fotogramas a `murasaki-surface` por un puente. `cmake --install` lo instala junto con su plugin de video y el ayudante `murasaki-sandbox` que lo aísla; necesita un driver Vulkan 1.3 (Mesa RADV/ANV o el driver de NVIDIA). Las escenas también necesitan el directorio `assets` de Wallpaper Engine, que `murasakid` encuentra en tu instalación de Steam. Los shaders compilados se guardan en caché en `~/.cache/murasaki/shader-cache`.

Antes de mostrar una escena, el motor la revisa (`murasaki-scene-vk --check`); una escena que usa algo que el motor aún no sabe dibujar se reproduce igual, y su página en la app la marca como incompleta con el motivo. Define `MURASAKI_SCENE_VK` con la ruta de un ejecutable del motor para usar otra compilación.

## Paquetes

`cmake -B build/pkg -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr -DMURASAKI_SYSTEMD_USER_UNIT_DIR=lib/systemd/user && cmake --build build/pkg && (cd build/pkg && cpack -G DEB)` genera un `.deb` (solo servicios de usuario, nada se activa solo: el mensaje tras instalar dice cómo hacerlo). `packaging/flatpak/` tiene el manifiesto de Flatpak y sus notas. Detalles en `docs/design/specs/2026-10-17-murasaki-packaging-design.md`.

## Otros compositores

Sway, labwc, Hyprland, river, niri y COSMIC funcionan mediante wlr-layer-shell; `docs/wlroots.md` explica qué cambia respecto a Plasma (capa, cómo se detecta una ventana que tapa el fondo, reglas de pausa sin KWin) y cómo probarlo anidado.

## GNOME

GNOME no tiene layer-shell: Murasaki usa ventanas y una extensión de GNOME Shell (`data/gnome-shell/`, GNOME 46 a 50, experimental). Ver `docs/gnome.md`.

## Seguridad

Nadie revisa el contenido del Workshop, así que Murasaki nunca ejecuta nada de un elemento, marca los elementos que incluyen programas, no da red a los fondos web salvo que lo permitas para cada uno, y ejecuta los motores de escenas en un sandbox de Landlock (`murasaki-sandbox`, instalado junto a los motores). `docs/security.md` tiene el modelo de amenazas, qué cubre cada capa y qué no, cómo permitir la red a un fondo y cómo desactivar el sandbox para depurar.

## Licencia

Doble licencia: GPL-3.0 o posterior, o una licencia comercial del titular de los derechos. Ver `LICENSING.md`.
