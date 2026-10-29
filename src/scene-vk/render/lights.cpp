#include "render/lights.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

namespace {

constexpr float kPi = 3.14159265358979f;
// where an unused slot of the older arrays puts its light: far in front of everything, so no distance is zero
constexpr float kFarAway = 1.0e6f;

Vec3 transformPoint(const Mat4 &m, Vec3 p)
{
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)};
}

Vec3 normalized(Vec3 v, Vec3 fallback)
{
    const float n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    return n > 1e-12f && std::isfinite(n) ? Vec3{v.x / n, v.y / n, v.z / n} : fallback;
}

float finiteOr(float v, float fallback)
{
    return std::isfinite(v) ? v : fallback;
}

size_t capOf(LightType type)
{
    return type == LightType::LegacyPoint ? kMaxLegacyLights : kMaxLightsPerType;
}

int shadowCapOf(LightType type)
{
    return type == LightType::Point ? kMaxShadowPointLights
        : type == LightType::Spot   ? kMaxShadowSpotLights
        : type == LightType::Directional ? kMaxShadowDirectionalLights
                                         : 0;
}

} // namespace

bool sceneHasShadows(const Scene &scene)
{
    const bool lightCasts = std::any_of(scene.lights.begin(), scene.lights.end(), [](const SceneLight &l) { return l.castShadow && shadowCapOf(l.type) > 0; });
    const bool meshCasts = std::any_of(scene.meshes.begin(), scene.meshes.end(), [](const MeshLayer &m) { return m.castShadow && m.model; });
    return lightCasts && meshCasts;
}

std::vector<LightSlot> lightSlots(const Scene &scene)
{
    std::vector<LightSlot> slots(scene.lights.size());
    const bool shadows = sceneHasShadows(scene);
    for (LightType type : {LightType::Point, LightType::Spot, LightType::Tube, LightType::Directional, LightType::LegacyPoint}) {
        int next = 0;
        int shadowed = 0;
        // the shadow casters of the type first (as many as the atlas has room for), then the rest, each group in file order
        for (int pass = 0; pass < 2; ++pass) {
            for (size_t i = 0; i < scene.lights.size(); ++i) {
                const SceneLight &l = scene.lights[i];
                if (l.type != type) {
                    continue;
                }
                if (pass == 0 && shadows && l.castShadow && shadowed < shadowCapOf(type)) {
                    slots[i].shadow = true;
                    ++shadowed;
                }
                if ((pass == 0) != slots[i].shadow) {
                    continue;
                }
                slots[i].slot = next < static_cast<int>(capOf(type)) ? next : -1;
                ++next;
            }
        }
    }
    return slots;
}

LightCounts lightCounts(const Scene &scene)
{
    LightCounts c;
    const std::vector<LightSlot> slots = lightSlots(scene);
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        const SceneLight &l = scene.lights[i];
        int &n = l.type == LightType::Point ? c.point
            : l.type == LightType::Spot     ? c.spot
            : l.type == LightType::Tube     ? c.tube
            : l.type == LightType::Directional ? c.directional
                                               : c.legacy;
        n = std::min(n + 1, static_cast<int>(capOf(l.type)));
        if (slots[i].shadow && slots[i].slot >= 0) {
            ++(l.type == LightType::Point ? c.pointShadow : l.type == LightType::Spot ? c.spotShadow : c.directionalShadow);
        }
    }
    return c;
}

std::map<std::string, int> sceneLightCombos(const Scene &scene, int shadowQuality)
{
    std::map<std::string, int> combos;
    const LightCounts c = lightCounts(scene);
    for (const auto &[name, count] : {std::pair{"LIGHTS_POINT", c.point}, std::pair{"LIGHTS_SPOT", c.spot}, std::pair{"LIGHTS_TUBE", c.tube},
                                      std::pair{"LIGHTS_DIRECTIONAL", c.directional}}) {
        if (count > 0) {
            combos[name] = count;
        }
    }
    if (!scene.perspective) {
        combos["SCENE_ORTHO"] = 1;
    }
    if (c.pointShadow + c.spotShadow + c.directionalShadow > 0) {
        combos["LIGHTS_SHADOW_MAPPING"] = 1;
        combos["LIGHTS_SHADOW_MAPPING_QUALITY"] = shadowQuality;
        for (const auto &[name, count] : {std::pair{"LIGHTS_POINT_SHADOW", c.pointShadow}, std::pair{"LIGHTS_SPOT_SHADOW", c.spotShadow},
                                          std::pair{"LIGHTS_DIRECTIONAL_SHADOW", c.directionalShadow}}) {
            if (count > 0) {
                combos[name] = count;
            }
        }
    }
    return combos;
}

LightUniforms packLights(const Scene &scene, const PropertyStore &props)
{
    LightUniforms u;
    u.counts = lightCounts(scene);
    for (Vec4 &p : u.legacyPosition) {
        p = {0.0f, 0.0f, kFarAway, 0.0f};
    }
    std::array<Vec3, kMaxLegacyLights> premultiplied{};
    const std::vector<LightSlot> slots = lightSlots(scene);
    for (size_t index = 0; index < scene.lights.size(); ++index) {
        const SceneLight &l = scene.lights[index];
        if (slots[index].slot < 0) {
            continue; // named "too many lights" by the loader
        }
        const size_t i = static_cast<size_t>(slots[index].slot);
        const bool visible = nodeVisible(scene, l.node, props);
        // the colour the shaders take is the colour times the intensity (an assumption: formats.md, "Lights")
        // (values held to ranges, so that a script or a property cannot make the lighting infinite: kVolumetricMaxFactor and ...MaxRadius)
        const auto held = [](float value, float high) { return std::clamp(finiteOr(value, 0.0f), 0.0f, high); };
        const Vec3 c = resolve(l.color, props);
        const float intensity = visible ? held(resolve(l.intensity, props), kVolumetricMaxFactor) : 0.0f;
        const Vec3 color{held(c.x, kVolumetricMaxFactor) * intensity, held(c.y, kVolumetricMaxFactor) * intensity, held(c.z, kVolumetricMaxFactor) * intensity};
        // a radius of zero would divide by zero in the falloff
        const float radius = std::clamp(finiteOr(resolve(l.radius, props), 1e-3f), 1e-3f, kVolumetricMaxRadius);
        const float exponent = std::max(0.0f, finiteOr(resolve(l.exponent, props), 0.0f));
        const Mat4 world = nodeWorldMatrix(scene, l.node);
        const Vec3 origin = transformPoint(world, {0, 0, 0});
        // a spot shines along its own +x, a directional light too (assumption read from the library: formats.md, "Lights")
        const Vec3 forward = normalized({world.at(0, 0), world.at(1, 0), world.at(2, 0)}, {1, 0, 0});
        switch (l.type) {
        case LightType::Point:
            u.pointColor[i] = {color.x, color.y, color.z, radius};
            u.pointOrigin[i] = {origin.x, origin.y, origin.z, exponent};
            break;
        case LightType::Spot: {
            // half-angles in degrees; the inner edge is kept inside the outer one (smoothstep needs its edges in order)
            const float outer = std::clamp(finiteOr(resolve(l.outerCone, props), 30.0f), 0.0f, 179.0f);
            const float inner = std::clamp(finiteOr(resolve(l.innerCone, props), 20.0f), 0.0f, outer);
            const float cosOuter = std::cos(outer * kPi / 180.0f);
            const float cosInner = std::max(std::cos(inner * kPi / 180.0f), cosOuter + 1e-4f);
            u.spotColor[i] = {color.x, color.y, color.z, radius};
            u.spotOrigin[i] = {origin.x, origin.y, origin.z, cosInner};
            u.spotDirection[i] = {forward.x, forward.y, forward.z, cosOuter};
            u.spotExponent[i] = {exponent, 0.0f, 0.0f, 0.0f};
            break;
        }
        case LightType::Tube: {
            const Vec3 end = transformPoint(world, l.controlPoint.value_or(Vec3{0, 0, 0}));
            u.tubeColor[i] = {color.x, color.y, color.z, radius};
            u.tubeOriginA[i] = {origin.x, origin.y, origin.z, exponent};
            u.tubeOriginB[i] = {end.x, end.y, end.z, 0.0f};
            break;
        }
        case LightType::Directional:
            u.directionalColor[i] = {color.x, color.y, color.z, 0.0f};
            u.directionalDirection[i] = {-forward.x, -forward.y, -forward.z, 0.0f};
            break;
        case LightType::LegacyPoint:
            u.legacyPosition[i] = {origin.x, origin.y, origin.z, 0.0f};
            u.legacyColorRadius[i] = {color.x, color.y, color.z, radius};
            // the colour premultiplied by the intensity only: with the squared radius as well (the other reading the name allows) the
            // shaders that divide by the squared distance wash 2590512455 out, its preview is not (formats.md, "Lights")
            premultiplied[i] = color;
            break;
        }
    }
    for (size_t i = 0; i < 3; ++i) {
        u.legacyPremultiplied[i] = {premultiplied[i].x, premultiplied[i].y, premultiplied[i].z, 0.0f};
    }
    u.legacyPremultiplied[0].w = premultiplied[3].x;
    u.legacyPremultiplied[1].w = premultiplied[3].y;
    u.legacyPremultiplied[2].w = premultiplied[3].z;
    return u;
}

std::vector<VolumetricLight> volumetricLights(const Scene &scene, const PropertyStore &props)
{
    std::vector<VolumetricLight> out;
    const std::vector<LightSlot> slots = lightSlots(scene);
    for (size_t index = 0; index < scene.lights.size(); ++index) {
        const SceneLight &l = scene.lights[index];
        if (!l.castVolumetrics || (l.type != LightType::Point && l.type != LightType::Spot) || slots[index].slot < 0 || !nodeVisible(scene, l.node, props)) {
            continue;
        }
        VolumetricLight v;
        v.type = l.type;
        if (out.size() >= kMaxVolumetricLights) {
            break;
        }
        const auto held = [](float value, float high) { return std::clamp(finiteOr(value, 0.0f), 0.0f, high); };
        v.density = held(resolve(l.density, props), kVolumetricMaxFactor);
        v.intensity = held(resolve(l.intensity, props), kVolumetricMaxFactor);
        v.radius = held(resolve(l.radius, props), kVolumetricMaxRadius);
        if (!(v.density > 0.0f) || !(v.intensity > 0.0f) || !(v.radius > 1e-3f)) {
            continue; // (a density of zero draws no beam)
        }
        const Vec3 c = resolve(l.color, props);
        v.color = {held(c.x, kVolumetricMaxFactor), held(c.y, kVolumetricMaxFactor), held(c.z, kVolumetricMaxFactor)};
        // (not below a thousandth: pow(0, 0) is undefined in GLSL)
        v.exponent = std::clamp(finiteOr(resolve(l.volumetricsExponent, props), 1.0f), 1e-3f, kVolumetricMaxFactor);
        const Mat4 world = nodeWorldMatrix(scene, l.node);
        v.origin = transformPoint(world, {0, 0, 0});
        v.forward = normalized({world.at(0, 0), world.at(1, 0), world.at(2, 0)}, {1, 0, 0});
        if (!std::isfinite(v.origin.x) || !std::isfinite(v.origin.y) || !std::isfinite(v.origin.z)) {
            continue;
        }
        v.shadowSlot = slots[index].shadow ? slots[index].slot : -1;
        float coneHalfAngle = 90.0f;
        if (l.type == LightType::Spot) {
            const float outer = std::clamp(finiteOr(resolve(l.outerCone, props), 30.0f), 0.0f, 179.0f);
            const float inner = std::clamp(finiteOr(resolve(l.innerCone, props), 20.0f), 0.0f, outer);
            v.cosOuter = std::cos(outer * kPi / 180.0f);
            v.cosInner = std::max(std::cos(inner * kPi / 180.0f), v.cosOuter + 1e-4f);
            coneHalfAngle = outer;
        }
        v.sphere = l.type == LightType::Point || coneHalfAngle >= kVolumetricConeLimit;
        if (v.sphere) {
            v.volume = translation(v.origin) * scaling({v.radius, v.radius, v.radius});
        } else {
            // own z along the axis, x and y any two axes square to it; the base circle reaches the cone's edge at the radius
            const Vec3 z = v.forward;
            const Vec3 helper = std::fabs(z.y) < 0.9f ? Vec3{0, 1, 0} : Vec3{1, 0, 0};
            const Vec3 x = normalized({helper.y * z.z - helper.z * z.y, helper.z * z.x - helper.x * z.z, helper.x * z.y - helper.y * z.x}, {1, 0, 0});
            const Vec3 y{z.y * x.z - z.z * x.y, z.z * x.x - z.x * x.z, z.x * x.y - z.y * x.x};
            const float reach = std::tan(coneHalfAngle * kPi / 180.0f) * v.radius;
            Mat4 basis = Mat4::identity();
            const Vec3 axes[3] = {x, y, z};
            const float lengths[3] = {reach, reach, v.radius};
            for (int col = 0; col < 3; ++col) {
                basis.at(0, col) = axes[col].x * lengths[col];
                basis.at(1, col) = axes[col].y * lengths[col];
                basis.at(2, col) = axes[col].z * lengths[col];
            }
            v.volume = translation(v.origin) * basis;
        }
        out.push_back(v);
    }
    return out;
}

} // namespace murasaki::scene
