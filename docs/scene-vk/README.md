# murasaki-scene-vk

Murasaki's own scene engine: a plain C++20 process (no Qt) that draws Wallpaper Engine scene wallpapers with Vulkan and hands the frames to Murasaki over the bridge. It is subproject 10.

Status: milestone 5, and the default engine for scenes it accepts (milestone 6: the daemon setting `sceneEngine` is `auto`). It draws **image layers** (the orthogonal camera, alpha/additive/normal blending, tint, alpha, brightness, alignment, parent groups, user-bound visibility/alpha/colour), their **effects** (WE shaders translated to Vulkan, multi-pass chains with named render targets, per-instance overrides and live user properties), **sprite sheets**, **camera parallax** and **particle systems** (sprites, sprite trails, ropes and rope trails, static and follower children, control points; see [particles.md](particles.md)), **text** (FreeType), **SceneScript** (QuickJS-ng, see [scripts.md](scripts.md)), **sound objects and the audio spectrum** (PipeWire), **bloom**, **colour blend modes**, **camera shake**, **user-property conditions**, **puppets** (mesh and skinned animation), **random sound playback**, **multi-texture materials**, fullscreen, compose and project layers, and Workshop shaders that need the compatibility shim ([shaders.md](shaders.md)). Everything else a scene can contain makes `--check` refuse the item; the item still plays, and its page in the app names it as incomplete. Between accepted and refused there is a third state, **degraded**: the scene loads and draws, but something in it is left out (for now, the effects of text objects). The loader and the render analysis name each such feature with `degrade(reason)` into `Scene::degraded` (discovery order, no duplicates); `--check` reports them in a `degraded` array of its JSON verdict (and a `degraded: a; b` line on stderr, for people), the engine logs `drawing without: a; b` at load, and `tools/compat/check_census.py` runs the check over a whole library. A degraded scene is still accepted: the exit status and `ok` do not change. The file formats are described in [formats.md](formats.md).

All of it is written from scratch: no code of any other Wallpaper Engine renderer was copied, which keeps the dual license possible (`LICENSING.md`).

## Units

Sources are under `src/scene-vk/`; the library `murasaki_scene_vk_lib` holds everything except `main.cpp`, so the tests link it.

| Directory | What it does |
|---|---|
| `core/` | `VkContext`: Vulkan 1.3 instance (validation layer on request), device choice, one graphics queue, VMA, command pool. `FrameBufferSet`: the three images of a viewport and how they leave the process (DMA-BUF with a driver-chosen modifier, LINEAR DMA-BUF, or shared memory through a host-visible copy). `BridgeSink`: the engine side of bridge protocol v2 (handshake, Frame/Release with generations, RequestLinear/RequestShm, Resize, control messages). |
| `assets/` | `imagedecode` (embedded pictures to RGBA8: libjpeg-turbo for JPEG, a libdeflate based PNG decoder for 8-bit PNG, stb_image for everything else and as the fallback), `texcache` (decoded pictures of 512 x 512 and over, LZ4, keyed by a 128-bit hash of the source bytes and the decoder version, in `$XDG_CACHE_HOME/murasaki/texture-cache`, 1 GiB, least recently used first; `MURASAKI_TEXTURE_CACHE=0` turns it off), `PkgArchive` (`.pkg` reader, entries read with `pread`), `Vfs` (item folder, then its packages, then the Wallpaper Engine `assets/` directory; case-insensitive fallback; `..` rejected), `parseTex` (`.tex` reader: mips, LZ4, block formats, embedded images, animation section). |
| `particles/` | `ParticleDefinition` (`particles/*.json`: emitters, initializers, operators, renderers, control points, children, material) and `ParticleSystem`, the CPU simulation in a structure-of-arrays layout with fixed steps and a seedable generator. No GPU in it. |
| `scene/` | `loadScene`: `scene.json` plus models and materials into a typed `Scene` (nodes, image layers with their base pass and effect instances, camera, parallax) and the list of `unsupported` reasons. `material` / `effect`: the `materials/*.json` and `effects/*/effect.json` recipes and a scene's per-pass overrides. `PropertyStore`: user properties from `--set-property` and Properties messages. |
| `render/` | `SceneRenderer`: the render graph. Per layer: base pass, ping-pong buffers, effect passes with named targets, composite into the scene target; then one pass scales the scene into the export image (fill, fit, stretch). `PassRunner` draws one pass (pipeline cache, per-draw uniform blocks in a ring). `uniforms`: where every uniform's value comes from. `ShaderLibrary`: translated shaders per (name, combos, slots), with the `TEX<n>FORMAT` combos taken from the bound textures. `ParticleRenderer`: vertex data for every live particle and one indexed draw per system. `TargetPool`, `GpuTexture`, `sprite` (sheet frames), `analysis` (everything that decides whether a scene can be drawn, on the CPU, for `--check`). |
| `audio/` | `Mp3Stream` (streaming decode over the vendored dr_mp3), `SoundPlayer` (a decode thread filling a 48 kHz stereo float ring), `SpectrumAnalyzer` (Hann window, FFT, the 16/32/64-band curve documented in its header), `AudioBackend` with a PipeWire implementation (`pw_thread_loop`, capture of the default sink's monitor and playback streams) and the fake backends the tests use, `SceneAudio` (what the renderer owns: sounds, volume, pause, capture on demand). |
| `puppet/` | `parsePuppet` (MDL meshes, skeletons and animations with hostile-input limits) and `puppetBoneMatrices` (layered animations to `g_Bones`); drawing is `render/puppetdraw.*`. See [puppets.md](puppets.md). |
| `text/` | `TextRasterizer` (FreeType coverage bitmaps, layout rules in the header), `systemFontBytes` (fontconfig fallback). |
| `script/` | `ScriptHost` (QuickJS-ng: one module context per script, budgets, memory limit), the JavaScript prelude (`Vec2/3/4`, `WEMath`, `WEColor`, `WEVector`, `engine`, `input`, `shared`, `thisLayer`, `thisScene`, audio buffers). See [scripts.md](scripts.md). |
| `shaders/` | `source` (WE shaders with includes and annotations), `translate` (the dialect to GLSL 450, std140 blocks, reflection), `cache` (translated shaders and failures on disk), `glslcompiler` (glslang). The dialect is described in [shaders.md](shaders.md). |
| `engine/` | `options` (command line), `engine` (the frame loop), `check` (`--check` feature detection). |
| `util/` | `Json` reader (member order kept), small vector/matrix math, logging. |
| `third_party/` | Vendored volk, VMA, stb_image, dr_mp3 and QuickJS-ng (see `docs/third-party.md`). glslang is fetched at configure time. |

## Data flow

```
murasaki-surface (BridgeItem) --socketpair--> murasaki-scene-vk
  Hello, Viewport, DmabufBuffers/ShmBuffers  <--  BridgeSink::start
  Pause, Resize, Scaling, Fps, Pointer, Properties, Release  -->  BridgeSink::service
  Frame (after the GPU finished the buffer)  <--  BridgeSink::present
```

At start the engine reads the item through the `Vfs`, loads the scene (`loadScene`), refuses it when it has unsupported parts (unless `MURASAKI_SCENE_VK_FORCE` is set), uploads the textures and builds the pipelines. Then the loop runs at the requested frame rate:

1. `BridgeSink::service()` handles whatever the consumer sent.
2. When a buffer is free (the consumer holds fewer than three), the loop records one command buffer: acquire the export image, `SceneRenderer::render`, release it for the consumer, submit, wait for the fence, copy to the ring for shared memory, send `Frame`.
3. `SceneRenderer` draws the scene pass only when something changed (first frame, a property, the scaling mode, the output size, the pointer when a shader or the parallax reads it) or when a shader reads the clock or the audio, or a sprite sheet plays; every other frame only runs the scaling pass. A still wallpaper therefore costs one tiny draw per frame.

Waiting for the fence before sending `Frame` is deliberate: the consumer samples the buffer as soon as it hears about it, the same rule as `glFinish` in the GL producer.

Transport: DMA-BUF with the modifier the driver picks among those it can render to and export with a single memory plane (tiled first), LINEAR on `RequestLinear`, shared memory on `RequestShm` or when the device cannot export (`MURASAKI_BRIDGE_FORCE_SHM=1` forces it). The pixel format is RGBA8 (DRM fourcc `AB24`).

## Running

Command line:

```
murasaki-scene-vk --murasaki-bridge-fd N --murasaki-size WxH --screen-root NAME --bg ITEMDIR \
                  [--assets-dir DIR] [--scaling fill|fit|stretch] [--fps N] [--render-scale F] [--volume N] [--silent] \
                  [--set-property KEY=VALUE]... [--disable-mouse] [--disable-parallax]
murasaki-scene-vk --check ITEMDIR [--assets-dir DIR]     # prints {"ok":bool,"reasons":[...],"engine":...,"version":...}
murasaki-scene-vk --version
```

`--volume` (0 to 128) and `--silent` set the sound objects' volume; the bridge's Volume message changes it live and Pause stops the sounds and the capture. Unknown `--options` are logged and skipped.


`--check` is what Murasaki runs to decide: the reasons are short sentences ending in the milestone that will lift them ("effects: M2", "particles: M3"), or a plain failure ("cannot read scene.json", "no usable Vulkan 1.3 device: ...").

### Environment variables

| Variable | Effect |
|---|---|
| `MURASAKI_VK_VALIDATE=1` | Enables the Khronos validation layer when it is installed (a log line says so when it is not). Validation messages go to stderr. |
| `MURASAKI_VK_DEVICE` | Picks the GPU: an index, a render node (`renderD128`) or a part of the device name. By default software rasterizers come last and integrated GPUs before discrete ones. |
| `MURASAKI_VK_QUEUE_PRIORITY` | `low` (the default) asks the GPU's scheduler for the low global priority (`VK_KHR_global_priority` or the EXT; no privilege needed): the wallpaper's frames wait behind the compositor's and the applications' work, and the adaptive resolution and frame rate, which measure that wait, give way. `medium` keeps the ordinary priority. A driver without the extension, or that refuses it, gives the ordinary one; the device line of the log says which (`queue priority low` / `ordinary`). The GPU does not stop work it has started, so the priority only pays with `MURASAKI_SCENE_VK_SPLIT_MPIX`: the frame goes in pieces and the other clients' work runs before the pieces still waiting |
| `MURASAKI_SCENE_VK_SPLIT_MPIX` | The frame is sent to the GPU in pieces of about this many million pixels drawn (default `4`; `0` sends it whole). Cut only between draws, never inside one, the pieces draw the same picture (40 library scenes: byte-identical where the scene itself is deterministic). With the low queue priority the compositor and the applications wait only for the piece being drawn: 300 frames of a light scene beside 2518958708 at full size took 12.7 s with the frame whole and 4.1 s with pieces of 4 Mpx (1.2 s alone), at a cost of 5-15 % more GPU time for the wallpaper (each piece ends in a cache flush); light scenes stay in one or two pieces |
| `MURASAKI_BRIDGE_FORCE_SHM=1` | Use shared memory instead of DMA-BUF. |
| `MURASAKI_SCENE_VK_RENDER_SCALE` | Scene target pixels per output pixel (0.25 to 2, default 1). The scene target covers only the part of the scene the output shows, plus a margin, at this density and never above one pixel per scene unit; below 1 is coarser and cheaper. |
| `MURASAKI_SCENE_VK_PROFILE=1` | Times every pass of the frame with GPU timestamp queries and logs a report every 150 frames (`profile-begin` / `profile-pass` / `profile-end` lines). `python3 src/scene-vk/tools/profile.py engine.log` prints the top costs. See [performance.md](performance.md). |
| `MURASAKI_SCENE_VK_REDRAW_ALWAYS=1` | Draw and send a frame every tick even when nothing changed. By default a still scene (no clock, pointer, parallax, particle, script or audio dependence, no pending change) sends one keep-alive frame per second and does no other GPU work; any bridge message that changes the picture (property, scaling, resize, pointer when read, unpause) draws at once. |
| `MURASAKI_SCENE_VK_LAYER_CACHE=0` | Draw every layer's base picture and effect chain on every scene pass. By default a layer whose passes read neither the clock, the pointer, the audio nor the scene drawn so far (and a scene without scripts) keeps its buffers until a property, size or mode change; a layer whose base pass is such keeps just the base picture. |
| `MURASAKI_SCENE_VK_REDUCE_CHAINS=0` | Keep the `godrays` and `shine` effects' intermediate targets at the half size WE declares them at; by default they work at a quarter (they only carry blurred light; SSIM against the half size chain is 0.9999 on 1815527157). |
| `MURASAKI_SCENE_VK_PASS_OPT=0` | Turn the pass optimiser off (`skip`: keep only the identity skip; `lossless`: that and the partial draws; anything else, the default: also the reduced passes). The optimiser (`render/passprobe.h`) measures each effect pass that reads only the picture so far once per signature (shader, combos, constants, textures, sizes, quality tier), by drawing it on synthetic pictures and comparing on the CPU (`render/ssim.h`): a pass that changes nothing is not drawn (strength zero, empty mask, a combo that turns the effect off); a pass that changes only a rectangle of the picture (a displacement gated by a mask) is drawn over that rectangle on top of a copy of its input; a pass whose result survives being drawn at half size and enlarged (SSIM 0.995 at 100% quality, 0.98 below it, where a quarter is also tried) is drawn at that size. Passes whose constants move every frame are not judged (gave up after four signature changes). `MURASAKI_SCENE_VK_PASS_OPT_LOG=1` logs every verdict; `scene-vk-snap` prints the counters (`pass-opt: skipped .. reduced .. partial .. probes ..`). `tools/compat/passopt_validate.py` scores the optimiser against the whole library. |
| `MURASAKI_SCENE_VK_ADAPTIVE_FPS=0` | Hold the requested frame rate whatever the GPU time. By default, when the average GPU time of the frames drawn exceeds 85% of the frame interval the rate steps down (60, 45, 30, 20, never above `--fps`), and steps back up when the next rate up would load the GPU under 55%. Every change is logged (`adaptive fps: N fps ...`). |
| `MURASAKI_SCENE_VK_GPU_BUDGET` | Share of each frame interval the GPU may take (default `0.25`, `0` turns it off). The render scale (`--render-scale`, the setting `sceneRenderScale`) is a ceiling: while the median GPU time of the drawn frames (a window of 48) is over the budget the engine lowers the resolution (85%, 70%, 55%, 50% of the ceiling: straight to the step predicted to fit, the cost following the pixel count) and only then the frame rate (straight to the rate that fits); it comes back when the next step up is predicted to fit. The median, not the mean: a frame's GPU time includes waiting for the GPU's other clients (another output's scene, the compositor while a window is dragged), which comes in bursts, and a burst under half the window is not taken for the scene's cost. A step up followed soon by a step down makes the next step up wait twice as long (up to 8 times; a long stable time forgets it), so the rate and the resolution do not flap between two steps. Every step is logged (`adaptive resolution: ...`). |
| `MURASAKI_SCENE_VK_HALF_PARTICLES=0` | Draw every particle system straight into the scene. By default a translucent or additive sprite system whose sprites add up to 1.5 times the scene's area (clouds, fog, light) is drawn into a transparent buffer at half size and put on the scene with the matching blend (premultiplied over, or a plain sum): the same picture for a quarter of the pixels. |
| `MURASAKI_TEXTURE_CACHE=0` | No decoded-texture cache (see `assets/`). |
| `MURASAKI_SHADER_CACHE=0` | No shader cache (keys are SHA-256, failures expire after a day, 256 MiB cap). The cache lives in `$XDG_CACHE_HOME/murasaki/shader-cache` (default `~/.cache/murasaki/...`): SPIR-V by source hash, and translated shaders (SPIR-V plus reflection, failures too) by sources, combos, bound slots and translator version. |
| `MURASAKI_STEAM_ROOT` | Steam library to look for `steamapps/common/wallpaper_engine/assets` in. The engine also derives it from an item path under `.../steamapps/workshop/content/` and tries the usual per-user locations. |
| `MURASAKI_SCENE_VK_FORCE=1` | Draw an unsupported scene anyway, with only the parts that are supported (development aid; the engine otherwise refuses it). |
| `MURASAKI_SCENE_VK_CHECK_DEVICE=0` | `--check` skips probing for a Vulkan device (tests, machines without a GPU). |

Murasaki side: `MURASAKI_SCENE_VK` names the engine binary; otherwise `murasaki-scene-vk` is looked up on `PATH`, in `~/.local/bin` and in `~/.local/lib/murasaki/scene/`.

## How Murasaki picks the engine

Every scene is drawn by this engine. The first time an item is shown `--check` runs in the background (it never blocks the surface or the GUI); its verdict, a "no" and also a failure to answer (crash, timeout after 8 s, garbage; kept for one day) go to `$XDG_CACHE_HOME/murasaki/scene-check.json`, keyed by the item folder, the newest modification time anywhere in its tree, the Wallpaper Engine assets directory's, and the engine binary (path, size, mtime), so an updated item or engine is asked again. In practice `--check` takes 0.1 to 0.3 s per item (it creates a Vulkan device unless `MURASAKI_SCENE_VK_CHECK_DEVICE=0`), plus the cost of reading the scene's texture headers. A crash of this engine is restarted once like any host. The GUI shows `Engine: Vulkan`, or `Engine: Vulkan (incomplete: reason)` for an item the check refused, on the page of a scene.


## Building and testing

`MURASAKI_SCENE_VK` (`AUTO` by default) turns the target on when the Vulkan headers and `liblz4` are found. The first configure downloads the glslang release tarball (a system glslang is used when there is one); the presets share the download through `MURASAKI_FETCH_DIR`. glslang is built as a static library only. Vulkan 1.3 is required at run time (dynamic rendering and synchronization2).

```
cmake --build --preset dev && env -u DISPLAY -u WAYLAND_DISPLAY ctest --preset dev -R 'scenevk|sceneselect|sceneengine'
```

| Test | Covers |
|---|---|
| `tst_scenevk_json` | the JSON dialect and its failures |
| `tst_scenevk_assets` | PKG, Vfs, TEX (every format, container version, truncation sweeps) |
| `tst_scenevk_scene` | the loader and every refusal reason |
| `tst_scenevk_particles` | particle definitions and the simulation: rates, lifetimes, operators, emitters' volumes, seeds, control points, map sequences, trails' history |
| `tst_scenevk_core` | exportable images read back through shared memory, mmap and an EGL import |
| `tst_scenevk_shaders` | shader sources, the translator (interface rewriting, std140 offsets against the compiler's), the cache |
| `tst_scenevk_materials` | materials, effect definitions, scene overrides |
| `tst_scenevk_uniforms` | where uniform values come from and how they are laid out |
| `tst_scenevk_analysis` | what makes a scene undrawable (shaders, textures, limits) and the sprite frame maths |
| `tst_scenevk_render` | the renderer on the GPU against exact pixels (orientation, rotation, blending, scaling modes, effects and chains, named targets, parallax, sprite sheets, dirty tracking, particles: squares in place, emitter boxes, pointer control points, ropes, followers, perspective) |
| `tst_scenevk_audio` | mp3 decoding, the player ring, the FFT and the spectrum's curve and smoothing, `SceneAudio` against fake backends |
| `tst_scenevk_pipewire` | the real PipeWire backend (manual: `MURASAKI_TEST_PIPEWIRE=1`; skipped otherwise) |
| `tst_scenevk_text` | layout: sizes, wrapping, alignment boxes, fonts |
| `tst_scenevk_script` | the script host with a fake scene: API surface, vectors, timers, budgets, errors, memory limit |
| `tst_scenevk_engine` | the real binary against a fake consumer: handshake, frames, pause, resize, transport requests, errors |
| `tst_scenevk_check` | `--check` JSON on synthetic items |
| `tst_sceneselect` | engine choice, verdict cache and crash ledger with fake engines |

GPU tests skip themselves without a Vulkan 1.3 device (or DMA-BUF export, or a render node). Fixtures are generated by the tests (`tests/scenevkfixtures.h`); nothing is copied from Workshop items. The Khronos validation layer is not installed on the development machine; the tests report zero validation messages whenever it is.

## What is not there yet

| Feature | Milestone |
|---|---|
| Array uniforms get no values (zero); `mat4x3` and friends only for engine matrices | later |
| Presets of Workshop items; Workshop shaders the compatibility shim cannot repair | on demand |
| Particle features: `vortex` and the other rarer operators, `eventspawn`/`eventdeath` children | on demand |
| `requestAnimationFrame`, network access, a `localStorage` that survives a restart | on demand |
| 3D models, lights, HDR (puppets are done: [puppets.md](puppets.md)) | when an item needs them |

Known limits: the scene target is rendered at up to twice the size it is shown at and reduced with one bilinear step (no mips), and effect buffers have the layer's size at that resolution; a still scene is redrawn at the frame rate through the cheap pass; embedded JPEG mips are all decoded at start (about 3 s of CPU for a 4096x4096 picture).

Failure handling: an exception during start-up or a frame sends an `Error` message to the consumer before the process exits with status 1; a lost GPU (`VK_ERROR_DEVICE_LOST`) exits with status 3 so the restart policy can tell it apart. `MURASAKI_SCENE_VK_FAULT=devicelost` is a test hook that raises that error on the third frame.

## Spanning (SpanView)

A scene assigned with `span` is drawn across several outputs by one `murasaki-scene-vk` per output. The consumer sends
`SpanView` (bridge message 43, defined in `src/bridge/spanview.h`, not in `protocol.h`) with the canvas size and this output's rectangle of it, in the compositor's logical pixels
(fixed point, 1/64 pixel). The engine applies the scaling mode to the whole canvas, at its own pixels per logical unit,
and draws only that rectangle; `enabled` 0 returns to drawing the whole scene. The engine announces Hello version 3;
a consumer sends `SpanView` only to a producer that did.
