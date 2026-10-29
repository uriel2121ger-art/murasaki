# Wallpaper Engine shader dialect and how murasaki-scene-vk translates it

Written from the shaders WE ships (`assets/shaders`, `assets/effects/*/shaders`) and the ones inside the author's Workshop items; no code of any other renderer was copied. The code is in `src/scene-vk/shaders/` (`source`, `translate`, `cache`, `glslcompiler`); the corpus tool is `scene-vk-shadercheck`.

## Where shaders live

A material names a shader without extension (`"shader": "effects/tint"`); the engine loads `shaders/<name>.vert` and `shaders/<name>.frag` (a `.geom` exists for particles only, M3). They come from the item (its folder or package) first, then the WE `assets/` directory, then from the folders of the shipped effects: every effect carries its own tree (`assets/effects/waterripple/shaders/effects/waterripple.frag`, `.../materials/effects/waterripple.json`, `.../materials/effects/waterripplenormal.png`), and a scene or a Workshop package names those files without the effect folder, so `Vfs::forItem` adds each `assets/effects/<name>/` as a root after everything else. Workshop-authored effects (`effects/workshop/<id>/...`, shaders under `shaders/workshop/<id>/`) are inside the item's package.

## The source

Plain GLSL text with HLSL habits. `#include "common.h"` refers to `shaders/common.h`; the shipped headers have no include guards and include each other, so a header is expanded once, at its first include (`ShaderSource::includes`). Headers seen: `common.h`, `common_blending.h`, `common_blur.h`, `common_composite.h`, `common_fog.h`, `common_foliage.h`, `common_fragment.h`, `common_particles.h`, `common_pbr.h`, `common_pbr_2.h`, `common_perspective.h`, `common_vertex.h`, `base/model_*_v1.h`.

Standard preprocessor directives only (`#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`, `#define`, `#undef`, `#include`) plus one WE directive: `#require LightingV1`, a request for the engine's lighting feature set (the lit shaders then call functions the engine supplies, `PerformLighting_V1`). It is recorded (`ShaderSource::requires_`) and removed; lighting is M5.

Variables the vertex shader reads: `attribute vec3 a_Position; attribute vec2 a_TexCoord;` (plus normals, tangents, bone indices and weights for models, and `a_TexCoordVec4*`-style attributes for particles). Stage outputs are `varying`s. The fragment shader writes `gl_FragColor`.

### Annotations (all of them)

Counted over the shipped shaders:

| Annotation | Count | Meaning |
|---|---|---|
| `// [COMBO] {"material":L,"combo":NAME,"default":N,...}` | 288 | A compile-time option. `NAME` is `#define`d to its value; the source tests it with `#if NAME`. `options` (a name to value map), `type`, `require` (the combo is only offered when other combos have values) and `category` are editor details. |
| `// [COMBO_OFF]`, `[COMBO_DISABLED]`, `[OFF_COMBO]` | 1 each | The same, switched off in the editor. They are still combos with their default. |
| `// [PASS] name shader` | 3 | Another pass of the same material (the shadow caster of lit shaders): `shadowcaster`, `shadowcasterfur4`, `shadowcasterfoliage4`. Its `shadow` pass is the caster of the material's shadows (formats.md, "Shadows"); other names are recorded and ignored. |
| `uniform T g_X; // {"material":K,"default":D,...}` | ~1390 | `material` is the key a material or an effect instance uses to set the uniform (`"constantshadervalues": {K: value}`); `default` its value when nothing sets it. Colours are `"type":"color"` with a "r g b" default; vectors are "x y"; `range`, `label`, `group`, `direction`/`conversion` (an angle given in degrees by the editor and converted to radians, `rad2deg`) and `int` are editor details; `nobindings` forbids property bindings. A uniform without annotation (`g_Time`, the matrices) is filled by the engine. |
| `uniform sampler2D g_TextureN; // {...}` | ~490 | Texture slot N. `combo` (416 uses in all) names a combo that turns on when a texture is bound there (`MASK`, `NORMALMAP`, `PBRMASKS`, ...); `hidden` marks an engine-fed slot; `default` names a render target to bind (`"_rt_MipMappedFrameBuffer"`) when the material gives none; `mode` (`opacitymask`, `normal`), `format`, `formatcombo`, `paintdefaultcolor`, `components`, `nonremovable`, `require`/`requireany`, `label` are editor details. |

Keys never used by the engine are kept in the annotation JSON; only `material`, `default`, `combo`, `hidden`, `label` are read.

### Types and names

Uniform types in the corpus: `float` (747), `sampler2D` (495), `vec4`, `vec2`, `mat4`, `vec3`, `mat4x3` (bone matrices, arrays), `mat3`, `uint`, `sampler2DComparison` and `sampler2DBackBuffer` (editor/shadow shaders), one `sampler3D`. Varyings are `vec2/3/4`, `float`, `uint`, `mat3`. Texture slots are named `g_Texture0` ... `g_Texture8`; the same slot may be given another name by a macro (`#define g_NormalMapSampler g_Texture1`).

Words that are identifiers in the dialect and reserved in GLSL 450 are renamed with a `we_` prefix (`sample`, `filter`, ...). `gl_VertexID`/`gl_InstanceID` become `gl_VertexIndex`/`gl_InstanceIndex`.

## What the translator produces

1. **Combos.** Every declared combo gets its default; a sampler's `combo` becomes 1 when a texture is bound to that slot; then the values the material and the scene give (`combos` in the material pass and in the scene's effect instance; names are upper-cased, since materials write them in lower case). All of them, including zeros, are `#define`d, plus `GLSL 1`. A combo the source tests but nobody declared (`VERSION`, `TRANSFORM`, `SPRITESHEET` in the generic shaders) exists only when a material sets it, which is what `#ifndef VERSION` relies on.
2. **Prelude.** Macros and functions we wrote for the helpers the shaders use: `mul(a, b)` is `(b) * (a)` (shaders are written row-vector times matrix, ours are column-major), `texSample2D`, `texSample2DLod`, `saturate`, `frac`, `lerp`, `atan2`, `ddx`, `ddy`, `rsqrt`, `CAST2/3/4/3X3/F/U`, and overloads of `pow` with a scalar or integer exponent for vectors and integers (HLSL-style calls such as `pow(v, 2.2)` or `pow(x, 3)`).
3. **Preprocessing.** glslang's preprocessor resolves the `#if`s over the combos, so only live declarations remain.
4. **Rewriting the declarations** found at the top level of the result:
   - loose uniforms become one std140 block per stage in set 0 (vertex binding 0, fragment binding 1), in declaration order; a `UniformInfo` gives each member's offset (computed from the std140 rules and checked against the compiler's offsets in the tests), size, `material` key and default text. Arrays take one 16-byte slot per element (a `float g[3]` is 48 bytes); `mat4x3` is four vec3 columns of 16 bytes.
   - samplers become `layout(set = 1, binding = N) uniform sampler2D g_TextureN`; `sampler2DComparison` is `sampler2DShadow`, `sampler2DBackBuffer` a plain `sampler2D`.
   - attributes get fixed locations by name (`a_Position` 0, `a_TexCoord` 1, `a_Normal` 2, `a_Tangent4` 3, `a_Color` 4, `a_BlendIndices` 5, `a_BlendWeights` 6, others from 8 in declaration order).
   - vertex `varying`s become outputs numbered in declaration order (a `mat3` takes three, arrays multiply); fragment `varying`s become inputs with the location of the same name in the vertex stage, or a spare one when the vertex stage has no such output (the fragment must not read it then); integer varyings are `flat`.
   - `gl_FragColor` becomes `layout(location = 0) out vec4 we_FragColor`.
5. **Clip space and orientation.** The vertex `main` is wrapped (`we_main`): clip y is negated and the depth is remapped from OpenGL's [-1, 1] to Vulkan's [0, 1]. The shaders compute clip positions y-up (`a_Position` of a full-screen pass runs from (-1, -1) at the bottom left to (1, 1)) while their texture coordinates and `g_PointerPosition` start at the **top** left (`a_TexCoord` is (0, 0) at the top left), as does every texture the engine reads from a file (row 0 first). The y flip makes Vulkan's framebuffer rows agree with that: every render target holds its picture top row first and a pass samples the previous one with the same coordinates, no further flipping anywhere. (The orientation of raw `.tex` payloads was checked on a sprite sheet: its first row is the top of the picture, like a decoded JPEG.)
6. **Compiling** with glslang (Vulkan 1.3, SPIR-V 1.6) and the SPIR-V plus reflection go to the cache.

## The render graph (what the shaders are driven by)

Each image layer is drawn like this (`render/scenerenderer.cpp`):

- no visible effect and nothing else that needs a buffer: the layer's base pass (the model's material, normally `genericimage2`) draws straight into the scene target with the layer's transform and blending;
- otherwise the base pass writes the layer's buffer A (layer-sized, straight alpha, no blending), each effect pass reads `previous` (the layer's current buffer) and whatever its `bind` list names, writes its `target` (an effect `fbo`, sized by its divisor) or the other ping-pong buffer, and the final buffer is composited into the scene with the base material's blending.
- Texture slots are filled in this order: an effect `bind` entry for the slot, `previous` for slot 0 when nothing else is there, the material texture (or a `_rt_` name given as the texture), the sampler annotation's `default` render target, else a white texel. `_rt_FullFrameBuffer` and `_rt_MipMappedFrameBuffer` are a snapshot of the scene so far (taken once per layer; `_rt_MipMappedFrameBuffer` has a mip chain when the shader reads that slot's `g_Texture<N>MipMapInfo`, the highest level); `_rt_imageLayerComposite_<id>_a/b` are the layer's own two buffers (on a 3D mesh, another layer's finished picture: formats.md, Meshes); any other `_rt_` name is a target sized by what its name says (`Half`, `Quarter`, `Eight`).
- Every uniform comes from render/uniforms.h: engine inputs (`g_Time`, `g_PointerPosition`, `g_ParallaxPosition`, matrices, `g_Texture<N>Resolution` = storage width and height, picture width and height, ...), then the material's constants (the pass's `constantshadervalues` overridden by the scene instance, user bindings resolved against the live properties), then the annotation default, else zero. The base pass receives brightness, alpha and the colour of the layer as `g_Brightness`, `g_UserAlpha` and `g_Color4` (VERSION 2); a colour tint with a shader that has no colour input is applied when the layer is composited.
- A shader that reads the clock or the audio spectrum makes the scene redraw every frame; one that reads the pointer or parallax, when the pointer moves.

## Cache

`$XDG_CACHE_HOME/murasaki/shader-cache/<key>.bin` (the folder of the plain SPIR-V cache). One file per (vertex source, fragment source, combo overrides, bound texture slots, translator version, glslang version), named by the SHA-256 of all of them, holds both stages' SPIR-V, the uniform layouts, samplers, attributes and the combos defined. A failure is stored too, with the compiler's message and the time: a broken shader costs one attempt, and the failure is believed for 24 hours (assets, compiler or engine may have changed). A cache hit does not start glslang. `kTranslatorVersion` must be bumped when the translation changes. `MURASAKI_SHADER_CACHE=0` turns it off.

Integrity: a file is used only if it passes validation. Both SPIR-V blobs need the magic number and a sane size, every uniform needs `offset + size <= blockSize` and a block of at most 16 KiB (the window of the dynamic descriptors), counts are bounded; anything else reads as absent and is rebuilt. The directory is trimmed to 256 MiB (oldest files first, every 64th store).

## Sandbox

glslang runs in a forked child with `RLIMIT_CPU` 5 s, `RLIMIT_AS` 2 GiB and a 15 s wall timeout (SIGKILL), and its output is capped (preprocessed text 4 MiB). A shader that cannot be compiled within those limits (for example a chain of nested `#define`s that doubles its size 22 times) fails with "the shader compiler exceeded its limits" and the failure is cached. This protects `--check` and the scene load alike, since both go through `preprocessGlsl` and `compileGlsl`.

## Matrices for pointer shaders

A layer's quad is a unit square here, while WE's effect quads are centred and measured in pixels of the layer's buffer. Shaders that unproject the pointer need the WE convention, so:

- `g_ModelViewProjectionMatrixInverse` maps NDC (y up) to buffer pixels from the layer's centre: `scaling(g_Texture0Resolution.xy) * translation(-0.5) * inverse(screenMVP)`; shaders divide by `g_Texture0Resolution.xy` and get -0.5..0.5.
- `g_EffectTextureProjectionMatrixInverse` is `scaling(2, 2) * translation(-0.5) * inverse(screenMVP)` (shaders halve the result), and `g_EffectTextureProjectionMatrix` its inverse.
- `screenMVP` is the layer's matrix on the screen (parallax included), also for effect passes drawn into the layer's buffers, whose own `g_ModelViewProjectionMatrix` is the identity. The vertex wrapper flips clip y once; the pointer is top-left based and the shaders flip it themselves.

Effect passes write into cleared targets without blending (straight alpha), the composite applies the base blending once. Effect targets are keyed by effect instance, name and size; a target replaced after a size change is retired a few frames later, never freed while a command buffer being recorded may still use it.

## Corpus result

`scene-vk-shadercheck` translates and compiles every (shader, combos, bound textures) the 72 library scenes use (base image materials and every effect pass, with the scene's per-instance overrides) and, separately, every shipped shader with default combos.

- **Scenes' shaders: 298 of 301 unique combinations compile (99.0 %).** The three failures are Workshop effects that are not valid GLSL (a non-constant global initialiser, an `int` target for a float uniform, a scalar assigned to a `vec4`); the dialect's own conversions were not enough and they are left to M2's refusal path (`shader compile: <name>`).
- **Every shipped shader, default combos: 45 of 59.** The rest are not scene shaders: the three shadow casters (`gl_ViewportIndex`; translated since piece 6 of phase 2: the output is dropped and the engine draws one viewport at a time, formats.md "Shadows"), the three `HLSL/` playlist shaders (no GLSL `main`), `puppettexturechannels` (an array sized by a combo the engine has to supply), `volumetricsfront` (`texSample2DBackBuffer`), `editorpaintbrush` and the lit shaders that need `LightingV1` (`generic3/4`, `foliage4`, `fur4`, `chroma4`: M5).

Causes seen while getting there, in order of how many shaders they stopped: the reserved word `sample`; `#require`; `pow` with a scalar or integer exponent on a vector; the effect trees not being on the search path; the Workshop shaders living in the item's package.

## Defines and conversions added in M4

- The translator defines `HLSL` as well as `GLSL`. The textures and targets here start at the top, like Direct3D's, and the shipped shaders flip the screen coordinates they read the framebuffer with under `#if HLSL` (refraction, copied backgrounds, the colour blend modes). Under `HLSL`, `common_perspective.h` defines its own `inverse(mat3)`, which GLSL rejects next to the built-in; the translator renames every `inverse` in such a shader and forwards the `mat2` and `mat4` sizes to the built-in. (Translator version 9.)
- `g_Texture<N>MipMapInfo`: the highest mip level of what is bound in slot N (levels - 1; 0 without a chain). A shader that reads it asks the renderer for a mip chain on the scene copy (`_rt_MipMappedFrameBuffer`); the translator maps it to `EngineInput::TextureMipMapInfo`.
- Uniforms added for the shaders of M4: `g_TexelSize`/`g_TexelSizeHalf`, `g_ModelMatrixInverse`, the orientation vectors, `g_RenderVar0/1`, and the audio spectrum arrays, which now carry the analyzer's bands.
- Bloom runs WE's own shaders (`downsample_quarter_bloom`, `downsample_eighth_blur_v`, `blur_h_bloom`, `combine`); `--check` refuses a scene with bloom when they are missing or do not compile.
- Colour blend modes use `genericimage3` with a `BLENDMODE` combo and the scene copy bound to slot 4; `--check` compiles that variant for every layer that uses one.

## The compatibility shim (M5)

Workshop shaders are written for WE's HLSL compiler, which is lenient where GLSL is not. A shader that does not compile strictly gets a second translation with a shim (`TranslateOptions::lenient`, set by `translateShader`; a strictly valid shader never sees it, and a shader that needed it is logged once as "compiled with the compatibility shim"):

- a global `const T x = expr;` whose expression reads a uniform, or a name moved before it, becomes `T x;` and the assignment runs first in `main()`;
- a float uniform annotated `"int": true` reads as an integer (`#define u int(u)`), for `for (int i = u_Min; ...)`;
- `texSample2D` and `texSample2DLod` take coordinates of any width (the extra components are dropped);
- `max` and `min` accept a scalar first operand against a vector;
- for each "'=' : cannot convert from A to B" the compiler reports, the right side of that assignment goes through `we_to_B` (truncating a vector to a float, widening a scalar to a vector, a float to an int); up to twelve rounds.

- the attributes HLSL puts in front of loops and branches (`[loop]`, `[unroll]`, `[unroll(4)]`, `[branch]`, `[flatten]`, ...) are dropped, and so is `const` or `inline` in front of a function's return type;
- tokens after a conditional directive's own are cut: a `;` closing an `#if` or `#elif` condition, more than a name after `#ifdef`, anything after `#else` and `#endif` (WE's preprocessor ignores them);
- for "left of '[' is not of type array, matrix, or vector", a chain of subscripts deeper than its name's declaration allows (a subscript of a float, or a second one of a float array) loses the extra ones, as HLSL reads a subscript of a scalar as the scalar;
- a vertex shader that writes to one of its attributes (`a_TexCoord.xy = ...;`, `a_Position += ...;`, `a_Position.x++`) gets the input under another name (`we_in_a_TexCoord`) and a global of the shader's name, copied from it at the top of `main()`; this is done in the main translation, not only by the shim, and the attribute keeps its name in the layout;
- for vectors of different widths in one operation, the longer operand keeps its leading components, whether it is a name, a parenthesised expression or a call.

All three shaders that kept 3184155462 away from this engine compile with it. The translator version is 10.

`#require LightingV1` is where WE writes `PerformLighting_V1`, the sum of the scene's lights at a point. `--check` refuses scenes with lights, so the translator writes the sum over none (a zero vector), strictly and leniently; a lit material (genericparticle with `LIGHTING`) then shows its ambient term, `g_LightAmbientColor`, which the engine fills from the scene's `ambientcolor` (and `g_LightSkylightColor` from `skylightcolor`).

The compiler runs once per text in a process: its answer, a failure included, is kept by a hash of the stage and the text, so a shader named by many layers, or retried by the shim, costs one run. glslang's built-in symbol tables are built in the engine's process before the first forked compiler starts, so every child inherits them; built in each child they cost about a second of CPU a shader, which made a cold `--check` of a scene with twenty shaders take minutes.
