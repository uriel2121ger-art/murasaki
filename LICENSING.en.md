# Licensing

[Español](LICENSING.md) · **English**

Murasaki is **dual-licensed**:

1. **GNU General Public License v3.0 or later** (`LICENSE`): free for everyone,
   including commercial use, as long as the GPL's terms are met (distributing
   the complete corresponding source of any modified or combined work under the
   GPL).
2. **Commercial license**: for anyone who wants to ship Murasaki, or code
   derived from it, without the GPL's obligations (for example inside a
   closed-source product). Contact the copyright holder: uriel2121ger-art
   (https://github.com/uriel2121ger-art).

Both licenses are offered by the sole copyright holder. To keep that possible:

- **Contributions** are accepted only under the contributor agreement in
  `CONTRIBUTING.en.md`, which grants the copyright holder the right to relicense
  them.
- **No third-party copyleft code is copied into this repository.** Code from
  GPL projects is never copied. Vendored code
  must be under a permissive license (MIT, BSD, Apache-2.0, zlib or similar)
  and is listed in `docs/third-party.md`.

## What the commercial license can and cannot cover

The commercial license covers Murasaki's own code. Some runtime components are
separate projects with their own licenses, and a commercial distribution has to
satisfy them on its own:

| Component | License | Note for a commercial build |
|---|---|---|
| Qt 6, KDE Frameworks, LayerShellQt | LGPL-3.0 (Qt also sells commercial licenses) | Link dynamically and allow relinking, or buy a Qt license |
| libmpv | LGPL-2.1+ only when built with `-Dgpl=false`; distro builds are often GPL | Ship an LGPL build of mpv |
| FFmpeg | LGPL-2.1+ unless built with GPL parts | Ship an LGPL build of FFmpeg |
| Live2D Cubism Core | Proprietary (Live2D) | Not distributed or covered by either Murasaki licence; the user installs it themselves (optional), and a commercial build must not bundle it |
| Inochi2D puppet format | BSD 2-Clause (reference implementation), format read as a specification | Credit Inochi2D Project and Kitsunebi Games; no code of it is in Murasaki |
| Wallpaper Engine assets and Workshop items | Owned by Valve / Kristjan Skutta / item authors | Never redistributed; always read from the user's own installation |

## Packages

- The **`.deb`** (`murasaki`) contains Murasaki only, with its copyright file (`/usr/share/doc/murasaki/copyright`).
- The **Flatpak** builds libmpv and FFmpeg as LGPL.

This file explains the project's licensing intent; it is not legal advice.
