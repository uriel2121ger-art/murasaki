# Puppets (draft)

Murasaki's own Live2D-style puppets: a desktop pet that is a parameter-driven 2D puppet (mesh, deformers, physics, expressions, motions) instead of an
animation file. Status: **draft; the spikes (section 1) are measured, the specification audit (sections 2 to 9) is written, the manifest check (sections 10 and 11) and the engine's pure parts (sections 12 to 16) exist as code and tests; nothing
is shown to the user yet.** Anima Engine does not render Live2D, so there is no Anima behaviour to match (checklist row 18).

Sections 2 to 9 are the specification audit (plan task 2): documents only, facts in our own words, every claim with the page it came from, read on 2026-10-22.
Sections 10 to 16 are written by the tasks that follow (names, the check, the Inochi2D payload, the clean-room record, motions, physics, the open back end).

## 1. Spikes (plan task 1, measurement only, no code kept)

Two questions, both about the picture of a puppet reaching a pet window.

* **(a) U5: can the overlay show frames of another process with real alpha under layer-shell, and what does it cost?**
* **(b) Can a GPU producer run inside the sandbox and hand dma-bufs to the consumer?**

### Method

A throwaway producer (`spike-host`, about 90 lines, not kept) drew premultiplied-alpha content into the bridge's buffers (`murasaki::bridge::Producer`,
EGL on the render node): a fully transparent frame, an opaque block that moves, and a half-transparent block (premultiplied 0.5, 0, 0, 0.5). A throwaway
window (`spike-view`, not kept) was built like the pet window (`PetWindow`: alpha buffer of 8 bits, transparent colour, a layer-shell surface in the overlay layer, the
OpenGL scene graph) and hosted the real `BridgeItem` of the Murasaki.Bridge module, with `fill: stretch`. Both ran in a private nested session
(`tools/bench/nested-session.sh`: private D-Bus, `kwin_wayland --virtual`, 1920x1080) in front of `murasaki-overlay-backdrop` (narrow colour stripes), so any
failure of blending shows against a known background. CPU and GPU are the per-process counters of `tools/bench/measure.py` (CPU = share of one core;
GPU = `drm-engine-gfx` of the process's DRM clients), over 10 s after 8 s of warm-up; Ryzen 7 9800X3D with its integrated Radeon, Mesa. One pet window at the
top-left corner, 1080 x 1080 px.

### Results

Alpha (U5). With the window over the stripes, the screenshot of the nested compositor gave, for the half-transparent block over a green stripe (48, 192, 64), the
colour (152, 96, 32), and over a yellow stripe (240, 208, 48) the colour (248, 104, 24). Premultiplied source over destination predicts (151, 96, 32) and
(247, 104, 24): **equal within one level of rounding**. The transparent parts showed the stripes untouched. So `BridgeItem` blends frames with alpha **provided
the producer writes premultiplied alpha** (straight alpha would be wrong); no change to `BridgeNode` was needed. Same result through dma-buf and through shared
memory, and with the producer behind the sandbox (below).

Cost of one 1080 px puppet picture (window process = Qt Quick + `BridgeItem`; host = the producer; each in % of one core, GPU in % of the engine's time):

| Transport | fps | window CPU | host CPU | window GPU | host GPU |
|---|---|---|---|---|---|
| dma-buf | 60 | 2.3 | 0.9 | 4.8 | 3.8 |
| dma-buf | 30 | 1.5 | 0.6 | 1.0 | 1.9 |
| dma-buf | 15 | 1.0 | 0.3 | 0.6 | 0.5 |
| dma-buf | 1 (idle) | 0.0 | 0.1 | 0.0 | 0.0 |
| dma-buf, 512 px | 60 | 2.3 | 0.8 | 0.8 | 1.8 |
| dma-buf, producer in the sandbox | 60 | 2.7 | 0.8 | 4.4 | 0.8 |
| shared memory (`MURASAKI_BRIDGE_FORCE_SHM=1`) | 60 | 9.8 | 6.5 | 2.9 | 9.8 |
| shared memory | 30 | 2.9 | 2.1 | 1.4 | 3.5 |

Resident memory: window about 141 MB (a Qt Quick process with the GL stack, shared pages counted), host about 76 MB (the EGL context; 87 MB with shared memory).
The GPU columns are single samples of a counter that moves in whole percent steps and has noise of several points (the 60 fps host reads 3.8 unsandboxed and
0.8 sandboxed for the same work): read them as "a few percent", not as a ranking. The compositor's own cost was not recorded (its process could not be told
apart reliably from the host session's in this run). The block drawn is trivial (clears with scissors), so the host's numbers are the cost of the **channel and
the buffers**, not of a rig; a real puppet adds its own draw cost (task 7 measures it with a real model).

### Sandbox (b)

`murasaki-sandbox --profile scene --item <empty dir> --require -- spike-host` (this machine reports `landlockAbi 8, tcpDeny, scopes, bwrap yes`): the producer
created its EGL context on the render node, **exported dma-bufs (`transport=Dmabuf`) and delivered 62 frames/s** to a real consumer (`bridge-view`, then the
nested window above). Nothing in the scene profile had to change. What was measured is an OpenGL (Mesa) producer; a Vulkan producer was **not** re-measured here,
but the existing `murasaki-scene-vk` is exactly that (Vulkan, behind the same profile, frames over the same bridge) and is the daily path of every scene, so no
new risk is expected; task 7 verifies it with the real engine.

### Decision

1. **The channel is the existing bridge** (`Producer` in `murasaki-puppet`, `BridgeItem` in the pet window). It shows alpha correctly, it costs a few percent of a
   core for a 1080 px pet at 60 fps and next to nothing at 15 fps or idle, and it runs behind the sandbox with the scene profile unchanged.
2. **Premultiplied alpha is the contract** of the puppet producer (to be written in the engine's bridge notes in task 7).
3. **Shared memory stays the fallback** (about four times the CPU, still single digits at 60 fps and 1080 px); no special code is needed, the bridge already falls back.
4. **No `QQuickItem` of our own that uploads textures** (the plan's fallback for U5) is needed. The plan's idea of a separate item is dropped.
5. Open for task 7: the pet window's input region from the hit mask (`alphamask`, `petinput`) was **not** exercised here, and the real pet window (`PetWindow` with
   `PetItem.qml`, flips, rotation, opacity layers) was replaced by a minimal window; the alpha result holds for the surface type, not for every item property.

## 2. Public sources read

Only documents were read. No source code of the Cubism SDK (Core, Framework, samples), of Inochi2D or of any other project was read to write this page, and none is copied here.

| Id | Document | Address | Used for |
|---|---|---|---|
| S1 | Cubism Specs, file format specifications (`FileFormats/*.md`, JSON-schema based) | https://github.com/Live2D/CubismSpecs (raw files under https://raw.githubusercontent.com/Live2D/CubismSpecs/master/FileFormats/) | section 3 |
| S2 | Cubism Editor manual, Standard Parameter List (page dated 2021-08-26) | https://docs.live2d.com/cubism-editor-manual/standard-parameter-list/ | section 4 |
| S3 | Cubism SDK manual, Cubism Core; Cubism Core API Reference r15 (2025-10-30) | https://docs.live2d.com/en/cubism-sdk-manual/cubism-core/ , https://docs.live2d.com/en/cubism-sdk-manual/cubism-core-api-reference/ | section 5 |
| S4 | Live2D Proprietary Software License Agreement 2.1 (2025-02-03); Open Software License Agreement 1.4; Free Material License Agreement 1.6 | https://www.live2d.com/eula/live2d-proprietary-software-license-agreement_en.html , https://www.live2d.com/eula/live2d-open-software-license-agreement_en.html , https://www.live2d.com/eula/live2d-free-material-license-agreement_en.html | section 7 |
| S5 | SDK Release License page and Expandable Application page | https://www.live2d.com/en/sdk/license/ , https://www.live2d.com/en/sdk/license/expandable/ | section 7 |
| S6 | Terms of Use for Cubism sample data | https://www.live2d.com/en/learn/sample/model-terms/ | section 7 |
| S7 | Inochi2D specification (docs repository): INP container, profiles, blend modes | https://github.com/Inochi2D/inochi2d-docs (`docs/source/spec/inp/index.rst`, `inp/profiles.rst`, `payload/index.rst`, `payload/blending.rst`) | section 6 |
| S8 | Inochi2D technical documents of the SDK repository (INP2) | https://github.com/Inochi2D/inochi2d/tree/main/tech-docs (`inp2.md`, `inp2-puppet.md`, `building-a-renderer.md`, `extending-inochi2d.md`) | section 6 |
| S9 | Licence files of the Inochi2D repositories | https://raw.githubusercontent.com/Inochi2D/inochi2d/main/LICENSE , https://raw.githubusercontent.com/Inochi2D/inochi2d-docs/main/LICENSE | section 7 |

Corrections to the plan's section 2 that this audit makes: **L10 is wrong as written** (Inochi2D's public documents specify the container and the blend modes, **not** the
puppet's nodes, parameters, bindings or physics; section 6); L6's "small C interface" is true of Core (S3 says its API is written in C) but the **header's file name was not
found in any page read**; no page read says the `.moc3` layout is public or that it is not; S3 only describes `.moc3` as the model format Core handles.

## 3. Cubism JSON formats (S1): what is specified and what is not

S1 gives one specification per file, as prose plus a JSON schema. **None of the files has a version history**: `Version` is a number, the schemas give no range, the examples
use 3 (and 1 for motion sync), `pose3` and `exp3` say 3 is assumed when it is missing, and S1 says nothing about Cubism 4 or 5 or about which editor writes which version.
S1 carries **no licence file** (the repository page shows none), so nothing here is copied from it: the tables below are facts restated.

Notes on S1 itself that matter to a reader: the schemas are checked with a Python 2.7 script; the physics schema has a misspelled keyword that makes its item block
unchecked; the motion-sync schema has keys outside `properties` and trailing commas; the Expressions description in model3 says "path" but the type is a list of
objects; the motion `Fps` is described in seconds although it is a rate. A reader must therefore **not rely on S1's schemas as a validator**, only as a description.

### 3.1 `*.model3.json` (the manifest)

| Specified | Not specified |
|---|---|
| Top level: `Version` (number), `FileReferences` (object): both required; `Groups`, `HitAreas`, `Layout` optional | Which `Version` values exist; what a newer version adds |
| `FileReferences`: `Moc` (string) and `Textures` (list of strings) required; `Physics`, `UserData`, `Pose`, `DisplayInfo`, `MotionSync` strings, `Expressions` list of `{Name, File}`, `Motions` object from a group name to a list; paths are relative to the manifest | Path syntax (separator, case, `..`, absolute): not said, so ours must be strict (section 8) |
| A motion entry: `File` required; `FadeInTime`, `FadeOutTime` (seconds), `Sound`, `MotionSync` optional | How a fade given here combines with the one inside the motion file; what the sound is for; which names the groups must have |
| `Groups`: items `{Target, Name, Ids}`; target `Parameter`; the names `EyeBlink` and `LipSync` are the documented ones | Whether other targets exist |
| `HitAreas`: items `{Name, Id}` | What `Id` refers to (the plan reads it as a drawable); how a hit area maps to a motion group |
| `Layout`: ten optional numbers (`width`, `height`, `x`, `y`, `center_x`, `center_y`, `top`, `bottom`, `left`, `right`) | Their meaning and units |
| The schema forbids unknown keys | Real models carry extras; our reader ignores unknown keys but checks the types of the known ones (section 8) |

### 3.2 `*.motion3.json`

| Specified | Not specified |
|---|---|
| `Version`, `Meta`, `Curves` required; `UserData` optional (items `{Time, Value}`) | |
| `Meta`: `Duration` (s), `Fps`, `CurveCount`, `TotalSegmentCount`, `TotalPointCount` required; `Loop`, `AreBeziersRestricted`, `FadeInTime`, `FadeOutTime`, `UserDataCount`, `TotalUserDataSize` optional | Defaults of the optional fields; what `AreBeziersRestricted` changes; how looping closes the seam |
| A curve: `Target` (`Model`, `Parameter` or `PartOpacity`), `Id`, `Segments` (one flat list of numbers), optional per-curve `FadeInTime`, `FadeOutTime` | The `Model` ids are `Opacity`, `EyeBlink`, `LipSync`; what each does to the model is not defined here |
| A curve starts with one point `[time, value]`; each segment is a kind code followed by its points. Codes: 0 linear, 1 cubic Bezier, 2 stepped, 3 inverse stepped. Linear and the two steps add one point, Bezier adds three. A Bezier's two inner control points have times fixed at one third and two thirds of the segment's span | How to solve a Bezier for a time (the parameter is not the time), the value before the first and after the last point, **where inside the segment a step happens** (stepped vs inverse stepped) |
| The counts in `Meta` describe the file; "curves cannot be empty" | No maximum of anything; the schema does not check that the counts match |
| Fades are "easing times" in seconds | The easing curve, and how the motion's, the curve's and the model manifest's fades combine |

### 3.3 `*.physics3.json`

| Specified | Not specified |
|---|---|
| `Version`, `Meta`, `PhysicsSettings` required | Everything about **how a rig is integrated**: no equation, no time step, no order of evaluation |
| `Meta`: counts (`PhysicsSettingCount`, `TotalInputCount`, `TotalOutputCount`, `VertexCount`), `EffectiveForces` with `Gravity` and `Wind` (two numbers each), `PhysicsDictionary` (id and name pairs), optional `Fps` | Default of `Fps`; units of the forces |
| A setting: `Id`, `Input` list, `Output` list, `Vertices` list (the pendulum chain), `Normalization` with `Position` and `Angle` ranges (`Minimum`, `Default`, `Maximum`) | How an input is normalised into the chain; what `Default` is the centre of |
| Input: `Source` (a parameter reference), `Weight`, `Type` (`X` or `Angle`), `Reflect`. Output: `Destination`, `VertexIndex` (0 based), `Scale`, `Weight`, `Type`, `Reflect` | What `Reflect` does; what `Scale` multiplies; whether a weight is 0 to 1 or 0 to 100 (the text says percent, the examples use 1, 50 and 100); `Angle` "might be fixed"; an out-of-range `VertexIndex` |
| Vertex: `Position` (x, y), `Mobility`, `Delay`, `Acceleration`, `Radius` | Units and the roles of these numbers in any formula |

This is the file where the plan's "tuned against the *described* behaviour" is hardest: **the format document alone does not determine the motion**. The only oracle would be
reference renders by the model's author or by Live2D's own player; neither exists here. Task 5 must therefore say in its test documents which behaviours are specified
(ranges, weights bounds, clamping to the parameter range) and which are our design choice (the pendulum model itself).

### 3.4 `*.pose3.json`, `*.exp3.json`, `*.cdi3.json`, `*.userdata3.json`

| File | Specified | Not specified |
|---|---|---|
| pose3 | `Type` = `Live2D Pose`, optional `Version`, `FadeInTime`, `Groups`: a list of groups, each a list of nodes `{Id, Link}`; in a group only one node is shown; a node drives the opacity of a part (and of its linked parts) | How a switch is chosen; the fade curve; what happens when the shown part's opacity is also animated |
| exp3 | `Type` = `Live2D Expression`, optional `Version`, `FadeInTime`, `FadeOutTime` (not below 0), `Parameters`: `{Id, Value, Blend}` with `Blend` one of `Add`, `Multiply`, `Overwrite`; the page's table gives for `Add` the stored value as setting minus the default | The default blend when omitted; the fade defaults; the order and priority of several expressions; the table's formulas for `Multiply` and `Overwrite` are the same, so how each is applied is only in the prose |
| cdi3 | `Parameters` (`Id`, `GroupId`, `Name`), `ParameterGroups` (same fields; `GroupId` is the parent), `Parts` (`Id`, `Name`), optional `CombinedParameters` (lists of exactly two ids); list order is meaningful | What a combined pair is; localisation of names |
| userdata3 | `Meta` counts and `UserData` items `{Target, Id, Value}`; the one target is `ArtMesh` | How the byte size is counted; not needed by us (ignored) |

`motionsync3.json` (audio-driven mouth shapes) is specified loosely and is **not used** by this feature (plan question 12).

## 4. Standard parameter ids (S2)

The page's rule is "eyes and mouth: closed is 0, open is 1" and the values may exceed the nominal range. The ids the engine drives, with the page's range as minimum, default,
maximum: `ParamAngleX`, `ParamAngleY`, `ParamAngleZ` (head, -30 / 0 / 30; plus is to the screen's right, up, and a tilt to the right); `ParamEyeLOpen`, `ParamEyeROpen`
(0 / 1 / 1; can be pushed to 1.5 or 2 by the author); `ParamEyeLSmile`, `ParamEyeRSmile` (0 / 0 / 1); `ParamEyeBallX`, `ParamEyeBallY` (-1 / 0 / 1);
`ParamMouthOpenY` (0 / 0 / 1); `ParamMouthForm` (-1 / 0 / 1); `ParamBodyAngleX`, `ParamBodyAngleY`, `ParamBodyAngleZ` (-10 / 0 / 10); `ParamBreath` (0 / 0 / 1);
`ParamHairFront`, `ParamHairSide`, `ParamHairBack` (-1 / 0 / 1, normally driven by physics). The brows (`ParamBrowLY`, `ParamBrowRY`, `ParamBrowLX`, `ParamBrowRX`,
`ParamBrowLAngle`, `ParamBrowRAngle`, `ParamBrowLForm`, `ParamBrowRForm`, all -1 / 0 / 1), `ParamCheek` (0 / per character / 1) and thirteen "add if needed" parameters (arms,
hands, bust, shoulders, base) also exist. **The page is advice to authors, not a guarantee about a given model**: a model may lack any of them or use others, so the engine
looks them up by id, drives the ones that exist and says nothing about the ones that do not. The eye-blink and lip-sync parameter lists of a model are its `Groups`
(section 3.1), which win over the standard ids.

## 5. `.moc3` and Cubism Core (S3, S4)

* S3 describes Core as a library of a C API for models made by Cubism 3 or later editors, with a function for the `.moc3` file version and a consistency check of a `.moc3`
  (added in r11). **No page read publishes the `.moc3` byte layout**, and none says it is secret; the plan's note (be75831: do not assume a public description) stands.
* Core is delivered inside the SDK packages on Live2D's site after the visitor accepts the licence agreements; the Core page says that, under the agreement, Core is not
  published on GitHub. A public download address (an often-quoted CDN path) and the name of the C header **could not be verified from the pages read**; task 9 must obtain
  them from the user's own SDK package, not from this document.
* Our own prototypes of the few functions we would call (plan 3.1) need the function names from S3's API reference (a PDF, `NativeCoreAPIReference_en_r15.pdf`, not read
  in this audit). That reading belongs to task 9, after the user confirms question 1's last part.

## 6. Inochi2D (S7, S8): what is public

| Part | Published | Not published |
|---|---|---|
| **INP v1 container** (S7) | Big-endian. Magic `TRNSRTS` and a zero byte (8 bytes); a 4-byte length; that many bytes of JSON (the model and rigging data); the texture section (an 8-byte `TEX_SECT` header, a 4-byte count, then blobs of a 4-byte length, a 1-byte encoding and data; encodings 0 PNG, 1 TGA, 2 BC7); an optional vendor section (`EXT_SECT`, a count, blobs of name length, name, length, payload). The page warns it will change before 1.0 | The JSON encoding details; the texture section has no total length; the byte layout of BC7 and of the blobs |
| **INP v2 container** ("Inochi2D 0.9", S8) | Little-endian, 32-bit aligned, a stream of tagged values (null, bool, int32, uint32, float32, UTF-8 string, blob; keys, arrays, objects with counts; blobs end with a CRC-32). Magic `TRNSRTS2`. Root object keys `INP_SECT` (the puppet, "encoded entirely in the INP format"), `TEX_SECT` (list of `{format, data}`, same texture encodings), optional `EXT_SECT` (reverse-domain names to blobs) | **The keys inside `INP_SECT`** |
| **Blend modes** (S7) | Thirteen core modes as Porter-Duff factor tables (normal, multiply, screen, lighten, color dodge, linear dodge, add, subtract, exclusion, inverse, destination in, clip to lower, slice from lower) and an extension with seven more (overlay, darken, color burn, hard light, soft light, difference, exclusion) | The exact colour formulas of the extended ones |
| **Draw list for a renderer** (S8, "preliminary") | Draw states (normal, define mask, masked draw, composite begin, end, blit); node type ids (node, visual, part, animated part, composite, solo, mask; deformers: mesh and lattice; bone; bone modifier; simple physics, deprecated) | What a node's fields are ("the `vars` layout is in the node documentation", which is not in the documents) |
| **Puppet content** (nodes, transforms, mesh data, parameters, bindings, interpolation, physics, masks as data, animations, the editor's INX) | **Nothing**. The payload page of S7 is an introduction that lists only the blend page; S8's puppet page defines only the three section keys | Everything |

**Audit conclusion for the open format.** The plan's option C assumed a reader "written from the specification and tested against the specification's own examples". For
Inochi2D that is **not possible from the public documents**: the container and the blend modes are specified, the part that matters (what a puppet *is*) is not. The ways to
get it are (1) read the BSD-2-Clause reference implementation or inspect real puppet files, which this run's clean-room rule excludes (facts from public specifications only),
and which also leaves a reader without a published oracle; or (2) specify the format ourselves.

**Decision for task 6 (2026-10-22): Inochi2D stays the open format** (question 2 keeps its default). The clean-room rule of this project covers Wallpaper Engine, linux-wallpaper-
engine, Open Wallpaper Engine and Anima Engine only; the Inochi2D reference implementation is under the BSD 2-Clause licence (section 7), so its source may be **read as the format's
de facto specification**. Section 12 restates what that reading established, in our own words; the reader and evaluator of task 6 are written from section 12, not from the source. The
version targeted is **INP v1, the 0.8 line (tag v0.8.7)**: its payload is JSON, which is what editors of that line write and what most published puppets are; the 0.9 line's INP v2 puts the
same kind of data in a binary tagged container whose keys are not documented anywhere read (a later converter at most).

The manifest of task 3 recognises an `*.inp` by its size and signature only (both container versions); task 6 reads version 1 and refuses version 2 as `puppet: unsupported format`.

## 7. Licence facts, as understood from the public pages (not legal advice)

Everything here is how the pages read to an engineer; the user or counsel decides what it means. Pages are translations of Japanese originals which prevail.

* **Cubism Core (S4, S3).** Under the Proprietary Software License Agreement: redistribution is limited to files the agreement lists as redistributable and only inside works
  given to end users with flow-down terms; modifying, porting or translating the software is not allowed, nor is deriving its source code; releasing code or content of the software under a licence not from Live2D, or combining it so that a part falls under a source-disclosing or modifiable
  licence, is restricted. Nothing read says that using a user-downloaded Core from an open-source application is allowed or forbidden. Core is obtained by accepting the
  agreements on Live2D's download pages. **Murasaki's plan is unaffected as long as it ships no Core, header or sample; whether a GPL program that loads a user-installed Core
  is acceptable to Live2D is a question the pages do not answer.**
* **Publication licence (S5).** Businesses publishing content with the SDK need one; individuals and small-scale enterprises (annual sales under 10 million yen) are exempt from
  the licence and fee, except for **Expandable Applications**. The definition of an Expandable Application (S5) includes works that use an open-ended number of models added as
  files or data (the page's examples: avatars, live-streaming applications, video generators) and says every such application must be reviewed and sign a special
  agreement before release, whatever the publisher's size, and that "fully free of charge" is, as a general rule, not eligible for approval. A separate notice for tracking
  software that reads Live2D models mentions a 20 million yen threshold for a business that uses it as a primary element. **A desktop pet that loads any model the user adds
  could fall under that definition if Murasaki were itself a Cubism-SDK-based application it publishes; the pages do not say how they apply to a program that only loads a
  library the user installed.** This is the largest open licence question of option A (section 9, Q-L1).
* **Model files (S4, S6).** A model's art belongs to its author; Live2D's sample models each have their own terms (some forbid alteration or commercial use, some follow third
  parties' terms). Nothing of any model is bundled by Murasaki; the fixtures of this feature are written by tests.
* **Specification repository (S1).** No licence is shown on the Cubism Specs repository page. We restate facts and copy no text.
* **Inochi2D (S9, S7, S8).** The reference library repository carries the BSD 2-Clause licence (checked in its LICENSE file; copyright "Inochi2D Project" and "Kitsunebi Games").
  The documentation repository's README says its text is released under a Creative Commons Attribution (3.0) licence, with the project's branding assets excluded; the
  licence variant in its LICENSE file was not opened. No licence header was visible in the four technical documents of the SDK repository. Murasaki copies no text or code of
  either.

## 8. Limits of ours for untrusted model folders

The formats state **no limits at all** (section 3), so every bound is ours. They are constants of `src/core/puppetlimits.h` (task 3), the one table; a test keeps this table and
the header equal. Every reader checks a count or a size against its limit **before** allocating, and a value over a limit is refused, never clamped. "Item" is the model
folder; sizes are bytes unless a unit is given.

| Name | Value | Meaning |
|---|---|---|
| `kMaxManifestBytes` | 1048576 | the manifest (`*.model3.json`) |
| `kMaxJsonDepth` | 16 | nesting of arrays and objects in any JSON file |
| `kMaxJsonValues` | 65536 | values (including keys' values) in one JSON file |
| `kMaxJsonStringBytes` | 4096 | one string, after unescaping |
| `kMaxIdBytes` | 128 | a parameter, part, group, hit-area or motion-group id or name |
| `kMaxPathBytes` | 1024 | a path named by a manifest |
| `kMaxPathComponentBytes` | 255 | one component of such a path |
| `kMaxItemFiles` | 1024 | entries (files and folders) in the item, all levels |
| `kMaxItemDirDepth` | 8 | folders below the item folder |
| `kMaxItemBytes` | 1073741824 | the sum of the sizes of the files of the item |
| `kMaxMocBytes` | 67108864 | the `.moc3` file |
| `kMaxInpBytes` | 268435456 | an Inochi2D `.inp` container |
| `kMaxPuppetJsonValues` | 1572864 | values (numbers and lists included) in the JSON payload of an `.inp` |
| `kMaxWarpCandidates` | 256 | triangles tried for one point of a mesh group |
| `kMaxWarpCellEntries` | 1048576 | grid entries of all the mesh groups of a puppet together |
| `kMaxWarpTriangles` | 16384 | distinct triangles of one mesh group (repeated and flat ones do not count) |
| `kMaxCoordinate` | 10000000 | the largest magnitude of a coordinate, offset, scale, zsort, binding value, parameter bound or physics number in a puppet or a physics file |
| `kMaxTotalTextureBytes` | 134217728 | the encoded textures of one `.inp` together |
| `kMaxInpJsonBytes` | 16777216 | the JSON payload of an Inochi2D `.inp` |
| `kMaxTextures` | 16 | texture files of one model |
| `kMaxTextureFileBytes` | 33554432 | one texture file |
| `kMaxTextureSide` | 8192 | width or height of a texture, pixels |
| `kMaxTexturePixels` | 33554432 | pixels of one texture |
| `kMaxTotalTexturePixels` | 67108864 | pixels of all the textures of a model |
| `kMaxMotionGroups` | 64 | groups named in `Motions` |
| `kMaxMotionsPerGroup` | 64 | motions in one group |
| `kMaxMotions` | 512 | motions in all groups |
| `kMaxFadingMotions` | 16 | motions fading out beside the current one |
| `kMaxMotionSeconds` | 3600 | the duration of a motion, and the time of any of its points, seconds |
| `kMaxMotionQueue` | 8 | motions waiting behind the one playing |
| `kMaxFadeSeconds` | 3600 | a fade time named by the manifest, seconds |
| `kMaxExpressions` | 128 | expressions |
| `kMaxHitAreas` | 128 | hit areas |
| `kMaxParameterGroups` | 16 | entries of `Groups` |
| `kMaxIdsPerGroup` | 64 | ids in one group |
| `kMaxMotionFileBytes` | 4194304 | one motion file |
| `kMaxMotionJsonValues` | 1048576 | values (numbers included) in one motion file |
| `kMaxExpressionFileBytes` | 262144 | one expression file |
| `kMaxPhysicsFileBytes` | 1048576 | the physics file |
| `kMaxPoseFileBytes` | 65536 | the pose file |
| `kMaxDisplayInfoFileBytes` | 1048576 | the display-info file |
| `kMaxUserDataFileBytes` | 1048576 | the user-data file (read by no one yet; sized for the check) |
| `kMaxMotionSyncFileBytes` | 1048576 | the motion-sync file |
| `kMaxParameters` | 512 | parameters of a model |
| `kMaxParts` | 512 | parts of a model |
| `kMaxDrawables` | 2048 | drawables (art meshes) of a model |
| `kMaxVerticesPerMesh` | 32768 | vertices of one mesh |
| `kMaxVerticesTotal` | 524288 | vertices of all meshes |
| `kMaxIndicesPerMesh` | 196608 | triangle indices of one mesh |
| `kMaxIndicesTotal` | 1572864 | triangle indices of all meshes |
| `kMaxDeformerDepth` | 64 | depth of the chain of deformer parents |
| `kMaxMasksPerPart` | 16 | masks of one part |
| `kMaxAxisPoints` | 64 | keypoints on one axis of a parameter |
| `kMaxBindings` | 16384 | parameter bindings of an Inochi2D puppet |
| `kMaxNodes` | 4096 | nodes of an Inochi2D puppet |
| `kMaxCurvesPerMotion` | 1024 | curves of one motion |
| `kMaxSegmentsPerMotion` | 262144 | segments of one motion |
| `kMaxPointsPerMotion` | 1048576 | points of one motion |
| `kMaxPhysicsRigs` | 128 | physics settings |
| `kMaxPhysicsInputs` | 1024 | inputs, all rigs |
| `kMaxPhysicsOutputs` | 1024 | outputs, all rigs |
| `kMaxPhysicsVertices` | 2048 | pendulum vertices, all rigs |
| `kMaxPhysicsStepsPerUpdate` | 8 | fixed steps one update simulates at most |
| `kMaxPhysicsIoPerRig` | 32 | inputs, and outputs, of one physics setting |
| `kMaxPhysicsVerticesPerRig` | 32 | pendulum vertices of one physics setting |
| `kMaxPoseGroups` | 64 | groups of the pose file |
| `kMaxPoseNodes` | 512 | nodes of the pose file |
| `kMaxPuppetPixels` | 2048 | the long side of a frame the engine renders, pixels (plan 4.2) |
| `kMinPuppetMaxPixels` | 1000000 | the lowest `puppetMaxPixels` setting |
| `kMaxPuppetMaxPixels` | 16000000 | the highest `puppetMaxPixels` setting |
| `kDefaultPuppetMaxPixels` | 4194304 | the default `puppetMaxPixels` |
| `kMaxTotalAssetBytes` | 33554432 | the motion, expression, physics and pose files an item names, read together by loadSessionAssets (a file named twice is read once); what is over is left out and named |
| `kSlowRecoveryFrames` | 300 | fast frames in a row before the engine doubles its own reduced frame rate again |
| `kStencilClearShare` | 16 | a stencil clear costs 1/this of a picture in the estimate (a fast clear, not a drawn picture) |
| `kMaxFrameCostWork` | 4194304 | triangles the cost estimate of one frame may look at; the items left are each counted as a whole picture |
| `kMinSlowFps` | 5 | the lowest frame rate the engine falls back to on its own when its GPU work is slow |
| `kMaxOutputCoordinate` | 1000000000000 | the largest magnitude of a vertex position the model hands the renderer (model units); a computed position beyond it, unlike a number read from a file, is clamped to it, so no float conversion downstream is out of range |
| `kCrashForgetHours` | 24 | a crash this long after the previous one starts the count again |
| `kEngineLongRunSeconds` | 1800 | an engine that ran this long (30 minutes) before it crashed was not broken by its model: the count starts again at one. A model that survives a minute and dies is still the model's |
| `kMaxPositionVertices` | 8388608 | vertex positions one frame may write for meshes and mask sources together |
| `kMaxHitMaskWork` | 4194304 | work units (about 10 ns each) one hit mask may cost, some 40 ms; the rest of the triangles of an item are covered by the one box that holds them |
| `kMaxSlowFrames` | 3 | slow frames in a row before the engine halves its own frame rate (to a floor of kMinSlowFps); it never gives the model up for being slow |
| `kMaxSlowFrameMs` | 250 | a frame whose GPU work takes longer than this (milliseconds) counts as slow |
| `kMaxHeavyFrames` | 30 | frames in a row over kMaxOverdrawFactor before the engine gives up on the model (puppet: too heavy) |
| `kMaxOverdrawFactor` | 64 | the area the triangles of one frame cover (an upper bound worked out from their boxes), mask sources drawn again and group images included, as a multiple of the picture's area; a frame over it is not drawn |

## 9. Conformance questions left open

Q-F1 to Q-F8 are about format meaning, Q-L about licences; none blocks task 3.

* Q-F1 (motion): the value of a Bezier curve for a time, and the value outside the first and last point. Plan: solve the Bezier parameter numerically from the fixed control-point
  times and hold the end values; marked "ours" in task 4.
* Q-F2 (motion): where a stepped and an inverse-stepped segment changes. Plan: stepped holds the segment's start value until its end point, inverse stepped jumps to the end
  value at once; marked "ours" and to be checked against a reference render when one exists.
* Q-F3 (motion): combination of fades (manifest, motion, curve). Plan: the product of the motion-level fade weight and the curve-level one, a smooth ease; "ours".
* Q-F4 (expression): the default blend (plan: `Add`) and the order of several expressions (plan: the order they were started, later on top).
* Q-F5 (physics): the whole integration (section 3.3); "ours", documented with its own equations in task 5, and tested for the properties the format does state.
* Q-F6 (physics): the weight scale, 0 to 1 or 0 to 100. Plan: read the number as a percentage when the file's own totals are at most 100 and say so in the check.
* Q-F7 (manifest): what a hit area's `Id` names and which motion group a tap plays. Plan: a drawable id; the group whose name equals the hit area's name, as the plan 4.6 says.
* Q-F8 (pose): the fade and the switching rule. Plan: the first node of the group whose part the model shows at start; "ours".
* Q-L1 (licence): whether a program that loads a user-installed Core, and lets the user add any number of models, counts as an Expandable Application for Live2D (section 7). To be
  answered by the user or counsel before task 9; until then option A stays optional and off.
* Q-L2 (licence): the exact licence of the Cubism Specs text (none shown) and of the Inochi2D documents (a CC attribution licence for the docs repository, not opened in full).
* Q-I1 (Inochi2D): settled on 2026-10-22: the 0.8.7 reference implementation is read as the specification (section 12).
* Q-I3 (Inochi2D): the length field of a texture blob is read as the length of the image bytes alone (the type byte is not counted); the spec text says only "a 4-byte payload length, a 1-byte encoding, the data". Nothing to check without a real file.
* Q-I2 (Inochi2D): whether puppets exist whose stored binding matrices differ from what the editor's fill of unauthored points would give (section 12.9). Nothing to check without real files.

## 10. Names

One table in `src/core/puppetnames.h`, listed here (a test keeps them equal). An entry printed by the check or shown in `Windows()` is a name below, alone or followed by `: ` and what it is
about (a file, a limit name); a caller matches the part before the first `: ` after the prefix. **Refused**: a reason of the check, the item cannot be shown. **Degraded**: the
check accepts the item with a gap. **Runtime**: a degrade of the overlay's `Windows()` while the engine runs (plan 4.7). Five names (`not a puppet`, `unsafe path`, `manifest invalid`, `unsupported manifest version`, `unsupported format`) are additions of task 3 to the plan's list of 4.8, which had no name for a folder that
is not a puppet or for a path rule broken.

| Name | Kind | Meaning |
|---|---|---|
| `puppet: not a puppet` | refused | the folder holds no model3.json and no .inp file, or holds both, or several of one |
| `puppet: unsafe path` | refused | the folder holds a symbolic link or a file that is not a regular file, or a path the manifest names is absolute, leaves the folder or breaks the path rules |
| `puppet: file missing` | refused | a file the manifest names is not in the folder |
| `puppet: too large` | refused | a count or a size is over a limit of the limits table (the entry says which) |
| `puppet: manifest invalid` | refused | the manifest is not strict JSON, repeats a key, nests too deep, lacks a required key or has a value of the wrong type |
| `puppet: unsupported manifest version` | refused | the manifest's Version is not one Murasaki reads (3) |
| `puppet: unsupported format` | refused | a named file has an extension its role does not allow, or a container does not start with a known signature |
| `puppet: unsupported moc version` | refused | the Cubism runtime reports a .moc3 version it does not support (reserved: the check cannot tell before the runtime is loaded) |
| `puppet: payload not read` | degraded | the check looked at the container's signature and size only (loadManifest); the puppet inside was not read, so it is not known to be drawable (checkItem reads it) |
| `puppet: needs the Cubism runtime` | degraded | the model is a Cubism model, which draws only through a Core library the user installs (Murasaki ships none); without it the item cannot be shown |
| `puppet: core unavailable` | runtime | the Cubism runtime could not be loaded: no path set, a path that fails the checks, or a library that does not load |
| `puppet: physics rig skipped` | runtime | a physics rig whose input or output parameter the model does not have is not simulated |
| `puppet: motion skipped` | runtime | a motion that cannot be read or names nothing the model has is not played |
| `puppet: resolution reduced` | runtime | the picture is drawn smaller than the pet's size asks for, because of the pixel limit |
| `puppet: engine restarted` | runtime | the puppet engine stopped and was started again |
| `puppet: blocked after crashes` | runtime | the model made the engine crash three times and is not started any more |
| `puppet: no GPU` | runtime | no usable Vulkan device for the engine: puppets are not shown |

## 11. `murasaki-puppet --check FOLDER`

Reads the manifest of a puppet item and prints one line of JSON (exit status 0 whenever a verdict was printed, 2 for a usage error). It looks at **names and sizes** only: the folder
is walked without following links, the manifest is parsed by a strict reader (RFC 8259 text, valid UTF-8, no repeated key, bounded depth, values and string length), every file
it names must pass the path rules, exist, have the extension of its role and be within its size limit. It decodes no texture and reads no `.moc3`, motion, expression or physics file
(those come with tasks 4 to 6 and 9). The skeleton runs without a sandbox; the daemon will run it behind the `puppet` profile (task 8).

```
{"ok":true,"format":"cubism","modelVersion":3,"parameters":null,"drawables":null,"textures":2,"motions":3,"expressions":4,"physics":null,"canvas":null,"reasons":[],"degraded":["puppet: needs the Cubism runtime"],"engine":"murasaki-puppet","version":"0.1.0"}
{"ok":false,"format":"cubism","modelVersion":3,"parameters":null,"drawables":null,"textures":null,"motions":null,"expressions":null,"physics":null,"canvas":null,"reasons":["puppet: file missing: t0.png"],"degraded":[],"engine":"murasaki-puppet","version":"0.1.0"}
```

* `format`: `cubism` (one `<stem>.model3.json`), `open` (one `<stem>.inp`) or `null`. A folder with both, or with more than one of either, is `puppet: not a puppet`.
* `canvas`: `[width, height]` in model units (rounded, at least 1) of an Inochi2D puppet drawn once at rest, which the daemon uses for the pet's aspect ratio; `null` for a Cubism model (its canvas is in the moc) or when not known.
* `modelVersion`: the manifest's `Version` (only 3 is read) or the container's version (1 or 2, from its signature). `null` until known.
* `textures`, `motions`, `expressions`: counts of what the Cubism manifest names, `null` unless `ok`. `parameters`, `drawables` and `physics` are always `null` here: they need the files the manifest only names.
* `degraded` has `puppet: needs the Cubism runtime` for every Cubism item: it can be drawn only through a Core library the user installs (plan 3.1).
* A detail is cut to 120 bytes and cleaned to valid UTF-8 (a hostile folder cannot make the line invalid JSON). At most 16 reasons are listed.

What the manifest reader accepts beyond the specification (section 3.1): unknown keys are ignored but the types of the known ones are checked; one leading byte order mark; `Version` 3 only
(`3.0` is 3); extensions of the roles are matched without regard to case (`.PNG`), file names exactly; a fade is a number from 0 to `kMaxFadeSeconds`; ids and names are at most `kMaxIdBytes`.
`loadManifest` checks an `.inp` for size and signature only (`TRNSRTS` + a zero byte, with a big-endian JSON length that must fit the file, is version 1; `TRNSRTS2` is version 2). `murasaki-puppet --check` goes further for an `.inp`
(`checkItem`, task 6): it reads and evaluates the whole puppet once at rest with the open back end, so a puppet that cannot be drawn is refused (`puppet: manifest invalid`, `puppet: too large` for a limit, `puppet: unsupported format` for version 2 or BC7) and
`parameters`, `drawables` (the Part nodes), `textures` and `physics` (SimplePhysics nodes) are counted.

## 12. The Inochi2D puppet, as read from the reference implementation (INP v1, tag v0.8.7)

**Provenance.** Repository https://github.com/Inochi2D/inochi2d at the tag `v0.8.7`, commit `e2235f6f29688d4013a0eec116a37ec8af8f1fb5` (a lightweight tag; the head of `main` on the day
of reading was `4975d247f9b946a74d18e0ba9b3e9475eb636efb`, 2026-10-03, which is the 0.9 line). **Licence: BSD 2-Clause**, from the `LICENSE` file of the tag
(https://raw.githubusercontent.com/Inochi2D/inochi2d/v0.8.7/LICENSE: "Copyright (c) 2020, Inochi2D Project"; the file on `main` adds "2020-2026, Inochi2D Project" and "Kitsunebi Games"); the
math library it depends on, `inmath` 1.3.x (https://github.com/Inochi2D/inmath, tag v1.3.2), is MIT-licensed (Copyright David Herberth 2011-2021, Inochi2D Project 2021-2024). **Credit: the
puppet format, its names and its behaviour are Inochi2D's work (Inochi2D Project and Kitsunebi Games); Murasaki contains no line of their code.** The source files read, all under `source/inochi2d/` of
the tag: `core/puppet.d`, `core/nodes/{package,common,defstack,drawable}.d`, `core/nodes/part/{package,apart}.d`, `core/nodes/composite/package.d`, `core/nodes/meshgroup/package.d`,
`core/nodes/shapes/package.d`, `core/nodes/drivers/{package,simplephysics}.d`, `core/meshdata.d`, `core/texture.d`, `core/param/{package,binding}.d`, `core/animation/{animation,player,package}.d`,
`core/automation/{package,phys,sine}.d`, `core/package.d`, `package.d`, `phys/{package,system}.d`, `math/{transform,serialization,triangle,camera,package}.d`, `fmt/{package,binfmt,serialize}.d`, and
the shaders `shaders/basic/*` and `shaders/composite*`, plus `inmath/{interpolate,linalg}.d`. Nothing was run; every statement is a reading, and the places where the reading found an oddity
in the reference are marked **quirk**. Where the reference and this section differ in a choice, the choice is listed in section 12.9.

### 12.1 The file

* **Container** (`fmt/package.d`, `fmt/binfmt.d`): the 8 bytes `TRNSRTS` + a zero byte; a 4-byte big-endian length; that many bytes of JSON (the puppet); the 8 bytes `TEX_SECT`, a 4-byte
  big-endian count, and per texture a 4-byte length, a 1-byte encoding (0 PNG, 1 TGA, 2 BC7) and the bytes; optionally the 8 bytes `EXT_SECT`, a 4-byte count and per payload a 4-byte name
  length, the name, a 4-byte length and the bytes. Accepted extensions `.inp` and `.inx` (the editor's project, same layout). The reader needs to know nothing about `EXT_SECT`.
* **Puppet JSON**, top-level keys: `meta`, `physics`, `nodes`, `param`, `automation`, `animations`. No version key. `nodes` is the root node (its name is forced to "Root"); `param`, `automation`,
  `animations` and `physics` are optional.
* `meta`: `name` (string), `version` (string, default "1.0-alpha"), and optional `rigger`, `artist`, `copyright`, `licenseURL`, `contact`, `reference` (strings), `rights` (an object of usage
  flags: `allowedUsers`, `allowViolence`, `allowSexual`, `allowCommercial`, `allowRedistribution`, `allowModification`, `requireAttribution`), `thumbnailId` (texture index; 4294967295 = none),
  `preservePixels` (bool: point sampling instead of linear). The usage rights are information for the user; Murasaki reads and shows none of them.
* `physics`: `pixelsPerMeter` (default 1000) and `gravity` (default 9.8).
* Vectors are JSON arrays of numbers in component order. Enums are strings (the member's name).

### 12.2 Nodes

Every node: `uuid` (uint32), `name`, `type` (the class), `enabled` (bool), `zsort` (float, default 0), `transform`, `lockToRoot` (optional bool), `children` (optional array of nodes). A child whose
`type` is unknown is skipped with its subtree (the reference does so silently); a child without `type` is an error. `transform` has `trans` (3 numbers, default 0), `rot` (3 numbers, **radians**,
default 0) and `scale` (2 numbers, default 1). Types found: `Node` (the plain node), `Part`, `Composite`, `MeshGroup`, `SimplePhysics`; `Shapes` and `AnimatedPart` exist in the source but are not
registered or not saved (their data never reaches a file); a Drawable base class carries `mesh`. (The `Part`, `Composite` and `MeshGroup` type strings are those class names; the spelling of the
`SimplePhysics` type string is confirmed in its source.)

**Transform.** Local matrix = translation · rotation · scale(sx, sy, 1) (column vectors: scale first). The rotation is the quaternion of Euler angles (x, y, z in radians; for the 2D puppets in the
wild only z is non-zero). A node's parameter offsets (12.4) are applied to the components before the matrix is built: translations and angles add, scales multiply. World = parent world · local-with-
offsets; with `lockToRoot` the parent is the puppet root instead. **Orientation: +x right, +y down on screen**, origin at the centre of the view; the vertices of a mesh are in the same pixel-like units.
**Quirk:** the reference's product of two transforms returns a translation that is wrong in general, used only for bounds; the matrix is right.

**Depth sort.** The sort key of a node is the sum of `zsort` + the parameter offset over the node and all its ancestors. The painter draws the **highest key first**, so a lower key is in front.
The puppet draws the list of "root parts" (every Part and Composite of the tree, found depth-first, a Composite's subtree not entered, a stable sort); a Composite sorts its own sub-parts the same way.
**Quirk:** a Composite found inside a Composite is flattened into the outer one (the inner one's own blend mode, opacity and masks are lost).

### 12.3 Meshes and parts

* **Mesh** (`mesh` of a Drawable): `verts` (flat x0, y0, x1, y1, ...), `uvs` (flat, optional), `indices` (flat, triangles as consecutive triples, unsigned 16 bit, so the format allows 65536 vertices; our limit `kMaxVerticesPerMesh` is 32768), `origin` (a
  2-vector, the pivot), `grid_axes` (editor data, ignored). A vertex is placed at (vertex - origin + deformation) transformed by the node's world matrix. UV (0, 0) is the top-left of the texture
  and v grows with y. The reference does not normalise the winding on load and does not cull, so the winding is not significant.
* **Part** keys after the node's and `mesh`: `textures` (three texture indices: albedo, emissive, bump; 4294967295 = none), `blend_mode`, `tint` (3 numbers, default 1), `screenTint` (3 numbers, default
  0), `emissionStrength` (default 1), `masks` (array of `{source: uuid, mode}`), `mask_threshold` (default 0.5), `opacity` (default 1). Legacy `masked_by` (uuids) with `mask_mode`. Masks whose source is
  not a drawable of the puppet are dropped. **Quirks:** the reference reads `emissionStrength` into `tint`; a missing texture does not advance the slot number.
* **Blend modes** (strings): Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, LinearDodge, AddGlow, ColorBurn, HardLight, SoftLight, Difference, Exclusion, Subtract, Inverse, DestinationIn,
  ClipToLower, SliceFromLower. The reference's fixed-function equations - the observable result of each mode, listed as formulas, not its implementation - (colour premultiplied, S the source, D the destination, Sa its alpha): Normal S + D(1-Sa); Multiply S·D + D(1-Sa); Screen S + D(1-S);
  Lighten max(S, D); ColorDodge S·D + D; LinearDodge S + D(1-S) (alpha Sa + Da(1-Sa)); AddGlow S + D (alpha as LinearDodge); Subtract D - (1-D)·S; Exclusion S(1-D) + D(1-S); Inverse S(1-D) + D(1-Sa);
  DestinationIn D·Sa; ClipToLower S·Da + D(1-Sa); SliceFromLower D(1-Sa). Overlay, Darken, ColorBurn, HardLight, SoftLight and Difference have no fixed-function form: they use the GPU's advanced blend
  equations when it has them and **fall back to Normal otherwise**.
* **Colour of a part** per pixel, with c the albedo texel, a its alpha, S(x, a) = 1 - (1-x)(1 - screenTint·a), M the tint, o the opacity: colour = S(c, a)·M·o, alpha = a·o. The texture is
  expected **premultiplied**. Emission and bump go to extra render targets (lighting) that a desktop pet does not use; this feature draws albedo only.
* **Masks** (stencil): clear the stencil to 0 if the part has at least one Mask-mode binding, else to 1; for each binding in order draw the source's geometry (only its pixels whose alpha is above the *source's*
  `mask_threshold`) writing 1 for Mask and 0 for DodgeMask; then draw the part where the stencil is 1. A Composite can have masks; it cannot be a mask source. A MeshGroup draws nothing as a mask. There
  is no inheritance of opacity from a parent Part: only a Composite groups.
* **Composite**: keys `blend_mode`, `tint`, `screenTint`, `mask_threshold`, `opacity`, `propagate_meshgroup` (missing means **false** on load), `masks`. No mesh. It draws its sub-parts into an off-screen
  group in their sorted order (each with its own masks), then blits the group with its own blend mode, opacity (alpha = a·o), tint and screen tint, under its own masks.

### 12.4 Parameters and bindings

* **Parameter**: `uuid`, `name`, `is_vec2` (bool, default false), `min` (2-vector, default (0, 0)), `max` (default (1, 1)), `defaults` (default (0, 0)), `axis_points` (two arrays of floats in 0..1, default
  [[0, 1], [0, 1]]; element 0 is x), `merge_mode` (Additive, Weighted, Multiplicative, Forced, Passthrough; default Additive), `bindings`. A binding whose `node` does not exist is dropped.
* **Binding**: `node` (uuid), `param_name`, `values` (a matrix `[x][y]`: x counts `axis_points[0]`, y counts `axis_points[1]`; floats, or for `deform` a list of 2-vectors per vertex), `isSet` (a
  boolean matrix of the same shape, "authored"), `interpolate_mode` (Nearest, Linear, Stepped, Cubic, Bezier; missing means Linear; the reference runs only Nearest, Linear and Cubic and **asserts on the other two**).
* **param_name** of a plain node: `zSort`, `transform.t.x/y/z`, `transform.r.x/y/z`, `transform.s.x/y`; of a Part, also `opacity`, `tint.r/g/b`, `screenTint.r/g/b`, `emissionStrength`, `alphaThreshold`; of a
  Composite: the node ones plus `opacity`, `tint.r/g/b`, `screenTint.r/g/b`; of `SimplePhysics`: `gravity`, `length`, `frequency`, `angleDamping`, `lengthDamping`, `outputScale.x/y`; and `deform` on a
  Drawable. How a pushed value combines with the node's own: sort offset **adds**; translation and rotation **add**; scale **multiplies**; opacity **multiplies**; tint **multiplies**; screen tint **adds**;
  emission strength **adds** to a base of 1 (the factor is 1 + sum); physics `length` adds and the others multiply; deformation **adds** per vertex. All offsets are reset every frame. The final values are
  clamped to 0..1 for opacity, tint and screen tint. **Quirk:** `alphaThreshold` has no effect in the reference.
* **Evaluating a parameter**: the value (a 2-vector, in the parameter's units) is first the sum of its additive pushes plus its stored value, times the average of the multiplicative pushes; a host that only sets
  the value gives just the value. Per axis `n = clamp((v - min) / (max - min), 0, 1)`. Along an axis with points P0 < ... < Pk, take the first segment i whose upper point exceeds n (the last segment if none),
  and `t = (n - Pi) / (Pi+1 - Pi)`; a 1D parameter ignores the y axis. Each binding then interpolates its matrix: **Nearest** takes the corner whose weight is at least one half on each axis; **Linear** is
  linear (1D) or bilinear (2D) between the four corners; **Cubic** uses the Catmull-Rom spline through four neighbours per axis with the ends repeated (outer neighbours clamped to the matrix); in 2D, four
  rows are interpolated along x and the four results along y. The Catmull-Rom form is the standard one, `0.5·(2p1 + (-p0 + p2)t + (2p0 - 5p1 + 4p2 - p3)t^2 + (-p0 + 3p1 - 3p2 + p3)t^3)`. The reference fills the points of a
  binding that are not authored (`isSet` false) from the authored ones when an editor changes them; the file stores the whole matrix.
* **Order of a frame** (`Puppet.update`): automation entries; reset of every node's offsets; every parameter in order (those driven by a physics driver are skipped); all transforms marked stale; drivers
  (SimplePhysics) update; node update (including mesh group deformation); draw.

### 12.5 Deformation by a MeshGroup

A MeshGroup is a node with a mesh and two flags, `dynamic_deformation` and `translate_children` (both missing = false). Its mesh is deformed by its own `deform` binding. A child Drawable's points are carried
by it: a point q inside a triangle of the **rest** mesh with corners P1, P2, P3 has barycentric coordinates (u, v), q = P1 + u(P2-P1) + v(P3-P1); with the deformed corners Q the point becomes
Q1 + u(Q2-Q1) + v(Q3-Q1); a point in no triangle is unchanged; when several triangles contain it the **last** one wins; edges count as inside.

* `dynamic_deformation` true: the child's point in the group's local space is `G^-1 · W · (v + childDeform)` (G the group's world matrix, W the child's); it is warped to q'; the child is drawn with the matrix
  G, so its world position is `G · q'`.
* `dynamic_deformation` false: the warp is applied to the child's **rest** vertex (without its own deformation); the displacement q' - q is converted into the child's local space with the inverse linear part
  of the matrix from child to group space, and added to the child's deformation; the child keeps its own matrix.
* The warp reaches Drawable children, goes through non-MeshGroup Drawables and through Composites with `propagate_meshgroup`, and stops at a nested MeshGroup. `translate_children` also moves non-Drawable
  children and non-propagating Composites by the warp of their origin (not implemented by this feature). **The reference's bitmap of "which triangle holds this integer point" is an optimisation; the
  warp itself is the exact barycentric one.** Ours: repeated and flat triangles are dropped (a repeated one counts once, at its last place), a group may have `kMaxWarpTriangles` distinct triangles and all the groups of a puppet `kMaxWarpCellEntries` grid entries together (a puppet over either is refused), and at most `kMaxWarpCandidates` triangles of a point's grid cell are tried.

### 12.6 SimplePhysics (a driver node)

JSON: `param` (the uuid of the parameter it drives), `model_type` (`pendulum` or `spring_pendulum`, default pendulum), `map_mode` (`angle_length`, `xy`, `length_angle`, `yx`; default `angle_length`), `gravity`
(default 1), `length` (pixels, default 100), `frequency` (Hz, default 1), `angle_damping` (0.5), `length_damping` (0.5), `output_scale` (2-vector, default (1, 1)), `local_only` (false). Effective gravity =
`gravity · puppet gravity · pixelsPerMeter` (so 9800 pixels/s^2 by default). Each frame: `h = min(dt, 10)`; the anchor is the node's world origin (its local translation when `local_only`); the state is advanced
in fixed steps of 0.01 s (the remainder last) by a classical fourth-order Runge-Kutta; a step that produces a non-finite number is undone.
* Pendulum: state angle θ and its rate; `θ'' = -(g/L) sin θ - 2ζ sqrt(g/L) θ'` with ζ the angle damping; the bob is `anchor + L(-sin θ, cos θ)`, at rest `anchor + (0, L)`.
* Spring pendulum: state bob position and velocity; ω = 2π·frequency, k = ω^2, rest length `L0 = L - g/k`; acceleration = `(0, g) - n(d - L0)k` plus a damping that is split into a part across and a part along
  the spring direction n (angle damping across, length damping along; critical values `2 sqrt(g/L)` and `2ω`). **Quirk:** the reference's rotation of the damping back into world space is not a true inverse
  rotation; this feature uses the true one.
* Output: the bob relative to the anchor, in the node's local frame; `rel = |bob - anchor| / L`; `angle_length` gives (atan2(-ax, ay)/π, rel), `xy` gives (rel·ax, 1 - rel·ay), `length_angle` and `yx` swap the two,
  with (ax, ay) the unit vector in the local frame; each is multiplied by `output_scale` and **forces** the parameter's value. At rest `xy`, `yx` and `angle_length` give (0, 0).

### 12.7 Automation and animations

* `automation` entries: `type` (`sine` or `physics`), `name`, `bindings` (`param`: the parameter's **name**, `axis` 0 or 1, `range`: 2-vector). Sine: `speed` (rad/s), `phase`, `sine_type` (Sin, Cos, Tan as
  0, 1, 2); value = `range.x + (range.y - range.x)·(f(speed·t + phase) + 1)/2`. The physics automation (a chain of verlet nodes) is not described here: it is not evaluated by this feature.
* `animations` is a map of name to `{timestep (default 0.0166 s), additive, length (frames), leadIn, leadOut, animationWeight, lanes}`; a lane has `interpolation`, `uuid` (the parameter), `target` (axis),
  `merge_mode` (default Forced) and `keyframes` (`frame`, `value`, `tension` 0.5). Time → frame is `t / timestep`; the value is the first key before the first frame, the last after the last, and between two
  keys `t' = (f - f0)/(f1 - f0)` with Nearest (the next key when t' > 0.5), Stepped (the previous), Linear, Cubic (Catmull-Rom with the ends clamped) or Bezier (a Hermite ease with the tension). The player
  keeps time by adding the frame delta, loops between `leadIn` and `leadOut` (a missing one is the start or the end), and writes each lane to its parameter axis scaled by a strength 0..1. **Animations are
  not played by this feature** (a desktop pet's motions come from the host); the reader skips the `animations` key without checking its content beyond the size.

### 12.8 What this feature takes and leaves

Takes: the container, `meta.name`, `meta.preservePixels`, the node tree (Node, Part, Composite, MeshGroup), meshes, parameters with value bindings and deform bindings, masks, blend modes, depth sort,
opacity, tint and screen tint, MeshGroup deformation, SimplePhysics (task 6 delivers the data; the stepping can be added to the evaluator without changing the reader).
Leaves: emission and bump (no lighting), `Shapes`, `AnimatedPart`, automation, animations, `EXT_SECT`, `translate_children`, the rights, the BC7 encoding (a puppet that uses it is refused: no decoder is bundled),
the advanced blend equations (Overlay, Darken, ColorBurn, HardLight, SoftLight, Difference fall back to Normal as the reference does without the GPU extension).

### 12.9 Choices of ours where the reading is not enough

* Parameter merge modes only matter for pushes from automation, drivers and animations; a host that sets a parameter's value gets exactly the value (`Forced`).
* The matrix of a binding is taken as stored, whatever `isSet` says; the editor's fill of unauthored points is not repeated (open question Q-I2: a puppet saved with stale unauthored points).
* A binding of `Stepped` runs as Nearest and `Bezier` as Linear (the reference asserts); the reader says so in its warnings.
* A deform binding whose vertex count differs from its mesh is dropped, with a warning.
* Textures are kept as encoded bytes; decoding and premultiplication belong to the renderer (task 7).
* Every count and size is bounded by the limits table (section 8) and a value over a limit refuses the puppet; a node that cannot be loaded (unknown type) is skipped with its subtree and named.

## 13. Clean-room record (so far)

| Reader / evaluator | Public documents consulted | Source read as a specification | Statement |
|---|---|---|---|
| Manifest check (task 3) | S1 (sections 3.1), nothing else | none | written from the Cubism Specs text; no Cubism SDK source read |
| Motion, expression, pose, parameters, look-at (task 4) | S1 (motion3, exp3, pose3, model3 groups), S2 | none | written from the format descriptions; curve evaluation, fades, queue and blink are our design |
| Cubism physics (task 5) | S1 (physics3) | none | the format does not specify the motion; the integrator is our design |
| Open puppet reader and evaluator (task 6) | S7, S8, section 12 of this file | Inochi2D `v0.8.7` (BSD 2-Clause), the files listed in section 12, read on 2026-10-22 **indirectly, through summaries in our own words (no code), not by the author of the code**; the type strings `Part`, `Composite` and `MeshGroup` could not be confirmed in the source (their paths answered 404) and are the class names | the reader and evaluator are written from section 12 in our own structure, with no code copied or translated line by line; the shapes of data are the format's |

No source code of the Cubism SDK (Core, Framework, samples), of Wallpaper Engine or Anima Engine, or of any other puppet system was read for this feature.

## 14. Motions, expressions, pose and the life of a puppet (task 4): what is ours

Code: `src/puppet/{params,motion,expression,pose,lookat}.*`; tests `tst_puppetmotion`, `tst_puppetlife`; golden values `tools/compat/puppet_curves.py` -> `tests/puppetcurvegolden.h` (the ctest `puppet_curves_golden` fails
when the header is out of date). The curve values are computed in the script by exact rational arithmetic (a Bezier by 90 rational bisections), independently of the C++ evaluator (floating point, 60 bisections), and
compared to 1e-9 on ~1900 points of 50 curves.

* **Curves (Q-F1, Q-F2).** Before the first point the first value, after the last the last value. Linear: the line. Stepped: the segment's first value until its end time, where the next segment takes over.
  Inverse stepped: the segment's first value at its start time exactly, the end value at any later time. Bezier: the four points' times never decrease (the reader refuses a file where they do), so the cubic that
  gives time from its parameter is monotone and is solved by bisection; the value is the cubic through the four values at that parameter. For the restricted case the format describes (inner times at a third and two thirds)
  the time is linear in the parameter and the solution is exact.
* **Fades (Q-F3).** One easing for every fade, `0.5 - 0.5 cos(pi x)`. The weight of a motion is fade-in x fade-out; a curve's own fades multiply further. A fade comes from the host's request, else the file's `Meta`,
  else one second. A looping motion has no fade out until it is stopped. A motion's end fades over the last fade-out seconds. A stopped motion fades over its fade-out and is finished when that is over.
* **Applying.** A parameter moves from its current value toward the curve's by the weight (so two motions and an expression compose in the order they are applied); the same for part opacity; the `Model` curves `Opacity`,
  `EyeBlink` and `LipSync` are returned as values for the host.
* **Scheduler.** Priorities none < idle < normal < force. Force starts at once, drops the queue and fades out what plays; normal replaces a playing idle (which fades out), otherwise waits in a queue of at most
  `kMaxMotionQueue` (a request over that is refused); idle starts only when nothing plays or waits. When the current motion is over the head of the queue starts. A 20 000-step random test checks the invariants.
* **Expressions (Q-F4).** Default blend `Add`, fades one second; several layers (at most 8) can be active, oldest first; `Add` adds `value x weight`; `Multiply` scales by `1 + (value - 1) x weight`; `Overwrite` moves toward
  the value by the weight; the result is clamped by the parameter.
* **Pose (Q-F8).** The first visible node of a group is shown at once; afterwards a node whose part is raised above what the pose left it at takes over (the previous one fades out while it fades in, over `FadeInTime`, 0.5 s by default);
  linked parts follow their node.
* **Life.** Breathing: a sine on `ParamBreath` over its whole range, period 3.2 s. Blinking: a seeded generator draws the wait (2 to 6 s); closing 0.10 s, held 0.05 s, opening 0.15 s; the eye parameters are multiplied by the
  openness; the result does not depend on the size of the steps. Look-at: the cursor's offset from the pet's centre divided by twice the pet's long side, clamped to -1..1 (x to the right, y up), dead zone 0.02, smoothed with a
  0.15 s time constant; it drives `ParamAngleX/Y/Z`, `ParamBodyAngleX`, `ParamEyeBallX/Y` over the side of each parameter's range the sign asks for. The host decides whether it runs (question 6: off until Mouse Tracking look).

## 15. Cubism physics (task 5): what the format states and what is ours

Code: `src/puppet/physics.*`; test `tst_puppetphysics` (55 cases). **The format lists the parts of a rig and their numbers and gives no equation** (section 3.3), so there is no oracle: the integrator is our design.
What the file states and the reader enforces: the lists and keys of section 3.3, weights from 0 to 100 as percentages, `Type` X or Angle, targets of kind `Parameter`, a range per rig with minimum <= default <= maximum,
`Fps` 1 to 240 (60 when absent), mobility 0 to 1, delay and acceleration 0 to 100, radius 0 to 1000; counts bounded by the limits table. The `Meta` counts are not trusted.

**The model (ours).**
* **Inputs.** Each input parameter is read as -1 .. 1 across its own range with its default at 0 (each side has its own scale), turned into the rig's own range with the rig's default at 0 and its maximum/minimum at +1/-1
  (`Normalization.Position` for type X, `.Angle` in degrees for type Angle), times the weight as a fraction, negated when `Reflect`. The X inputs add up to the horizontal place of the chain's root; the Angle inputs add up
  to the angle its rest direction is turned by.
* **Chain.** Particle 0 is the root, at (baseX, 0). Particle i hangs from particle i-1 at a segment length (`Radius`, else the distance of the listed positions). Rest direction = gravity turned by the base angle, with the
  wind added as a sideways push of its relative size. Each particle is a damped spring toward the point at its rest direction from its parent: `omega = 3 x Acceleration / max(Delay, 0.01)` capped at `0.8 / step` (a stable
  semi-implicit Euler), damping ratio `1 - 0.8 x Mobility`; then the segment is projected back to its length and the speed along it takes the parent's. The root follows the input at once. At rest (inputs at their defaults)
  the chain hangs along gravity and stays exactly there.
* **Time.** A fixed step of `1 / Fps`; one update simulates `floor(dt / step)` of them, at most `kMaxPhysicsStepsPerUpdate` (8; the rest of a long pause is dropped, not simulated); the inputs move linearly across the steps of an
  update. The first update starts the rig at rest wherever the inputs are, so there is no jolt (and none after `reset()`).
* **Outputs.** The output reads the segment from particle `VertexIndex - 1` to `VertexIndex` (an index of 0 or past the chain skips the rig): type Angle is its signed angle from gravity in degrees, type X its horizontal
  length. The value is normalised by the rig's range as the inputs are, times `Scale` x weight fraction (negated when `Reflect`), and **added** to the destination parameter (on the side of its range the sign asks for), then clamped by the
  parameter. The engine rebuilds the parameters each frame (defaults, motions, expressions, pose, then physics), so the addition is to the frame's value, not an accumulation.
* **Missing parts.** A rig whose input or output parameter the model does not have, whose output vertex is not a pendulum particle, or that has fewer than two vertices is skipped and named
  (`puppet: physics rig skipped: <id> (<why>)`, the runtime name of section 10); the other rigs run.

**Properties tested:** a chain at rest does not move in 10 000 steps; a moved head swings the hair and it settles within 1e-3; the energy of a shaken chain decays; outputs stay inside their parameter's range with absurd scales;
weight, scale and reflect act as stated; an angle input turns the settled chain by exactly that angle; wind holds the chain to one side; two runs with the same inputs are bit-identical over 10 000 steps; a long update
equals the same number of short ones and an update of an hour or `1e300` s does not explode; bad `dt` is ignored; random inputs for 20 000 updates stay finite and in range.

## 16. The open back end (task 6)

Code: `src/puppet/{model,openformat}.*`; test `tst_puppetopen` (92 cases, every puppet written by the test). Written from section 12, with no code of the reference.

* **The abstraction (`model.h`).** `ModelBackend` offers `parameters()` (a `ParameterSet`: ids, ranges, defaults), `parts()` (a `PartSet` of opacities the host can set), `shape(i)` (the static uvs and indices), `bounds()` and
  `evaluate(values, parts, Frame&)`, which fills the frame's draw list in painter's order. A draw item is a `Mesh` (positions in model units, y down, opacity, tint, screen tint, blend mode, albedo texture, masks) or a `BeginGroup` / `EndGroup`
  pair (the end carries blend, opacity, tint and masks of the group). `Frame::maskSources` holds the geometry of every drawable some mask uses, drawn or not. The Core wrapper of task 9 will implement the same interface.
* **The reader (`OpenModel::load`, `loadFile`).** INP version 1 only. Container: signature, JSON length (<= `kMaxInpJsonBytes`), the JSON, the optional `TEX_SECT` (<= `kMaxTextures` blobs of <= `kMaxTextureFileBytes`; PNG and TGA kept as bytes, BC7 and unknown
  encodings refused); `EXT_SECT` and anything after is not read. The JSON is read by the strict reader with the depth `2 x kMaxDeformerDepth + 8` and `kMaxPuppetJsonValues` values. Refused whole (not repaired): bad container, JSON that is not strict, a node
  without type or uuid, a repeated uuid, a tree deeper than `kMaxDeformerDepth` or over `kMaxNodes`, a mesh with an odd coordinate count, a triangle list that is not a multiple of three, **an index outside the vertices**, uvs of the wrong length, a count over
  `kMaxVerticesPerMesh`, `kMaxIndicesPerMesh`, `kMaxVerticesTotal`, `kMaxIndicesTotal`, `kMaxDrawables`, `kMaxParts`, `kMaxMasksPerPart`, `kMaxParameters`, `kMaxBindings`, `kMaxAxisPoints`, an unknown blend or mask mode or interpolation, axis points that are not
  strictly increasing inside 0..1, a binding matrix of the wrong shape, a parameter range or default that is not valid, a value of the wrong type. Skipped and named in `info().warnings` (what the reference also skips or cannot run): a node of an unknown type
  with its subtree, a binding whose node is missing or whose name the node does not have, a deform binding with the wrong number of offsets, a mask source that is not a Part, a texture index the file lacks, `Stepped` (runs as Nearest) and `Bezier` (as Linear).
* **Names.** A one-dimensional parameter is `name`; a two-dimensional one is `name.x` and `name.y`; a repeated name gets `#<n>`. Parts are the Part and Composite nodes, by name (a repeat gets `#<uuid>`); their opacity multiplies the node's.
* **Evaluation.** Section 12.4 as written: each parameter's value is normalised by its range, the keypoint cells and weights found, every binding combined into the node's offsets (adds, multiplies and clamps as listed), then world matrices (a 4 x 4 chain with the
  Euler quaternion, `lockToRoot` to the root's own local matrix), then the depth keys, mesh group deformation (exact barycentric warp over the rest triangles through a uniform grid, the last triangle holding a point wins, nothing for a point in no triangle),
  then the draw list. Composite sub-parts are flattened as in the reference. Tests compare drawables numerically against hand-computed values: linear, nearest and cubic interpolation, bilinear and bicubic 2D, translation, rotation, scale, hierarchy and
  `lockToRoot`, opacity, tint and screen tint, depth order, composites, masks, mesh group warps in dynamic and static mode, propagation rules, textures, and a fixed-seed mutation fuzz (3000 mutated puppets: no crash, finite results).
* **Not in this task** (section 12.8): stepping of `SimplePhysics` (read and counted), automation, animations, `translate_children`, emission and bump, BC7, texture decoding and premultiplication (the renderer's job: the reference expects premultiplied colour).
* **Memory.** The strict reader keeps lists of numbers as plain arrays, but a deform binding is a list of two-number lists, one value tree node each. Measured with `murasaki-puppet --check` on a generated puppet of 5 MB of JSON and 1.35 million
  values (one part of 10 000 vertices, 15 parameters with 3 deform keypoints each): 0.14 s and a peak of 84 MB resident. `kMaxPuppetJsonValues` (1 572 864) bounds that to about 100 MB; **it may refuse large published puppets**, to be tuned against real
  files (open issue).

## 17. Hardening notes of the review of tasks 1 to 3

* **Opening files of an item.** The folder is validated once by name, so every later reader opens with `openInItem()` (`manifest.h`): `openat2` from the item's own directory with `RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
  RESOLVE_NO_MAGICLINKS`, or, where `openat2` is missing or filtered, a walk of one `openat(O_NOFOLLOW)` per component. The manifest reader, the container signature check and the deep check of an `.inp` use it. Readers of motions,
  expressions, physics, pose and textures must use it too (`readItemFile` is the helper). A test replaces a folder by a link between the scan and the read, with both implementations.
* **MotionSync.** `FileReferences.MotionSync` is a path and is validated like every named file (path rules, `.motionsync3.json`, presence, `kMaxMotionSyncFileBytes`). The `MotionSync` inside a motion entry is the *name* of a
  setting of that file (S1), not a path: it is a string of at most `kMaxIdBytes`. `Sound` is a path checked for syntax only (it is never loaded).
* **`.inp` signal.** `loadManifest` reports `puppet: payload not read` for an `.inp`; `checkItem` (what `murasaki-puppet --check` runs) reads the puppet and removes it.
* **Detection of the manifest** ignores case (`X.MODEL3.JSON`, `X.INP`), as the extensions of the named files do; the names inside a manifest are exact.
* **Details shown** (`tidyDetail`) are cut at a character boundary within 120 bytes and every invalid UTF-8 byte, C0 and C1 control, zero-width and bidi character, line or paragraph separator and the byte order mark becomes U+FFFD.
* **Numbers.** The strict reader refuses a number that overflows (`1e999`) and also one that underflows to nothing (`1e-999`); other values are only checked to be numbers, and their consumers bound them (fades, ranges).
* **Open (Q-F9).** The fade ranges (`FadeInTime`, `FadeOutTime` from 0 up to `kMaxFadeSeconds`) are ours: S1 gives no range.

## 18. Hardening notes of the review of tasks 4 to 6

* **Mesh group grid.** A hostile index list could multiply the grid (65 536 copies of one wide triangle: 1 GB). Now: duplicate and flat triangles are dropped, at most `kMaxWarpTriangles` distinct triangles per group, at most `kMaxWarpCellEntries` grid entries over all the groups
  (CSR layout, counted before anything is allocated), at most `kMaxWarpCandidates` triangles tried per point, and the deformed corners of a group are computed once per frame, not once per child. Tests: the duplicate list loads and draws in well under a second, a fan of
  `kMaxWarpTriangles + 1` triangles and a set of wide thin triangles are refused, and the grid budget is shared by many groups.
* **Opening the `.inp`.** `OpenModel::loadFile(folder, relative)` goes through `openInItem` (no link on the way, the last component included) and reads at most `kMaxInpBytes + 1`. Tests: a link on the way, a link as the file, `..`, an absolute name, a pipe, a folder, a file one byte over.
* **Magnitudes.** Every coordinate, offset, scale, zsort, binding value and parameter bound of a puppet, and every position, scale and range of a physics file, is refused above `kMaxCoordinate` (10^7). What the arithmetic still overflows (a chain of 64 nodes of scale 10^7) is made finite:
  a node whose matrix is not finite collapses to a point, an unorderable depth becomes 0, a vertex that is not finite becomes 0, and a parameter normalised to a non-finite number becomes 0. A physics rig whose state is not finite starts again from rest.
* **Textures** are ranges of the file the model keeps (`textureBytes`), not copies: the peak is the file plus the JSON tree, and the encoded textures together are capped by `kMaxTotalTextureBytes` (128 MiB).
  The worst case of the JSON tree is a puppet made of small objects or mixed lists, about 130 bytes a value: `kMaxPuppetJsonValues` bounds it near 200 MB; the favourable case measured in section 16 is 84 MB.
* **Physics** cannot be copied (its rigs point into its settings; a move is fine) and does nothing for a parameter set of another size than the one it was bound to. `ParameterSet::at`, `value`, `PartSet::id` and `opacity` check their index.
* **Motions fading out** beside the current one are capped (`kMaxFadingMotions`, the oldest is cut short).
* **Q-F3b (open).** A curve's own `FadeInTime` / `FadeOutTime` multiplies the motion's fade here. S1 does not say whether it should replace it, and the golden script shares its author with the evaluator, so the choice cannot be tested against anything. A test pins it, and
  `handDerivedFromTheSpecificationWording` holds values worked out by hand from the wording of S1 (Bezier control points at a third and two thirds, linear, the two steps). To be settled with a real model.
* **Determinism.** The physics is deterministic for one binary on one machine; the results of `sin`, `cos`, `atan2` and `hypot` can differ between C libraries, so nothing may use the bit patterns as a contract between machines.
* **Composite in composite** is flattened as in the reference and now says so in `info().warnings`.

## 19. The engine process (task 7)

Code: `src/puppet/{render,engine,session,view}.*`, `src/puppet/main.cpp`, `src/bridge/puppetview.h`, the `puppet` profile of `src/sandbox`, `src/overlay/puppetenginehost.*`. Tests: `tst_puppetrender`, `tst_puppetengine`,
`tst_puppetview`, `tst_puppetenginehost`, `tst_sandbox` (puppet cases), `tst_crashledger`.

```
 murasaki-overlay                                               murasaki-sandbox --profile puppet --item DIR --require -- murasaki-puppet --engine ...
   PuppetEngineHost (restart policy, CrashLedger)  --starts-->    murasaki-puppet (Vulkan, no Qt)
   bridge consumer (BridgeItem or a client)  <== frames, RGBA8 premultiplied, dma-buf (shm fallback) ==  BridgeSink
                                             ==> Pause, Fps, Resize, Pointer, Properties (JSON commands), Quit ==>
                                             <== HitMask (message 49) ==
```

* **The channel** is the bridge, as decided by the spikes of section 1: dma-buf by default, shared memory as the fallback; the picture is **premultiplied alpha**. The engine announces protocol version 6 in `Hello`
  (`bridge::kPuppetVersion`), which adds `HitMask`; a consumer that sees less never gets one. `BridgeClient` (the consumer the QML items use) accepts a Hello of 6, reports `supportsPuppet()`, and
  decodes a `HitMask` only from a peer that said 6, with the generation of its current buffer set and a grid that matches that set's size (anything else is ignored, as an unknown message type is).
* **Messages in.** `Pause` (hidden, paused, or off-screen: nothing is evaluated or drawn, the loop waits on the socket with a one second timeout); `Fps` (1 to 240; the host decides 60, 30 or 15 for battery - there is no adaptive
  rate); `Resize` (a new buffer set; the picture is brought down to `kMaxPuppetPixels` on the long side keeping the aspect); `Pointer` (picture pixels; used only while looking is on); `Properties` (the JSON commands of
  `PuppetSession`: `set`, `part`, `reset`, `look`, `breath`, `blink`; a refused command changes nothing and is logged, it is not a failure); `Quit`.
* **Message out.** `HitMask` (`src/bridge/puppetview.h`): cols x rows bits of 8 x 8 pixel cells (larger cells above 256 cells a side), made from the meshes that are drawn (not from texture alpha), sent with the first picture and
  after that when 2% of the cells changed or, for any change, after 30 pictures. It carries the buffer-set generation so a mask made before a `Resize` is dropped. The overlay converts it to its input region (task 8).
* **When it draws.** At the fps cap, only while not paused, and only when something that shows can change: the model has `ParamBreath` or an eye parameter with breathing/blinking on, the gaze is moving, or the host sent
  a command or a resize. Otherwise one picture a second goes out so that a consumer that waits for one never waits long. The model is not evaluated when it is not drawn (tested with a counter).
* **Life.** Breathing and blinking on by default, effective only for models that have `ParamBreath` / `ParamEyeLOpen`, `ParamEyeROpen`; looking at the pointer off until `{"look": true}` (plan question 6).
* **The picture.** The model's rest bounds fit the picture, centred, aspect kept. The renderer draws the frame's draw list: meshes with a texture, tint, screen tint and opacity (colour = S(c, a) x tint x opacity,
  alpha = a x opacity, textures premultiplied on upload), the blend modes of 12.3 as fixed-function blending, stencil masks (cleared to 0 when any Mask binding exists, else 1; each binding writes 1 or 0 where its source's alpha is above
  the source's threshold; the item is drawn where the stencil is 1), and composite groups (drawn into an off-screen image, then put on the picture with the group's blend mode, opacity, tint and masks). Six modes that have no fixed-function
  form (Overlay, Darken, ColorBurn, HardLight, SoftLight, Difference) draw as Normal. **One deviation from the reference's equations:** it draws on a screen whose alpha it never reads; ours is the alpha of a picture put over a desktop,
  so the modes about colour (Multiply, ColorDodge, Subtract, Exclusion, Inverse) take Normal's "over" for alpha, and only DestinationIn, ClipToLower and SliceFromLower keep the reference's alpha factors. Without it an Inverse over an
  opaque backdrop would cut a hole in the pet.
* **Textures.** Before the first one is decoded their header sizes (PNG IHDR, TGA header; `OpenModel::textureSize`) are summed in 64 bits against `kMaxTexturePixels` and `kMaxTotalTexturePixels` (`textureBudgetOk`), so sixteen PNGs
  of 8192 x 4096 are refused before 2 GB of RGBA exist; the sum is kept again over what was really decoded, since a header can lie. They are then decoded by the bounded decoder of the scene engine (PNG, TGA, ... within `kMaxTextureSide`,
  `kMaxTexturePixels`), premultiplied, and **the encoded bytes (the whole `.inp`, up to `kMaxInpBytes`) are let go by `OpenModel::releaseFile()` right after the decode loop**, before the upload; the decoded pictures are freed after the upload.
  (The first version of this section said the encoded bytes were dropped after the upload, which was not so: the file lived as long as the engine.)
* **Exit statuses** (`src/puppet/exitcodes.h`): 0 the host closed the socket or sent Quit; 1 runtime failure; 2 usage; 3 the model cannot be shown (unreadable, a Cubism model, a texture the decoder refuses or the device has no memory for, a
  first frame the model refuses, "puppet: too heavy"); 4 no Vulkan device (or one without what is needed); 5 device lost.
  The sandbox helper's own 125 and 127 are not the engine's.
* **The `puppet` sandbox profile** (`murasaki-sandbox --profile puppet --item DIR [--core FILE] [--require]`): the model folder, the system, the Vulkan and Mesa description, `/dev/dri` (to list; libdrm needs it, or RADV sees no device) and the
  process's own `/proc` entries to read; the render nodes (`/dev/dri/renderD*` only), a few devices and **its own two shader caches (`puppet-shader-cache`, `puppet-gpu-cache`, not the scene engines')** to write; a seccomp denylist, with `clone` refused for new namespaces and `clone3` answered ENOSYS (section 20); a card node is kept out by the mount namespace and, on Landlock ABI 5 or later (kernel 6.10), by the missing `IOCTL_DEV` right - not by the missing write permission; no home, fonts, audio, session or display (the mount namespace hides the sockets), no TCP; no core dumps; `SIGKILL` when the parent goes. The one optional extra read-only path is the Cubism Core file the
  user installed (nothing loads it yet). A policy table test pins the lists, and enforcement tests run the helper (reads, writes, TCP, the GPU node, the parent's death). The engine was also run under the helper with `--require` on a real GPU
  (`theExecutableUnderTheSandbox`).
* **The host** (`PuppetEngineHost`, Qt Core): starts the engine (behind the helper when there is one; with `requireSandbox` and no helper it refuses) with a fresh socketpair whose consumer end is signalled to the caller, marks the model in
  the `CrashLedger` while it opens (`firstFrameShown()` clears the marker; an overlay that dies while opening is charged at its next start, as for files), and applies the policy (starting and stopping never block: `started` / `errorOccurred`, SIGTERM then a timer for SIGKILL; `start()` is main thread only because `PR_SET_PDEATHSIG` fires when the forking *thread* dies; a consumer fd nobody is connected to is
  closed): **a crash restarts the engine** (`puppet: engine restarted`);
  **the third crash of the same model blocks it** (`puppet: blocked after crashes`), also across restarts of the overlay and until the file changes - unless the count started again (section 20, I-7); a refusal (3), no GPU (4: `puppet: no GPU`) or no sandbox (125, 127) is reported and not retried
  and not counted. A crash is a signal or any status that is not documented as a refusal. `CrashLedger::recordCrash` is new for this. Tests drive it with a fake engine script and the real ledger.
* **Not in this task, and not wired yet: motions, physics, expressions and pose** (done in task 8: section 21). `PuppetSession` has breathing, blinking, gaze and the `set` / `part` / `reset` commands only. `MotionManager`, `Physics`, `ExpressionManager` and `Pose` (tasks 4 and 5) are not
  used by the engine and there is no `play(group, priority)` command (plan 4.6). Task 8 (or a 7b) does it, with `PuppetSession::step` calling `Physics::update` and `MotionManager::update` under the same `kMaxStep` cut as the life, and the files they read
  opened with `readItemFile`. The open `.inp` back end has no motion files of its own (an `.inp` carries animations the reader counts and does not play); until then a puppet shown by this build breathes, blinks and looks, and nothing else.
* **Not in this task** (task 8): the pet window and `PetItem.qml` hosting the picture, the input region from the `HitMask` (the overlay's `alphamask` code), `AddLocalPuppetItem`, `Windows()` fields, the daemon's check behind the sandbox, states, clicks,
  revision 20. Also not done: the Cubism back end (task 9), texture alpha in the hit mask, and any per-vendor tuning of the pipelines.
* **Verified here:** the renderer tests (26) pass on the machine's AMD GPU (RADV) and on llvmpipe, a software Vulkan device, with the same pixels; the engine test (16) runs the real loop on a hand-made model in a thread and the real
  executable, plain and under the sandbox, reading the pictures from a shared-memory ring. **Unverified:** the dma-buf path with a real consumer (the tests force shared memory; the spike of section 1 covered dma-buf with the bridge
  item), other GPU vendors, the cost of a real puppet (the numbers of section 1 are those of an empty picture), and the overlay's use of the `HitMask`.

## 20. Notes of the review of task 7

Review `p8c-review.md` over `6a0ff79..1f28368`. What was done, with the test that pins each point; what was only written down.

* **C1, the consumer.** `BridgeClient` accepted Hello 1 to 5 only, so the engine's 6 ended the connection. Now: up to `kPuppetVersion`, `supportsPuppet()`, `HitMask` decoded only from a peer that said 6, with no fds, the current generation and
  `cols` / `rows` equal to `ceil(size / cell)` of the current buffer set (this is also M11); a mask that fails any of those is ignored, like an unknown type. `tst_bridgeclient`: Hello 6 accepted, 7 a violation, and a mask from a v5 peer, one of a stale
  generation, one with a bad grid or length, and one before the first buffer set are all ignored.
* **I-1, overdraw (GPU).** `frameCost` (`view.cpp`) works out on the CPU the area of every drawn triangle clipped to the picture, a mask source again at each use, a picture's worth for each stencil clear and each group image. A frame over
  `kMaxOverdrawFactor` (64) pictures, or that needs more than `kMaxPositionVertices` positions, is not drawn; `kMaxHeavyFrames` (30) such frames in a row end the engine with status 3 ("puppet: too heavy"). *Second review:* the estimate is an upper bound worked out from the boxes
  of the triangles (half a box inside the picture, the clipped box across an edge), a stencil or group clear is 1/16 of a picture, a group is put on the picture by the box of what it holds, the work is bounded (`kMaxFrameCostWork` triangles) and the answer is kept while nothing that shows
  changes; a synthetic character of 20 groups and 40 masks is drawn. The first version counted a whole picture for every clear and rejected such a model. Only the estimate can end the model. The time of `submitAndWait` is watched but only to protect the machine: `kMaxSlowFrames` (3) frames
  over `kMaxSlowFrameMs` (250 ms) in a row halve the engine's own frame rate (floor `kMinSlowFps`, 5) with a warning, `kSlowRecoveryFrames` (300) fast frames double it back, and on a software device (llvmpipe) there is no watch. (The first version refused the model after three slow frames, which
  punished a busy shared GPU for the model's sake.) Tests: `tst_puppetview::frameCostCountsClippedAreaMasksAndGroups`, `tst_puppetengine::aHeavyModelIsNotDrawnAndGivenUp` (2000 full-picture quads: no
  frame drawn, status 3; 20 quads: drawn), `aSlowGpuFrameGivesTheModelUp` (the limit lowered to 0 ms). **Not covered:** the estimate ignores texture fetch cost and blend-mode cost per pixel, and overestimates long thin triangles (their boxes); a single frame under it that is slow on a weak GPU is drawn, and the engine lowers its rate after three.
* **I-2, the hit mask (CPU).** Cell ranges are clamped in `double` before they become integers (they were undefined for a coordinate like 1e30); rows are walked by the triangle's span in that row of cells (the cells outside it cannot hold a hit,
  so the result is the same as before); the work is budgeted (`kMaxHitMaskWork` units, about 40 ms: a row costs 15, a cell tested 7, a cell already set 1) and past it the rest of an item's triangles are covered by the one box that holds them
  (more than they cover, which is the safe side for an input region); the engine does not recompute the mask when `evaluate` says nothing changed and the size is the same (`hitMaskRuns()` counts the calls). Tests: 5000 full-picture triangles
  under 50 ms, 5000 thin slivers under the budget and a superset of the exact ones, 1e30 and -1e30, a mask not recomputed for an idle model.
* **I-3, the `puppet` profile.** Its shader caches are its own; render nodes only are writable; a seccomp-BPF denylist (hand-written, no libseccomp) makes `io_uring_*`, `bpf`, `userfaultfd`, `perf_event_open`, `ptrace`, `process_vm_*`, `keyctl`, `add_key`,
  `request_key`, `open_by_handle_at`, `mount` and the new mount API (numbers fixed in the source when the headers lack them), `pivot_root`, `chroot`, `unshare`, `setns`, `kexec_*`, module loading, `pidfd_getfd`, `kcmp`, swap, `reboot` and `acct` fail with EPERM;
  *second review:* `clone` with any `CLONE_NEW*` flag is EPERM (a nested user namespace would have given back every capability inside it and made `unshare` / `setns` being refused pointless) and `clone3`, whose flags a filter cannot read, is ENOSYS so glibc falls back to `clone`
  (an EPERM would have broken thread creation); any other ABI (32 bit, x32) is refused whole; the filter is applied after Landlock and fails closed only under `--require`. The program is built by `buildPuppetFilter()` and a test runs it through a small BPF interpreter (ia32, x32,
  every denied call, clone flags), and a real child keeps threads, fork and subprocess. Tests: the policy table (the caches differ from the scene's), a write to the scene caches is refused, `renderD128` opens and a card node does not, the helper runs a program whose `io_uring_setup` and `ptrace` return EPERM
  (and the same program unfiltered gets the real answers). Checked by hand on RADV and llvmpipe with `vulkaninfo` under `--profile puppet --require`: **the first try lost RADV** because libdrm lists `/dev/dri` to find the GPU (the directory is now read-only
  in the profile; only the render nodes are writable); a card node can be opened for reading at most and has no ioctl right. Not done: an allowlist (Mesa and the Vulkan drivers use too many calls for one to be safe to write blind).
* **I-4, the positions buffer.** A mask source is written again for each item that uses it, so the first sizing (twice the vertices) lost masks. The renderer now counts what the frame needs and grows the buffer (up to `kMaxPositionVertices`); beyond it the frame
  is only cleared and every item is counted as skipped; a mask source that could not be drawn counts as skipped too. Tests: a 5000-vertex source shared by 10 parts (all ten show, no skips), 129 meshes of 65 535 vertices (cleared, 129 skipped, and a frame
  that fits is drawn afterwards).
* **I-5, textures and the file.** See the Textures bullet of section 19. Tests: header sizes of PNG and TGA, sixteen 8192 x 4096 headers refused by the budget (they cannot be decoded, so the refusal is before any decode), the boundary at one pixel, 32 bit
  sizes that overflow a signed product (the test found `static_cast<int64_t>(w) * h` overflowing for 4294967295 squared), `releaseFile` and a model that still evaluates.
* **I-6, positions beyond a float.** What leaves `OpenModel` is clamped to `kMaxOutputCoordinate` (1e12; the one case where the limits table clamps a computed value instead of refusing a read one) and the renderer clamps again before the float conversion
  (non-finite becomes 0). Tests: the 64-node chain of `extremeNumbers` reaches the bound and never passes it; the renderer draws 1e300, infinities and NaN without trouble.
* **I-7, the crash ledger.** Decision: three crashes block a model *that breaks the engine*, so the count starts again at one when the engine had been running `kEngineLongRunSeconds` (30 minutes; a first version said 60 seconds, which let a model that dies every 61 s restart for ever at 1440 launches a day) before it died (the host measures it, from the first picture) or when the previous crash was over
  `kCrashForgetHours` (24) ago. Not done: forgetting on a clean first frame (a model that shows its first picture and then dies is still the model's: `aCrashAfterTheFirstFrameCountsToo` stays). Tests with an injected clock (`tst_crashledger`) and a fake engine
  that lives 0.3 s (`tst_puppetenginehost`: with the long run at 0.2 s it is restarted for ever, at 60 s it is blocked at the third).
* **I-8, the session.** Written in section 19 ("Not in this task, and not wired yet") and in the plan (task 8): motions, physics, expressions and pose are not part of `PuppetSession` yet.
* **Minor, done:** M3 (the memory assertion of `hostileMeshGroups` measures a forked child's growth over its own start, with a control that allocates 300 MB, and the time is relative to a plain group), M4 (the vacuous asserts are real ones), M5 (a failed `HitMask`
  send is offered again), M7 (a first frame the model refuses, and a renderer that cannot be made for a texture the device has no memory for, exit 3, not 1 or 4), M8 (`.INP`, and a name of 4 characters or fewer, are judged by `hasInpExtension`), M9 (the host
  never blocks, the dead code and the fd leak are gone, main thread only), M11 (in C1), M12 (`security.md`).
* **Minor, only noted (plan, phase 9 or later):** M1 (a legitimate puppet of long thin triangles can use all of `kMaxWarpCellEntries` and is refused whole; degrade the group to no warp with a warning instead, or raise the cap, once a real model has been
  measured), M2 (`Warp::carry` tests the last `kMaxWarpCandidates` of a cell and leaves a point in an earlier triangle unmoved, silently; add a load-time warning for a cell that exceeds it), M6 (the engine's process limits are `RLIMIT_CORE` only; consider
  `RLIMIT_CPU`, `RLIMIT_NPROC`, `RLIMIT_FSIZE`, and a cgroup `MemoryMax` from the host), M10 (`murasaki-puppet --check`, which the daemon runs on hostile data, is the same executable as the engine and so maps Vulkan, glslang and the image decoders; split it into
  `murasaki-puppet-check` or load the engine lazily).
* **Second review (`p8d-review.md`, `1f28368..2360a4b`), the rest.** Done: the seccomp points above (S-1, S-3, S-4, S-5, S-6, S-7), the cost and the slow watch (I-A, I-B, I-C), the 30 minute rule (L-1) and its clock from the first picture (L-2), `start()` after `stop()` (N-2: a leaving engine is killed first), the null
  `QCoreApplication` (N-1), the 64 bit grid size of the client (N-3), the cost of a triangle past the hit-mask budget (H-1), and a test that a card node answers no KMS ioctl (S-2; it skips before ABI 5 and asserts only what the platform can). Noted, not done: N-5 (`hitMaskOf` copies the item's vertices to pixels each time it runs; a
  reused vector would save 8 MB of transient memory for the largest model), H-2 (the time asserts of the hit-mask tests are wall clock: best of three, 50 ms and 100 ms; a loaded machine could still miss them), a backoff for `restartDelayMs` (a model that dies every minute is restarted every minute and a quarter, for ever, if it
  survives 61 s each time: three crashes within 24 h block it, so it is the 24 h rule that frees it), and the dma-buf path with a real consumer.

## 21. The session plays what the model brings (task 8: motions, expressions, physics, pose)

`PuppetSession` (section 19) takes `SessionAssets` (`src/puppet/session.h`): motion groups (each motion with the fades the manifest gave), expressions by name, the physics rigs and the pose. `loadSessionAssets(Manifest)` (`assets.*`) reads them from a Cubism manifest through `readItemFile`, with the bounded
readers of tasks 4 and 5; what cannot be read is left out and named (`puppet: motion skipped: <file>`, `puppet: physics rig skipped: <file>`), never a refusal of the item. A group or expression with nothing readable does not exist. The `.inp` of an Inochi2D puppet has no such files (the
folder rule of section 3 allows one `.inp` and no manifest), so today the assets are empty for every puppet this build can show, and only a Cubism back end (task 9) will fill them; the wiring, the commands and the tests are there already.

* **Commands** (the bridge `Properties` JSON, atomically validated: a refused command changes nothing): `{"play": {"group": G, "index": n, "priority": "idle"|"normal"|"force"}}` (without `index`, one at random from the engine's seed), `{"stop": true}` and `{"expression": name | null}`, next to the
  commands of section 19.
* **Order.** Per evaluation: the host's values, then the life (breathing, blinking, gaze), the motions, the expressions, the physics (the rigs swing from the parameters those left, in fixed steps, under the same `kMaxStep` cut as the life), then the pose (it fades one part of a group in and the others out; with a pose the part
  opacities persist from frame to frame, without one they start from the host's each time). The result is compared with the last evaluation's: nothing changed, nothing is drawn.
* **`animated()`** is true while a motion plays or fades or waits, for two seconds after a motion or an expression is started or ends (a fade outlasts the event), and while a rig moves by more than 1e-5 or has any energy; otherwise an idle puppet is redrawn once a second as before.
* Tests: `tst_puppetsession` (no GPU: a motion on command, the refusals, the priorities, expression fades, a hair rig that swings and settles, a rig that cannot be bound, the pose, `animated()`, the seed, the loader) and `tst_puppetengine::aMotionPlaysThroughTheBridge`.

## 22. The puppet as a pet (task 8: daemon, overlay, API revision 20)

* **The item.** `AddLocalPuppetItem(path)` / `RemoveLocalPuppetItem(id)` (`docs/dbus-api.md`, revision 20): a folder (one `.inp`, or one `*.model3.json` and what it names) becomes the pet item `local:puppet:<sha1 of the canonical path>`, `format` `puppet`; `LocalPuppetImporter::validate` is
  `loadManifest` (no link, the path rules, the limits table), the folders are kept in `local-puppets.json`, and the item's `puppet` object and `variants` (`model`, `motion:<Group>`, `expression:<Name>`; none of them an animation file) are in the API document. A Cubism item is listed with
  `MissingDependency` and `puppet: needs the Cubism runtime`.
* **The deeper check** is `murasaki-puppet --check FOLDER` (section 11, with the new `canvas` key) behind `murasaki-sandbox --profile puppet`, run by the daemon's `PuppetChecker`: one at a time, 30 s, the answer kept by the folder and a signature of its top level, so a replaced model is checked again; it follows
  `importRequireSandbox` as the imports do (required: it does not run unconfined). A check that cannot run (no helper, no sandbox when required, the deadline, a crash, a line that is not JSON) answers `puppet: check failed: <why>`, and the item shows as invalid with that reason. The answer is merged into the item (`ItemsChanged`).
* **The picture.** `PetWindow` draws a puppet pet with `PuppetItem` (`src/overlay/puppetitem.*`, a `BridgeItem`): the engine's frames come over the bridge (dma-buf, shared memory when the GL context is not EGL, as on a bare X server) with real alpha. The item launches the engine with the command line of
  `PuppetEngineHost::command` (the same one the host tests pin) behind the sandbox (`importRequireSandbox`), restarts it with the base class' backoff, and tells a refusal from a crash with `classifyExit` (`puppet/exitcodes.h`, shared with `PuppetEngineHost`). `PuppetEngineHost` stays as the policy object
  with its own tests; the overlay uses the item.
* **Crashes** are the overlay controller's: `engineCrashed` goes to `CrashLedger::recordCrash(model file, afterLongRun)` (the rules of section 20), and the third blocks the model: the window is dropped, the engine ends with it, `Windows()` says `puppet: blocked after crashes`. A first picture does *not* forgive earlier engine
  crashes (the ledger's `forget` on a file that plays is for crashes of the overlay while it opened the file). A refusal (3, 4, 125, 127) is named (`model refused`, `puppet: no GPU`, `sandbox unavailable`) and not counted. A crash of the overlay while it opens the model is the file ledger's, as for any pet.
* **The input region** is the footprint the engine sends (`HitMask`, bridge message 49) turned into a silhouette mask (`maskSetFromHitMask`, a grid of at most 128 cells): the pointer, hide, fade and dodge work on the puppet's mesh, not on texture alpha.
* **States, clicks, look.** A state's variant `motion:<Group>` plays that group (the base `model` plays nothing and a state that leaves a group ends the motion), `expression:<Name>` sets that expression; the window plays a request when `PetView::Puppet::playSerial` changes, and the item remembers the last motion and the expression and sends them again after a restart.
  A click plays `TapBody`, else `Tap`, when the model has one (no hit areas to tell where the click landed: that comes with the Cubism back end). Mouse Tracking `look` is the gaze: the controller turns the mirror off for a puppet, the window turns the global pointer into picture pixels and sends it to the engine.
* **Windows()** has, for a puppet pet only, `puppet: {state, fps, crashes, group, expression, look}` (`state`: `starting` until the first picture, `running`, `restarting`, `blocked`, `refused`, `nogpu`) and the degraded names above. A puppet's frame rate is the placement's `fps` (60 by default) under the power cap; its size is the pet's width and the canvas' aspect.
* **Not done in task 8, done in task 10 (section 23):** `puppetMaxPixels`, a click on a hit area, the GUI and `murasakictl`. Still open: `puppetCorePath` is only stored (task 9 is not built), and the sound mouth driver (question 12).
* **Tests:** `tst_localpuppets`, `tst_puppetchecker`, `tst_murasakiservice` (the two calls), `tst_overlaycontroller` (the seven puppet cases), `tst_puppetmask`, and by hand or `MURASAKI_NESTED_TESTS=1 ctest -R overlay_puppet_x11` the whole chain on a private Xvfb: the item, the sandboxed check, the real engine drawing (RADV or, with
  `VK_ICD_FILENAMES`, llvmpipe), a red pixel read back from the screen, a crash restarted, three crashes blocking, the engine ending with its pet. That run found three faults the unit tests could not: a crashed engine closes its socket before its parent hears it exit (so `BridgeItem::exitWaitMs`), `hasFrame` stays true across a restart (so
  `pictureShown`), and the file ledger's forgiveness on a first picture.

### Review of task 8: what was fixed and what stays

* The checker (`PuppetChecker`): a helper that cannot be started ends the check (`puppet: check failed: the checker cannot be started`; Qt emits no `finished` for it, and the queue used to wait for ever); its output is drained as it arrives and kept to 64 KiB, past which it is killed (`its output is too long`) and its standard error goes nowhere (a
  hijacked helper cannot grow the daemon); an answer about a folder that changed while it was checked is dropped. What the checker says is cleaned before it joins the item (counts 0 to 1e6, at most 16 strings of 200 characters, a canvas of at most 16384 a side). The manifest, parsed by the daemon itself with the strict reader and
  the limits table (not in the sandbox: only the deep check is), is cached by the folder's top level.
* The engine's end: `BridgeItem::setExitWaitMs` (300 ms for a puppet) makes the item (1) wait for the exit status of a host whose socket closed and kill it if it lives on (a broken engine's end is a crash and counts), and (2) wait for the old host to end, and kill it, before a restart, a new configuration, a new window or the item's destruction
  returns: **never two engines alive at once**; the e2e samples the engines of its own process tree during a setting change and a crash. A host that was stopped on purpose is not a crash.
* `loadSessionAssets` has a total budget (`kMaxTotalAssetBytes`, 32 MiB) and reads a file named more than once a single time.
* `Windows()["puppet"].state` is `starting` until the first picture; a puppet item that names no model file inside its folder is `refused` (`puppet: manifest invalid: model file`) instead of vanishing; the session caps the time the rigs and the pose have waited to one second.

## 23. ctl, GUI, settings, clicks and a stop that never blocks (task 10)

* **`murasakictl pets puppet`** (needs API revision 20; an older service gets "Puppets need a newer Murasaki service"): `add <folder>` (prints the item id), `remove <item>`, `list` (puppet items with their check: checked or not, backend, canvas, counts, reasons and degrade names),
  `check <folder>` (runs `murasaki-puppet --check` behind `murasaki-sandbox --profile puppet` when both exist, the same way the daemon does, and prints what it says) and `status` (the `puppet` object of `Windows()`: state, fps, crashes, group, expression, look). Formatting lives in `src/ctl/ctlutil.*` and is unit tested;
  `tst_ctlpets::puppetCommands` drives the real calls against a fake service.
* **GUI.** The library has "A puppet folder…" in the Add-pet menu (enabled only when `Daemon.puppetsSupported`, that is revision >= 20), cards say "Puppet", and the pet page for a puppet shows its check (backend, canvas, motion groups, expressions) and the reasons or degrade names in words
  (`PetPage.puppetNote`). A Cubism model says plainly that Murasaki does not ship the Cubism runtime and does not draw Cubism models yet. The optimized-copy box is hidden for a puppet (it is drawn live; nothing is converted). Strings are in Spanish in `murasaki_es.ts`.
* **Settings.** `puppetMaxPixels` (1 000 000 to 16 000 000, default 4 194 304): the size, in pixels, of the picture the engine draws; a bigger window gets a smaller picture enlarged by the GPU, and `Windows()["degraded"]` says `puppet: resolution reduced`. `puppetCorePath`: a string stored and checked, nothing else. `SetSettings` refuses a path that is not a plain absolute file
  owned by the user and not writable by others (`PuppetSettings::coreFileProblem`); **nothing opens, loads or `dlopen`s it** (task 9 waits for the licence answer, see the plan).
* **Clicks on hit areas.** The window sends `{"click": {"u", "v", "fallback": [...]}}` (fractions of the picture, mapped back through rotation and flips). `PuppetSession` validates it; the model's hit areas are tested where the drawable is drawn (`ModelBackend::drawableId`), the first hit area with a motion group of its name plays
  one at random, and with no hit area hit the first fallback group the model has (`TapBody`, then `Tap`) plays. The controller no longer plays a tap itself.
* **A stop that never blocks.** `HostRetirement` (`src/bridge/qml/bridgesupport.*`) takes a stopped host process, kills it after `killAfterMs` without waiting, and tells whether any is still leaving; `BridgeItem::startHost` waits (25 ms retries) while one leaves, so there is **never more than one engine alive**, and the main thread
  never sleeps in `waitForFinished`. A lost connection with a live process arms a kill timer; its finished handler delivers the status (a crash counts, a stop on purpose does not). Tests: `tst_hostretirement`, `tst_bridgeitem`, `tst_puppetenginehost`, and the e2e's sampling of the engines of its own process tree.
* **Nested run.** `MURASAKI_NESTED_TESTS=1 ctest -R overlay_puppet_x11` now also runs `murasakictl pets puppet list` and `status` against the live engine. A nested KWin run (the plan's blink, look and click motion screenshot) was not possible on this machine; the X11 run checks the red pixel, a setting change with one engine alive,
  a crash restarted and three crashes blocked.
* **Clean-room record.** Every format fact came from the formats' public documents and from files made by our own writers; no SDK, runtime, sample model or code of Live2D, Inochi2D or any wallpaper engine was read or copied. The only third-party names in the tree are the format and file names (`.inp`, `.model3.json`, `TapBody`) that a user's model carries.

### Review of task 10: what was fixed

* `murasakictl pets puppet check` follows the daemon's rule: with `importRequireSandbox` on (the default, and also when the daemon cannot be asked) it passes `--require` to the sandbox and refuses with `sandbox unavailable` when `murasaki-sandbox` is missing; the parser never runs bare unless the setting is off.
* `BridgeItem::stopHost` disarms the exit timer (it could kill the next engine and count a crash against a model that did nothing). An engine that falls over by itself while it is being retired is told to the item (`retiredHostExited`) and counts in the crash ledger; one the retirement kills does not. A pending restart delay is no longer shortened by the wait for a leaving engine.
* The lines of `puppet list` and `status` go through `printable()`; a hit area with no id matches nothing; a collapsed or non-finite triangle covers nothing.
* Tests keep their temporary directories under `~/.cache/murasaki-tt` (ctest sets `TMPDIR`; the path is short because one test makes a Unix socket in it).

