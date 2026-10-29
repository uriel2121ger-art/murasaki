#include "render/uniforms.h"

#include <algorithm>
#include <cstring>

namespace murasaki::scene {

namespace {

struct Value {
    std::array<float, 4> v{};
    int count = 0;
};

// Number of float components in a GLSL type name; 0 when the type is not a plain scalar or vector.
int components(const std::string &type)
{
    if (type == "float" || type == "int" || type == "uint" || type == "bool") {
        return 1;
    }
    if (type == "vec2" || type == "ivec2" || type == "uvec2") {
        return 2;
    }
    if (type == "vec3" || type == "ivec3" || type == "uvec3") {
        return 3;
    }
    if (type == "vec4" || type == "ivec4" || type == "uvec4") {
        return 4;
    }
    return 0;
}

bool integerType(const std::string &type)
{
    return type == "int" || type == "uint" || type == "bool" || type.rfind("ivec", 0) == 0 || type.rfind("uvec", 0) == 0;
}

void writeScalarOrVector(uint8_t *dst, const std::string &type, const Value &value)
{
    const int n = components(type);
    for (int i = 0; i < n; ++i) {
        // A shorter value fills the rest with its last component for scalars splatted to vectors ("1" for a vec3
        // colour reads as grey) and with zero otherwise.
        float f = 0.0f;
        if (i < value.count) {
            f = value.v[static_cast<size_t>(i)];
        } else if (value.count == 1) {
            f = value.v[0];
        }
        if (integerType(type)) {
            if (type[0] == 'u') {
                const uint32_t u = f < 0.0f ? 0u : static_cast<uint32_t>(f);
                std::memcpy(dst + i * 4, &u, 4);
            } else {
                const int32_t s = static_cast<int32_t>(f);
                std::memcpy(dst + i * 4, &s, 4);
            }
        } else {
            std::memcpy(dst + i * 4, &f, 4);
        }
    }
}

// A GLSL matrix as std140 lays it out: one 16-byte slot per column, `rows` floats used in each. Takes the upper left
// block of `m` (column-major, 4 x 4); types are "mat2", "mat3", "mat4" and the "matCxR" forms.
void writeMatrix(uint8_t *dst, const std::string &type, const Mat4 &m)
{
    int columns = 0;
    int rows = 0;
    if (type.size() == 4 && type.compare(0, 3, "mat") == 0 && type[3] >= '2' && type[3] <= '4') {
        columns = rows = type[3] - '0';
    } else if (type.size() == 6 && type.compare(0, 3, "mat") == 0 && type[4] == 'x' && type[3] >= '2' && type[3] <= '4' && type[5] >= '2'
               && type[5] <= '4') {
        columns = type[3] - '0';
        rows = type[5] - '0';
    }
    for (int c = 0; c < columns; ++c) {
        for (int r = 0; r < rows; ++r) {
            std::memcpy(dst + c * 16 + r * 4, &m.m[static_cast<size_t>(c * 4 + r)], 4);
        }
    }
}

Value fromText(const std::string &text)
{
    Value v;
    v.count = parseFloats(text, v.v.data(), 4);
    for (float &f : v.v) {
        if (!std::isfinite(f)) {
            f = 0.0f;
        }
    }
    return v;
}

// A user property as floats; a colour property may be 0..255.
Value fromProperty(const PropertyValue &p, bool color)
{
    Value v;
    if (const bool *b = std::get_if<bool>(&p)) {
        v.v[0] = *b ? 1.0f : 0.0f;
        v.count = 1;
    } else if (const double *d = std::get_if<double>(&p)) {
        v.v[0] = static_cast<float>(*d);
        v.count = 1;
    } else if (const auto *vec = std::get_if<std::vector<double>>(&p)) {
        v.count = static_cast<int>(std::min<size_t>(4, vec->size()));
        for (int i = 0; i < v.count; ++i) {
            v.v[static_cast<size_t>(i)] = static_cast<float>((*vec)[static_cast<size_t>(i)]);
        }
    } else if (const auto *s = std::get_if<std::string>(&p)) {
        v = fromText(*s);
    }
    if (color && v.count >= 3 && (v.v[0] > 1.0f || v.v[1] > 1.0f || v.v[2] > 1.0f)) {
        for (int i = 0; i < std::min(v.count, 3); ++i) {
            v.v[static_cast<size_t>(i)] /= 255.0f;
        }
    }
    return v;
}

// Constants are stored lower-cased (see parseConstants); `key` is the uniform's lower-cased material key.
const Constant *findConstant(const MaterialPass &pass, const std::string &key)
{
    const auto it = pass.constants.find(key);
    return it == pass.constants.end() ? nullptr : &it->second;
}

// One of the light arrays (render/lights.h): every element takes a 16 byte slot in std140, whether the shader declares it a vec4 or
// (g_LightsPosition) a vec3; a declaration without brackets is the first element.
void writeLights(uint8_t *dst, const UniformInfo &u, const LightUniforms &lights)
{
    const auto put = [&](const auto &values) {
        const size_t count = std::min<size_t>(std::max<uint32_t>(1u, u.arrayCount), values.size());
        const size_t floats = u.type == "vec3" ? 3 : u.type == "vec4" ? 4 : 0;
        for (size_t i = 0; i < count && floats > 0; ++i) {
            const float v[4] = {values[i].x, values[i].y, values[i].z, values[i].w};
            std::memcpy(dst + i * 16, v, floats * sizeof(float));
        }
    };
    switch (u.input) {
    case EngineInput::LPointColor: put(lights.pointColor); break;
    case EngineInput::LPointOrigin: put(lights.pointOrigin); break;
    case EngineInput::LSpotColor: put(lights.spotColor); break;
    case EngineInput::LSpotOrigin: put(lights.spotOrigin); break;
    case EngineInput::LSpotDirection: put(lights.spotDirection); break;
    case EngineInput::LSpotExponent: put(lights.spotExponent); break;
    case EngineInput::LTubeColor: put(lights.tubeColor); break;
    case EngineInput::LTubeOriginA: put(lights.tubeOriginA); break;
    case EngineInput::LTubeOriginB: put(lights.tubeOriginB); break;
    case EngineInput::LDirectionalColor: put(lights.directionalColor); break;
    case EngineInput::LDirectionalDirection: put(lights.directionalDirection); break;
    case EngineInput::LightsPosition: put(lights.legacyPosition); break;
    case EngineInput::LightsColorRadius: put(lights.legacyColorRadius); break;
    case EngineInput::LightsColorPremultiplied: put(lights.legacyPremultiplied); break;
    default: break;
    }
}

// One of the shadow arrays (render/shadows.h): vec4 elements take 16 bytes, mat4 elements 64 (std140).
void writeShadows(uint8_t *dst, const UniformInfo &u, const ShadowUniforms &shadows)
{
    const size_t count = std::max<uint32_t>(1u, u.arrayCount);
    const auto vectors = [&](const auto &values) {
        if (u.type != "vec4") {
            return;
        }
        for (size_t i = 0; i < std::min(count, values.size()); ++i) {
            const float v[4] = {values[i].x, values[i].y, values[i].z, values[i].w};
            std::memcpy(dst + i * 16, v, sizeof v);
        }
    };
    const auto matrices = [&](const auto &values) {
        if (u.type != "mat4") {
            return;
        }
        for (size_t i = 0; i < std::min(count, values.size()); ++i) {
            std::memcpy(dst + i * 64, values[i].m.data(), 64);
        }
    };
    switch (u.input) {
    case EngineInput::ShadowPointProjection: vectors(shadows.pointProjection); break;
    case EngineInput::ShadowPointProjectionTransform: vectors(shadows.pointTransform); break;
    case EngineInput::ShadowProjection: matrices(shadows.spotProjection); break;
    case EngineInput::ShadowProjectionTransform: vectors(shadows.spotTransform); break;
    case EngineInput::ShadowCascadeProjection: matrices(shadows.cascadeProjection); break;
    case EngineInput::ShadowCascadeTransform: vectors(shadows.cascadeTransform); break;
    case EngineInput::ShadowCascadeTexel: vectors(shadows.cascadeTexel); break;
    default: break;
    }
}

} // namespace

bool changesEveryFrame(const std::string &name)
{
    return name == "g_Time" || name == "g_Frametime" || name == "g_Daytime" || name.rfind("g_AudioSpectrum", 0) == 0;
}

bool readsLight(EngineInput input)
{
    return input >= EngineInput::LPointColor && input <= EngineInput::ShadowCascadeTexel;
}

bool dependsOnPointer(const std::string &name)
{
    return name == "g_PointerPosition" || name == "g_PointerPositionLast" || name == "g_PointerState" || name == "g_ParallaxPosition";
}

void fillUniforms(const StageProgram &stage, const UniformContext &ctx, uint8_t *block, std::set<std::string> *unknown)
{
    std::memset(block, 0, stage.blockSize);
    const FrameInputs &in = ctx.inputs;
    for (const UniformInfo &u : stage.uniforms) {
        uint8_t *dst = block + u.offset;
        const auto put = [&](Value v) { writeScalarOrVector(dst, u.type, v); };
        const auto scalar = [&](float f) {
            Value v;
            v.v[0] = f;
            v.count = 1;
            put(v);
        };
        const auto vec2 = [&](Vec2 a) {
            Value v;
            v.v = {a.x, a.y, 0, 0};
            v.count = 2;
            put(v);
        };
        const auto vec3 = [&](Vec3 a) {
            Value v;
            v.v = {a.x, a.y, a.z, 0};
            v.count = 3;
            put(v);
        };

        if (!ctx.matrixOverrides.empty() && u.type == "mat4") {
            if (const auto o = ctx.matrixOverrides.find(u.name); o != ctx.matrixOverrides.end()) {
                writeMatrix(dst, u.type, o->second);
                continue;
            }
        }
        if (!ctx.overrides.empty()) {
            if (const auto o = ctx.overrides.find(u.name); o != ctx.overrides.end()) {
                Value v;
                v.v = o->second;
                v.count = components(u.type);
                put(v);
                continue;
            }
        }
        const Mat4 &screen = ctx.screenMvp ? *ctx.screenMvp : ctx.mvp;
        switch (u.input) {
        case EngineInput::Time:
            scalar(static_cast<float>(in.time));
            break;
        case EngineInput::Frametime:
            scalar(static_cast<float>(in.frametime));
            break;
        case EngineInput::Daytime:
            scalar(in.daytime);
            break;
        case EngineInput::PointerPosition:
            vec2(in.pointer);
            break;
        case EngineInput::PointerPositionLast:
            vec2(in.pointerLast);
            break;
        case EngineInput::PointerState: {
            Value v;
            v.v = {in.pointer.x, in.pointer.y, static_cast<float>(in.buttons & 1), static_cast<float>((in.buttons >> 1) & 1)};
            v.count = 4;
            put(v);
            break;
        }
        case EngineInput::ParallaxPosition:
            vec2(in.parallax);
            break;
        case EngineInput::Screen:
            vec3(in.screen);
            break;
        case EngineInput::EyePosition:
            vec3(in.eye);
            break;
        case EngineInput::ModelViewProjection:
            writeMatrix(dst, u.type, ctx.mvp);
            break;
        case EngineInput::ModelViewProjectionInverse: {
            // WE's layer quads are centred on the layer and measured in pixels of its buffer, so the inverse takes the
            // pointer to buffer pixels from the layer's centre, and shaders divide by g_Texture0Resolution to get
            // coordinates in -0.5..0.5. Ours are unit quads from the corner: move the origin and scale by that size.
            const TextureInfo &t0 = ctx.textures[0];
            const float w = t0.width > 0 ? static_cast<float>(t0.width) : 1.0f;
            const float h = t0.height > 0 ? static_cast<float>(t0.height) : 1.0f;
            writeMatrix(dst, u.type, scaling({w, h, 1.0f}) * translation({-0.5f, -0.5f, 0.0f}) * inverse(screen));
            break;
        }
        case EngineInput::ModelMatrix:
            writeMatrix(dst, u.type, ctx.model);
            break;
        case EngineInput::ViewProjection:
        case EngineInput::AltViewProjection:
            writeMatrix(dst, u.type, ctx.viewProjection);
            break;
        case EngineInput::EffectModelViewProjection:
            // the effect's quad is centred and quadSize wide; the screen matrix takes the unit square onto the layer
            writeMatrix(dst, u.type, screen * translation({0.5f, 0.5f, 0.0f}) * scaling({1.0f / ctx.quadSize.x, 1.0f / ctx.quadSize.y, 1.0f}));
            break;
        case EngineInput::TextureReductionScale:
            scalar(1.0f);
            break;
        case EngineInput::EffectTextureProjection:
        case EngineInput::EffectTextureProjectionInverse: {
            // Shaders read the pointer's layer coordinates from the layer's centre (y up, -0.5..0.5) as
            // 0.5 * (inverse * ndc).xy / w, so the inverse is the screen matrix's inverse with the origin moved to
            // the centre and xy doubled.
            const Mat4 inv = scaling({2.0f, 2.0f, 1.0f}) * translation({-0.5f, -0.5f, 0.0f}) * inverse(screen);
            writeMatrix(dst, u.type, u.input == EngineInput::EffectTextureProjection ? inverse(inv) : inv);
            break;
        }
        case EngineInput::TexelSize:
            vec2(in.texel);
            break;
        case EngineInput::TexelSizeHalf:
            vec2({in.texel.x * 0.5f, in.texel.y * 0.5f});
            break;
        case EngineInput::ModelMatrixInverse:
            writeMatrix(dst, u.type, inverse(ctx.model));
            break;
        case EngineInput::NormalModelMatrix: {
            // the inverse transpose of the model matrix's upper 3x3 (a mat3 in std140: three columns padded to four floats)
            const Mat4 inv = inverse(ctx.model);
            Mat4 normal = Mat4::identity();
            for (int row = 0; row < 3; ++row) {
                for (int col = 0; col < 3; ++col) {
                    normal.at(row, col) = inv.at(col, row);
                }
            }
            writeMatrix(dst, u.type, normal);
            break;
        }
        case EngineInput::OrientationRight:
        case EngineInput::ViewRight:
            vec3(in.viewRight);
            break;
        case EngineInput::OrientationUp:
        case EngineInput::ViewUp:
            vec3(in.viewUp);
            break;
        case EngineInput::ViewForward:
            vec3(in.viewForward);
            break;
        case EngineInput::LightAmbientColor:
            vec3(in.ambient);
            break;
        case EngineInput::LightSkylightColor:
            vec3(in.skylight);
            break;
        case EngineInput::FogDistanceColor:
            vec3(in.fogDistanceColor);
            break;
        case EngineInput::FogHeightColor:
            vec3(in.fogHeightColor);
            break;
        case EngineInput::FogDistanceParams:
        case EngineInput::FogHeightParams: {
            const Vec4 &f = u.input == EngineInput::FogDistanceParams ? in.fogDistanceParams : in.fogHeightParams;
            Value v;
            v.v = {f.x, f.y, f.z, f.w};
            v.count = 4;
            put(v);
            break;
        }
        case EngineInput::OrientationForward:
            vec3({-in.viewForward.x, -in.viewForward.y, -in.viewForward.z}); // towards the camera
            break;
        case EngineInput::RenderVar0:
        case EngineInput::RenderVar1: {
            const Vec4 &r = u.input == EngineInput::RenderVar0 ? ctx.renderVar0 : ctx.renderVar1;
            Value v;
            v.v = {r.x, r.y, r.z, r.w};
            v.count = 4;
            put(v);
            break;
        }
        case EngineInput::Texture0Rotation: {
            Value v;
            v.v = ctx.spriteRotation;
            v.count = 4;
            put(v);
            break;
        }
        case EngineInput::Texture0Translation: {
            Value v;
            v.v = {ctx.spriteTranslation[0], ctx.spriteTranslation[1], 0, 0};
            v.count = 2;
            put(v);
            break;
        }
        case EngineInput::TextureResolution: {
            const TextureInfo t = u.aux >= 0 && static_cast<uint32_t>(u.aux) < kMaxTextureSlots ? ctx.textures[static_cast<size_t>(u.aux)] : TextureInfo{};
            Value v;
            v.v = {static_cast<float>(t.width), static_cast<float>(t.height), static_cast<float>(t.imageWidth), static_cast<float>(t.imageHeight)};
            v.count = 4;
            put(v);
            break;
        }
        case EngineInput::LPointColor:
        case EngineInput::LPointOrigin:
        case EngineInput::LSpotColor:
        case EngineInput::LSpotOrigin:
        case EngineInput::LSpotDirection:
        case EngineInput::LSpotExponent:
        case EngineInput::LTubeColor:
        case EngineInput::LTubeOriginA:
        case EngineInput::LTubeOriginB:
        case EngineInput::LDirectionalColor:
        case EngineInput::LDirectionalDirection:
        case EngineInput::LightsPosition:
        case EngineInput::LightsColorRadius:
        case EngineInput::LightsColorPremultiplied:
            if (in.lights) {
                writeLights(dst, u, *in.lights);
            }
            break;
        case EngineInput::ShadowPointProjection:
        case EngineInput::ShadowPointProjectionTransform:
        case EngineInput::ShadowProjection:
        case EngineInput::ShadowProjectionTransform:
        case EngineInput::ShadowCascadeProjection:
        case EngineInput::ShadowCascadeTransform:
        case EngineInput::ShadowCascadeTexel:
            if (in.shadows) {
                writeShadows(dst, u, *in.shadows);
            }
            break;
        case EngineInput::TextureMipMapInfo: {
            // (an assumption, survey Q5.3: the highest level, so that `lod = roughness * info` stays inside the chain)
            const TextureInfo t = u.aux >= 0 && static_cast<uint32_t>(u.aux) < kMaxTextureSlots ? ctx.textures[static_cast<size_t>(u.aux)] : TextureInfo{};
            Value v;
            v.v = {static_cast<float>(std::max(1u, t.mips) - 1), 0, 0, 0};
            v.count = 1;
            put(v);
            break;
        }
        case EngineInput::TextureTexel: {
            const TextureInfo t = u.aux >= 0 && static_cast<uint32_t>(u.aux) < kMaxTextureSlots ? ctx.textures[static_cast<size_t>(u.aux)] : TextureInfo{};
            const float w = static_cast<float>(std::max(1u, t.width)), h = static_cast<float>(std::max(1u, t.height));
            Value v;
            v.v = {1.0f / w, 1.0f / h, w, h};
            v.count = 4;
            put(v);
            break;
        }
        case EngineInput::ViewportViewProjections:
            if (ctx.viewportViewProjections && u.type == "mat4") {
                const size_t count = std::min<size_t>(std::max<uint32_t>(1u, u.arrayCount), ctx.viewportViewProjections->size());
                for (size_t i = 0; i < count; ++i) {
                    std::memcpy(dst + i * 64, (*ctx.viewportViewProjections)[i].m.data(), 64);
                }
            }
            break;
        case EngineInput::Bones: {
            // one mat4x3 (64 bytes in std140) a bone; bones beyond what the model has stay zero
            if (ctx.bones) {
                const size_t count = std::min<size_t>(u.arrayCount, ctx.bones->size() / 16);
                std::memcpy(dst, ctx.bones->data(), count * 64);
            }
            break;
        }
        case EngineInput::AudioSpectrum: {
            const bool right = u.aux >= 1000;
            const int bands = u.aux % 1000;
            const AudioSpectrum &a = in.audio;
            const float *values = bands == 16 ? (right ? a.right16.data() : a.left16.data())
                : bands == 32                 ? (right ? a.right32.data() : a.left32.data())
                : bands == 64                 ? (right ? a.right64.data() : a.left64.data())
                                              : nullptr;
            // an array of floats in std140 takes a 16 byte slot per element
            for (int i = 0; values && i < bands && static_cast<uint32_t>(i) < u.arrayCount; ++i) {
                std::memcpy(dst + i * 16, &values[i], sizeof(float));
            }
            break;
        }
        case EngineInput::None: {
            const Constant *c = !u.materialKey.empty() && ctx.material ? findConstant(*ctx.material, u.lookupKey) : nullptr;
            Value v;
            bool have = false;
            if (c) {
                if (!c->user.empty() && ctx.properties) {
                    if (const PropertyValue *p = ctx.properties->find(c->user)) {
                        v = fromProperty(*p, u.isColor);
                        have = v.count > 0;
                    }
                }
                if (!have && c->count > 0) {
                    v.v = c->value;
                    v.count = c->count;
                    have = true;
                }
            }
            if (!have && !u.defaultText.empty()) {
                v = fromText(u.defaultText);
                have = v.count > 0;
            }
            if (have) {
                put(v);
            } else if (u.materialKey.empty() && unknown) {
                unknown->insert(u.name); // nothing in the engine or the annotations knows this one: it stays zero
            }
            break;
        }
        }
    }
}

} // namespace murasaki::scene
