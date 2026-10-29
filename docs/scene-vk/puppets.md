# Puppets in murasaki-scene-vk

A puppet is an image layer whose model JSON has `"puppet": "models/x.mdl"`: the layer is a mesh instead of a quad, and the object may play animations that move the mesh's bones. The reader is `src/scene-vk/puppet/mdl.{h,cpp}`, the drawing `render/puppetdraw.*` and `SceneRenderer::drawLayer`. The format was worked out from the models of the author's library (one: the sun of 3757825891, 159 vertices, 8 bones, one animation); nothing was copied. What is verified and what is assumed is said below.

## The .mdl file (little-endian)

```
"MDLV0013", 14, 16, 17, 19, 21 or 23, NUL            9 bytes (other versions are refused)
u32 flags, u32 1, u32 1, NUL string material     the material path, then zero padding
u32 flags, u32 vertexBytes                       the mesh block (found by checking, see below)
vertexBytes / 80 vertices                         position 3f, normal 3f, tangent 4f, blend indices 4 x u32,
                                                  blend weights 4f, uv 2f
u32 indexBytes, indexBytes / 2 u16 indices        triangles
"MDLS0002" or "MDLS0004", NUL, u32 end, u32 boneCount          the skeleton; `end` is the file offset of the next section
per bone: NUL name, u32 1, i32 parent, u32 64, 16f bind matrix (row vectors, translation in the last row, LOCAL to
          the parent), NUL string                 ... then data the engine does not use, up to `end`
"MDLA0005" or "MDLA0006", NUL, u32 end, u32 animationCount
per animation: u32 id, u32 0, NUL name, NUL mode ("loop", "mirror", "single"), f32 fps, u32 length (frames), u32 0,
               u32 trackCount, then per track: u32 0, u32 byteSize, byteSize / 36 frames of
               position 3f, euler angles 3f (radians), scale 3f
```

Verified on the real model: the counts and sizes line up with the sections' end offsets, every vertex index is below the vertex count, the bind matrices are local (bone 1's translation equals its animation position), and the animation holds 8 tracks of 61 frames for a length of 60 at 30 fps. Assumed: the order of position, angles and scale in a frame (the values look right: scales of 1 and 0.626), that a bone's blend index is the index in the file's bone list, that several animations follow each other with a gap of zeros (the one model has a single animation), and what `blend` and `rate` of an animation layer mean.

Versions 13, 14 and 16 (older writers) have a 52 byte vertex: position, four bone indices, four weights, uv; there is no normal or tangent, and the reader widens it to the 80 byte one (a normal toward the viewer, a tangent along x). 17 and later have the 80 byte vertex. The skeleton versions 0001 to 0004 and the animation versions 0001 to 0006 lay out their records alike as far as the 138 models of the library show (checked: every one reads). The older versions (MDLV0019 with MDLS0002 and MDLA0005, found in 3261565977: one bone, one animation of 1801 frames) have the same layout as the newer ones as far as that model shows: the mesh block, the bone record and the animation and track headers match field by field, and the section `end` offsets and track sizes line up. Whether a newer version added fields that a model with several bones or animations would show is not known; the version sets are explicit so an unknown one is still refused.

A skinned 3D mesh (an `.mdl` with a skeleton as a `"model"` object) uses the same skeleton and animation sections after its parts and the same bone matrices; formats.md, Meshes, describes it.

Later models (3799253558, 3796494818) added what the first ones did not show, all handled: the string after a bone's matrix is the editor's constraint JSON and can be several hundred bytes (cap 64 KiB; "Bone physics" below reads it); a bone's parent may come later in the file (the matrices are built in a parent-first order, a loop of parents is refused); some animations are followed by track-shaped records beyond the declared ones (twenty tracks for eighteen bones, then empty ones: walked over and not kept, counted against the keyframe cap); and the gap between two animations can be any number of zero bytes (35 in one model), so the next record is found by looking, over at most 256 bytes, for an id, a zero, a name and a mode of loop, mirror or single, or an empty mode after a non-empty name (the FBX clips of 3164411828 and 3495221617, which then play as `loop`), followed by an fps above 0, a length and a track count within the limits and a first track whose size is whole frames. Between the FBX clips sit 6 zero bytes, six floats (a bounding box) and a few more fields. The u32 before a track's size is not always 0 (1 in 2618806466). What follows the animations (an `MDLE0002` section in 3799253558) is not read.

**Additive layers** (`"additive": true`): the layer adds (frame - bind pose) times its blend on top of the other layers instead of sharing their blend, so two of them sum. Alone it is the same as a plain layer. Assumed from how the frames hold absolute local transforms (a track's first frame equals the bind pose); unverified against WE.

The mesh block is found by scanning for a `vertexBytes` that is a multiple of 80 followed by an index block that ends before the skeleton and whose indices are all in range (with every check made explicit).

A puppet nobody rigged has no skeleton section at all (the header's first u32 is 9 where a rigged one has 0x1800009; seen in 3822491687, 3818947747 and 3820429404). Its vertex is 48 bytes: position, normal, tangent and uv, without bone indices or weights, and the mesh block runs to the end of the file (seven bytes of zeros follow the indices). Without a skeleton only that vertex is read (the longer ones carry bone indices, which only a skeleton gives a meaning); the reader widens each vertex to 80 bytes with bone 0 at full weight, and the puppet is drawn as a static mesh: no bones, no animations, no SKINNING.

A rigged mesh whose block flags are 0x181000e (not 0x180000f) has an 84 byte vertex: the long one with a float after the position (0 in every file of the library: 3810943704, 3577990983). The reader takes it when the 80 byte vertex does not fit, and drops the float.

## Limits

Vertices 200,000; indices 1.2 million; bones 128 (`g_Bones[]` must fit the 16 KiB uniform window); animations 64; frames per track 65,536; keyframes in all 2 million; animation layers 16 per object. A parent bone must come before its children, a vertex may only name bones that exist, positions must be finite, every count and size is checked against the data before anything is allocated. A model that fails any of this is refused by `--check` as `puppet: <reason>`.

## Drawing

- The mesh coordinates are in the object's own space: centred on its origin, y up, in scene units. The node's matrix (origin, angles, scale, parents) places the mesh; the object's `size` is not used for it. A puppet with effects draws into the layer's buffer (the object's size, the mesh scaled onto it), the effects run on that, and the layer composites like any other.
- The vertices go to the GPU as they are in the file (80 byte stride). The material's shader is WE's own (`genericimage4` and friends), through the translator.
- **No `animationlayers`:** the mesh is drawn in its bind pose, unskinned (assumption: WE does not play a model's animation unless the object lists animation layers). 3757825891's sun is like that.
- **With animation layers** (and a skeleton with weighted vertices): the shader gets `SKINNING 1` and `BONECOUNT n`, n the bones the vertices name (the largest index any of a vertex's four names, weighted or not, plus one: the shader reads all four), and a `g_Bones[n]` of `mat4x3`, the skinning matrix of each of those bones (the matrices are worked out for the whole skeleton on the CPU, into buffers reused from frame to frame): its animated world matrix times the inverse of its bind world matrix. A bone's local transform is `translation * rotation(euler) * scale`.
- **Layers** `{"animation": id, "blend": w, "rate": r, "visible": ...}`: an animation plays at `r` times its fps; `loop` wraps at its length, `mirror` runs there and back, `single` holds the last frame; frames interpolate linearly. Several layers blend by their weights (normalised when they sum to more than 1); what is left (weights summing to less than 1, or a bone no layer has a track for) stays the bind pose. `visible` may be a user property. All of this is assumption, not checked against WE; the library's 2D puppets with layers (3463520581, 3779406919, 3808922316, ...) and its skinned 3D meshes (3732632761, 3164411828, 3495221617, formats.md) play them this way.

## What the format has that this engine does not use

A survey of the 15 `.mdl` files in the author's library (`scene.pkg` entries of the Workshop items; the scan looks for the section tags `MD??xxxx`):

| Section | Where | What it is | Here |
|---|---|---|---|
| `MDLV0023` | 14 files | the common version | read |
| `MDLV0019`, `MDLS0002`, `MDLA0005` | 1 file (3261565977, `beach_sunset_train_puppet.mdl`) | an older version of the file and of its skeleton and animation sections | refused by `--check` (another branch is teaching the reader this version) |
| `MDAT0001` | 38 puppets of 25 items | attachment points: a name (`hands`, `eyes`, `头发`), the bone it sits on and a matrix | read; objects whose `attachment` names one stand on it ("Attachments" below); the script API (`getAttachment*`) still answers with no attachments |
| `MDLE0002` | 4 files (3779406919 body and eye, 3799253558) | a trailing section of 4x4 float matrices, one per bone or group of bones; purpose not worked out | skipped |
| morph targets / blend shapes (WE writes `MDMP`) | none | | not read; `getBlendShapeIndex` answers -1 |
| bone physics (impulses, simulated chains) | 78 bones in 18 items | spring bones | simulated, see "Bone physics" below; IK chains (`ik`, 4 bones) are read and not evaluated |

Two things the reader and the engine do today that a generator found (`tests/tst_scenevk_puppetgen.cpp`, `tests/support/mdlwriter.h`): a parent bone that comes after its child in the file is refused (the format does not say bones are ordered; the test for acceptance is an expected failure until the reader puts bones in dependency order), and a skeleton over 128 bones is refused with the vague reason "bad skeleton header". For the share the layers leave over (blends below one, additive layers) a bone's bind matrix is taken apart, once when the model is read, into translation, scale and the Euler angles of `Rz * Ry * Rx` (all three: the 3D meshes' binds turn about every axis, and 2D puppets mirror bones with a half turn about x, which taking only the z angle used to lose). Each bind angle is then taken the whole turns nearest the first frames of the tracks that pull the bone (their blend-weighted mean; an additive layer uses its own track's first frame): a mirror read back as -pi blends with frames that store +pi as the same half turn, and the choice does not change while a track spins round, so nothing jumps.

## Texture coordinates

A model's texture coordinates run over the **picture**, not over the texture file. A `.tex` is often padded (a 3053 x 2160 picture in a 4096 x 4096 file: `imgW/imgH` against `texW/texH` in the header) and the mesh's u and v of 0..1 are the picture's: the sprite sheet quads of image layers already stop at `uMax`/`vMax`, and the puppet's coordinates are scaled by them when the mesh is uploaded (`SceneRenderer::preparePuppets`; a sprite sheet texture is left alone). How many it touches (library survey, 2026-10-21, read-only, 391 puppet models of the Workshop items): 227 have a picture smaller than their texture in the header (`imgW`/`imgH` against `texW`/`texH`), but in 145 of those the file stores mip 0 already cut to the picture, so `uMax` and `vMax` are 1 and nothing is scaled (the renderer's pictures of 3366554210's fourteen layers, for one, are the same with and without the scaling). **82 puppets in 39 items have a padded mip 0** (`uMax` or `vMax` below 1) and are the ones that change. Evidence that their coordinates run over the picture: 3794862132's body has u 0.014..0.985 and v up to 1.003 over a picture of 0.745 x 0.527 of its file; the mesh is 2963 x 2173 units, the picture 3053 x 2160: 0.9705 of the picture's width, not of the file's 4096. Without the scaling the mesh showed the picture stretched (here 1.34 times in x, 1.9 times in y) and, below its content, the padding, which the texture editor fills by repeating the last row: vertical stripes in the torso of 3794862132. With it the picture matches the item's `preview.jpg`. A model whose picture fills its stored texture is not changed (validated on 22 items: those with an unpadded mip 0 render byte for byte the same). Test: `tst_scenevk_render`, `aPuppetsTextureCoordinatesRunOverThePictureNotThePaddedFile`.

## Attachments

An object with `"attachment": "name"` and a `parent` that is a puppet layer stands on a named point of that puppet's rig. Survey (2026-10-21, 13 scenes: 3264246690, 3363252053, 3441873795, 3448290956, 3462491575, 3486806915, 3492627662, 3779406919, 3794862132, 3808922316, 3810943704, 3463520581, 3822822401): 176 objects carry an `attachment` (1, 1, 5, 1, 7, 6, 3, 4, 9, 59, 12, 58, 10 per scene; many are groups with no image of their own that carry the layers) and **every one names an `MDAT0001` entry of the parent's model; none names a bone**. All 176 resolve. The objects are image objects or bare groups.

**The MDAT section** (little-endian), sitting right after the skeleton: the skeleton's `end` is its offset.

```
"MDAT0001", NUL, u32 end, u16 count          `end` is the offset of the "MDLA" section that follows
per point: u16 bone, NUL name (UTF-8), 16f matrix (row vectors, translation in the last row)
```

Verified on the 13 scenes' models: the entries end exactly at `end`, and `end` is where "MDLA" starts. Of the library's 41 matrices only 2 (jadegirl, `right` and `Attachment`, 16 and 19 degrees about z) have a rotation, the rest are a translation. The matrix is **local to its bone** (assumption from the numbers: the translations, tens to hundreds of units, are not the bone's world position, and the placement with them gives a complete picture on 3462491575 and 3808922316, where without them a head, hair and face stood apart). A model whose MDAT does not line up (counts, a bone out of range, `end` not where the entries stop) has no points; the animations are then found by their tag.

**A model with an MDAT section used to lose its animations**: the animation section was only accepted right at the skeleton's `end`, which for these models is the MDAT, so their animations were not read (the 25 items' puppets stood in the bind pose). They are read now.

**Placement.** The child's world matrix is `parent's node matrix * point * child's own origin, angles, scale`, where `point` is the bone's animated world matrix (physics included, "Bone physics" below) times the point's matrix, in the puppet's mesh space. The point's rotation is inherited, so a child on an arm turns with it (assumption: renders of 3462491575 and 3808922316 come out as complete pictures with it; nothing in WE's own behaviour was checked). The renderer refreshes the points once a frame before the layers are drawn, parents first (a puppet that stands on another's point comes after it); a puppet that is not skinned keeps the points of its bind pose. `scene.cpp: nodeWorldMatrix` applies the point, so scripts' world matrices and the draw agree.

**Not placed, reported.** `--check` says `attachment '<name>': <reason>` for an object whose parent is missing (`the object has no parent`), is not a puppet (`the parent is not a puppet`) or whose model has no point of that name (`the parent's model has no such point`, or `has no attachment points`); it then stays a plain child of the parent's node. The scripts' `getAttachment*` and the attachments of 3D meshes (`mesh attachments`) are unchanged.

## Bone physics

**What the data is.** The string after a bone's matrix is the editor's constraint JSON. A survey of the author's library (674 Workshop items; `scene.pkg` entries; read-only, 530 `.mdl` files, 2026-10-20) found 1635 bones with such a string, in 59 items. 1557 of them are `{"a":null,"s":null,"tp":...}` or `{"a":null,"lamax":null,"lamin":null,"rax":null,"ray":null,"raz":null,"s":null,"tm":...,"tp":...}`: no flags, nothing to simulate. 78 bones in 18 items carry the full set (the keys of the table below). The key names are short; their meaning below is read off the names and the values, not from a specification.

| Key | Bones | Values seen | Read as (assumption) |
|---|---|---|---|
| `r` | 78 | true 50, false 28 | the bone's angles follow the animated ones through a spring |
| `t` | 78 | true 28, false 50 | the same for its position |
| `rs`, `ts` | 78 | 200 (63), 300 (14), 29 / 200 (51), 300 (22), 500 (2), 368, 29 | spring stiffness (1/s^2) of rotation, translation |
| `rf`, `tf` | 78 | 20 (56), 5 (7), 29..41 / 20 (55), 5 (20), 10 | damping (1/s) |
| `ge` | 78 | true 3, false 75 | gravity acts on this bone |
| `gd` | 78 | `0 -1 0` (76), two tilted | gravity direction (a unit vector as a string) |
| `la`, `lamin`, `lamax` | 78 / 1449 | `la` true 15; limits `+-3.14159` about z (74), `+-1.5708` (1), others | angle limits, as an offset from the animated angle (the full turn limits change nothing) |
| `lt`, `ltmax` | 78 | false 78, 100 | translation limit as an offset from the animated position (never on in the library; implemented) |
| `rax`, `ray`, `raz` | 1408 | `raz` true 34 (the 3D rig), false otherwise | axes the rotation spring works on; absent: about z only |
| `tax`, `tay`, `taz` | 35 | true 34 | axes of the translation spring; absent: x and y |
| `a` | 1635 | `1 0 0` (76), null (1557) | the bone's own direction: the lever gravity, wind and impulses turn |
| `ik` and `ikce ikcp ikcpn ikd ikg ikr ikrd ikrmax ikrmin ikse` | 78 / 66 / 34 | `ik` true 4 | an IK chain: read, **not evaluated** |
| `m` (20), `ri` (30), `ti` (30), `tm` (3 to 200), `se`, `re`, `s` (0, 50, 113), `tp` | | | read as nothing: purpose not worked out. `tp` is a vector, in 182 bones `-nan(ind) -nan(ind) -nan(ind)` |

The scene's own forces are the `general` keys `gravitydirection` (149 scene items, `0 -1 0` in 144), `gravitystrength` (1.0 in 148, 2.0 once), `windenabled` (true in 2 items, neither with a puppet), `windstrength` (1.0 in 146; 112.68, 356.1 and 1000 once each) and `winddirection` (`0.707 0.707 0` in 145). The scene loader reads them into `Scene::boneWorld`; nothing else uses them (`--check` reports an enabled wind as degraded, `scene wind (bones only)`, since particles do not feel it). The keys `windenabled`, `windstrength`, `winddirection` do not occur in a bone's constraint.

**What is simulated.** `puppet/bonephysics.{h,cpp}`. A bone whose constraint has `r` or `t` keeps its own position and angles (in its parent's space) and a velocity for each. Fixed steps of 1/60 s, at most 0.1 s (6 steps) caught up in one frame as the particles do, a clock that goes back simulates nothing. Each step is a semi-implicit Euler step: the acceleration is `stiffness * (animated - simulated) + force - damping * velocity`, so the bone lags the animation, overshoots a little and settles on it; with no force the simulated pose is the animated one to within rounding. The spring constants are clamped (stiffness 12000, so the explicit spring step stays stable; damping 10000, which is applied implicitly and is stable whatever it is); angle offsets are limited to half a turn and position offsets to 10000 units even with no `lt` (a spring of no stiffness under gravity would otherwise drift); velocities and impulses are clamped; a non-finite state resets the bone. A rotating bone is moved by the force across its lever (`a`, in the parent's space, over the distance to its first child: torque = lever x force / length); a translating bone by the force itself. The simulated bones are worked out parent first inside the skinning pass of `puppetBoneMatrices` (the parent's simulated matrix is the child's frame), so the cost is linear in the bones (at most 128), a few multiplications each and step. Only bones with `r` or `t` set are simulated.

- **Gravity** (the bone's `ge`): the constraint's `gd` times the scene's `gravitystrength` times 980 units/s^2 (assumed: 9.8 m/s^2 at 100 units a metre).
- **Space:** a bone's `gd` is a mesh-space vector. The scene's `gravitydirection` is read and not used by the bones (the constraint's own `gd` is). The wind's `winddirection` is a scene-space vector and is turned into the mesh's space with the layer's rotation, flips and scale each frame (assumption: WE does the same). `gravitystrength`, `windstrength` and the direction vectors are bounded when the scene is loaded.
- **Wind** (the scene's `windenabled`): `winddirection` times `windstrength` times 1 unit/s^2 (assumed) times a gust factor, on every simulated bone. The gust is two slow sine waves (0.35 and 0.9 Hz) whose phases come from `MURASAKI_SCENE_VK_SEED`: the same seed gives the same sway, bit for bit; with no seed it is random per run.
- **Limits** (`la`, `lt`): the offset from the animated pose is clamped and the velocity that goes on outward is dropped.
- **Impulses** (`applyBonePhysicsImpulse(bone, directional, angular)`): added to the bone's velocity at its next step: the directional one (units/s) moves a translating bone, or turns a rotating one about its lever; the angular one (rad/s) a rotating bone. Several impulses add. A bone that does not simulate ignores them. `resetBonePhysicsSimulation(bone)` puts one bone, or all without an argument, back on the animated pose at rest; a bone that does not resolve (unknown name, NaN, out of range) resets nothing. Units are an assumption.
- A puppet with spring bones is skinned even without `animationlayers` (its bones then sway about the bind pose); without them it is unskinned as before.

**Not done, reported.** IK chains (`ik`, 4 bones of the library): the bone keeps the animated pose, `--check` says `puppet bone IK (no goal in the data)` ("IK chains" below). A 3D mesh with a rig whose bones carry springs is read and drawn with the animated pose (`bone physics of a 3D mesh`, `mesh bone IK`). Collision between bones and the unexplained keys above are not read. The stiffness and damping units, gravity and wind scales and the meaning of `ri`/`ti`/`tm`/`se` are assumptions: nothing here was compared with the original engine's behaviour.

## IK chains

What the data says (library survey, 2026-10-21, `.mdl` constraint JSON; the two items with an `ik` bone, 3799253558 and 2979454904, four bones in three models):
- The end of a chain has `ik: true`, `ikd: 2` (ancestors in the chain), `ikg: true`, `ikr: true` with `ikrmin: 0`, `ikrmax: 3.1415927`, `ikrd: 25`, `ikce: false`, in 3799253558 also `ikcp: false`, `ikcpn: 1`, `ikse: true`; the chain's three bones (the end and its two ancestors) are the bones with the skeleton's sim type word 3 (the `u32` after a bone's name; 1 for a plain bone, 0 for a root), which is how a chain is found: in all three models the `st 3` bones are exactly the end and its `ikd` ancestors. The end also has `r: true` (a rotation spring), which is simulated (see above), and `la` with the full turn (no limit).
- **No goal**: `ikcp` (read as "the goal follows a control point", `ikcpn` the point) is false in every bone of the library, and the animations of the chain's bones in these models are constant at their bind pose (checked track by track: the same position and angles on every frame). Nothing says where the chain should reach to, so a solver would reach for where the end already is, which changes nothing. The meaning of `ikg`, `ikr*`, `ikse` is not worked out.
- So it is not implemented and the reason is stated: `--check` reports `puppet bone IK (no goal in the data)`; it would say `puppet bone IK (control point goal)` for a model whose end names a control point. What would be needed: a source of goals (a control point of a particle system, a script, the cursor), and WE's behaviour for it.

## Tests

`tst_scenevk_puppetgen` writes models of any shape (a 12 bone chain with 200 frames, 128 bones, 4096 frames, cycles) and compares the engine's bone matrices at several times and layer mixes (loop, mirror, single, blends below and above one, tracks missing for some bones, unknown animations) with a CPU reference of the rules above.


`tst_scenevk_bonephysics` (the constraint reader and its limits, rest, return to the pose after an impulse, gravity, limits, wind and its seed, the step clock, 128 bones, script impulses, `--check` reasons), `tst_scenevk_puppet` (the reader on models written by the test, truncation sweeps, hostile counts, the bone matrices, loop/mirror/single, blending), `tst_scenevk_render` (the mesh in place and scaled, skinning through `g_Bones`, blends, hidden layers, a puppet with an effect), `tst_scenevk_scene` (loading) and, opt-in with `MURASAKI_TEST_MDL=path`, a real model.
