#pragma once

// What the scene's lights give the lit materials: the light arrays of WE's shader interface, filled per frame in world space (the
// camera plays no part), and the combos that size them.
//
//   lpoint        g_LPoint_Color[i] = (colour * intensity, radius)       g_LPoint_Origin[i] = (world origin, exponent)
//   lspot         g_LSpot_Color[i]  = (colour * intensity, radius)       g_LSpot_Origin[i]  = (world origin, cos(inner cone))
//                 g_LSpot_Direction[i] = (where it shines, cos(outer cone))  g_LSpot_Exponent[i] = (exponent, 0, 0, 0)
//   ltube         g_LTube_Color[i]  = (colour * intensity, radius)       g_LTube_OriginA[i] = (world origin, exponent)
//                 g_LTube_OriginB[i] = (the control point in world space, 0)
//   ldirectional  g_LDirectional_Color[i] = (colour * intensity, 0)      g_LDirectional_Direction[i] = (towards the light, 0)
//   point (older) g_LightsPosition[4] = world origins, g_LightsColorRadius[4] = (colour * intensity, radius),
//                 g_LightsColorPremultiplied[3] = colour * intensity of lights 0..2 in .rgb and of light 3 spread over the three .w
//                 (the shaders that read it divide by the squared distance)
//
// A light keeps its slot whatever happens to the others; a hidden one gives black. The slots of a type are the lights that cast
// shadows first (at most kMaxShadowLights of the type, file order), then the others in file order: the lighting function shadows
// slots 0 .. LIGHTS_<TYPE>_SHADOW - 1 (render/shadows.h, docs/scene-vk/formats.md "Shadows"). The counts are the scene's (`LIGHTS_POINT`, `LIGHTS_SPOT`, `LIGHTS_TUBE`, `LIGHTS_DIRECTIONAL`, at most
// kMaxLightsPerType each), so showing or hiding a light never recompiles a shader. Which local axis a spot or directional light
// shines along, the premultiplied intensity and the cone reading are assumptions (formats.md, "Lights").

#include "scene/properties.h"
#include "scene/scene.h"

#include <array>
#include <map>
#include <string>
#include <vector>

namespace murasaki::scene {

// LIGHTS_SHADOW_MAPPING_QUALITY: WE's shaders know 1 (one comparison), 2 and 3 (nine); the engine draws with 2.
inline constexpr int kShadowQuality = 2;

struct LightCounts {
    int point = 0;
    int spot = 0;
    int tube = 0;
    int directional = 0;
    int legacy = 0; // the older `point` lights (g_LightsPosition[4]: at most kMaxLegacyLights)
    // the lights of each type whose shadows are drawn (slots 0 .. n - 1 of the type): none unless the scene has a shadow caster
    int pointShadow = 0;
    int spotShadow = 0;
    int directionalShadow = 0;
};
LightCounts lightCounts(const Scene &scene);

// Whether the scene draws shadows: a point, spot or directional light casts them and a mesh object casts (castShadow).
bool sceneHasShadows(const Scene &scene);
// The slot of each light of the scene (by index into scene.lights) among the lights of its type, -1 for one beyond the limit;
// and whether its shadow is drawn.
struct LightSlot {
    int slot = -1;
    bool shadow = false;
};
std::vector<LightSlot> lightSlots(const Scene &scene);

// The combos the scene sets on every shader that reads the lights: the counts that are not zero and, in an orthographic scene,
// SCENE_ORTHO (the shipped genericimage3/4 then take a fixed view vector, (0, 0, 1)). A scene with shadows adds LIGHTS_SHADOW_MAPPING,
// LIGHTS_SHADOW_MAPPING_QUALITY and the shadowed counts LIGHTS_POINT_SHADOW, LIGHTS_SPOT_SHADOW, LIGHTS_DIRECTIONAL_SHADOW.
std::map<std::string, int> sceneLightCombos(const Scene &scene, int shadowQuality = kShadowQuality);

struct LightUniforms {
    LightCounts counts;
    std::array<Vec4, kMaxLightsPerType> pointColor{}, pointOrigin{};
    std::array<Vec4, kMaxLightsPerType> spotColor{}, spotOrigin{}, spotDirection{}, spotExponent{};
    std::array<Vec4, kMaxLightsPerType> tubeColor{}, tubeOriginA{}, tubeOriginB{};
    std::array<Vec4, kMaxLightsPerType> directionalColor{}, directionalDirection{};
    std::array<Vec4, kMaxLegacyLights> legacyPosition{}, legacyColorRadius{};
    std::array<Vec4, 3> legacyPremultiplied{};
};

// The scene's lights as they are now (nodes after keyframes and scripts, user properties resolved).
LightUniforms packLights(const Scene &scene, const PropertyStore &props);

// A light that draws its beam in the air (castvolumetrics; point and spot lights): the values WE's volumetrics shaders take as
// g_RenderVar1..4 and the volume they are drawn over, a unit sphere or (a spot narrower than kVolumetricConeLimit degrees) a unit cone
// with its apex at the origin and its base circle at z = 1, placed by `volume` (own space to world: the radius and, for a cone, the
// base's width are in it). docs/scene-vk/formats.md, "Volumetric lights", has the readings that are assumptions.
inline constexpr float kVolumetricConeLimit = 80.0f; // half-angle in degrees from which a spot's volume is its whole sphere
// Beams drawn a frame (the library's most is 13 lights in an item; the rest are named "too many volumetric lights" by nothing: they are
// simply not drawn) and the ranges the values are held to, so that a wild property cannot produce infinities or an undefined pow(0, 0).
inline constexpr size_t kMaxVolumetricLights = 16;
inline constexpr float kVolumetricMaxRadius = 1e5f;
inline constexpr float kVolumetricMaxFactor = 1e3f; // density, intensity, each colour component and the exponent
struct VolumetricLight {
    LightType type = LightType::Point;
    Vec3 origin;
    Vec3 forward;         // a spot's axis
    Vec3 color;           // the light's colour (not multiplied by the intensity)
    float intensity = 1.0f;
    float radius = 1.0f;
    float exponent = 1.0f; // volumetricsexponent: the falloff towards the radius
    float density = 1.0f;
    float cosInner = 1.0f, cosOuter = 0.0f;
    int shadowSlot = -1;   // the light's index in the shadow arrays of its type (render/shadows.h), -1: it casts no shadow
    bool sphere = true;
    Mat4 volume = Mat4::identity();
};
// The lights that draw a beam this frame, in file order (at most kMaxVolumetricLights): visible, with a positive density, intensity and
// radius, the values held to the ranges above. A light the shadow
// atlas has no room for (or a scene without casters) has no shadow slot.
std::vector<VolumetricLight> volumetricLights(const Scene &scene, const PropertyStore &props);

} // namespace murasaki::scene
