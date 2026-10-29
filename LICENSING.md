# Licencias

**Español** · [English](LICENSING.en.md)

Murasaki tiene **doble licencia**:

1. **GNU General Public License v3.0 o posterior** (`LICENSE`): libre para todos,
   incluido el uso comercial, siempre que se cumplan los términos de la GPL
   (distribuir bajo la GPL el código fuente completo de cualquier obra
   modificada o combinada).
2. **Licencia comercial**: para quien quiera distribuir Murasaki, o código
   derivado de él, sin las obligaciones de la GPL (por ejemplo dentro de un
   producto de código cerrado). Contacta al titular de los derechos:
   uriel2121ger-art (https://github.com/uriel2121ger-art).

Ambas licencias las ofrece el único titular de los derechos. Para que siga siendo posible:

- **Las contribuciones** solo se aceptan bajo el acuerdo de contribución de
  `CONTRIBUTING.md`, que da al titular el derecho de relicenciarlas.
- **No se copia código copyleft de terceros en este repositorio.** Nunca se
  copia código de proyectos GPL. El código incluido de terceros debe tener una
  licencia permisiva (MIT, BSD, Apache-2.0, zlib o similar) y figura en
  `docs/third-party.md`.

## Qué puede cubrir la licencia comercial y qué no

La licencia comercial cubre el código propio de Murasaki. Algunos componentes de
ejecución son proyectos aparte con sus propias licencias, y una distribución
comercial tiene que cumplirlas por su cuenta:

| Componente | Licencia | Nota para una compilación comercial |
|---|---|---|
| Qt 6, KDE Frameworks, LayerShellQt | LGPL-3.0 (Qt también vende licencias comerciales) | Enlazar dinámicamente y permitir reenlazar, o comprar una licencia de Qt |
| libmpv | LGPL-2.1+ solo si se compila con `-Dgpl=false`; las compilaciones de las distros suelen ser GPL | Distribuir una compilación LGPL de mpv |
| FFmpeg | LGPL-2.1+ salvo que se compile con partes GPL | Distribuir una compilación LGPL de FFmpeg |
| Live2D Cubism Core | Propietaria (Live2D) | No lo distribuye ni lo cubre ninguna licencia de Murasaki; el usuario lo instala por su cuenta (opcional), y una compilación comercial no debe incluirlo |
| Formato de puppets de Inochi2D | BSD 2-Clause (implementación de referencia), formato leído como especificación | Dar crédito a Inochi2D Project y Kitsunebi Games; Murasaki no contiene código suyo |
| Assets de Wallpaper Engine y elementos del Workshop | Propiedad de Valve / Kristjan Skutta / los autores de cada elemento | Nunca se redistribuyen; siempre se leen de la instalación del propio usuario |

## Paquetes

- El **`.deb`** (`murasaki`) contiene solo Murasaki, con su archivo de copyright (`/usr/share/doc/murasaki/copyright`).
- El **Flatpak** compila libmpv y FFmpeg como LGPL.

Este archivo explica la intención del proyecto respecto a las licencias; no es asesoría legal.
