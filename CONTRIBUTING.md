# Cómo contribuir

**Español** · [English](CONTRIBUTING.en.md)

## Convenciones

- C++20 + Qt 6. Formatea con `.clang-format`. El código y los identificadores van en inglés.
- Los commits siguen Conventional Commits en inglés (`feat:`, `fix:`, `chore:`, `docs:`, `test:`).
- Cada cambio de comportamiento viene con una prueba de Qt Test. Escribe la prueba primero.
- Nunca escribas secretos en el código; léelos del entorno o de un archivo `.env` que no se sube al repositorio.

## Límites legales

- Nunca subas assets de Wallpaper Engine, elementos del Workshop ni archivos de Anima Engine, tampoco como datos de prueba. Las pruebas generan datos sintéticos en directorios temporales.
- Nunca copies código de otros motores de fondos de pantalla; implementa el comportamiento y los formatos de archivo a partir de documentación pública y de los propios archivos.
- Las suscripciones al Workshop pasan por el cliente de Steam del usuario (URLs `steam://`). Nunca te hagas pasar por un App ID de Steam.

## Acuerdo de contribución (doble licencia)

Murasaki tiene doble licencia (GPL-3.0 o posterior y una licencia comercial, ver `LICENSING.md`). Al enviar una contribución aceptas que:

1. la escribiste tú, o tienes derecho a enviarla;
2. se licencia al proyecto bajo la GPL-3.0 o posterior; y
3. concedes al titular de los derechos (uriel2121ger-art) una licencia perpetua, mundial, gratuita e irrevocable para usarla, modificarla y relicenciarla en cualquier condición, incluida la licencia comercial.

Añade una línea `Signed-off-by:` (`git commit -s`) para confirmarlo. No copies código de otros proyectos salvo que tenga una licencia permisiva y figure en `docs/third-party.md`; nunca copies código de proyectos GPL (léelos solo para entender su comportamiento).
