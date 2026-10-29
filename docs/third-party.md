# Third-party code in this repository

Only permissively licensed code may be vendored (see `LICENSING.md`). Each entry lists the path, upstream, version and license.

| Path | Upstream | Version | License |
|---|---|---|---|
| `src/scene-vk/third_party/volk/` | https://github.com/zeux/volk | `vulkan-sdk-1.4.341.0` (`volk.h`, `volk.c`) | MIT |
| `src/scene-vk/third_party/vma/` | https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator | `v3.3.0` (`vk_mem_alloc.h`) | MIT |
| `src/scene-vk/third_party/stb/` | https://github.com/nothings/stb | commit `2c980bb59875b0d32144a71867fbdebb2f77cd20` (`stb_image.h` v2.30) | MIT or public domain (dual) |
| `src/scene-vk/third_party/dr_libs/` | https://github.com/mackron/dr_libs | commit `dfe8377631000664666519fdb83da193fd8037f4` (`dr_mp3.h` v0.7.4, SHA-256 `997b7ee18de6e6b81e2a83f1ea9fc62aef25c62b28d48db95635f49e65de0a2f`) | Public domain (Unlicense) or MIT-0 (minimp3 inside: CC0) |
| `src/scene-vk/third_party/quickjs/` | https://github.com/quickjs-ng/quickjs | `v0.17.0` (`quickjs.c/.h`, `dtoa`, `libregexp`, `libunicode`, the headers they include; no `quickjs-libc`), tarball SHA-256 `559bc4c420475e55c7ab4510adbc562f55d7524d75e8e89d79ce4bb02f5687d9` | MIT |
| `src/surface/protocols/wlr-foreign-toplevel-management-unstable-v1.xml` | https://gitlab.freedesktop.org/wlroots/wlr-protocols | master at commit `bf4fc79abc359eea5a0edec0ac6d4a2b2955f82a`, SHA-256 `4ecc4588858e29fe680a33521e1f22bcf22071d66d1553fe96b4ddec03d591d2` (protocol description only; `wayland-scanner` generates the code at build time) | MIT-style (copyright Ilia Bozhinov, the license text is in the file) |
| glslang (not vendored: fetched at configure time, or the system package when installed) | https://github.com/KhronosGroup/glslang | `16.6.0`, tarball SHA-256 `9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2` | BSD-3-Clause, Apache-2.0 and MIT parts |

Notes:

- `stb_image.h` carries one local change, marked `murasaki:` (`stbi__getn` returns at once for an empty read: UBSan reports the null `memcpy` of an empty PNG IDAT chunk).
- `dr_flac.h` (v0.13.4) and `dr_wav.h` (v0.14.6) sit beside `dr_mp3.h` in `third_party/dr_libs/` (same commit as `dr_mp3.h`, public domain or MIT-0), and `stb_vorbis.c` beside `stb_image.h` in `third_party/stb/` (same commit, public domain or MIT): the FLAC, WAV and Ogg Vorbis sound decoders of `murasaki-scene-vk` (SHA-256 `111144e7...cff945c`, `03e70c1a...c4493`, `4c7cb2ff...e65636`).
- volk, VMA, stb_image, dr_mp3 (dr_libs) and QuickJS-ng are used only by `murasaki-scene-vk` (`src/scene-vk`). Each directory keeps the upstream license file next to the source.
- glslang is built as a static library only: no binaries, tests, HLSL front end or SPIRV-Tools optimizer. `MURASAKI_FETCH_DIR` (set by the presets) keeps the downloaded tarball between build directories.
- lz4 (BSD-2-Clause) and the Vulkan headers are system dependencies, not vendored.
- libjpeg-turbo (IJG and BSD-3-Clause licenses, `libjpeg-turbo8-dev`) and libdeflate (MIT, `libdeflate-dev`) are optional system dependencies of `murasaki-scene-vk`: the first decodes the JPEG textures with SIMD, the second inflates PNG textures. Without them the engine decodes with the vendored stb_image, several times slower on large pictures.
- libav (libavformat, libavcodec, libswscale, libavutil; LGPL-2.1-or-later in the distribution builds: `libavformat-dev` and friends) is an optional system dependency of `murasaki-scene-vk` for the MP4 streams of video textures. It is linked dynamically (into a plugin of its own, `libmurasaki-scene-vk-video.so`, that the engine loads with dlopen only when a scene has a video: libavcodec drags in every codec's libraries, 125 of them) and never vendored, so the LGPL's relinking terms hold; the engine uses it only through its public API, from memory (no files, no network), with the MP4 demuxer and a list of six video decoders; hardware decoding goes through libva (MIT) with the machine's own VAAPI driver, opened on the render node the engine already draws with, and any failure of it means software decoding. Without it the engine refuses scenes with video textures, which then show their preview.
- The **Inochi2D puppet format** (INP v1, tag `v0.8.7` of https://github.com/Inochi2D/inochi2d, BSD 2-Clause, and `inmath` 1.3.x, MIT) is a *specification read*: its source was read to document the format (`docs/pets/puppets.md`, section 12) and no code of it is vendored or translated. Credit: Inochi2D Project and Kitsunebi Games.
- **Live2D Cubism Core** is not distributed and not vendored; a future optional back end would load a Core library that the user installs themselves (`docs/pets/puppets.md`, section 5).
  Notices: Inochi2D, BSD 2-Clause License, "Copyright (c) 2020, Inochi2D Project" (tag `v0.8.7`; the file on `main` reads "Copyright (c) 2020-2026, Inochi2D Project" and "Copyright (c) 2020-2026, Kitsunebi Games"). inmath, MIT License, "Copyright (c) 2011-2021 David Herberth, 2021-2024 Inochi2D Project". Their source was read as a specification (through summaries); nothing of it is in this repository.
