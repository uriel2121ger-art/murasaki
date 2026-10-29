# M5-3D survey

492 scene items, 21 use a perspective camera, lights or meshes.

- perspective camera: 10 (with meshes 9, only flat layers 1; camera paths 0)
- lights: 17 (in orthographic scenes 9)
- meshes: 13 items, 228 mesh objects
- light types: {'point': 4, 'lspot': 25, 'lpoint': 13, 'ltube': 4, 'ldirectional': 2}; casting shadows 11, volumetrics 11; lights per item max 13
- lit shaders: genericimage4 LIGHTING x2

| id | title | persp | lights | meshes | flat layers | lit shaders |
|---|---|---|---|---|---|---|
| 2244339517 | Passing Breeze - Synthwave (3D) | yes | point | 65 | 5 | 0 |
| 2590512455 | Brutal DOOM |  | point,point,point | 0 | 0 | 0 |
| 3164411828 | Halo 3 Spinning Rat |  | lspot,lspot,lspot,lspot,lspot,lspot,lspot,lspot,lspot,lspot,lspot,lspot,lspot | 0 | 0 | 0 |
| 3217188597 | 田园小屋2 |  | lpoint | 0 | 0 | 0 |
| 3270865076 | Gwen Stacy On a Panel [4K] |  | ltube,ltube,ltube | 0 | 0 | 0 |
| 3287715210 | 发光少女 4K动态壁纸 |  | lpoint | 0 | 0 | 0 |
| 3354366708 | 【Customize自定义】Hatsune Miku 初音未来 星河沉梦——夜莺 |  |  | 3 | 0 | 0 |
| 3417919390 | All My Fellas 1080p |  | lpoint,lpoint | 1 | 0 | 0 |
| 3429481890 | Majora's Mask 3D: Full 3 Day Circle / Cl |  | lpoint,lspot,ltube | 2 | 0 | 0 |
| 3465215190 | Rainy Day |  | lpoint,lpoint | 0 | 0 | 0 |
| 3477054430 | Cat with headphones on the roof | yes | lspot,lspot,lspot,lspot | 5 | 4 | 0 |
| 3495221617 | Cave Story Outerwall 3D | yes | lpoint | 1 | 2 | 0 |
| 3503626457 | wario | yes | lpoint,lpoint | 20 | 3 | 0 |
| 3509243656 | 三体实时演算 | Three-Body problem - SYKM | yes |  | 8 | 59 | 0 |
| 3589454154 | 土星 | Saturn - Sykm | yes | lpoint,ldirectional | 24 | 56 | 0 |
| 3662790108 | 实时太阳系 Live Solar System - SYKM | yes | lpoint | 72 | 346 | 1 |
| 3772288941 | Cecilia Immergreen (hololive) |  |  | 1 | 0 | 0 |
| 3777414605 | Abandoned Church | yes | lspot | 0 | 1 | 0 |
| 3812243386 | Saturn - Vishal | yes | lpoint,ldirectional | 24 | 56 | 0 |
| 3815542599 | Dystopic City Port [4K, Ambient] |  | lspot,lspot,lspot,lspot,lspot,lspot | 0 | 0 | 0 |
| 3816342243 | Ягодка Сандбокс: Лоадскрин | yes |  | 2 | 1 | 0 |

## What the 22 refusals need (int6, 2026-10-20)

`tools/compat/m5survey.py` against `compat/int6-realpath`; 3732632761 is added by hand (an `.mdl` with a skeleton as a 3D object).

| Piece | Items | Needs |
|---|---|---|
| A. volumetric lights in orthographic scenes | 3217188597, 3287715210, 3465215190, 3815542599 | no layer is lit; the lights only cast volumetrics (and shadows in two) |
| B. 2D lit layers | 2590512455, 3270865076 | `genericimage2`/`genericimage4` with `LIGHTING` (normal maps), `point` and `ltube` lights |
| C. `generic4` meshes, orthographic | 3354366708, 3772288941, 3164411828, 3417919390, 3429481890 | `"model": "x.mdl"` objects, depth buffer, `generic4`, lights |
| D. perspective camera | 2244339517, 3477054430, 3495221617, 3503626457, 3509243656, 3589454154, 3662790108, 3777414605, 3812243386, 3816342243 | C plus the perspective projection, camera orientation, `FOG`, `REFLECTION`; up to 72 meshes and 346 layers |
| E. skinned 3D mesh | 3732632761 | C plus the puppet skeleton and animation layers |

Mesh materials: `generic4` 469 passes (combos `LIGHTING`, `REFLECTION`, `FOG`, `RIMLIGHTING`); the rest are one scene's own
shaders (2244339517: `fug_*`, `grid`, `neonmoon`, `neonsun`, with `BLENDMODE`) and `chroma4` (3816342243).

## Status

- **Piece 3, meshes (2026-10-20, feat/univ-b).** `"model": "x.mdl"` objects load and draw: static MDLV0021/0023 models of any number of parts, each with its own material (`generic4` translated with its combos), the scene's depth buffer, `depthtest` / `depthwrite` / `cullmode`, file order (docs/scene-vk/formats.md, Meshes). Of the C items, 3354366708 and 3772288941 (no lights) are now drawn: `--check` accepts them (3354366708 is degraded only by its `camera fade`). 3417919390 and 3429481890 are refused by their lights only (piece 1); 3164411828 by its lights and its skinned rat (`skinned 3D mesh`, piece 8), like 3732632761 (E). In the perspective items (D) the meshes now load except the MDLV0007 (2244339517) and MDLV0016 (3509243656) models, the skinned ones, and parts textured by a layer's composite (`_rt_imageLayerComposite_*` on a mesh: 3509243656, 3662790108).

## Status: fog and HDR (piece 5, 2026-10-20, feat/univ-c)

Done: `general.fogdistance*` / `fogheight*` read (user-bindable) and drawn for every pass whose `FOG` combo is on (meshes, image
layers, particles) in perspective scenes; HDR scenes draw in RGBA16F buffers with WE's HDR bloom chain (`hdr_downsample`,
`combine_hdr` from `materials/util`) and clamp into the 8 bit output (formats.md, "Fog" and "HDR scenes"). "hdr", "hdr bloom" and
"combo fog" are no longer reasons: `--check` names neither on any of the 37 HDR items; "general fog" remains only for an
orthographic scene (none in the library). With the lights merged (feat/univ-a), 3477054430 is drawn with its fog; 3495221617
and 3777414605 stay refused for their skinned meshes only (piece 8). In the lights table below, `hdr bloom`, `combo fog` and
`general fog` were measured before this merge and are no longer named. Assumed, not established: the fog params' mapping, FOG_COMPUTED with any band, the bloom's tap
offsets, knee and strength normalisation.

## Status: lights and 2D lit layers (pieces 1 and 2, 2026-10-20, feat/univ-a)

Done: light objects of every type load (formats.md, "Lights"), the light arrays are filled per frame in world space with the
counts as combos, the engine's own `PerformLighting_V1` adds one shipped `common_pbr_2.h` term per light, and `genericimage2/3/4`
layers with `LIGHTING` are lit where they are in the scene (also through their effect buffers). "lights: M5", "light config" and
"combo lighting" are gone; what is not drawn is named: `shadows`, `volumetrics`, `light cookie`, `light control point`, `light
script`, `light animation`, `light binding`, `light scale`, `tube without end`, `too many lights`. `--check` on the 15 light items:

| id | verdict | degraded |
|---|---|---|
| 2590512455 | degraded | combo reflection |
| 3217188597 | degraded | volumetrics |
| 3270865076 | degraded | general zoom, combo reflection, media thumbnail |
| 3287715210 | degraded | hdr bloom, shadows, volumetrics |
| 3417919390 | complete | |
| 3429481890 | degraded | light scale, combo fog, fbo uvs |
| 3465215190 | degraded | particle renderer flags, shadows, volumetrics, media thumbnail |
| 3477054430 | degraded | general fog, media thumbnail, light scale |
| 3503626457 | degraded | camera fade, particle renderer flags, shadows |
| 3589454154 | degraded | hdr bloom, shadows, user shortcuts |
| 3812243386 | degraded | hdr bloom, shadows, user shortcuts |
| 3164411828 | refused | skinned 3D mesh (piece 8) |
| 3495221617 | refused | skinned 3D mesh (piece 8); general fog, shadows |
| 3777414605 | refused | skinned 3D mesh (piece 8); general fog, camera fade, shadows, volumetrics |
| 3815542599 | degraded | volumetrics, fbo uvs, script error (its `Spark.alpha/rate` scripts are a pasted JSON fragment: they never run, the values stay) |

12 of the 15 are accepted (after the review fixes: `light scale` named, a script that does not compile no longer refuses). Snapshots against the previews (`refshots.py`, ~/.cache/murasaki-tests/compat/p2d): 2590512455 decided
the older lights' premultiplied colour (the plain colour * intensity; with the squared radius the picture washes out), the
tubes of 3270865076 light the sign as in the preview; 3417919390 and 3477054430 have previews that start on a black frame.

## Status: perspective camera (piece 4, 2026-10-20)

Done: the projection (vertical fov, nearz/farz, the 16:9 canvas), the camera object as the camera (origin, angles, fov, zoom,
visibility), the `camera` block with any orientation (also turning an orthographic scene), camera paths with `queuemode`,
parallax and shake in perspective, layers placed in 3D, the view uniforms (formats.md, "Camera"). "perspective camera" and
"camera orientation" are no longer reasons; the 10 items stay refused for lights and meshes (pieces 1 and 3). Checked with
`MURASAKI_SCENE_VK_FORCE` snapshots against the previews: 3816342243 (clock at the top, snow), 3662790108 (the loading screen
fills the picture), 3509243656 and 3812243386 (the HUD rows land where the previews have them), 3503626457 (the date hung on the
camera follows its path).

## Status: skinned meshes, older versions, layer composites (piece 8, 2026-10-20, feat/univ-b)

Done (formats.md, Meshes; puppets.md): skinned `.mdl` models as 3D objects (the puppet vertex in every part, the puppet's skeleton
and animations after the parts, `animationlayers` on the object, `SKINNING`/`BONECOUNT`/`g_Bones` for `generic4`); static
MDLV0007 and MDLV0016 models (no bounds before 19, no per-part vertex format before 16, the 32 byte vertex of flags 0x0b);
`_rt_imageLayerComposite_<id>_a` on a mesh (the hidden source layer is rendered for the mesh); FBX clip animations with an empty
mode (they were dropped: the rat of 3164411828 played nothing). `--check`: 3732632761 and 3509243656 are accepted (3509243656
degraded by `hdr bloom`, `combo fog`); 3164411828, 3495221617, 3662790108 and 3777414605 are refused by their lights only;
2244339517 loads its MDLV0007 models and is refused by its own shaders (`BLENDMODE`, `SHADING`), `_rt_Reflection` and lights.
Checked with `MURASAKI_SCENE_VK_FORCE` snapshots: 3732632761's globe turns with its bone (the cracks move between 2 s and 10 s),
3164411828's rat spins about its vertical axis (drawn unlit, it is black-grey without its 13 spot lights).

Still open: morph targets (none in a mesh object; formats.md says what driving them needs); MDAT on meshes (none). The sky sphere of
3509243656 covered its clock and HUD from 7 s in file order; `transparentsorting` (read since, formats.md "Depth and order":
opaque meshes first, the rest back to front) puts them in front of it.

## Status: shadows (piece 6, 2026-10-20, feat/univ-a)

Done (formats.md, "Shadows"): point, spot and directional lights with `castshadow` draw shadows. The casters are the meshes (on
unless the object says `"castshadow": false`, read from the library), drawn with their material's caster (`// [PASS] shadow`, else
the shipped `shadowcaster`) into a depth atlas of this engine's layout (2048 wide, blocks of 512-pixel cells: six cube faces per
point light, one picture per spot, three cascades per directional light); the lit shaders get `LIGHTS_SHADOW_MAPPING`,
`LIGHTS_SHADOW_MAPPING_QUALITY` 2 and the shadowed counts, and the engine's `PerformLighting_V1` takes each shadowed light's factor
through the shipped helpers (`CalculateProjectedCoordsPoint` / `PerformPointShadowMapping`, `CalculateProjectedCoords` /
`PerformShadowMapping`, `CalculateProjectedCoordsCascades`). The atlas is drawn picture by picture, only where a caster moved by half
a texel or more. `shadows` is no longer named; what is not drawn is: `tube light shadows`, `older point light shadows`, `too many
shadowed lights`, `shadows from 2D layers`, `shadows on particles`, `shadow caster <shader>`. `--check` on the items whose lights
cast: 3495221617 complete; 3589454154 and 3812243386 degraded (`user shortcuts`); 3503626457 (`camera fade`, `particle renderer
flags`); 3777414605 (`camera fade`, `volumetrics`); 3287715210 (`volumetrics`) and 3465215190 (`particle renderer flags`,
`volumetrics`, `media thumbnail`) have no mesh, so nothing casts and nothing is named for shadows. Snapshots (`refshots.py`,
~/.cache/murasaki-tests/compat/p2g; HSV distance to the preview 0.121-0.617; 3777414605 0.431 -> 0.121, 3812243386 0.314 -> 0.167, 3503626457 0.609 -> 0.617 against
the p2e run, which predates the fog and HDR merge as well): the
church of 3777414605 under its spot and Wario's level of 3503626457 under its two point lights show no acne and no stray dark areas.
GPU cost (formats.md, "Shadows"): 3495221617 12.3 -> 14.3 ms, 3589454154 17.7 -> 33.4 ms a frame at 1920 x 1080.

Still open: what WE's own atlas size, filtering and quality setting are; whether `cascadedistance0..2` split the view as read here
(the library writes them increasing: 0.3 / 0.4 / 8, 556 / 1112 / 2039); shadows on particles (their shaders read the atlas in slot
4, which the particle draw does not bind); the cascades' cost in 3589454154.
