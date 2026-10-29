#pragma once

// Shadows: where each shadow-casting light's depth pictures go in the shadow atlas, the matrices the shadow casters are drawn with
// and the values the lit shaders read to find a point's depth again (docs/scene-vk/formats.md, "Shadows"). Pure arithmetic, no
// Vulkan state: tst_scenevk_uniforms checks it directly.
//
// The atlas (`_rt_shadowAtlas`, WE's name for it) is one depth image four cells wide (ShadowSettings::cell pixels a cell: 512, or
// 256 while the render scale is reduced); every light takes blocks of cells, in slot order (point, spot, directional), each at the
// first free place scanning the rows from the top:
//   point        2 x 3 cells: the six cube faces, as the shipped CalculateProjectedCoordsPoint lays them out (columns +, -; rows
//                x, y, z), each a cell square
//   spot         2 x 2 cells: one perspective picture along the cone
//   directional  2 x 2 cells for each of the two near cascades, 1 x 1 for the far one (its texels are large anyway)
// The atlas is as tall as the rows used; it depends only on the counts and the cell, so it is constant for a scene (until the render
// scale crosses 1). Size and layout are this engine's choice: WE's are not known.
//
// What the lit shaders read (the lighting function of shaders/translate.cpp, with the shipped helpers of common_pbr_2.h):
//   g_LFeature_ShadowPointProjection[i]          (-f / (f - n), -n f / (f - n), -1, 0): the face's projection after the cube face
//                                                 view the helper builds (depth n..f to 0..1, w = the distance along the face's axis)
//   g_LFeature_ShadowPointProjectionTransform[i] the light's block in the atlas: (u, v of its top left corner, width, height)
//   g_LFeature_ShadowProjection[i]               a spot's view-projection (world to clip, x y in -1..1 y up, depth 0..1)
//   g_LFeature_ShadowProjectionTransform[i]      its block
//   g_LFeature_ShadowCascadeProjection[3 i + k]  cascade k of directional light i (orthographic, the same conventions)
//   g_LFeature_ShadowCascadeTransform[3 i + k]   its block
//   g_LFeature_ShadowCascadeTexel[3 i + k]       (the world size of one of its texels, 0, 0, 0): the lighting function moves the point
//                                                 that far along its normal before it looks the cascade up (a normal offset bias)
// (WE's names for the point and spot arrays; the cascade ones are this engine's, WE's are not known.)

#include "render/lights.h"
#include "scene/properties.h"
#include "scene/scene.h"
#include "util/math.h"

#include <array>
#include <cstdint>
#include <vector>

namespace murasaki::scene {

inline constexpr uint32_t kShadowCell = 512;
inline constexpr uint32_t kShadowAtlasWidth = 4 * kShadowCell;
inline constexpr int kShadowCascades = 3;

// What the engine's quality ladder sets (SceneRenderer::setRenderScale): the cell size and LIGHTS_SHADOW_MAPPING_QUALITY.
struct ShadowSettings {
    uint32_t cell = kShadowCell;
    int quality = kShadowQuality;
};
// The point faces are drawn with a field of view a little wider than 90 degrees: the shipped helper reads a face at this share of
// its half-size (0.47 at quality 1 and 2, 0.48 at 3, 0.49 above), so the filter's taps stay inside the face.
float shadowPointCompensation(int quality);

struct ShadowUniforms {
    std::array<Vec4, kMaxShadowPointLights> pointProjection{}, pointTransform{};
    std::array<Mat4, kMaxShadowSpotLights> spotProjection{};
    std::array<Vec4, kMaxShadowSpotLights> spotTransform{};
    std::array<Mat4, kShadowCascades * kMaxShadowDirectionalLights> cascadeProjection{};
    std::array<Vec4, kShadowCascades * kMaxShadowDirectionalLights> cascadeTransform{};
    std::array<Vec4, kShadowCascades * kMaxShadowDirectionalLights> cascadeTexel{};
};

// One picture a light's casters are drawn into: the matrix (world to OpenGL clip space, what the caster shader's
// g_ViewportViewProjectionMatrices holds) and the viewport, in atlas pixels (top left origin).
struct ShadowView {
    Mat4 viewProjection = Mat4::identity();
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

// A rectangle of atlas pixels (top left origin).
struct ShadowBlock {
    float x = 0.0f, y = 0.0f, width = 0.0f, height = 0.0f;
};

// The pictures of one light: six for a point light (+x, -x, +y, -y, +z, -z), one for a spot, three for a directional light.
struct ShadowLightViews {
    std::vector<ShadowView> views;
    ShadowBlock block; // a point light's whole block (its six faces)
};

// What a shadow plan needs of the camera (directional cascades follow the view): its view-projection (world to OpenGL clip) and
// the distances of its near and far planes from the eye along the view (an orthographic view: 0 and its depth).
struct ShadowCamera {
    Mat4 viewProjection = Mat4::identity();
    float nearDistance = 0.01f;   // where the first cascade starts
    float farDistance = 10000.0f;
    // The distances of the view's depth range (clip z -1 and 1) when it does not start at nearDistance (an orthographic view's
    // cascades start at the scene's plane, in the middle of its range); negative: nearDistance and farDistance.
    float clipNear = -1.0f;
    float clipFar = -1.0f;
};

// An axis-aligned box in world space (the shadow casters' bounds); empty when nothing casts.
struct ShadowBounds {
    Vec3 min{0.0f, 0.0f, 0.0f}, max{0.0f, 0.0f, 0.0f};
    bool empty = true;
    void add(Vec3 p);
};

struct ShadowPlan {
    uint32_t atlasWidth = 0, atlasHeight = 0; // 0: no light casts a shadow
    ShadowUniforms uniforms;
    std::vector<ShadowLightViews> lights; // the shadowed lights in slot order (point, spot, directional); a hidden one has no views
};

// The plan of this frame: blocks, matrices and the receivers' values, for the lights as packed (render/lights.h) and the casters'
// bounds. The cascades split the camera's view at the light's cascadedistance0..2 (sorted; unset: kDefaultCascadeDistances), each
// fitted with a box around its slice of the view (a sphere, so the box does not change size as the camera turns), snapped to the
// atlas's pixels so a moving camera does not make the edges crawl, and deep enough towards the light to hold every caster.
inline constexpr std::array<float, 3> kDefaultCascadeDistances{1152.0f, 2304.0f, 4224.0f};
// The depth ranges are rounded outwards to coarse steps (a point light's and a spot's to an eighth of the power of two below them,
// a cascade's to its box's radius), so a caster or a camera that drifts does not change the depth mapping every frame.
ShadowPlan planShadows(const Scene &scene, const PropertyStore &props, const LightUniforms &lights, const ShadowCamera &camera,
                       const ShadowBounds &casters, const ShadowSettings &settings = {});

// The matrix that takes OpenGL clip space (depth -1..1, what the caster shaders write and the translated vertex stage takes to
// Vulkan's 0..1) to the depth 0..1 the receivers compare against: z' = (z + w) / 2.
Mat4 depthGlToZeroOne();

} // namespace murasaki::scene
