# Wallpaper Engine scene formats, as murasaki-scene-vk reads them

What the engine learned about PKG, TEX and `scene.json`, in prose. It comes from reading real Workshop items (read-only, in the author's library) and from comparing the engine's frames with the items' own previews. Where a statement is an assumption rather than something checked against data, it says so. The code that implements each part is named so the two can be read side by side.

Provenance: this document and the code it describes were written from knowledge of the formats; no code of any other renderer was copied or transliterated.

## Item layout

A Workshop item is a folder (`.../steamapps/workshop/content/431960/<id>/`). `project.json` names its `type` (`scene`, `video`, `web`, `application`; the case varies: `Scene`, `Web`), its entry `file` (a scene's `scene.json`, but the name is not fixed: one item calls it `gifscene.json`), a `preview` image and, for presets, a `dependency` (the id of the item they customize; a preset has no scene of its own). User properties are declared under `general.properties`.

A scene's files usually sit inside one package in the folder. It is called `scene.pkg` in most items and `gifscene.pkg` in others, so the engine takes `scene.pkg` first and then any other `*.pkg` in name order. Resources the scene needs but the item does not carry (the standard shaders, the particle presets, the stock textures) come from the Wallpaper Engine installation's `assets/` directory, which has the same layout as a package. Lookups therefore search: item folder, packages, assets (`assets/Vfs`).

Paths inside scene files use forward slashes, sometimes backslashes, and do not respect case; the file system on disk does. The `Vfs` normalizes separators and falls back to a case-insensitive match, and refuses any path with a `..` component.

## PKG (`assets/pkg.cpp`)

All integers are little-endian 32-bit.

1. A length-prefixed ASCII version string, `PKGV0001` up to `PKGV0024` in the library. The revisions do not change the layout described here; they track what the editor ships.
2. The entry count.
3. For every entry: a length-prefixed name, the offset and the length of its data.
4. The data of all entries, back to back. Entry offsets count from the first byte after the table (not from the start of the file).

The reader keeps the file open and reads an entry with `pread` when asked. It rejects a table that is cut short, an absurd count (more than about a million), names over 4 KiB and any entry that reaches past the end of the file.

## TEX (`assets/tex.cpp`)

A texture is a chain of sections introduced by nine-byte tags (eight characters and a NUL). All numbers are little-endian 32-bit.

**Header.** `TEXV0005`, `TEXI0001`, then the pixel format code, the flags, the *storage* size (width, height), the *image* size (width, height) and one unknown number. The storage size is what the GPU image has (often padded to a power of two: 4096x4096 for a 3979x2480 picture); the image size is the picture inside it. A shader samples u from 0 to image width / storage width and v from 0 to image height / storage height, and the engine does the same (`GpuTexture::uMax/vMax`); sampling 0..1 would show the padding. When the flag word has bit 6 set (below), one more number follows the unknown one, before the container tag.

**Flags.** Bit 0: no interpolation (nearest sampling). Bit 1: clamp UVs (otherwise repeat). Bit 2: the texture is animated (a `TEXS` section follows). Bit 3: clamp to border. Bit 5: video. Bit 6 (64): a volume texture, see below. Bit 19 (524288) marks single or dual channel textures whose alpha lives in the second channel; M1 does not use it.

**Format codes.** 0 RGBA8 (bytes R, G, B, A in memory), 1 RGB888, 2 RGB565, 4 DXT5, 6 DXT3, 7 DXT1, 8 RG88, 9 R8, 10 RG1616f, 11 R16f, 12 BC7, 13 RGBA1010102, 14 RGBA16161616f, 15 RGB161616f. Codes 4, 6 and 7 are the S3TC block formats; they are uploaded as BC3, BC2 and BC1 (`PixelFormat` maps one to one). RGB888 and RGB565 are expanded to RGBA8 on read. RGBA1010102 (13) is a packed 32-bit word per pixel, uploaded as `A2B10G10R10_UNORM_PACK32` (red in the low ten bits, alpha in the top two: the usual DXGI R10G10B10A2 layout; assumption, because no file in the library or the app's assets uses it, so the order could not be checked against pixels). RGB161616f (15) is three half floats a pixel; it is widened to RGBA16F with alpha 1.0 (`0x3C00`). A code not listed is refused with `unsupported texture format N (unknown)` when pixels are wanted; a header-only read (`--check` feature detection) still parses the layout. Whatever the code, `TEX<n>FORMAT` shows the file's own number (`GpuTexture::rawFormat`).

**Image container.** `TEXB0001` to `TEXB0004`, then the image count. From `TEXB0003` on, a signed number follows: the free-image format, where -1 means raw pixels and anything else means each mip holds an ordinary image file in that format. `TEXB0004` adds a flag for video. Only videos use the extra per-mip editor fields that `TEXB0004` describes; a picture in a `TEXB0004` container is laid out exactly like `TEXB0003` (a real item showed this: reading the editor fields from a picture runs off the data). A video (flag bit 5 of the file, whatever the container version says: the library's `TEXB0004` ones carry a zero in the video field too) holds an MP4 stream as the payload of its first mip, uncompressed; the other mips, if any, are not pictures. The header read learns the stream's size and nothing else. The engine plays it (`assets/videoplugin.cpp`, a plugin the engine loads with dlopen on the first video: next to the executable, in `../lib/murasaki` or where `MURASAKI_SCENE_VK_VIDEO_PLUGIN` says): libavformat reads the bytes from memory as MP4 only, libavcodec decodes H.264, HEVC, VP8, VP9, AV1 or MPEG-4 on a thread of its own, on the GPU through VAAPI (on the render node of the device the engine draws with) when the machine can and in software when it cannot or when `MURASAKI_SCENE_VK_VIDEO=cpu` says so, converting no more pictures a second than the loop's frame rate that stays four pictures ahead of the scene's clock and waits there (a scene that is not drawn costs nothing), libswscale makes RGBA of at most 1920 x 1080 on the CPU path (a stream's own size is limited to 8192 each way; held in memory a stream is limited to 128 MiB, and a stream in a file over 16 MiB is not held at all: it is read where it lies in the package, with the decoder's own descriptor, up to 2 GiB, the size the package format can store). With the decoder's surfaces drawn by the GPU (`docs/scene-vk/performance.md`) the picture is as big as the stream up to 3840 x 2160, which costs the texture's 33 MB of video memory and no CPU, and the picture whose time has come is copied into a one-mip texture before each frame. The stream starts over at its end; its sound is not played; the matrix of the colour conversion is the stream's own, else BT.709 from 720 lines up. Without libav in the build, or without the plugin where the engine looks, `--check` refuses a scene with a video texture.

**Volume textures (flag bit 6).** The 28 colour look-up tables of the app's `assets/materials/lut/` (32 x 32 x 32 cubes) carry this flag. They differ from other textures in two places: the header has one extra number after the unknown one (four bytes that look like an opaque colour; it is kept and ignored), and every mip has a depth number (32) right after its height. The picture is an embedded PNG 32 wide and 1024 tall, the 32 slices one below the other (checked on `neutral.tex`: red grows along x, green along y inside a slice, blue with the slice number, so it is an identity cube), while the header announces sizes that do not match it (1024 x 32), so the decoded picture's size replaces the header's. The engine loads such a texture as that plain 2D image, `Tex::volumeDepth` holding the slice count; a shader sampling it as a 3D texture is not supported, and no scene or asset in the library references these files.

**Per image:** the mip count, then for every mip its width and height; in `TEXB0002` and later a compression flag and the uncompressed size; then the stored size and that many bytes. A compression flag of 1 means the bytes are one LZ4 block (`LZ4_decompress_safe` with the stored and uncompressed sizes; a result of a different length is corruption). Mips are listed largest first. A picture uses its first image only; the other images are skipped. A sprite sheet (animated flag) keeps all of them, see "Animation" below. A file whose outer or inner tag is not `TEXV0005`/`TEXI0001`, or whose image container is not `TEXB0001` to `TEXB0004`, is refused naming the tags it has (none exists in the library).

**Embedded images.** In real items most pictures are JPEG (free-image code 2) or PNG inside the mips, one file per mip, nine mips typical. The reader decodes them with stb_image to RGBA8; the decoded size replaces the stored mip size. The number of mips and their sizes must halve cleanly for Vulkan, so the engine uses the mips as far as they do and otherwise generates the chain by blits.

**Single and dual channel textures.** The engine reads R8 as (r, r, r, 1) and RG88 as (r, r, r, g), which is how grey and mask textures are consumed. This is an assumption, not checked against a scene that draws one directly.

**Animation (`TEXS0001` to `TEXS0003`).** After the images, when the animated flag is set: the frame count, in `TEXS0003` the sheet width and height, then per frame an index, a duration in seconds and the frame's place in the sheet: position and the two axis vectors (width and height for an upright frame). `TEXS0001` stores these as integers and without the second components, the others as floats. A texture with several frames is a sprite sheet: the material's `SPRITESHEET` combo makes the shader place the frame with `g_Texture0Rotation` and `g_Texture0Translation` (position and axes of the frame in the sheet divided by the sheet size; see render/sprite.h), the frames play in order with their durations and loop. **Sheets on several images.** The first field of a frame is the image of the container the frame lies on (always 0 on a single-image sheet; in the library 16 scenes have sheets with 2 to 7 images, and in each the largest index is the image count minus one, the pages of one large sheet: 3131523956 has 300 frames on five images of 7680 x 7920). The frame's position and axes are in the texels of its own image, whose size may differ from the first one's (2164591875's second image is 1024 x 512, the first 1024 x 1024). `parseTex` keeps every image of a sheet as `Tex::pages` while the pixel data stays under the 512 MiB limit (all images counted together); an image that would pass it, and all after it, are left out and counted in `droppedPages` (the scene is then degraded as `sprite sheet pages`, and the frames that name a missing image show the last one kept: 3241818992 keeps two of six (its images are 199 MB each) and 3269986564 two of four (232 MB each); 2852537340 and 2734996911, two images of exactly 256 MiB, fit). The GPU keeps one image per page (`GpuTexture::pageView`) and the renderer samples the page of the frame being shown, dividing the frame's place by that page's size (`spriteFrame` with page sizes; `g_Texture0Resolution` is the page's). A frame with a quarter-turn (axes (0, -h) and (w, 0), seen in 2164591875 and 2734996911) needs nothing else: all four axis components go into `g_Texture0Rotation`. Particle sprite sheets still read the first image only (none in the library has more). A one-frame sheet is just a picture. `--check` refuses a sheet drawn with a shader that has no such support.

## Texture formats in the library

Counted by a script that walks every `.tex` file, loose or inside a package, of the Workshop library (8433 textures) and of the app's `assets/` (311 textures, none packaged), reading only the headers and the section chain. The numbers are for the first image of each file.

| What | Count |
|---|---|
| `TEXV0005` / `TEXI0001` | 8744 / 8744 (no other outer or inner tag) |
| `TEXB0001` / `TEXB0002` / `TEXB0003` / `TEXB0004` | 43 / 64 / 5675 / 2962 (28 of the 4s are the volume LUTs) |
| `TEXB0004` whose video field is 1 | 0 (the 46 videos are told by flag bit 5; their field is 0) |
| Free-image format -1 (raw pixels) / 13 (PNG) / 2 (JPEG) | 6030 / 2535 (28 of them the LUTs) / 179 |
| Pixel format 0 RGBA8888 | 4141 (28 of them the LUTs) |
| 4 DXT5 / 6 DXT3 / 7 DXT1 | 1765 / 27 / 55 |
| 8 RG88 / 9 R8 | 678 / 2078 |
| 1, 2, 10, 11, 12, 13, 14, 15 | 0 each: RGB888, RGB565, RG1616f, R16f, BC7, RGBA1010102, RGBA16161616f and RGB161616f occur nowhere (they are read, covered by tests on synthetic files) |
| Mip count of the first image | 1: 4485, 2: 147, 3: 934, 4: 1238, 5: 1082, 6: 179, 7: 192, 8: 178, 9: 148, 10: 112, 11: 16, 12: 5 |
| First mip LZ4-compressed / stored | 5562 / 3154 |
| Flag bit 0 (nearest) / 1 (clamp) / 2 (animated) / 5 (video) / 6 (volume) | 146 / 8262 / 313 / 46 / 28 (bit 3, clamp to border, never) |
| Flag bit 19 | 82, on DXT5, RG88 and R8 textures |
| Flag bits 20 / 21 / 22 / 23 | 9 / 8 / 11 / 10, on RGBA8 textures. Meaning unknown; ignored |
| `TEXS0001` / `TEXS0002` / `TEXS0003` | 1 / 8 / 304 |

(Every texture with the animated flag has a `TEXS` section: 313 = 1 + 8 + 304.)

Textures refused or degraded by their format in the real-path sweep (`compat/univ2-realpath`, 492 items): none. Before this phase the only textures that did not read were the 28 volume LUTs of the app's assets (`unknown image container`), which nothing references.

## `scene.json` (`scene/scene.cpp`)

Top level: `general`, `camera`, `objects`.

**`general`.** `orthogonalprojection` gives the scene size in scene units (`width`, `height`); a scene without it uses a perspective camera (see "Camera" below). `clearcolor` is "r g b" in 0..1. `cameraparallax`, `cameraparallaxamount`, `cameraparallaxdelay` and `cameraparallaxmouseinfluence` drive the camera parallax (each may be a user binding); `camerashake` (with `camerashakeamplitude`, `camerashakeroughness`, `camerashakespeed`) moves the whole picture (see "Camera shake" below); `bloom` (with `bloomstrength`, `bloomthreshold`, `bloomtint`, each possibly user-bound) adds the glow pass; `hdr` switches to floating point buffers and the HDR bloom (see "HDR scenes"); `fogdistance*` and `fogheight*` are the fog (see "Fog"). `clearenabled` and the keys named in "Complete, degraded, refused" below are read as that section says; `camerapreview` and the rest are ignored.

**`camera`.** `eye`, `center`, `up`. Every ortho scene in the library looks straight down -z with y up. The eye position is *not* applied: some scenes were saved with an eye far from the origin (an editor leftover) and are meant to be seen without moving anything; with the eye applied they were visibly shifted. A camera that is not upright or does not look down -z is refused.

**Coordinates.** x to the right, y up, origin at the lower left of the projection, which spans exactly `orthogonalprojection`. The engine's projection is `[0, width] x [0, height]` and image rows run top to bottom in the output. Depth does not matter (no depth test); objects draw in file order, later on top.

**`objects`.** Each has an `id`, a `name`, and strings `origin`, `scale`, `angles` ("x y z"; angles in radians), plus:

- `image`: the path of a model JSON (an image layer). `size` ("w h" in scene units) is the quad's size. `alignment` places the quad's anchor: `center` (default), or a combination of `top`, `bottom`, `left`, `right` such as `topleft`; the quad is `size` wide and the origin sits at the named point of it.
- `parent`: the id of another object. The child's transform is relative to it (origin, rotation and scale compose), and a hidden parent hides the child. An object with no drawable (no `image`, `particle`, `text`, `shape`, `sound`, `light` or `model`) is a group, only a transform.
- `visible`, `alpha`, `color`, `brightness`: a literal, or an object `{"user": "<property>", "value": <default>}` binding it to a user property; the value is what applies while the property has not been set. The same shapes occur for `origin`, `scale`, `angles`, an image layer's `size` and `parallaxDepth`, and the engine applies a bound vector live: the property's value (a string "x y z", "x y" for a size or a depth, an array, or one number that sets every component) replaces the file's, on load and at every property update; a property that is missing or does not read as numbers leaves the file's value, a short value sets only the components it has, and angles are taken as the file gives them (radians, like the file's own). A script or keyframes on the same property own it and the binding is dropped (as for a script, the file's `animation` plays). A text layer's size and a fullscreen layer's size are not bindable. A value `{"script": ..., "value": ...}` is SceneScript (see [scripts.md](scripts.md)); `{"user": {"name": ..., "condition": ...}, "value": ...}` is a boolean that holds while the property equals the condition (a visibility, or an effect's `visible`; on any other property it is refused as "property condition").
- Rotation: a positive z angle turns the picture counter-clockwise in the y-up scene (checked with an item whose layer is turned a quarter turn). Euler angles apply x, then y, then z.
- Kinds that are refused: `model` objects (M5), a `light` of an unknown type (`light type '<type>'`; the others: Lights below), `animationlayers` (puppets, M5), a `colorBlendMode` outside 1 to 32. Particles, text, sound objects, effects and colour blend modes are described in their own sections.
- `parallaxDepth` ("x y", default 1 1) is how much the camera parallax moves the layer; 0 keeps it still. Parallax: the displacement eases towards `(-(px - 0.5), py - 0.5) * amount * influence` (pointer in 0..1, top-left origin; scene y is up, so only x flips) with a follow factor of `10 * max(1 - delay / 3, 0)` per second (`delay` 0 follows at once), and a layer moves by `depth * displacement * scene size`.
- Keys read and ignored because they do not change an image layer drawn with the ortho camera: `solid`, `perspective`, `ledsource`, `locktransforms`, `copybackground`.

**Model JSON.** `material` names the material file. `width`/`height` give the layer's size when the object has none, otherwise the object's `size` wins; `autosize` leaves the size to the picture, which is what a layer with no size of any kind gets (details in "Complete, degraded, refused"). `puppet` makes it a puppet (M5).

**Material JSON.** `passes`, an array; M1 reads exactly one. A pass has `shader` (`genericimage2` and `genericimage` are the plain image shaders; anything else is M2), `blending`, `textures`, `combos` and `constantshadervalues`.

- `blending`: `translucent` (source alpha over: SRC_ALPHA, ONE_MINUS_SRC_ALPHA), `additive` (SRC_ALPHA, ONE), `normal` (the picture's colour replaces what is below, alpha ignored). All blend in plain (not linear) colour, as the originals do.
- `textures`: names, not paths. The file is `materials/<name>.tex` (so `particle/halo` is `materials/particle/halo.tex`). Names starting with `_rt_` are render targets, not files (M2). The other slots (`g_Texture1` and on: masks, normal maps) are files bound beside the first; a render target in slot 0 with others after it is refused.
- `combos`: the shader's compile-time options. The keys are lower case in material files. `version` only picks which colour uniform the shader reads; `spritesheet` needs animation, which the texture check handles. Any other option that is set to a non-zero value (`lighting`, `reflection`, `blendmode`, ...) is refused with its name.
- `constantshadervalues`: `alpha`, `brightness` and `color` multiply the layer, like the object's own `alpha`, `brightness` and `color`; the tint is `color * color * brightness` and the alpha the product of both alphas. (The `genericimage2` shader names these `g_UserAlpha`, `g_Brightness` and, for `version` 2, `g_Color4`.)

### Solid layers and keyframed properties

- **Solid layers** (`models/util/solidlayer.json`, `"solidlayer": true`, or a material with the `flat` shader and no textures): drawn with WE's `flat` shader, the object's `color` and `alpha` (and the material's) as `g_Color` and `g_Alpha`, over the object's `size`. They go through effects, scripts and blending like any layer. A vertex attribute the shader body never names (the `flat` shader's `a_Color` without VERTEXCOLOR) is not an input.
- **Keyframed properties** (`"alpha": {"value": 1, "animation": {"c0": [{"frame": 0, "value": 1}, ...], "options": {"fps": 30, "length": 480, "mode": "single"}}}`): an object's `origin`, `scale` and `angles` (components `c0..c2`) and an image or text layer's `alpha` (`c0`). The scene clock times them: frame = seconds * fps, `single` holds the last value past `length`, `loop` wraps, `mirror` runs back. `"relative": true` in the `animation` object makes the keyframe values offsets: the value is the property's own `value` plus the keyframe (component-wise), for every animated property (origin, scale, angles, alpha, effect constants, a camera object's zoom). The library has 524 relative animations in 33 scenes: 381 on `angles`, 125 on `origin`, 18 on `scale` (none on alpha or constants, which the rule covers anyway). A track linked to another of the same object (`options.parent: {"key": "zoom"}`, with `children: [{"key": "origin"}]` on the parent) plays on the parent's clock: the parent's fps, length and mode replace the child's (a choice: in the library they always agree); a `single` clip holds its last key past the end. Between keyframes the value follows the bezier the keys' handles describe. What the files show: every key has `front` and `back` handles with `enabled`, `x` and `y`; the default pair is front (1, 0) and back (-1, 0) and the others are small fractions (-0.97, 0.14), so a handle is read as a share of its segment (x of the frames to the neighbouring key, y of the value difference), the default pair is an ease in and out, and a handle that is not enabled is the straight line's third. This is inferred, not checked against WE: keyframe values are exact, the shape between them is the uncertain part (`magic`, `lockangle` and `locklength` are ignored). An alpha bound to a user property is the property's (its keyframes do not play); a property that also has a `script` keeps its keyframes: they are sampled first and the script runs after them each frame (what its `update` returns wins), and scripts steer them with `getAnimation()` (scripts.md). `options.startpaused` (144 animations in the library, most of them a media widget's intro) holds the animation at frame 0 until a script plays it; `options.name` is what `getAnimation(name)` finds. Other animated properties (effect constants, `size`, `brightness`, colours) keep their static value. Caps: 4096 animated properties, 4096 keyframes per component.

### HDR scenes

`"hdr": true` (read once, when the renderer is built) makes the scene target and every buffer drawn on the way to it (the layers' ping-pong buffers, effect `fbos` and `_rt_*` targets except `r8` ones, the scene copies, the half-size particle buffer) RGBA16F instead of RGBA8, so values above 1 survive layers, effects, blending and particles. An ordinary scene keeps RGBA8 everywhere and the same shaders: nothing about it changes. Every pass of an HDR scene is translated with the combo `HDR` = 1, which WE's shaders read as "the scene is HDR" (`generic4` and `generic2` multiply by their HDR brightness, the emissive maps overbright); assumption from the shader text (the uniform is labelled `ui_editor_properties_hdr_brightness`), not checked against WE.

With `bloom` on (or user-bound and on), the bloom is WE's HDR chain, run with WE's own shaders as its `materials/util` files define the passes: `hdr_downsample_bloom` (shader `hdr_downsample`, combo `BLOOM`) takes the scene to level 1 at half size; `hdr_downsample` halves level i into level i + 1; `hdr_upsample` (combo `UPSAMPLE`, additive) adds each level onto the next larger one from the smallest up; `combine_hdr_upsample_linear` (shader `combine_hdr`, combo `LINEAR`) adds level 1 to the scene and clamps into an 8 bit target that the scaling pass shows. The `LINEAR` variant is the one that fits an 8 bit (not sRGB) output: the other one converts to linear light for an sRGB back buffer. Without `bloom`, the scaling pass reads the float target and the 8 bit output clamps it (a filtered picture may differ from clamping first where values above 1 meet an edge). `displayhdr` (an HDR display's output, combo `DISPLAYHDR`) is not drawn: the output is always 8 bit SDR, named `display hdr`; no library item sets it.

The keys: `bloomhdrstrength` and `bloomhdrthreshold` replace `bloomstrength` and `bloomthreshold`, `bloomtint` is the tint, `bloomhdriterations` the number of levels (rounded, 1 to 16, stopping at 1 x 1; editor default 8), `bloomhdrscatter` the shader's `scatter` (default 1.619), `bloomhdrfeather` the soft knee (default 0.1); each may be user-bound. What the engine fills in, all assumed (WE sets them itself; no library preview can tell them apart exactly):

- `g_RenderVar0` of `hdr_downsample`: the four taps' offsets, half a texel of the picture read, (+x, +y, -x, -y). The shipped bicubic variant's arithmetic (`0.5 / g_RenderVar0.xy` as the texture size) fits half a texel of its input.
- `blend` (`g_BloomBlendParams`, default "1 1 0 1"): (threshold, threshold - knee, 2 knee, 0.25 / knee) with knee = threshold * feather: brightness above the threshold passes, and below it a knee fades in quadratically. The default (knee 0) is the hard threshold.
- The strength is divided by the square root of the sum of the levels' weights (level k reaches level 1 times scatter^(k-1)). The shader multiplies every step up by the scatter; with the editor's 1.619 and 8 levels the plain sum is 74 times the strength, which turned every large area near the threshold white (3819787989: the whole figure), and dividing by the whole sum left almost no glow where the previews show one (2444136289's neon lines). The square root was chosen against those previews and 3819955539: a fit, not a fact.

Memory: a half-float buffer is 8 bytes a pixel against 4, so an HDR scene's scene target, layer buffers and effect targets take twice the video memory of the same scene in 8 bits (a 1920 x 1080 buffer: 16.6 MB instead of 8.3 MB), plus the bloom levels (a third of the scene target's size together). The pass optimiser (identity skips, reduced and partial draws, render/passprobe.h) judges no pass in an HDR scene: its probe draws and compares in 8 bits, where a pass that only clamps values above 1 would look like the identity (a probe in half floats is future work).

Cost (`scene-vk-snap`, 1920 x 1080, 60 frames, this machine's iGPU): 3819787989 6.3 -> 8.9 ms, 2734996911 6.6 -> 10.3 ms, 2508022802 34.2 -> 38.5 ms per frame; most of it is `combine_hdr` (2 ms: a full-size pass reading the half-float scene) and the float buffers' bandwidth. Ordinary scenes: unchanged (1087005684 2.5-2.7 ms before and after).

### Fog

`general.fogdistance` and `general.fogheight` switch two bands (each key may be user-bound, and the renderer follows a property live); each has `…color` ("r g b"), `…start`, `…end`, `…startdensity` and `…enddensity` (keys checked in the three library scenes that have fog: 3495221617, 3777414605, 3477054430). A band that is on sets the combos `FOG_DIST` / `FOG_HEIGHT` and `FOG_COMPUTED` on every pass whose shader's own `FOG` combo resolves to on (WE's `generic4`, `genericimage4`, the particle shaders declare it, default 1; a material's `"fog": 0` opts out) and fills `g_FogDistanceColor` / `g_FogDistanceParams` and `g_FogHeightColor` / `g_FogHeightParams`. WE's shipped fog code turns those into a state `(distance from the eye - x) / y` (height: world y), saturated, and mixes the colour towards the fog's by `z + w * state^2`. The params the engine fills are (start, end - start, start density, end density - start density), the densities kept in 0..1 (assumption; it fits every library scene: 3495221617 thickens from density 0 at 1 unit to 0.92 at 600, its height band from -14 down to -49, which a negative end - start makes grow downwards). `FOG_COMPUTED` is set with any band, so image layers (`genericimage4`) fog per pixel like meshes (assumption: WE's condition for it is not known). A band of length 0 is a step. Under the orthographic camera there is no eye distance or height to fog by: the fog is not drawn and is named `general fog`, as is any other `fog*` switch that is on (no library item has either).

## Complete, degraded, refused (phase 1)

A scene is complete, degraded (drawn, with the features it is drawn without named in `Scene::degraded`, the log and `--check`) or refused. Nothing is skipped silently. This section is what the loader reads or names beyond the earlier ones; the reasons are stable strings the compat harness counts.

**Library survey** (462 items the int6 sweep drew, `"started": true`; scene.json and model jsons read from the item or `scene.pkg`, counts are items, `tools/compat/m4survey.py files_of`):

| key | items that use it |
|---|---|
| `general.clearenabled` false | 0 (416 write true, 39 omit it) |
| `general.camerafade` true / false | 458 / 3 (the editor writes it everywhere); with a camera path object: 17 |
| `general.zoom` other than 1 | 16 (1.01 .. 1.14); a camera object's own `zoom` is a different key, read and applied |
| `general.perspectiveoverridefov` | 163 (all orthographic: nothing to override) |
| `general` `hdr` on | 32 (21 with `bloom` on, 11 without; 37 in the library of 2026-10-20b; drawn, see "HDR scenes") |
| `fog*`, `lightconfig` in `general` | 0 (of these items; 3 perspective scenes of the later library have fog, see "Fog") |
| model `autosize` true (never false) | 450, none of them with `width`/`height` |
| model `width`/`height` | 49 (133 objects, every one equal to the object's `size`) |
| model `nopadding` / `instanced` / `solidlayer` | 49 / 52 / 52 |
| model `projectlayer`, `usertexturereference` | 0 |
| image object `instanceoverride` | 0 (302 particle objects have it, read since M3) |
| image object `attachment` / `attachments` | 13 / 0 |
| image object `instance` | 52 scenes, 828 objects (every one a solid placeholder, see "User textures and instances" below) |
| objects `reflected` / `sprite` | 0 / 0 (`camera` 17 and `shape` 17 belong to other readers) |
| base pass `usertextures` / `usershadervalues` | 40 / 1 (35 are `$mediaThumbnail`); the effect instances and `instance` blocks add more (shapes below) |
| base pass combos `lighting` / `reflection` / `fog` set | 0 / 6 / 5 |
| effect `fbos` with `fit` / `uvs` | 7 / 2 (cursorripple, glitter) |

**Read.**
- `general.clearenabled` (user bindable, default on). Off, the scene target is cleared on the first frame only and later frames draw over the last one (trails). No library item turns it off, so this is the plain reading of the switch, not checked against a frame from WE.
- Model `width`/`height`: the layer's size when the object has no `size` (0 or absent); an object's own `size` always wins, which is what every library item that has both agrees with. `autosize` is the default: a layer with no size at all takes the picture's (renderer, unchanged). `autosize` false without a width and height is read the same way (assumption).
- Model `projectlayer` (the "Full Composition Layer"): a layer with no size takes the scene's size, set once the size is final (after an `auto` projection settled).
- Model `nopadding`: no margin for effects to spill into is how every layer is drawn here, so it needs nothing. Model `instanced` only says the object carries an `instance` block (see below).

**Named (`Scene::degraded`), only when the value is in use.**

| reason | when |
|---|---|
| `display hdr` | `general.displayhdr` is true: the output stays 8 bit SDR (see "HDR scenes") |
| `general fog` | a fog band is on (or user-bound) in an orthographic scene, or a `general` key starting `fog` other than the `fogdistance*` / `fogheight*` ones is `true` (see "Fog") |
| `camera fade` | `general.camerafade` is true and a camera object's `path` file has paths and the scene is orthographic (its paths do not play, so there is nothing to fade; `camera paths` is named too) or the file cannot be read. In a perspective scene the fade is drawn ("Camera") |
| `image instanceoverride` | an image object has a non-empty `instanceoverride` |
| `attachment`, `attachments` | an image object names one |
| `instance values` | an `instance` block has a combo the engine has no data for (a non-zero one beyond `version`, `spritesheet`, `fog` and the `combo lighting/reflection` set that are named as they are on a material), a user-bound combo, or sits on a model that is not `instanced`. Its textures, constants and those combos are otherwise applied (below) |
| `reflection target` | a 3D mesh part's material names `_rt_Reflection` (2244339517's `grid`, an object scaled to zero): nothing is `reflected` in the library, so the slot holds transparent black (what WE binds there is not known: the survey's Q4.1) |
| `puppet model missing` | an image's model names a puppet `.mdl` that is not in the item nor in the install (2955378002 names `models/1x1_puppet.mdl` fifty times and does not carry it): the layer is drawn as the plain picture, with no skeleton. Assumption: WE draws the rest of the scene without what it cannot load. A file that is there and is not a model is still a refusal |
| `usershadervalues` | a pass of a material, an effect's material, an effect instance or an `instance` block has a `usershadervalues` entry whose value is not a property name (none in the library: the named ones are bound, see "Material pass keys") |
| `user shortcuts` | not named any more: a `usertextures` entry is a `usershortcut` (in `project.json` a property type with value "" the user fills with an application); the host sends the icon of the application the user chose and the slot shows it, the slot's listed `default_dock_icon*` until then (docs/design/specs/2026-10-21-user-shortcuts-design.md) |
| `usertextures <type>`, `usertextures system <name>`, `usertextures value` | a user texture of a kind the engine does not know: an object with another `type`, a `system` texture other than the two media ones, or a number or bool. None in the library |
| `instance texture slot` | an `instance` block names a texture beyond the 32 slots the engine reads (not applied) |
| `usertexturereference` | a model json sets it |
| `sprite sheet pages` | a sprite sheet has images (pages) that would take its pixel data over the 512 MiB limit: they are not loaded and the frames on them show the last page kept |
| `fbo fit`, `fbo uvs` | an effect fbo has `fit` with no size (a flag), or `uvs` other than `clamp` and `repeat` (none in the library) |
| `combo reflection` | a base-pass `reflection` combo is user-bound (`{"user": ..}`; not followed, the combo keeps the material's value). A literal one is drawn (see "Reflection"). `lighting` and `fog` are no longer named: the scene's lights and ambient colour reach the one (Lights, below), the scene's fog the other (see "Fog") |
| `tube light shadows`, `older point light shadows` | an `ltube` or an older `point` light has `castshadow` on: the editor offers shadows for point, spot and directional lights only, and the light is drawn unshadowed |
| `too many shadowed lights` | more than 4 point, 4 spot or 2 directional lights of a scene cast shadows: the rest are drawn without (the atlas's room, "Shadows") |
| `shadows from 2D layers` | an image or shape layer has `"castshadow": true` (none in the library): only 3D meshes cast |
| `shadows on particles` | a lit particle system (`LIGHTING`) in a scene with shadows: the particle draw does not bind the atlas, so particles are lit unshadowed |
| `shadow caster <shader>` | a casting part's caster shader (`shadowcaster`, or the one its material's shader names) does not compile: the part casts nothing |
| `tube light volumetrics`, `directional light volumetrics`, `older point light volumetrics` | a light of that kind has `castvolumetrics` on: its beam is not drawn (point and spot beams are, "Volumetric lights"; the editor offers it for no other kind) |
| `volumetrics shader <name>` | a shader of the beams (`volumetricsback`, `volumetricsfront`, `blur_k3`, `passthrough`) is missing or does not compile (no Wallpaper Engine install): no beam is drawn |
| `light cookie` | a spot has `usecookie` on: its picture (`cookie`) is not projected, the cone is plain |
| `light control point` | a light other than a tube has a `controlpoint` (what binding one does there is not known) |
| `light script`, `light animation` | a script on a light's `castshadow`, `castvolumetrics`, `usecookie` or `lightsourcesize` (not run: the file's value stays), keyframes on a switch or on those (not played). Scripts and keyframes on the colour, intensity, radius, exponent, cones, `density` and `volumetricsexponent` are followed ("Lights") |
| `light binding` | a tube's `controlpoint` is bound to a user property (the file's end stays); every other light value follows its binding |
| `tube without end` | an `ltube` has no `controlpoint`: it lights as a point at its origin |
| `light scale` | a light's node has a scale other than 1 (or a bound one): it moves a tube's end, but is not applied to the radius. What WE does with it is not known: the two library scenes that scale a light (3429481890 by 2.7 x 1.5 x 1.5, 3477054430 by 1.9) were rendered with the radius left alone, times the mean, the largest and the smallest scale: the picture of 3477054430 does not change at all and 3429481890's differs by under 0.5 % (squared error against its preview), so there is nothing to choose by |
| `script error` | a property script does not compile (3815542599's hold a pasted JSON fragment): it never runs, the property keeps its static value, the failure is logged once when the scripts start; or a script is stopped: it throws a `TypeError` or a `ReferenceError` in its init, at the top level of its module or in the frames `--check` runs (the reasons are logged). That is the policy for every such error: WE logs it and draws the scene without the script, and the API this engine gives is the whole of WE's declared one (`lib.sceneScript.d.ts`, `scripts-api.tsv`), so a missing name is missing there too: `cannot read property .. of null` (a layer it looks up by name is not in the scene), a global in the wrong case (2955378002), `thisLayer.getParticleSystem` (3448877775), `thisScene.getSprite` (3212731906) and a bare `scene` (3326873240, 3470764447), none of which WE has. `getVideoTexture` on a layer without a video texture is `null` as in WE, so a script that uses it without a check throws there too. What still refuses is the engine's own limit and not the script's: an `InternalError` (out of time or memory), a `RangeError` and the browser APIs named in `kUnsupported`. Since the SceneScript review fixes (`03fd34f`) every stopped script is named; before, an init failure was logged and not named, which made 13 scenes look complete that already lost those scripts (the univ3 sweep's `script error` +16; the logs of univ2 and univ3 hold the same errors) |
| `custom model geometry (not drawn)` | a script calls `thisScene.createModelData`: the data is kept and no layer draws it (3798688689's particle trails), so the scene is drawn without that geometry; found by the same source scan as `kUnsupported`, so it is named even when the call never runs in `--check` |
| `slow scripts` | the cheapest of the three frames `--check` runs takes the scripts more than 50 ms (under 20 fps, the engine's lowest rate); over 200 ms the scene is refused |
| `slow script start` | the scripts' start (every module, then every init) takes more than 250 ms, which delays the first picture. `--check` gives each init the second the renderer gives it: an init still running after it refuses the scene (`InternalError: interrupted`). Until 2026-10-24 the check gave an init a quarter of that and refused 3798688689, whose init builds about fifty particle meshes in JavaScript in ~0.23 s and runs when drawn |
| `too many lights` | more than 32 lights of one type (more than 4 of the older `point` type): the rest do not reach the shaders |
| `object 'reflected'`, `object 'sprite'` | an object of that kind (`light`, `model`, `camera` and `shape` have their own readers) |

`perspectiveoverridefov` is not named: it is written (95 or 90.76) only in orthographic scenes (163 library items and the WE assets' preview scenes), where there is no perspective camera for it to act on, and no perspective scene has it. The set of object kinds WE knows is camera, image, light, model, particle, reflected, shape, sound, sprite; an object with none of them is a group.

## Camera

**Facts from the library** (the 10 perspective items: 2244339517, 3477054430, 3495221617, 3503626457, 3509243656, 3589454154,
3662790108, 3777414605, 3812243386, 3816342243). None has `orthogonalprojection` (it is `null`); `general` holds `fov` (50 in 7,
42.43, 85, and one bound to a user property), `nearz` (0.01 or 0.1), `farz` (10000, 5389, 30) and `zoom` 1; none has
`perspectiveoverridefov`. The `camera` block's `eye` and `center` are one unit apart and point anywhere (up is always +y). Nine
have a camera object (`camera: default`) with an `origin`, `angles` (radians), its own `fov` (close to `general`'s: 42.4 / 42.43,
52.38 / 50, 85.05 / 85, 40-55 for the five switchable cameras of 3777414605) and a `path` file; three path files hold paths (21 in
3503626457, one empty path each in 3777414605's), all with `options` `{"cameramode": "fly", "fps": 30, "length": 30-240, "mode":
"single", "wraploop": false}`, `eye` and `center` keyframed in world units (`c0..c2`), `up` (0 1 0, one path tilting it), `fov` a
plain list of keys, `zoom` null. Object origins are world units: from 0.3 (UI layers) to a few hundred (backdrops 169-1134 away),
image layers scaled by 0.0001-0.1 (a 2560 x 1440 layer at scale 0.0001 is 0.256 x 0.144 units).

**Which camera.** A visible camera object is the camera; the `camera` block is used only when there is none (2244339517, whose
scripts move it with `setCameraTransforms`). Checked on 3662790108: the block's eye looks along +z at layers that would then be
behind it, while the camera object at z 0.454 looking down -z has the loading screen (2560 x 1440 at scale 0.0001, z 0.3) exactly
filling a 16:9 picture at fov 50; and on 3816342243, whose camera object and block agree. The object looks down its own -z with
its +y up, turned by its `angles` like any object (Rz Ry Rx): 3816342243's camera (-pi, pi/2, -pi) looks down -x, and the clock
layer it shows is turned the same way. Of several visible camera objects the first counts (3777414605 switches them by a user
property). A negative scale on the object is taken as it is (a mirror along x cannot be told from one along z turned half round):
the camera looks down its own -z column, so a negative z scale turns it round, and the picture is never mirrored. The camera
object's `fov` is used (assumption: it is the one WE's camera panel edits; it differs from `general`'s in
3495221617 and 3777414605); `general.fov` is the block camera's. `zoom` narrows the view (2: half the width and height).

**Projection.** The fov is vertical (the 3662790108 check above: a horizontal 50 would leave the loading screen at 60 % of the
height). A perspective scene's picture is a 16:9 canvas (3840 x 2160 units, `kPerspectiveCanvas`) under that fov; the scaling
modes place it like an orthographic scene, so `fill` on a 16:10 screen keeps the vertical fov and crops the sides, and on a 21:9
one crops top and bottom (assumption: WE probably keeps the vertical fov at any aspect and shows more at the sides; not
checked, the previews are square crops). `nearz` and `farz` default to 0.01 and 10000 and are the depth range. The depth is
the plain (not reversed) OpenGL one taken to Vulkan's 0..1, so with near 0.01 and far 10000 almost all the precision is near the
camera and far surfaces will z-fight; a reversed depth is a later change, shared with the meshes. Every fov (`general`, a camera
object, a path, a script) is kept within 1..170 degrees (`clampFov`).
`perspectiveoverridefov` is not read (see "Complete, degraded, refused").

**Paths.** While a camera object has paths it moves along them: each one plays once from its start for `length / fps` seconds,
its `eye`, `center` and `up` curves (the keyframed property animations' bezier keys) making the camera's look-at, its `fov` and
`zoom` lists replacing the object's when present. A path without both an `eye` and a `center` curve (3777414605's, all null)
is skipped like a hidden one (`"visible": false`). Then the next: `queuemode` `random` picks another path at random (never the
same twice running when there are two or more; the seeded generator's output modulo the choices, so a scene repeats its order on
any system), anything else takes them in file order and starts over (after a long pause whole rounds are skipped at once). The
path moves the object itself (its origin and angles), so objects parented to the camera (3503626457's date) follow; the path is
in world units, so a camera that hangs on another object gets the pose in its parent's space (the parent's world matrix undone).
**Camera fade.** With `general.camerafade` on, the picture darkens to black at the start and the end of every path of a perspective scene and
comes back: linear, `kCameraFadeSeconds` (0.4 s) at each end, at most a third of a short path (the library's paths are one to two seconds). The
darkening is `g_Fade` of the present pass (it multiplies the scene as shown, bloom included), so the scene itself is untouched. Assumptions: the
duration, the curve, black (rather than transparent), and a fade-in at the very start of the wallpaper; WE's are not known). A script's write to `scene.camerafade` is kept as a stored value and has no effect (the switch follows the file and its property). `camerafade` is a boolean of the `general` record, bindable to a user
property (followed live here). `cameramode`, `events` and `wraploop` are not read.

**Scene zoom.** A zoom of zero or less, or not a number (in the file, a property or a script), is taken as 1 (a script's setter keeps the zoom it had). `general.zoom` (the editor's camera zoom; a script's `setCameraTransforms({zoom})` and `thisScene` property `zoom` write the same
value; it may be bound to a user property) multiplies the zoom of the camera object, so 2 shows half the width and height about the view's
centre. Every library scene that sets it (17, all orthographic) has between 1.01 and 1.14: a slight magnification, which the authors use to
hide the border that the parallax and the camera shake would show. `zoom` is a plain float of the `general` record, bindable to a user property (followed live). Assumption: that it magnifies about the centre, as if it multiplied the camera object's zoom.
Evidence: compared with the previews (square, so the centre square of a 16:9 frame; the previews are animated and the crop is not known), the
squared error falls from 2064 to 885 on 2639529402 (zoom 1.14) and from 2753 to 2138 on 2631739087 (1.05); the scenes with 1.01 to 1.03 move
by less than the preview's own noise.

Verdicts: a perspective scene plays its paths, so `camera paths` is not named there. `camera fade` is named only where a path file has paths or cannot be read: orthographic scenes whose camera objects point at an
empty `{"paths": []}` (all of them in the library) are no longer degraded for it.

**Layers in 3D.** An image, text or shape layer is its quad at its world transform: centred on its origin (z included), facing
+z at angles 0, turned by its angles, sized by `size` x scale; nothing turns it towards the camera (the library turns the layers
it wants to face the camera the way the camera is turned, 3816342243). They draw in file order (the depth buffer is the mesh
piece's). Effects work as in 2D: the layer's buffers are drawn flat and the composite quad goes through the camera. A fullscreen
layer stays the whole view. Text is rasterized at its own point size (scale 1): its scale takes the box into world units.
Scripts see the screen as `engine.canvasSize` and the cursor as its pixel there (assumption; the cursor is not unprojected into
the world). In an orthographic scene whose camera block is turned, the cursor scripts see is mapped as without the turn (the
view's centre plus the pointer's offset over the zoom): the turn is not undone (no library scene turns its camera).

**Parallax and shake** (assumptions; only 3495221617 has the parallax on, 3477054430 and 3777414605 the shake). The parallax
displacement (the same smoothed amount as in 2D, `cameraparallaxamount` and the mouse influence in it) moves the eye in its own
right/up plane by that much times the camera's reach: the distance from its eye to its centre (the camera block's, or the
playing path's; one unit for a camera object, which has no centre, and what every camera block of the library has), so nearer
things move more; the per-layer `parallaxDepth` is not used. The shake moves the picture on the screen
as in 2D (a share of the canvas).

**Uniforms.** `g_ViewProjectionMatrix` (and `g_AltViewProjectionMatrix`) is the projection times the view; `g_ModelViewProjectionMatrix`
adds the object's world matrix. `g_EyePosition` is the eye (after the parallax), `g_ViewForward`, `g_ViewRight`, `g_ViewUp` the
camera's axes; particles' default `g_OrientationRight / Up / Forward` are right, up and -forward (sprites face the camera), and
perspective particles use the scene camera. An orthographic scene's camera block may point anywhere too: its direction turns the
scene about its centre (the default, -z with y up, is no turn). There `g_EyePosition` is above the middle of the view window
(x and y in scene units, after the camera object, zoom and shake), 10000 units up, so lit materials (generic4's specular and
reflection) see the view along -z: the same convention as the mesh renderer's.

### Media cover

The host (murasaki-surface, outside the sandbox: the engine has no session bus and no network) follows the `org.mpris.MediaPlayer2.*` players of the session bus and tells each Vulkan scene engine, with messages of the bridge protocol version 4 (`src/bridge/mediaview.h`; a producer that does not announce it is never sent them): `Media` (JSON: the state, title, artist, album, album artist, genres, position, rate, duration of the player shown) and `MediaThumbnail` (the cover as one sealed memfd of RGBA8, at most 512 x 512, or none). The engine keeps the cover as the textures `$mediaThumbnail` and `$mediaPreviousThumbnail` (the cover before the last change) and the slots a `usertextures` entry names show them; the layers drawn from them are drawn again when the cover changes. The same state feeds the SceneScript media hooks (scripts.md).

- **Who is told, and when.** Only an engine whose scene uses the media: after loading, the engine sends `MediaWants` (1 when some layer, effect or mesh material has a `$media...` slot or some script exports a `media*Changed` hook, else 0) and the host sends it nothing before that. The players are not read at all while no attached engine wants them. The setting `mediaShare` (default on; Settings, "Media") turns the whole thing off: nothing is read and a scene is told that no player runs. A setting the surface has not read from the daemon yet counts as off (it never fails open), for `mediaShare` and for `mediaRemoteArt` alike.
- **Which player.** The one that is Playing; if two play, the one that became Playing most recently; if none plays, the one that played last; failing that the first with a title (by bus name). On a file system whose read can hang (FUSE, NFS, SMB, CIFS, AFS) a cover file is refused; one worker thread, one job at a time (the cover asked for last waits behind a stuck one). At most 16 players are followed: when a 17th name appears the oldest that is not Playing is dropped (a flood of fake names cannot push out the player that plays), and if all play the newcomer is ignored.
- **The cover.** `mpris:artUrl`: `file://` (this machine's regular files only, at most 8 MiB, judged on the open descriptor: not on procfs, sysfs or devtmpfs, not where `/proc/self/fd` says `/proc`, `/sys` or `/dev`, no FIFO or device, opened with `O_NOFOLLOW` after resolving links) or `https://` (the setting `mediaRemoteArt`, on by default, can turn the download off). Residual risk of the file case, accepted: any picture the user can read may be named by a player and so be shown to the scenes and their scripts. What is downloaded and from where: the one picture a local player names, from the host that URL names, over https, by the surface and never by the engine; direct (the system proxy is not used; the name is resolved once to be judged and once by the connection), no content encoding, one download at a time and a new address every 2 s at most, 8 MiB, 10 s of transfer and 15 s in all, at most 4 redirects (https only), no cookies or credentials; the name is resolved first and the download goes ahead only if EVERY address it resolves to is public (not loopback, private, link-local, unique-local or site-local, CGNAT, multicast, documentation or benchmarking ranges; IPv4-mapped, NAT64, 6to4 and Teredo forms judged by the IPv4 inside). Residual risk, accepted because the address comes from a local player and not from a wallpaper: the connection resolves the name again, so a name that answers differently between the check and the connection (DNS rebinding) is not caught. Anything else (`http://`, `data:`, relative) is no cover.
- **Decoding.** On a worker thread of the host (reading and decoding never run on the GUI thread; a cover wanted after another cancels it), with `QImageReader`: only PNG, JPEG, WebP and GIF (first frame) are accepted (not SVG, PSD, XPM, TGA, ICO...), the size is read from the header first (at most 8192 on a side and 16 Mpixel), a picture whose header gives no size is refused (the reader's global allocation limit is left alone) and the decoded size in bytes is checked after, the picture is scaled to 512 on its longer side and converted to RGBA8. A file cover is read at most every 0.5 s, a download started at most every 2 s. The last 8 downloaded covers are kept by address.
- **Rates.** State messages are coalesced over 200 ms, a cover is sent at most once a second, a new one replaces one still waiting in the socket queue, and the position is not streamed: it is sent with each change and the engine extrapolates it.
- **Untrusted.** Names, titles and lists are cut (4096 characters looked at, then 256 bytes, 8 genres), control characters dropped, numbers clamped; the engine parses the JSON again and checks that the memfd is a regular file of exactly width x height x 4 bytes with the shrink, grow and write seals before it maps it (a cover that fails is no cover; the engine carries on).

### User textures and instances

**`usertextures`** is an array on a material pass, an effect instance's pass or an `instance` block, parallel to `textures` (index = texture slot). Library survey (items, base / effect instance / `instance` block): an entry is `null` (no user binding), or
- `{"name": "$mediaThumbnail", "type": "system"}` (28 / 31 / 21 items) and `{"name": "$mediaPreviousThumbnail", "type": "system"}` (1 / 42 / 0): the album art of the media playing now / before this track (WE's media integration). The surface follows the session's MPRIS players, reads the cover of the one it shows and sends it to the engine (below, "Media cover"); until there is one, and whenever the track has none, the slot shows what the file lists there;
- a bare string, the name of a user property (23 / 24 / 15 items): in `project.json` it is a `scenetexture` property (`value` "" by default, which the user fills with an image) in 674 of 700 uses (counted per slot); 2 name a `slider` and 24 a property `project.json` does not declare;
- `{"name": "s01c", "type": "usershortcut"}` (3 / 1 / 21 items): a dock shortcut slot, the icon of an application the user picks (the dock scenes 3733754206 and others, 32 slots with `default_dock_icon01..32` as the texture listed at that slot).
No other type exists in the library. The entry shape of the effect instances is `[null, <entry>]` for the blend effects (slot 1 is the picture blended in, `[null, prev, next]` for `blendgradient`).

**Shortcut slots.** A `usertextures` entry `{"type": "usershortcut", "name": K}` is a slot whose picture is the icon of the application the user chose for the property K in the GUI (the picker lists the installed applications from their `.desktop` files). The host rasterises the icon (256 x 256, scaled to fit and centred on a transparent canvas), sends it with `ShortcutIcon` (bridge version 5) and the engine keeps it as the texture `$shortcut:K`; the slot reads it while it exists and the file's own texture otherwise (nothing chosen, an application that is gone, an icon that could not be used, the user's choice cleared: a `ShortcutIcon` with width and height 0). A script's `engine.openUserShortcut(K)` sends `ShortcutLaunch`; the host decides, see docs/security.md. A slot keyed by a name the host never sends an icon for keeps its default.

**What shows with nothing filling a slot.** The slot's own texture: the file lists one at that index in `textures` (the material's, or the effect instance's `textures` list, which overrides it, or an `instance` block's), and that is what is drawn. The library always lists the author's placeholder there (`Album Cover`, `placeholder`, `default_dock_icon01`, `util/black`, `Image`, and in 11 slots a `_rt_imageLayerComposite_<id>_a`, the layer's own picture so far, which the renderer already resolves). A slot with no texture listed is drawn as any unbound slot is (the shader's `default` annotation, else a white texel). This is what WE shows with no player running and no choice made, as far as the Workshop previews tell: the preview pictures of the media scenes (2518958708, 3601075812, 3733754206) show the placeholder cover or the dock's default icons, never an empty slot (assumption: the previews are made with nothing playing; not compared with a WE frame). The engine reads no media (an MPRIS player, `$mediaThumbnail` as the cover of what plays, is future work: the engine runs in a sandbox without the session bus, so it would have to come from the host), gets the icon of a `usershortcut` slot from the host (below) and does not load an image a user picked for a texture property, so a texture property always shows the default. Setting such a property has no effect (future work); a name that is not in `project.json` is the same. Naming: neither the media thumbnails nor the shortcuts are named: the host fills them at runtime (a player's cover, bridge/mediaview.h; the icon of the application a user picked, bridge/shortcutview.h) and a slot nothing fills shows its default. A bare property name is not named at load: until the user picks a picture the default is what WE shows. When a property update sets such a property to a non-empty value, the renderer logs once per property `drawing without: user texture <property>` (`SceneRenderer::reportedUserTextures()`, `Scene::userTextureProperties` lists them); the slot keeps its default. An unknown kind is named `usertextures <type>`.

**`instance` block** (an image object whose model says `"instanced": true`; the model also says `"solidlayer": true` and its material is `materials/util/solidlayer_instance.json` (genericimage2), `solidlayer_instance_4.json` (genericimage4) or `solidlayer_instance_depthtest_4.json`: one pass, `util/white`, combos `version` 2, translucent). 828 objects in 52 scenes; the keys are `id` (the object's id in the compiled scene, ignored), `combos` (always `{"version": 2}`), `textures` (one entry: `util/white` 299 times, or the picture the editor put there, `default_dock_icon*`, a user image) and in 603 of them `usertextures` (the entry kinds above). No `constantshadervalues`, no other combo, no `usershadervalues` appear. The block is WE's per-object override of the material's first pass (the same three arrays an effect instance's pass has): the layer is the model's material with the block's values merged in: a texture replaces the material's in its slot (a `null` or empty one keeps it), a constant or combo replaces the material's value of that key and the keys the block does not name keep the material's. It is read that way here: the textures replace the base pass's slots (loaded and checked like a material's; one beyond slot 31 is named `instance texture slot`), `version`/`spritesheet` and zero combos are set, `constantshadervalues` merge per key (`alpha`, `brightness`, `color` as a material's do, also on a flat layer). A flat-colour material (the `flat` shader whose first texture is null or `""`) has no picture to replace: its block's textures are ignored (a material that lists `[""]` for the first texture is a flat colour too; it was refused before). A model that is not `instanced` does not take a block (degraded `instance values`).

## Effects and materials in full (M2)

**`usershadervalues`** (a pass of a material, of an effect instance's `passes[]` or of an `instance` block): `{"<constant's material key>": "<property name>"}`. Each entry binds that constant to the project property, as a `{"user": ..}` constant is bound: the property's current value is read at each draw (a colour is `r g b` in 0..1, or 0..255 as every bound colour; a slider or bool is a number), and until the property exists the constant keeps its literal (`constantshadervalues`, or the shader's default). The binding wins over a literal given in the same pass, and an instance's entry over the material's (assumptions, survey Q6.1 and Q6.2). The layer's own `alpha`, `brightness` and `color` keys are read by the loader from `constantshadervalues` only, so a `usershadervalues` entry for those is not followed.

**Material pass keys** seen in the shipped materials: `shader`, `blending` (`translucent`, `normal`, `additive`, or absent), `depthtest`, `depthwrite`, `cullmode` (`nocull`, `normal`), `textures`, `combos`, `constantshadervalues`, `alphawriting`, `culling`, `usershadervalues`, `usertextures`. The depth, cull and alpha-writing keys do not matter for 2D layers; 3D meshes follow `depthtest`, `depthwrite` and `cullmode` (see Meshes). A constant is a number, a string of numbers (colours are "r g b" in 0..1; a Windows `-nan(ind)` component reads as 0) or `{"user": property, "value": default}`; `{"script": ...}` is SceneScript.

**`effects/<name>/effect.json`:** `passes` (each: `material`, optionally `target`, `bind`, or `command: copy` with `source` and `target`, or `command: swap` with `source` and `target` (two declared fbos trade places for good: a ping-pong), or `compose`), `fbos`, plus editor keys (`name`, `group`, `preview`, `gizmos`, `dependencies`, `performance`, `replacementkey`). `fbos` entries: `name` (`_rt_QuarterCompoBuffer1`...), `scale` (a divisor: 4 is a quarter of the layer's size, 1 or absent full size), `format` (`rgba8888`, `rgba_backbuffer`, `r8`), and rarely `unique`, `fit`, `width`, `height`, `uvs`. Size: an explicit `width` x `height` wins; else `fit`, read as the long side in pixels with the layer's aspect kept (256 on a 1920 x 1080 layer is 256 x 144; the chain's quality step does not shrink it; an assumption, survey Q6/Q7.1); else the layer's size over `scale`. `uvs` `repeat` samples the fbo with wrapping coordinates in the passes that read it (clamped otherwise, the default); another value is named (`fbo uvs`). `bind` entries `{"name": N, "index": I}` put the render target `N` (or `previous`, the layer as it was before the effect or the last pass that wrote the layer's buffer) in texture slot `I`. A pass without `target` draws into the layer's own buffer. Some files in the shipped tree end a list with a comma (`fluidsimulation`), which the JSON reader tolerates.

**Scene instances** (an object's `effects` entry): `file`, `id`, `name`, `username`, `visible` (a bool or a user binding) and `passes`, one entry per pass of the effect with `constantshadervalues` (keyed by the uniform's `material` annotation), `combos` and `textures` (a list by slot; `null` keeps the material's texture).

**Where the files are.** A package made from a scene carries the effect's files at the root of its namespace (`effects/waterripple/effect.json`, `materials/effects/waterripple.json`, `shaders/effects/waterripple.frag`). In the WE installation they live under the effect's own folder (`assets/effects/waterripple/materials/...`), which is why the file system searches those folders too. Texture names in an effect material are resolved as `materials/<name>.tex` as usual (`effects/waterripplenormal`, `masks/<generated name>`).

## What the library contains

Over the 105 items in the author's Steam library (a snapshot, so the counts are only a guide): 72 scenes (one of them a preset of another item), 31 videos and one web item. `--check` accepts 6 of the scenes. The refusals, by cause: 44 scenes use particles, 43 effects, 9 audio, 7 bloom, 5 text, 3 camera parallax, 3 a shader other than the generic image ones, 3 the `_rt_` render targets, 3 animated textures, 2 SceneScript, 2 colour blend modes, one a camera shake. Most scenes trip several at once, which is why the totals exceed the count. The seventh image-only scene is a spritesheet (M2).

## Text, sound, blend modes and shake (M4)

**Text objects** (`text` in place of `image`; `text.value` may be a string, a user binding or a script). Layout, derived from how WE's own clocks look:

- `pointsize` is in scene units with four scene pixels to the point at scale 1: a 32 point clock on a 1920x1080 scene has an em of 128 pixels. The object's scale applies afterwards, so the bitmap is made at `pointsize x 4 x scale` pixels and drawn 1:1.
- The bitmap is the layout box, not the ink: the widest line by the font's ascender-to-descender height, plus one line height per further line, lines left aligned inside it. The box hangs on the object's `origin` by `horizontalalign` (`left` puts its left edge on the origin, `right` its right edge, `center` the middle) and `verticalalign` (`top` hangs it below the origin, `bottom` stands it on the origin, `center` the middle). `maxwidth` wraps at spaces.
- `font` is a path in the item or assets (`fonts/...ttf`) or a system name (`systemfont_consolas`), which fontconfig maps to an installed font. `color`, `alpha`, `brightness`, `colorBlendMode` and `visible` work as on images.
- `effects` run on the text layer like on an image layer, with the same instance keys (`visible`, per-pass constants, scripts and keyframes on them, user bindings); the layer's buffer is the size of the text texture (box plus padding), and the effects see the glyphs as white with the coverage as alpha, tinted afterwards by `color`. `animationlayers` belong to puppets (images with a model) and are not read on text.
- `padding` (default 0, 32 on WE's stock clocks) is empty room added on every side of the layout box, in scene units: the texture is `box + 2 x padding` in each direction, transparent around the glyphs, and the alignment anchors the padded box: with `left` and a padding P the glyphs start P units to the right of the origin (an assumption, as the padding's purpose is inferred). Derived from the library: the same clock at the same point size has `size` 202 x 126 with padding 32 and 302 x 226 with padding 82 (item 2134765860, a difference of twice the padding in both directions), so the object's `size` is the text box grown by the padding. The point of it is that an effect (a glow, a blur, a shadow) has room to draw outside the glyphs; `size` itself is not read for text, the box is computed from the font. Assumption: `maxwidth` wraps the text itself, before the padding is added.
- A text object is an image layer with a generated texture (`__text:<node>`); the renderer rasterizes again only when the string, size or font changes.

**Shape layers** (`"shape": "quad"`, 39 objects in 17 library scenes, nearly all carrying `effects/lightshafts/effect.json`). In WE a shape is a layer class of its own rather than an image file, so the object has no `image` and, in the library, no `size`. The engine draws it as an image layer whose base picture is the stock white texture (`materials/util/white.tex`) with translucent blending, `size` from the object when it has one and 1024 x 1024 scene units otherwise, placed by `origin`, `scale` and `angles` like any layer. It carries `effects` (with their scripts and keyframes), `alpha`, `brightness`, `color`, `colorBlendMode` and `visible` as images do; `locktransforms`, `disablepropagation` and `attachment` are ignored, and `"castshadow": true` is named `shadows from 2D layers`. Any other shape value is refused as `shape '<value>'`.

Assumptions, not checked against WE (the exact material it uses is unknown): the white translucent base and the 1024 unit default side. The light-shafts effect, which is what the library puts on shapes, runs with `DIRECTDRAW`, ignores its input picture and draws the rays in the quad's texture coordinates, so the base only matters for the other effects; the side decides how far the rays spread (the library's scales run from 0.15 to 5.5, which fits a quad of about a thousand units on scenes of 3840 x 2160 and 5376 x 3584 and would not fit one of a few hundred). The previews of 3276911872, 3299228616 and 3366554210 are too small and too different in time to decide it more closely.

**The audio spectrum** (`g_AudioSpectrum*`, `engine.registerAudioBuffers`) is a capture of the default output's monitor, that is, of everything the system plays (like WE), not only of the scene's own sounds; it is opened only when a shader or script reads it, and closed while the scene is paused.

**Sound objects** (`sound`: a list of paths, the ones that exist are the playlist, at most 16; `volume`; `startsilent`; `playbackmode`; `mintime`, `maxtime`): played through a PipeWire stream (application "Murasaki scene", role Music) at `volume x bridge volume / 128`, looping. The player starts silent and decodes nothing until the first Volume message; at volume 0 or while paused it stops decoding and the stream is corked, so a muted or covered scene costs no wakeups. `playbackmode` is `loop` (the default), `random` or `single` (the first file once, then silence; assumption: WE's "single" plays once and stops); anything else is refused as `audio: playback mode ...`. Files are mp3, Ogg Vorbis, FLAC or WAV (decoded by what the file starts with; any other extension is refused as `audio: only mp3, ogg, flac and wav sounds`). A playlist of up to 512 files is played one at a time: each file is read when its turn comes and dropped when it is over, so only the playing one (up to 160 MiB) is in memory; `--check` opens four files of a playlist (the first, the last and two between) and refuses the scene if one is broken; a file over 160 MiB is not played (the player skips it) and the scene is degraded as `sound file over 160 MiB (not played)` instead (1432621679's three-hour mp3).

- **loop:** the playlist plays in order, again and again; one file loops without a gap. (Assumption: WE plays several files of a looping object one after the other.)
- **random:** a random file of the playlist plays to its end (never the same one twice in a row when there are several), then the object rests for a random time between `mintime` and `maxtime` seconds (defaults 1 and 5; a maximum under the minimum is raised to it, both are limited to 0..3600), and again. The first play starts at once. Assumption, derived from the editor's labels; 3778592720 uses it with three files and 1 s of rest.
- The volume (a number or a user property), the bridge's Volume and Pause apply to both: while paused the rest does not run down, and at volume 0 nothing is decoded.

**Conditions.** An `effects[]` entry's `visible`, an object's `visible` and the bloom's `enabled` may be a user binding with a `condition` (the value the property must equal; combo and bool properties alike). The property store drives them live.

**Colour blend modes.** `colorBlendMode` N (1 to 32, WE's numbering in `common_blending.h`) draws the layer, after its effects, through a last pass of the shipped `genericimage3` shader with `BLENDMODE=N`. That pass reads a copy of the scene drawn so far as `g_Texture4` (`_rt_FullFrameBuffer`, found from the layer's own screen position) and writes `ApplyBlending(...)` in place of the target's colour, without GPU blending; the target's alpha is kept.

**Camera objects** (`"camera": "default"` on an object; 33 in about 27 library scenes, 18 of the scenes orthographic). Keys: `fov`, `path` (a json file of camera paths), `queuemode` (`random` or in order), `zoom` (a number, a user property, or keyframes), `origin` and `angles` (like any object: literal, script, keyframes or a user property), `visible` (a user property or condition: two or three cameras switched by a property occur), `solid`, `disablepropagation`, `locktransforms` (ignored). In an orthographic scene the first visible camera object sets the view; with none the projection is the scene's. Assumptions, from 3276911872, 3326873240 and 3605722997 (not checked against WE): the origin's x and y are an offset of the view's centre from the scene's centre (the scripted origins of 3276911872 are `slider * canvas size` with the project's sliders running -1..1 and defaulting to 0, so the default keeps the picture centred; the literal ones are `0 0 500`, or a few units; the z, a distance, is not used), so the picture moves the other way; `zoom` 1 is the scene's projection and 2 shows half the width and height about the (offset) centre, 0.5 twice as much (a Lens-size slider of 0.1..2 in the project). A zoom bound to a user property takes its value live and its keyframes do not play; a script on the zoom is not run (named as degraded: `camera zoom script`). In an orthographic scene `fov` and the angles are read and unused. A script property bound to a user property (`"scriptproperties": {"x": {"user": "x3", "value": 0.5}}`, which the lens position scripts of 13 library scenes use) starts at the user property's value when it is set, else at the file's (a value the property kind cannot hold, such as text for a slider, leaves the default), and follows the property while running: a change reaches the script's `scriptProperties` object before its `applyUserProperties` runs. The `path` file (`{"paths": [...]}`, each path with `center` and `eye` channel sets `c0..c2` of the keyframe shape of animations) is empty in every orthographic scene of the library; there a non-empty one, or a `path` file that cannot be read, is named as degraded (`camera paths`) and not played. Perspective scenes play them (see "Camera"). The pointer scripts and solid layers see is mapped through the camera object: its scene position is the view's centre plus (pointer - 0.5) times the window over the zoom (the shader pointer and the parallax keep the screen position). The camera shake is a movement of the picture on the screen, so it is divided by the zoom. Camera parallax moves a layer in scene units, so a zoom enlarges its on-screen movement as it does the layer (assumption: WE's behaviour is not known). A fullscreen layer covers the view, not the scene, so it is not zoomed twice.

**Camera shake** (assumption, WE documents no formula). With `camerashake` on, the whole picture moves by smooth noise, independently in x and y, by at most `camerashakeamplitude` percent of the scene's size on each axis (0.5 on a 1920 wide scene: 9.6 units either way). `camerashakespeed` is the number of noise lattice steps per second and `camerashakeroughness` mixes in two finer octaves (0: one smooth wave; 1 and above: visibly jittery). The numbers are chosen to be subtle; nobody has compared them with WE itself.

**Bloom.** `general.bloom` with `bloomstrength` (default 2), `bloomthreshold` (0.65) and `bloomtint`. WE's own shaders run as a post pass: `downsample_quarter_bloom` (quarter size, drops what is below the threshold), `downsample_eighth_blur_v` and `blur_h_bloom` (blurs at an eighth size), `combine` (adds the glow to the scene). The pass runs on the scene before the scaling to the output, so a text or particle glow looks as it does in WE.

**Workshop scripts and their files.** A script that comes from a Workshop item declares `export let __workshopId = '<id>'`, and the layers it creates by model path (`thisScene.createLayer('models/bar.json')`) name the file as if the item's package were the root; the scene package carries it as `models/workshop/<id>/bar.json`. The engine maps the path that way when the short one does not exist (assumption: nothing documents it, but the packages of 2284273415 and 2365605586 hold exactly these files and nothing at the short path).

**Auto projection.** `orthogonalprojection: {"auto": true}` without a width and height has no size of its own (assumption, nothing documents it): the scene takes the size of the image layer whose lower left corner, after its scale, sits on (0, 0), else of the largest layer. 1222540043 is a 1500 x 1000 picture at (750, 500) under a flag layer scaled 1.05, so its scene is 1500 x 1000 and the picture fills the output. **Puppets** (`puppet` in a model JSON, `animationlayers` on an object) are described in [puppets.md](puppets.md).

**Fullscreen, compose and project layers (M5 fixes).** A `fullscreen` layer is the whole viewport whatever the object says: its quad is the clip space square and it has no transform (WE's own `passthrough` shader without `TRANSFORM` takes its vertices as clip positions; drawn with the layer's unit quad it showed a shrunken copy of the scene in a corner: 1885306489). A model with `passthrough` that is not fullscreen (`util/composelayer.json`, `projectlayer.json`) uses the `composelayer` shader, which writes its quad from the texture coordinates and finds the picture from the layer's screen position: the base pass of such a layer gets the layer's real matrix and a unit quad, so the layer's buffer holds the part of the scene behind it and its effects run on that (3184155462: the audio visualiser and the post-process layers, SSIM 0.43 to 0.93).

**Clear colour.** `general.clearcolor` of exactly 0.7 0.7 0.7 is the editor's default, and almost every scene still has it. Where such a scene draws nothing, the preview images show black, not grey (checked on 2280451215, 1370984500 and 1885306489, whose pictures do not cover the whole scene), so the untouched default reads as black. Any other colour is used as written (assumption: nothing documents which WE does).

## Meshes (phase 2, piece 3)

**Mesh objects** (`"model": "models/<name>/<name>.mdl"` in scene.json; 235 objects in 13 library items). Keys: `origin`, `angles`, `scale`, `parent`, `visible` (the node, as for every object, keyframes and user bindings included), `solid` (read; no script reaches meshes yet), `castshadow` (on unless the file says false: see "Shadows"), `disablepropagation` (not read). A `model` that names a JSON file whose `puppet` is the .mdl is read the same way (none in the library). An object with `effects` draws without them (named `effects on a 3D mesh`).

**The .mdl of a mesh** (`puppet/mdl.cpp`, `parseMeshModel`; read from the library's 131 static and 7 skinned models, nothing copied):

```
"MDLV0007", "0016", "0019", "0021" or "0023", NUL  9 bytes (the versions of the library's mesh objects; others are refused)
u32 flags, u32 1, u32 partCount                    flags 0x0f (0x0b: no tangents); 0x0180000f a skinned mesh (every part has the
                                                   puppet vertex and the skeleton follows the parts)
per part:
  (zero bytes before every part but the first: 6 in version 23, 2 in 21; their meaning is not known)
  NUL string material                              "materials/models/<name>/<material>.json": the part's own material
  u32 wideIndices                                  0: u16 indices, 1: u32 indices (models over 65,536 vertices)
  6f bounds                                        min xyz, max xyz (version 19 on; before, worked out from the vertices)
  u32 flags                                        the vertex format (version 16 on; version 7 uses the header's)
  u32 vertexBytes                                  then vertexBytes / stride vertices:
                                                   bits 1 position 3f, 2 normal 3f, 4 tangent 4f, 8 uv 2f, in that order:
                                                   0x0f is 48 bytes, 0x0b 32 (widened to 48 on reading: a tangent along +x)
                                                   0x0180000f: position 3f, normal 3f, tangent 4f, bone indices 4 x u32,
                                                   bone weights 4f, uv 2f (80 bytes: the puppet vertex, docs/scene-vk/puppets.md)
  u32 indexBytes, indices                          triangles, counter-clockwise seen from outside (every triangle of the
                                                   library's models agrees with its vertex normals that way)
zero bytes (7 in version 23, 3 in 21, 1 in 16, none in 7 static; 6 in 23, none in 19 skinned)
a skinned mesh: "MDLS0002|4" skeleton and "MDLA0005|6" animations, laid out as a puppet's (puppets.md)
then sections the draw does not read: MDMP (morph targets), MDAT (attachments), MDLE (version 7 ends in "MDLE0001", NUL,
u32 1, two zero bytes)
```

**The older versions, byte by byte** (checked on every file: the 24 MDLV0007 models of 2244339517 and the MDLV0016 one of 3509243656, `Hollow Cylinder`; each reads to its last index with the counts lining up, every index in range). MDLV0007 `grid.mdl`: `MDLV0007 00` | `0b000000` flags (position, normal, uv) | `01000000` | `01000000` one part | `materials/models/grid/grid.json 00` | `00000000` u16 indices | `80000000` 128 bytes = 4 vertices of 32 | the vertices | `0c000000` 12 bytes = 6 indices `0 1 2 0 2 3` | `MDLE0001 00 01000000 0000`. The other 23 have flags 0x0f and 48 byte vertices laid out as version 23's (position, normal, tangent, uv: `triangle.mdl`'s first vertex is (19.37, 0, 1.25), normal (0, 0, 1), tangent (1, 0, 0, 1)); `geo4.mdl` has 65,449 vertices under u16 indices. MDLV0016 `Hollow Cylinder.mdl`: `MDLV0016 00` | `0f000000` | `01000000` | `01000000` | `materials/models/Hollow Cylinder/diffuse_0.json 00` | `00000000` u16 | `0f000000` the part's vertex format | `60300000` 12,384 bytes = 258 vertices of 48 | vertices | `00060000` 1,536 bytes = 768 indices | one zero byte. No bounds before version 19 (MDLV0019, the rat of 3164411828, has them); the meaning of a version 7 file with several parts is not known (none in the library: the reader takes the zero bytes between parts as for the newer ones).
Verified: on all 106 static MDLV0021/0023 models of the library the parts, counts and index ranges line up to the last byte. A model with several parts (up to 66 in the library) draws each part with its own material. A model several objects use is read once (the loader keeps it by path) and uploaded once into device-local buffers through a staging copy, shared by those objects; the u16 indices stay u16 on the GPU, and the CPU copy of the vertices and indices is let go after the upload. Limits: 256 parts, 4 million vertices and 16 million indices a model, positions finite, every index below its part's vertex count; a skinned model also has the puppet's limits on its skeleton and animations (128 bones, ...), and every vertex's bone indices must name bones of the skeleton. A model whose header and parts disagree about skinning, a skinned one without its skeleton right after the parts, a skeleton after static parts, or a vertex format other than the two is refused with its reason (`3D model: ...`). MDMP morph targets draw the mesh at rest and are named `morph targets`, MDAT attachments are named `mesh attachments` (a 2D puppet's MDAT is read: docs/scene-vk/puppets.md, "Attachments") (neither occurs in a mesh object of the library: see below).

**Skinned meshes** (phase 2, piece 8: 3732632761, 3164411828, 3495221617, 3662790108, 3777414605). Verified on the 7 skinned models of the library (MDLV0019 and 0023; 1 to 4 parts; 2 to 117 bones): the parts read with the 80 byte stride up to the skeleton, whose `end` offset and animation records line up as a puppet's; every bone index is below the bone count. The bind matrices rotate about all three axes (FBX imports), and a track's first frame rebuilds its bone's bind matrix exactly as `translation * Rz * Ry * Rx * scale` with the frame's angles (checked on fbxAnimation2, critter and the rat), which confirms the Euler order the puppets assumed. The object's `animationlayers` are read as an image layer's (`animation`, `blend`, `rate`, `visible`, `additive`, user bindings); with at least one, a skeleton the vertices use and animations, the parts' materials get `SKINNING 1` and `BONECOUNT <the bones the vertices name>` and `g_Bones` holds each bone's skinning matrix (animated world times inverse bind world, in the model's own space: generic4 skins `a_Position` and the normal with it before `g_ModelMatrix` places the mesh). Without animation layers the mesh is drawn as the file has it, unskinned (the bind pose, as for puppets; 3777414605's church and 3662790108's Deimos). The bones are computed on the CPU each frame for each skinned object (117 bones at most in the library), into buffers reused from frame to frame; a skinned mesh keeps the scene drawing every frame even when no shader reads the time. The vertex goes to the GPU as it is (80 byte stride, the puppet's attributes). Animations whose mode string is empty are clips an FBX import added (3164411828's rat plays `Armature|ArmatureAction.001 (Clip 91)`, which has tracks of its own): they play as `loop`. One clip of the library has no tracks (3495221617's `Yeag`, played at rate 0.03): it leaves its mesh in the bind pose; what its trailing fields (a frame range, -1) refer to is not known.

**Another layer's composite on a mesh** (`_rt_imageLayerComposite_<id>_a`, or `_b`, as a part's texture: 3509243656's compass ring and custom sky, 3662790108's night side of the Earth). The object `<id>` is an image layer that, in all three, is hidden (`"visible": false`) and comes after the mesh in the file; the mesh object lists it in `dependencies`. The renderer draws that layer into its own buffers when the mesh needs it, inside the mesh's draw and before the mesh's rendering instance (as the scene copy for `REFLECTION` is made), whatever the layer's visibility, alpha or place in the file: its base picture and its effects, through the same code as a layer with effects (`drawLayer` with `bufferOnly`), and the part samples the buffer the chain ends in (the layer's finished picture, whichever of a and b holds it; `_b` names the same finished picture, as nothing in the library names it on a mesh). A layer is rendered at most once a frame: every mesh and part that names it, and the layer's own draw in its place in the file when it is visible, read the buffer of that one rendering (so a layer whose effects read the scene so far sees the scene as it was at the first of those draws). The tint a layer gets when it is composited into the scene (`color` with a shader that takes no `g_Color4`) is not in the buffer. `--check` refuses a part that names an object that is not an image layer of the scene; any other `_rt_` name on a mesh is still refused. Assumptions: that WE gives the mesh the layer's finished picture rather than the a buffer at some point of its chain, and that a hidden layer still renders for the meshes that depend on it (the previews show the compass and the night lights).

**Morph targets and attachments.** No mesh model of the library has an MDMP or MDAT section (the 138 models that are not puppets were scanned): MDMP is only in three 2D puppets (3577990983, 3810943704), MDAT in 38 2D puppets of 25 items. Nothing in the library drives a blend shape: no scene key and no script calls `setBlendShapeWeight`; whether animation tracks carry weights is not known (shipped `generic4.vert` reads the targets from a `morph` texture by `gl_VertexID` with `g_MorphOffsets` and `g_MorphWeights`, so it needs both the section's layout and a source of weights). A mesh with MDMP stays drawn at rest and named `morph targets`, one with MDAT named `mesh attachments`; no script of the library calls `getAttachment*` (the 25 items use the object key `attachment`, a child placed on a puppet's attachment, which is the 2D puppet's `attachment` degradation, not this one).

**Materials.** Almost every part uses WE's shipped `generic4`, translated like any shader, with the material's combos and constants (`color` is the tint, `alpha` the opacity, `roughness`, `metallic`, `reflectivity`, ...). `g_ModelMatrix` is the node's world matrix, `g_NormalModelMatrix` its inverse transpose, `g_ViewProjectionMatrix` the renderer's projection (the orthographic one until the perspective camera, piece 4), `g_EyePosition` the view's centre at the near plane (an orthographic view looks straight down -z). `LIGHTING` draws the ambient term (`ambientcolor` / `skylightcolor` by the normal's slope) plus `PerformLighting_V1`, the engine's own sum over the scene's lights (Lights, below). `FOG` draws the scene's fog (see "Fog"; `g_EyePosition` is the camera's eye in a perspective scene). `REFLECTION` samples the scene drawn so far (`_rt_MipMappedFrameBuffer`, the default of its slot 3) at the screen position bent by the normal; the copy has a mip chain and `g_Texture3MipMapInfo` (see "Reflection"), so a rough material reflects blurred. Cost: one full-size copy (a blit of the scene target, plus the chain's blits) per reflective mesh per frame, made before the mesh's rendering instance and shared by its parts; a scene with many reflective meshes pays it for each. An empty texture slot takes the picture its sampler names as default (`util/white`, a toon gradient, the scene so far).

**Depth and order.** The scene target has a depth buffer of its own size (D32_SFLOAT, else X8_D24_UNORM_PACK32: Vulkan guarantees one of the two as a depth attachment, and one of D24_UNORM_S8_UINT and D32_SFLOAT_S8_UINT, which are the last resort; created with the first mesh, kept with the target). All the parts of one mesh are drawn in one rendering instance, after one barrier on the colour and depth targets. Objects are drawn in file order, as before: a mesh is drawn where it stands in the list, straight into the scene target, tested against the depth (less or equal) and written to it as its material says (`depthtest`, `depthwrite`: "enabled" unless "disabled"); the first mesh of a frame clears the depth to the far plane. 2D layers, text and particles neither test nor write depth, so a layer drawn after a mesh covers it and one drawn before is covered by it. Without `transparentsorting` (below) that is file order. File order rather than meshes first: in 3772288941 the mesh (the wind-up key on the girl's head) comes between the full-screen background and a full-screen overlay; drawn before the background it would be hidden, and the preview shows it above the background and under the overlay. **`transparentsorting`** (general; on in 6 perspective scenes of the library, off or absent everywhere else; ASSUMED reading): the scene pass draws the opaque meshes first, in file order (a mesh is opaque when every part writes depth with `normal` blending), then every other object, 2D layers, text, particles and translucent meshes, sorted back to front by the view-space z of its world origin (the farthest first; equal depths keep file order). The order is worked out again each frame (scripts and the camera move objects). It puts 3509243656's HUD and clock (z 4, near the eye) after its sky sphere, as the preview has them; in 3495221617 (refused for its lights) the order of its sky layers and cloud mesh changes, and neither order matches the preview's framing. A fullscreen or compose layer is sorted by its origin like any other (an assumption with no library case to check). `customsortorder` (in no scene) is not read and is named `custom sort order`. `cullmode` "normal" (the default) culls the faces seen from behind (counter-clockwise on screen is the front; a mirroring scale turns it over), "nocull" draws both sides. Translucent parts are blended in the model's order without sorting.

**Assumptions** (not checked against WE): the meaning of the zero bytes between parts; the Euler order of a mesh node's `angles` (the 2D one, z * y * x, which the puppets use); the eye position of an orthographic view; that the editor's defaults when a material omits a depth key are "enabled" and cull "normal" (every mesh material of the library writes all three).

## Lights (phase 2, pieces 1 and 2)

**Light objects** (`"light": "lpoint" | "lspot" | "ltube" | "ldirectional"`, and the older `"point"`; 48 objects in 17 library
items; `scene/scene.cpp`, `lightObject`). The node is read like any object's: `origin`, `angles`, `scale`, `parent`, `visible`
(with conditions), keyframes and scripts on the transform. The light's own keys: `color` (0..1, may be bound to a user property),
`intensity` (bound, or keyframed: the keyframes play), `radius` (scene units), `exponent` (falloff), `innercone` / `outercone`
(spot, degrees), `controlpoint` (tube), `castshadow` and `cascadedistance0..2` (see "Shadows"), and the keys later pieces draw: `castvolumetrics`, `density`,
`volumetricsexponent`, `cascadedistance0..2`, `lightsourcesize`, `usecookie` / `cookie`. Defaults where a key is missing (the
values most library lights carry, not checked against the editor): white, intensity 1, radius 1000, exponent 2, cones 20 and 30.
Every value but the tube's end may be bound to a user property; keyframes on any of the values below own it (the binding is dropped, as
for any keyframed property). **Scripts and keyframes** on a light's colour, intensity, radius, exponent, inner and outer cone, density and
volumetrics exponent run like those of a layer: the script is attached to the light's node as the property `light.<key>` (`light.color`,
`light.intensity`, `light.radius`, `light.exponent`, `light.innercone`, `light.outercone`, `light.density`, `light.volumetricsexponent`), which
`thisObject`'s getters and setters know, and the lights are packed again every frame (the scene pass runs when they change). A switch
(`castshadow`, `castvolumetrics`, `usecookie`) or `lightsourcesize` with a script keeps its file value and is named `light script`. The
library has this on one light only (an intensity script in 3662790108, which the script count still refuses) and keyframes on the
intensity of two scenes; the rest is for what workshop scenes may do.
What is not drawn yet is named (table above: `light cookie`, ...). `general.lightconfig` (`{"spot": 13}`,
`{"point": 1, "pointshadow": 1}`, ...) counts the light objects of each kind and those casting shadows: in all 15 library items
that have it, it matches the objects, so the counts are taken from the objects and the key is not read.

**What the shaders get** (`render/lights.h`, filled once a frame in world space, after keyframes, scripts and properties; the
camera plays no part). WE's lit shaders size their light arrays by the combos `LIGHTS_POINT`, `LIGHTS_SPOT`, `LIGHTS_TUBE`,
`LIGHTS_DIRECTIONAL`: here each is the number of lights of that type in the scene, at most 32, set on every shader that asks for
`#require LightingV1` or declares the arrays itself, and part of the shader's key (one compile per lit shader and count). A light
keeps its slot whatever the others do; a hidden one gives black, so showing or hiding a light recompiles nothing. In an
orthographic scene those shaders also get `SCENE_ORTHO` (genericimage3/4 then take a fixed view vector).

| Array | `.xyz` | `.w` |
|---|---|---|
| `g_LPoint_Color`, `g_LSpot_Color`, `g_LTube_Color` | colour * intensity | radius |
| `g_LPoint_Origin` | world origin | exponent |
| `g_LSpot_Origin` / `g_LSpot_Direction` | world origin / where it shines | cos(inner cone) / cos(outer cone) |
| `g_LSpot_Exponent` | (exponent, 0, 0) | 0 |
| `g_LTube_OriginA` / `g_LTube_OriginB` | world origin / the control point in world space | exponent / 0 |
| `g_LDirectional_Color` / `g_LDirectional_Direction` | colour * intensity / towards the light | 0 |
| `g_LightsPosition[4]`, `g_LightsColorRadius[4]` (older `point` lights, `genericimage2`, `generic2`) | world origin / colour * intensity | - / radius |
| `g_LightsColorPremultiplied[3]` | colour * intensity of lights 0..2 | the fourth light's colour * intensity, one component per `.w` |

**`PerformLighting_V1`** (`shaders/translate.cpp`, written for this engine from the shader interface; nothing of WE's own text).
Where a shader says `#require LightingV1` the translator puts the arrays above and a function of that name and signature
(`worldPos, albedo, normal, viewVector, specularTint, baseReflectance, roughness, metallic`; the shaders pass `f0` sixth), which adds
per light one term of the shipped `common_pbr_2.h`: `ComputePBRLightShadow` towards the point light's origin, towards the spot's
origin with the colour scaled by `smoothstep(cos outer, cos inner, angle to the axis)`, towards the nearest point of the tube's
segment, and `ComputePBRLightShadowInfinite` along a directional light's direction; the shadow factor is 1 except for the shadowed
lights ("Shadows"). A shader that does not include `common_pbr_2.h` gets a function that returns black (it then shows its ambient
term only).

**2D lit layers.** `genericimage2/3/4` with `LIGHTING` (normal map in slot 1, `PBRMASKS` in slot 2) are the shipped shaders as
they are; the engine feeds them `g_LightAmbientColor` (`general.ambientcolor`), `g_LightSkylightColor` (`skylightcolor`) and the
lights. Under `LIGHTING` these shaders place their vertices through the world (`g_ModelMatrix`, then `g_ViewProjectionMatrix`), so a
lit layer's model matrix is its world matrix with the parallax shift, and a lit layer drawn through its own buffers (effects) gets
its world matrix with a view-projection that takes the world back onto the buffer: it is lit where it is in the scene. The packed
lights are compared every frame and a change runs the scene pass; the buffers other layers keep are not touched by it. A lit pass
is not kept in a layer's buffers while the lights can move (the scene has keyframes or scripts) or, in an orthographic scene with
camera parallax, while its layer has a parallax depth (the parallax moves it under the lights).

**Reflection.** `REFLECTION` on a 2D layer (`genericimage2` / `genericimage4`) is drawn: the layer's shader reads the scene drawn so far through the slot whose sampler defaults to `_rt_MipMappedFrameBuffer` (slot 3; the plain copy `_rt_FullFrameBuffer` in slot 4 gets the same picture) at the position the normal map bends, at `lod = roughness * g_Texture3MipMapInfo`. The copy has a full mip chain (a linear blit of each level from the one above: a 2x2 box filter) when a shader reads the `MipMapInfo` of the slot, and none otherwise (a layer without `REFLECTION` copies nothing). Assumption (survey Q5.3): `g_Texture<N>MipMapInfo` is the highest level of the chain (levels - 1), so that `roughness` 1 lands on the last, 1 x 1, level; it is 0 for a texture without a chain, and a file texture gives its own count. A copy is taken for each reflective layer, at the moment the layer is drawn: a layer reflects what is drawn under it, not the layers above. `reflection` bound to a user property is not followed (`combo reflection`).

**Readings that are assumptions**, checked where the library allows:
- Colour * intensity in `.rgb` (assumed; the falloff then decides the reach).
- The older `g_LightsColorPremultiplied` is colour * intensity. The other reading of the name, also times the squared radius, washes
  2590512455 out (its legacy lights sit 500-2000 units in front of the picture with radii of 400-1800); its preview is a dark red
  picture, which only the plain product gives (snapshot against the preview, 2026-10-20).
- Cones are half-angles in degrees; Direction.w = cos(outer), Origin.w = cos(inner) (follows from the shader's smoothstep order).
- A spot shines along its node's +x, and a directional light's direction is the opposite of its +x (towards the light). Read from
  the library: the 13 spots of 3164411828 (at z 500 over a picture at z 0) all meet the picture's plane in front of them along +x
  and behind them along -z, and the spots of 3815542599 are turned about z only, which would change nothing for a light along z.
- A tube runs from its origin to its `controlpoint`, taken in the light's own space (the three tubes of 3270865076 share one
  control point while their origins differ, as copies of one light would). The world-space reading draws 3270865076 the same at
  preview size (its tubes are barely turned and their lit sign is far from both ends), so the preview does not decide; a point at
  the origin only (no segment) leaves the left of the picture darker than the preview.
- Light `scale` is not applied to the radius (named `light scale`).
- A point exactly at a light's origin gives the shipped term a zero vector to normalise: NaN, as in WE.

## Volumetric lights (phase 2, wave 3)

A point or spot light with `castvolumetrics` draws its beam in the air with WE's own shaders (`volumetricsback`, `volumetricsfront`,
`blur_k3`, `passthrough`, read from the install like every shader; their text is not in this repository) and the shipped materials'
state (`materials/util/volumetrics_back|front|blur_h|blur_v|combine.json`: front faces additive without depth, the blur and the
combine without depth, the combine additive). `SceneRenderer::drawVolumetrics`, `volumetricLights` (render/lights.h).

What the shaders declare (read from the shipped files; no code taken): `volumetricsback` has a vertex stage only (an empty fragment
main): `gl_Position = VP * g_AltViewProjectionMatrix * a_Position`, so `g_AltViewProjectionMatrix` is the volume's own-space to world
matrix here. `volumetricsfront` has the same vertex stage (a spot's xy scaled by 0.99; combos `POINTLIGHT`, `FULLSCREEN`,
`REVERSEDEPTH`) and passes the clip position on. Its fragment stage reads the depth of the volume's back faces (`g_Texture1`,
`_rt_volumetricsBack`), a limit depth (`g_Texture3`, `_rt_volumetricsSingle`, read with a texel fetch), with `SHADOW` the shadow atlas
(`g_Texture0`, `_rt_shadowAtlas`, comparison sampler) and with `COOKIE` a picture (`g_Texture2`). It rebuilds the world position of the
front face, of the end (the nearer of the back face and the limit) from the screen position and depth through `g_EffectModelMatrix`,
walks `sampleCount` steps between them (2, 3, 5 or 8 without shadow or cookie, 12, 24, 32 or 64 with, by `QUALITY` 1 to 4, 0 or less =
the smallest) and sums `pow(saturate(1 - distance / radius), exponent)` times, for a spot, `smoothstep(outer, inner, cos)` times the
shadow comparison (`g_AltModelMatrix` projects the point into the light's picture; a point light uses the shipped
`CalculateProjectedCoordsPoint`), times the fog's alpha with `FOG`. The result is `density * light intensity * (ray length / radius)
(halved for a point light) * sum / samples * colour * 0.1`, alpha 1. The values come in `g_RenderVar0..4`: the atlas block of the light
(spot: its `g_LFeature_ShadowProjectionTransform`; point: the point block), `(radius, inner cos, outer cos, intensity)`, `(origin,
density)`, a spot's axis (a point light's: its `g_LFeature_ShadowPointProjection`) and `(colour, exponent)`.

What this engine does (the readings that are assumptions are marked):
- **Volume.** A point light: a unit sphere scaled to the radius. A spot: a unit cone (apex at the light, base circle at distance
  `radius` along its axis, half-width `radius * tan(outer)`); a spot with an outer cone of 80 degrees or more: the sphere (the cone
  would be degenerate; the shader's cone term does the shaping). Assumption: WE's meshes are not known; any volume that holds the beam
  gives the same picture, since the shader's falloff and cone terms end the light.
- **Back faces** (cull front, depth test and write, nearest wins) into a half size depth image, cleared per light. **Front faces**
  (cull back, additive, no depth) into a half size light buffer shared by all the beams. Depth conventions: the engine's clip depth
  is OpenGL's (-1 to 1, mapped to 0 to 1 by the translated vertex stage); the images hold 0 to 1 and the prelude's
  `texSample2DBackBuffer` / `texLoad2D` return them as -1 to 1, which is what `v_ScreenPos.z / w` is, so `g_EffectModelMatrix` is the
  inverse of the scene's view-projection (OpenGL clip, y up) for the start, the end and the limit alike.
- **Limit depth.** The scene's depth buffer where a 3D mesh drew it this frame (opaque meshes stop a beam), else the far plane (a 2D
  scene's layers have no depth: the beam goes through them to the back of the volume). Assumption: what `_rt_volumetricsSingle` holds.
- **Order.** After every layer, particle and mesh of the scene pass (so it lights the finished picture), before the bloom. Assumption:
  WE's order is not known (open question).
- **Then** the light buffer is blurred (`blur_k3`, horizontal and vertical: its materials' passes, in the order of their names) and
  added to the scene (`passthrough`, additive). Assumption: the blur hides the few samples; its buffer size (half) is ours.
- **Inputs.** `QUALITY` 3 (not a combo of the shader's header: WE takes it from a setting we do not have; assumption), `POINTLIGHT`
  for a point light, `SHADOW` when the light casts a shadow, the scene has casters and the atlas has room for it (the beam then reads the
  light's picture; its volume counts as a receiver so the picture is drawn), `FOG` and the scene's fog combos like any shader. Colour
  is the light's colour and the intensity enters on its own (as the shader multiplies both; assumption), the falloff exponent is
  `volumetricsexponent` (the shader's `VAR_EXPONENT`; assumption, the keys' names are the only evidence), `density` is the light's.
- **Limits.** At most 16 beams a frame (`kMaxVolumetricLights`; the library's most is 13 lights in an item), the rest are not drawn. Values are
  held to ranges: radius at most 1e5, density, intensity, each colour component and the exponent at most 1e3, the exponent at least 1e-3
  (`pow(0, 0)` is undefined in GLSL). A scene that is not cleared each frame (`clearenabled` off) keeps its picture, so the beams are added
  to a copy of it each frame (which the bloom and the scaling pass then read), never to what is kept. Where the scene's depth format
  cannot be sampled the meshes do not stop the beams (logged once). The lights, the shaders and the requests are worked out once a frame or
  kept between frames (the shaders until a property or the render scale changes).
- **Camera inside a volume.** Nothing special is done (`volumetrics_fullscreen.json`'s `FULLSCREEN` variant is not used): the front faces
  are culled, so in a perspective scene a beam whose volume holds the eye is not drawn where the eye is inside it. Orthographic scenes
  (every library item that has beams but one) are not affected: the eye is far from every volume.
- **Not drawn.** A light with density, intensity or radius at or below zero, a hidden light, tube and directional lights (named, above).
  `COOKIE` is never set: a cookie light's beam is drawn plain and `light cookie` stays named. `FULLSCREEN` (the camera inside a volume,
  `volumetrics_fullscreen.json`) is not used: in a perspective scene the beam of a light whose volume holds the eye is not drawn where
  the front faces are culled. The beam's colour accumulates in the scene's buffer format (8 bit: it clips at 1 where beams overlap).
- Library (`--check`, 2026-10-21): 3217188597, 3287715210, 3465215190, 3815542599 and 3777414605 no longer name `volumetrics` (3217188597 and
  3287715210 are drawn complete; the others keep their other names). `scene-vk-snap` 960 x 540, 30 frames, seed 7: the beams change 2 to 21 % of the
  pixels (a sun glow, the cones of the street lamps of 3815542599).
- Tests: `tst_scenevk_render` (the beam inside a cone and not outside, density 0, a hidden light, a point light's sphere, a caster
  blocking the beam; with a test shader pair that has the shipped interface), `tst_scenevk_scene` (the degraded names).

## Shadows (phase 2, piece 6)

**Which lights, which casters.** A point, spot or directional light with `castshadow` casts shadows (at most 4, 4 and 2 per scene:
the rest are drawn unshadowed and named `too many shadowed lights`); tubes and the older `point` lights do not (named). Only 3D
meshes cast. A mesh object casts unless it says `"castshadow": false`: the library's 235 mesh objects never write `true`, 24 write
`false` and 211 leave the key out, among them every mesh of the scenes whose lights cast shadows (3495221617, 3503626457,
3589454154, 3777414605, 3812243386), which would otherwise have nothing to cast (an assumption: the editor's default is not traced).
Every part of a casting mesh casts except those with additive blending (light added to the scene, a glow or a halo: an assumption,
WE's rule is not known); a mesh whose parts are all additive is no caster at all. A scene draws shadows only when a light casts and a mesh does: the 2D
scenes whose point lights say `castshadow` (3287715210, 3465215190) have no caster, so nothing changes for them and nothing is named.
Image layers with `"castshadow": true` (none in the library) are named `shadows from 2D layers`; lit particles are lit unshadowed and
named `shadows on particles`.

**The caster pass.** Each casting part is drawn with its material's caster: the shader its shader names in `// [PASS] shadow <shader>`
(`chroma4` -> `shadowcaster`, `foliage4` -> `shadowcasterfoliage4`, `fur4` -> `shadowcasterfur4`), else WE's shipped `shadowcaster`
(`generic4`, the shader of every library mesh, has no such line; `materials/util/shadowcaster.json` names the same shader).
`shadowcaster` gets the material's textures and constants and only the combos `SKINNING`, `BONECOUNT` and `ALPHATOCOVERAGE`; a
material's own caster gets all its combos. The casters are depth only, both faces, with a slope-scaled depth bias (2 steps and twice
the slope), so a lit surface does not shadow itself. The shipped casters take `g_ViewportViewProjectionMatrices[6]` and send instance
`i` to viewport `i` (`gl_ViewportIndex = gl_InstanceID`); Vulkan needs a device feature for that output, so the translator drops it
and the engine draws a part once per picture with that picture's viewport and `firstInstance = i` (one uniform block and texture set
for all of a light's pictures). A skinned caster uses the frame's bones. Where the shaders are not installed (tests) a built-in
`shadowcaster` of the same interface stands in.

**The atlas** (`_rt_shadowAtlas`; size and layout are this engine's choice, WE's are not known). One depth image (D32_SFLOAT, else
X8_D24, else D16), four cells wide, a cell being 512 pixels (so 2048 wide; 256 and 1024 wide while the render scale is below 1, which
also lowers `LIGHTS_SHADOW_MAPPING_QUALITY` to 1). Each shadowed light, in slot order (point, spot, directional), takes blocks of cells
at the first place they fit, scanning the rows from the top and each row from the left: a point light 2 x 3 cells (its six cube faces
of 512, laid out as the shipped `CalculateProjectedCoordsPoint` reads them: columns + and -, rows x, y, z, each a third of the block
measured as 0.3333), a spot 2 x 2 cells, a directional light 2 x 2 for each of its two near cascades and 1 x 1 for the far one (its
texels are large anyway). The atlas is as tall as the rows used, and depends only on the light counts and the cell, so it stays the
same size for a scene. It is cleared to the far plane before its casters are drawn.
Sampled with a comparison sampler (`sampler2DComparison` is `sampler2DShadow`): less or equal is lit, linear filtering when the
format allows (the comparisons filtered 2 x 2), and lit outside the image (a white border). `g_Texture6Texel` is (1 / width,
1 / height, width, height); in general `g_Texture<N>Texel` is that of whatever is bound in slot N.

**What the shaders read** (`render/shadows.h`). The scene sets `LIGHTS_SHADOW_MAPPING` 1, `LIGHTS_SHADOW_MAPPING_QUALITY` 2 (WE's
shaders know 1: one comparison, 2 and 3: nine; WE's own setting is not in the scene, quality 2 is this engine's choice) and the
shadowed counts `LIGHTS_POINT_SHADOW`, `LIGHTS_SPOT_SHADOW`, `LIGHTS_DIRECTIONAL_SHADOW` on every lit shader; the shadowed lights take
the first slots of their type (then the others in file order), and the lighting function shadows slots 0 .. count - 1. In the
shipped shaders these combos appear only as `LIGHTS_SHADOW_MAPPING` (which declares the atlas, `SHADOW_ATLAS_SAMPLER` in slot 6 of
generic4/genericimage4/chroma4/foliage4/fur4, slot 4 of the particle shaders) and `LIGHTS_SHADOW_MAPPING_QUALITY` (the filter); the
per-type counts are read by the injected lighting function only, so their meaning here is this engine's own.
- Point light i: `CalculateProjectedCoordsPoint(worldPos, origin, g_LFeature_ShadowPointProjection[i],
  g_LFeature_ShadowPointProjectionTransform[i])` then `PerformPointShadowMapping`. Read from the shipped helper: the face is the
  dominant axis of worldPos - origin (x before y before z on a tie, the sign picking + or -), its view is a fixed rotation about the
  light (the face looks down its own -z), the projection is `(x, y, P.x z + P.y, P.z z + P.w)` of that view, x and y are read at 0.47
  (quality 1 and 2; 0.48 at 3, 0.49 above) of the face's half-size and placed by the transform `(u, v, width, height)` of the light's
  block. So Projection = (-f / (f - n), -n f / (f - n), -1, 0): w is the distance along the face's axis and the depth runs 0..1 from
  the near n to the far f; each face is drawn with the same view and x, y widened by 0.94 (a field of view a little over 90 degrees),
  so the filter's taps stay inside the face. n and f: as far as the farthest caster's bounds (or the radius when nearer), from half
  the nearest one's distance (at least f / 10000).
- Spot i: `CalculateProjectedCoords(worldPos, g_LFeature_ShadowProjection[i])` (x and y to 0..1 top row first, depth z / w) then
  `PerformShadowMapping(coords, g_LFeature_ShadowProjectionTransform[i])` (placed by the block). The projection is a perspective one
  along the spot's axis as wide as twice its outer cone (at most 170 degrees), depth 0..1, n and f as for a point light.
- Directional light i: three cascades, `g_LFeature_ShadowCascadeProjection[3 i + k]` and `g_LFeature_ShadowCascadeTransform[3 i + k]`
  (this engine's names: WE's are not known). The shipped `CalculateProjectedCoordsCascades` sets .w to 1 for a point outside the
  box (|x| or |y| at least 0.99, or |z| at least 0.99; the `REVERSEDEPTH` variant is for a reversed depth, which this engine does not
  use); the lighting function takes the first cascade whose box holds the point (p1, else p2, else p3, by their .w), and a point
  outside all three is lit. The view is split along its depth at the light's `cascadedistance0..2` (sorted; the library has 0.3 /
  0.4 / 8 and 556 / 1112 / 2039; a light without them gets 1152 / 2304 / 4224, an assumption): cascade k covers the view from the
  last split (the near plane for the first) to `cascadedistance<k>`. Each cascade's box is orthographic along the light, around the
  sphere that holds its slice of the view (so its size does not change as the camera turns), its centre snapped to the atlas's pixels
  (so a moving camera does not make the edges crawl), and deep enough towards the light to hold every caster; the depth range is
  rounded outwards to steps of the box's radius (a point light's and a spot's to an eighth of the power of two below it), so a camera or
  a caster that drifts keeps the depth mapping, and the picture, as it is.
- Normal offset: `g_LFeature_ShadowCascadeTexel[3 i + k]` (this engine's name; x is the world size of one texel of cascade k, the box's
  width over its block's pixels). Before it looks a cascade up the lighting function moves the point 1.5 of that cascade's texels along
  its normal, so a slope lit at a grazing angle does not shadow itself in the large texels of the far cascades (translator version 42).
- In an orthographic scene the cascade distances are measured from the scene's plane (the middle of the view's 20000 units of depth,
  10000 from the near end), not from the near end: measured from there the first 556 / 1112 / 2039 units end long before the plane and no
  receiver would be in a cascade (an assumption: WE's are not known).
- The depth looked up is kept at most 1 (a receiver beyond the far end of a light's picture is lit unless a caster is in front of it:
  the comparison of a float depth does not clamp the reference).
- How WE combines the factor is the shipped `ComputePBRLightShadow`: it scales the specular distribution and the wrapped diffuse.

**When the atlas is drawn** (performance). At the start of a scene pass, picture by picture, and only what changed:
- a picture no receiver can see is left empty: the receivers are the draws that read the atlas (lit meshes and lit 2D layers, by
  their bounds; a lit fullscreen or puppet layer counts as everywhere); an unlit sky box reads nothing;
- a caster part is drawn only into the pictures whose frustum its bounds reach;
- a part whose bounds cover less than a texel of a picture in both directions is not drawn into it (a big part's runs are not tested
  again: the part passed as a whole): what it casts there is lost in the filter;
- a big part (3 x 16384 indices or more) is cut into runs of 6144 indices with their own bounds, so the picture of a small cascade draws
  only the runs it can see;
- each picture is kept on its own (where its casters were, whether a receiver saw it, when it was drawn) and is drawn again only when a
  caster in it lands more than half a texel (a whole one in a cascade) elsewhere, or 2 x 10^-5 of the depth range, than when it was
  drawn, through the old and the new matrices (the light's, the camera's for a cascade, the caster's), or a skinned caster in it moved
  its bones; and then no sooner than 1/30 s after it was last drawn (the near cascades and a point light's faces) or 1/10 s (the far
  cascade and spots), the scene seconds. Until then the picture and the receivers' values for it stay as drawn, together. A point
  light's six faces are drawn together when its depth range changed (they share it). Anything else (a caster shown or hidden, a
  light's shadow switching, the cell size) draws the whole atlas again; a lit 2D layer kept in its own buffer is drawn again after a
  shadow pass;
- a static mesh that casts gets a copy of its positions alone (12 bytes a vertex), which `shadowcaster` reads.
So a scene whose lights and meshes stand still draws its shadows once. Measured (`scene-vk-snap` 1920 x 1080, GPU ms per frame over
the last 30 of 60 frames, the Radeon of a Ryzen 7 9800X3D, before -> after): 3495221617 12.3 -> 14.3 (a point light, a skinned
critter redrawing the faces it is in), 3589454154 17.7 -> 33.4 (a directional light whose two near cascades, 0.3 and 0.4 units deep,
are redrawn each frame as the scene moves, with Saturn and an asteroid belt of 2.1 million triangles in each). Lit draws bind the
atlas in the slot whose sampler defaults to `_rt_shadowAtlas` (meshes and 2D layers); a comparison sampler with nothing bound (the
particle shaders' slot 4) reads a 1 x 1 depth at the far plane, so it is lit.
