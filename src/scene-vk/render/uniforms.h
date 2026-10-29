#pragma once

// Filling a translated shader's uniform blocks. A uniform's value comes from, in order:
//   1. an explicit override of the draw (the engine's per-draw values, for example the tint of a composite);
//   2. the engine inputs below (time, pointer, matrices, texture sizes, ...), recognised by name;
//   3. the material's constants (`constantshadervalues`), found by the uniform's `material` annotation, with a user
//      property binding resolved against the live properties;
//   4. the annotation's `default`;
//   5. zero.
// Names the engine fills (the uniforms of the shipped shaders that carry no `material` annotation):
//   g_Time, g_Frametime, g_Daytime, g_PointerPosition (top-left origin, 0..1 over the scene), g_PointerPositionLast,
//   g_PointerState, g_ModelViewProjectionMatrix (+Inverse), g_ModelMatrix, g_NormalModelMatrix (its inverse transpose), g_ViewProjectionMatrix,
//   g_EffectTextureProjectionMatrix (+Inverse, identity), g_ParallaxPosition, g_Screen, g_EyePosition, g_ViewForward / Right / Up,
//   g_AltViewProjectionMatrix (the view-projection), g_OrientationRight / Up / Forward (the camera's axes unless the draw sets them),
//   g_LightAmbientColor / g_LightSkylightColor, the light arrays of render/lights.h (g_LPoint_*, g_LSpot_*, g_LTube_*,
//   g_LDirectional_*, g_LightsPosition, g_LightsColorRadius, g_LightsColorPremultiplied), the shadow values of render/shadows.h
//   (g_LFeature_Shadow*), g_ViewportViewProjectionMatrices (shadow casters), g_Texture<N>Texel (1 / size and size of slot N),
//   g_Texture<N>Resolution (texture width, height, picture width, height), g_Texture0Translation/Rotation (the
//   sprite-sheet frame placement; identity unless the draw sets it), g_AudioSpectrum{16,32,64}{Left,Right} (zeros
//   until audio exists, M4), g_FogDistanceColor / Params and g_FogHeightColor / Params (the scene's fog).

#include "render/lights.h"
#include "render/shadows.h"
#include "scene/material.h"
#include "scene/properties.h"
#include "shaders/translate.h"
#include "audio/spectrum.h"
#include "util/math.h"

#include <array>
#include <optional>
#include <cstdint>
#include <map>
#include <vector>
#include <set>
#include <string>

namespace murasaki::scene {

constexpr uint32_t kMaxTextureSlots = 9; // g_Texture0 .. g_Texture8

struct TextureInfo {
    uint32_t width = 1;       // storage
    uint32_t height = 1;
    uint32_t imageWidth = 1;  // picture inside it
    uint32_t imageHeight = 1;
    uint32_t mips = 1;        // mip levels of the storage (1: no chain)
};

struct FrameInputs {
    double time = 0.0;       // seconds since the engine started
    double frametime = 1.0 / 30.0;
    float daytime = 0.0f;    // 0..1 over the local day
    Vec2 pointer{0.5f, 0.5f}; // top-left origin, 0..1 over the scene
    Vec2 pointerLast{0.5f, 0.5f};
    uint32_t buttons = 0;
    Vec2 parallax{0.0f, 0.0f};
    Vec3 screen{1.0f, 1.0f, 1.0f}; // width, height, width / height of the scene target
    Vec3 eye{0.0f, 0.0f, 1.0f};        // the camera's position (g_EyePosition) and axes (g_ViewForward, g_ViewRight, g_ViewUp; the
    Vec3 viewForward{0.0f, 0.0f, -1.0f}; // particles' default g_OrientationRight / Up / Forward face them: right, up and -forward)
    Vec3 viewRight{1.0f, 0.0f, 0.0f};
    Vec3 viewUp{0.0f, 1.0f, 0.0f};
    Vec2 texel{1.0f, 1.0f};  // 1 / the scene buffer's size in pixels
    Vec3 ambient{0.2f, 0.2f, 0.2f};  // the scene's ambientcolor and skylightcolor (lit materials: g_LightAmbientColor, ...)
    Vec3 skylight{0.3f, 0.3f, 0.3f};
    // The scene's fog bands (g_FogDistanceColor / Params, g_FogHeightColor / Params): params are (start, end - start, start
    // density, end density - start density), what WE's fog code reads (docs/scene-vk/formats.md, "Fog").
    Vec3 fogDistanceColor{0.0f, 0.0f, 0.0f};
    Vec4 fogDistanceParams{0.0f, 1.0f, 0.0f, 0.0f};
    Vec3 fogHeightColor{0.0f, 0.0f, 0.0f};
    Vec4 fogHeightParams{0.0f, 1.0f, 0.0f, 0.0f};
    const LightUniforms *lights = nullptr; // the scene's lights this frame (g_LPoint_Color, ...); null: no light reaches anything
    const ShadowUniforms *shadows = nullptr; // where the lit shaders find the lights' shadows in the atlas (g_LFeature_Shadow*)
    AudioSpectrum audio;     // g_AudioSpectrum16/32/64Left/Right
};

struct UniformContext {
    const MaterialPass *material = nullptr;
    const PropertyStore *properties = nullptr;
    FrameInputs inputs;
    Mat4 mvp = Mat4::identity();
    Mat4 model = Mat4::identity();
    Mat4 viewProjection = Mat4::identity();
    // Puppet shaders: the skinning matrices (16 floats a bone, the layout of a std140 mat4x3), null without a skeleton.
    const std::vector<float> *bones = nullptr;
    // Shadow casters: the matrices of the pictures the draw goes to (g_ViewportViewProjectionMatrices), null otherwise.
    const std::array<Mat4, 6> *viewportViewProjections = nullptr;
    Vec4 renderVar0;                        // particle shaders: g_RenderVar0 / g_RenderVar1
    Vec4 renderVar1;
    // Where the layer sits on screen, for shaders that turn the pointer into layer coordinates through the inverse
    // matrices while their own quad is drawn into a buffer. Unset: `mvp` is the screen matrix.
    std::optional<Mat4> screenMvp;
    // Effect passes draw the layer's quad in the layer's own units, centred (a_Position runs from -size / 2 to size / 2 as in WE, whose
    // shaders move vertices by amounts in pixels); the clip square is the default.
    Vec2 quadSize{2.0f, 2.0f};
    std::array<TextureInfo, kMaxTextureSlots> textures{};
    std::array<float, 4> spriteRotation{1.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 2> spriteTranslation{0.0f, 0.0f};
    std::map<std::string, std::array<float, 4>> overrides;
    std::map<std::string, Mat4> matrixOverrides; // the same for 4 x 4 matrices (the volumetric lights' g_AltViewProjectionMatrix, ...)
};

// Writes every uniform of `stage` into `block` (stage.blockSize bytes, zeroed first). `unknown`, when given, collects
// the names of uniforms nothing could fill (they stay zero).
void fillUniforms(const StageProgram &stage, const UniformContext &context, uint8_t *block, std::set<std::string> *unknown = nullptr);

// Whether a uniform name makes a shader depend on something that changes from frame to frame.
bool changesEveryFrame(const std::string &uniformName);
bool dependsOnPointer(const std::string &uniformName);
// Whether an engine input is one of the light arrays (render/lights.h).
bool readsLight(EngineInput input);

} // namespace murasaki::scene
