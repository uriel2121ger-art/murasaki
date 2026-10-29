// murasaki-scene-vk uniform filling: where a uniform's value comes from and how it is laid out in the std140 block.
// CPU only: shaders are synthetic and only translated (the compiler runs, the GPU does not).

#include "render/uniforms.h"
#include "scene/material.h"
#include "shaders/translate.h"
#include "util/json.h"

#include "render/lights.h"
#include "render/shadows.h"
#include "render/camera.h"
#include "render/shake.h"

#include <QTest>

#include <cmath>
#include <cstring>
#include <vector>

using namespace murasaki::scene;

namespace {

ShaderSource src(const std::string &text)
{
    std::string error;
    auto s = parseShaderSource("t", text, [](const std::string &, const std::string &) { return std::nullopt; }, &error);
    if (!s) {
        qFatal("%s", error.c_str());
    }
    return *s;
}

// A vertex stage that declares the uniforms to test (the fragment stage is a stub).
TranslatedShader shaderWith(const std::string &declarations)
{
    const ShaderSource v = src(declarations + "attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1.0); }\n");
    const ShaderSource f = src("void main() { gl_FragColor = vec4(1.0); }\n");
    std::string error;
    auto t = translateToGlsl(v, f, {}, &error);
    if (!t) {
        qFatal("%s", error.c_str());
    }
    return t->layout;
}

float f32(const std::vector<uint8_t> &b, uint32_t offset)
{
    float v;
    std::memcpy(&v, b.data() + offset, 4);
    return v;
}

int32_t i32(const std::vector<uint8_t> &b, uint32_t offset)
{
    int32_t v;
    std::memcpy(&v, b.data() + offset, 4);
    return v;
}

const UniformInfo &find(const TranslatedShader &t, const std::string &name)
{
    for (const UniformInfo &u : t.vertex.uniforms) {
        if (u.name == name) {
            return u;
        }
    }
    qFatal("no uniform %s", name.c_str());
}

std::vector<uint8_t> fill(const TranslatedShader &t, const UniformContext &ctx, std::set<std::string> *unknown = nullptr)
{
    std::vector<uint8_t> block(t.vertex.blockSize, 0xCD); // garbage first: the filler must zero what it does not set
    fillUniforms(t.vertex, ctx, block.data(), unknown);
    return block;
}

// A scene of nodes and lights built in place: `add` puts a light on a node at `origin` (angles in radians), under `parent` (an id).
struct LightScene {
    Scene scene;
    SceneLight &add(LightType type, Vec3 origin, Vec3 angles = {}, int parent = -1)
    {
        SceneNode n;
        n.id = static_cast<int>(scene.nodes.size()) + 100;
        n.parentId = parent;
        n.origin = origin;
        n.angles = angles;
        scene.nodeIndexById[n.id] = scene.nodes.size();
        scene.nodes.push_back(n);
        SceneLight l;
        l.type = type;
        l.node = scene.nodes.size() - 1;
        scene.lights.push_back(l);
        return scene.lights.back();
    }
    int group(Vec3 origin, Vec3 angles = {})
    {
        SceneNode n;
        n.id = static_cast<int>(scene.nodes.size()) + 100;
        n.origin = origin;
        n.angles = angles;
        scene.nodeIndexById[n.id] = scene.nodes.size();
        scene.nodes.push_back(n);
        return n.id;
    }
};


Vec4 times(const Mat4 &m, Vec3 p)
{
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3), m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3)};
}

// Where a caster draw puts a world point: the atlas pixel (top left origin) and the depth written, the way the translated vertex
// stage (y flipped, depth (z + w) / 2) and the view's viewport take it.
struct AtlasHit {
    float x = 0, y = 0, depth = 0;
};
AtlasHit casterHit(const ShadowView &v, Vec3 p)
{
    const Vec4 c = times(v.viewProjection, p);
    const float nx = c.x / c.w, ny = c.y / c.w, nz = c.z / c.w;
    return {v.x + (nx * 0.5f + 0.5f) * v.width, v.y + (0.5f - ny * 0.5f) * v.height, (nz + 1.0f) * 0.5f};
}

// What the shipped lookup of a point light's shadow reads (common_pbr_2.h, CalculateProjectedCoordsPoint, written out here from
// its arithmetic): the face of the dominant axis, its view, the projection in `info`, the 0.47 compensation, the 2 x 3 cells.
AtlasHit pointLookup(Vec3 p, Vec3 o, Vec4 info, Vec4 place, float atlasW, float atlasH, int *faceOut = nullptr)
{
    const Vec3 d{p.x - o.x, p.y - o.y, p.z - o.z};
    const float ax = std::fabs(d.x), ay = std::fabs(d.y), az = std::fabs(d.z);
    int face = 0;
    Vec3 r, u, b;
    if (ax >= ay && ax >= az) {
        face = d.x >= 0 ? 0 : 1;
        r = {0, 0, d.x >= 0 ? 1.0f : -1.0f};
        u = {0, 1, 0};
        b = {d.x >= 0 ? -1.0f : 1.0f, 0, 0};
    } else if (ay >= ax && ay >= az) {
        face = d.y >= 0 ? 2 : 3;
        r = {1, 0, 0};
        u = {0, 0, d.y >= 0 ? 1.0f : -1.0f};
        b = {0, d.y >= 0 ? -1.0f : 1.0f, 0};
    } else {
        face = d.z >= 0 ? 4 : 5;
        r = {d.z >= 0 ? -1.0f : 1.0f, 0, 0};
        u = {0, 1, 0};
        b = {0, 0, d.z >= 0 ? -1.0f : 1.0f};
    }
    const auto dot = [](Vec3 a, Vec3 c) { return a.x * c.x + a.y * c.y + a.z * c.z; };
    const float vx = dot(r, d), vy = dot(u, d), vz = dot(b, d);
    const float w = info.z * vz + info.w;
    const float z = (info.x * vz + info.y) / w;
    float cu = vx / w * 0.47f + 0.5f, cv = vy / w * -0.47f + 0.5f;
    const float col = static_cast<float>(face & 1), row = static_cast<float>(face >> 1);
    cu = (cu + col) * place.z * 0.5f + place.x;
    cv = (cv + row) * place.w * 0.3333f + place.y;
    if (faceOut) {
        *faceOut = face;
    }
    return {cu * atlasW, cv * atlasH, z};
}

// CalculateProjectedCoords then PerformShadowMapping's placement (spots), and the cascades' reading (the .w flag of a point outside).
AtlasHit projectedLookup(const Mat4 &m, Vec3 p, Vec4 place, float atlasW, float atlasH, bool *outside = nullptr)
{
    const Vec4 c = times(m, p);
    const float x = c.x / c.w, y = c.y / c.w, z = c.z / c.w;
    if (outside) {
        *outside = std::fabs(x) >= 0.99f || std::fabs(y) >= 0.99f || std::fabs(z) >= 0.99f;
    }
    return {((x * 0.5f + 0.5f) * place.z + place.x) * atlasW, ((0.5f - y * 0.5f) * place.w + place.y) * atlasH, z};
}

// A scene of lights and one mesh that casts (a model without parts: what the lights code asks is only whether one casts).
void addCaster(Scene &scene, bool casts = true)
{
    MeshLayer mesh;
    mesh.model = std::make_shared<MeshModel>();
    mesh.castShadow = casts;
    scene.meshes.push_back(mesh);
}

bool nearHit(const AtlasHit &a, const AtlasHit &b, float pixels = 0.05f, float depth = 1e-4f)
{
    return std::fabs(a.x - b.x) <= pixels && std::fabs(a.y - b.y) <= pixels && std::fabs(a.depth - b.depth) <= depth;
}

QString showHit(const AtlasHit &h)
{
    return QStringLiteral("(%1, %2 depth %3)").arg(h.x).arg(h.y).arg(h.depth);
}

bool near4(const Vec4 &v, Vec4 e, float tolerance = 1e-4f)
{
    return std::fabs(v.x - e.x) <= tolerance && std::fabs(v.y - e.y) <= tolerance && std::fabs(v.z - e.z) <= tolerance && std::fabs(v.w - e.w) <= tolerance;
}

QString show4(const Vec4 &v)
{
    return QStringLiteral("(%1, %2, %3, %4)").arg(v.x).arg(v.y).arg(v.z).arg(v.w);
}

#define VERIFY_V4(actual, ...)                                                                                                 \
    do {                                                                                                                      \
        const Vec4 a_ = (actual);                                                                                             \
        QVERIFY2(near4(a_, Vec4{__VA_ARGS__}), qPrintable(show4(a_)));                                                       \
    } while (false)

constexpr float kHalfPi = 1.57079632679f;

} // namespace

class TstSceneVkUniforms : public QObject
{
    Q_OBJECT

private slots:
    void engineInputsByName()
    {
        const TranslatedShader t = shaderWith(
            "uniform float g_Time;\nuniform float g_Frametime;\nuniform float g_Daytime;\nuniform vec2 g_PointerPosition;\n"
            "uniform vec2 g_PointerPositionLast;\nuniform vec4 g_PointerState;\nuniform vec2 g_ParallaxPosition;\nuniform vec3 g_Screen;\n"
            "uniform vec3 g_EyePosition;\nuniform vec4 g_Texture0Resolution;\nuniform vec4 g_Texture2Resolution;\n");
        UniformContext ctx;
        ctx.inputs.time = 12.5;
        ctx.inputs.frametime = 0.02;
        ctx.inputs.daytime = 0.25f;
        ctx.inputs.pointer = {0.25f, 0.75f};
        ctx.inputs.pointerLast = {0.5f, 0.5f};
        ctx.inputs.buttons = 2;
        ctx.inputs.parallax = {0.1f, -0.2f};
        ctx.inputs.screen = {1920, 1080, 16.0f / 9.0f};
        ctx.textures[0] = {4096, 2048, 3979, 2000};
        ctx.textures[2] = {64, 64, 64, 64};
        std::set<std::string> unknown;
        const auto b = fill(t, ctx, &unknown);
        QVERIFY(unknown.empty());
        QCOMPARE(f32(b, find(t, "g_Time").offset), 12.5f);
        QCOMPARE(f32(b, find(t, "g_Frametime").offset), 0.02f);
        QCOMPARE(f32(b, find(t, "g_Daytime").offset), 0.25f);
        QCOMPARE(f32(b, find(t, "g_PointerPosition").offset + 4), 0.75f);
        QCOMPARE(f32(b, find(t, "g_PointerPositionLast").offset), 0.5f);
        QCOMPARE(f32(b, find(t, "g_PointerState").offset + 8), 0.0f); // left button up
        QCOMPARE(f32(b, find(t, "g_PointerState").offset + 12), 1.0f); // right button down
        QCOMPARE(f32(b, find(t, "g_ParallaxPosition").offset + 4), -0.2f);
        QCOMPARE(f32(b, find(t, "g_Screen").offset + 4), 1080.0f);
        QCOMPARE(f32(b, find(t, "g_EyePosition").offset + 8), 1.0f);
        QCOMPARE(f32(b, find(t, "g_Texture0Resolution").offset), 4096.0f);
        QCOMPARE(f32(b, find(t, "g_Texture0Resolution").offset + 8), 3979.0f);
        QCOMPARE(f32(b, find(t, "g_Texture2Resolution").offset + 4), 64.0f);
    }

    void cameraShakeIsBoundedSmoothAndScaledByItsSettings()
    {
        const Vec2 size{1920, 1080};
        const Vec2 none = cameraShake(1.0, 0.0f, 1.0f, 3.0f, size);
        QCOMPARE(none.x, 0.0f);
        QCOMPARE(none.y, 0.0f);
        float maxX = 0.0f, maxY = 0.0f, maxStep = 0.0f;
        Vec2 previous = cameraShake(0.0, 0.5f, 1.0f, 3.0f, size);
        for (int i = 1; i < 6000; ++i) {
            const Vec2 v = cameraShake(i / 60.0, 0.5f, 1.0f, 3.0f, size);
            maxX = std::max(maxX, std::fabs(v.x));
            maxY = std::max(maxY, std::fabs(v.y));
            maxStep = std::max(maxStep, std::fabs(v.x - previous.x));
            previous = v;
        }
        QVERIFY2(maxX <= 0.5f * 0.01f * 1920.0f + 1e-3f, "the shake never leaves its amplitude");
        QVERIFY2(maxX > 0.5f * 0.01f * 1920.0f * 0.5f, "and uses most of it");
        QVERIFY(maxY <= 0.5f * 0.01f * 1080.0f + 1e-3f);
        QVERIFY2(maxStep < 2.5f, qPrintable(QString::number(maxStep))); // smooth: a 60th of a second moves it by a unit or two at most
        // twice the amplitude, twice the movement
        const Vec2 a = cameraShake(2.3, 0.5f, 1.0f, 3.0f, size);
        const Vec2 b = cameraShake(2.3, 1.0f, 1.0f, 3.0f, size);
        QVERIFY(std::fabs(b.x - 2.0f * a.x) < 1e-3f);
        // a faster speed visits the same values sooner
        QCOMPARE(cameraShake(1.0, 0.5f, 0.0f, 6.0f, size).x, cameraShake(2.0, 0.5f, 0.0f, 3.0f, size).x);
        // two axes are not the same wave
        QVERIFY(a.x / 1920.0f != a.y / 1080.0f);
    }

    void matricesAreColumnMajorAndInvertible()
    {
        const TranslatedShader t = shaderWith("uniform mat4 g_ModelViewProjectionMatrix;\nuniform mat4 g_ModelViewProjectionMatrixInverse;\n"
                                              "uniform mat4 g_EffectTextureProjectionMatrix;\nuniform mat3 g_Rot3; // {\"material\":\"rot\"}\n");
        UniformContext ctx;
        ctx.mvp = translation({3, 4, 5}) * scaling({2, 2, 2});
        const auto b = fill(t, ctx);
        const uint32_t m = find(t, "g_ModelViewProjectionMatrix").offset;
        QCOMPARE(f32(b, m + 0), 2.0f);          // column 0, row 0
        QCOMPARE(f32(b, m + 12 * 4), 3.0f);     // column 3 holds the translation
        QCOMPARE(f32(b, m + 13 * 4), 4.0f);
        const uint32_t inv = find(t, "g_ModelViewProjectionMatrixInverse").offset;
        QCOMPARE(f32(b, inv + 0), 0.5f);
        QCOMPARE(f32(b, inv + 12 * 4), -2.0f);  // (0 - 3) / 2, then the origin moves to the layer's centre (-0.5)
        // the effect texture projection is the inverse of (doubled xy, centred) inverse: it maps layer coordinates back
        const uint32_t id = find(t, "g_EffectTextureProjectionMatrix").offset;
        QCOMPARE(f32(b, id), 1.0f);
        QCOMPARE(f32(b, id + 4), 0.0f);
        QCOMPARE(f32(b, id + 12 * 4), 4.0f); // the inverse maps x to x - 4, so this one maps it back with +4

        // the general inverse really inverts
        const Mat4 a = rotationEuler({0.3f, 0.2f, 0.9f}) * translation({1, 2, 3}) * scaling({2, 3, 4});
        const Mat4 p = a * inverse(a);
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) {
                QVERIFY(std::abs(p.at(r, c) - (r == c ? 1.0f : 0.0f)) < 1e-4f);
            }
        }
        // a singular matrix gives the identity instead of garbage
        QCOMPARE(inverse(Mat4()).at(0, 0), 1.0f);
    }

    void materialConstantsDefaultsAndBindings()
    {
        const TranslatedShader t = shaderWith(
            "uniform float g_Alpha; // {\"material\":\"Alpha\",\"default\":0.75}\n"
            "uniform vec3 g_Tint; // {\"material\":\"tint\",\"default\":\"1 0.5 0\",\"type\":\"color\"}\n"
            "uniform vec2 g_Pair; // {\"material\":\"pair\"}\n"
            "uniform int g_Count; // {\"material\":\"count\",\"default\":2}\n"
            "uniform float g_Scalar; // {\"material\":\"scalar\"}\n"
            "uniform float g_Nobody;\n");
        MaterialPass pass;
        Constant c;
        c.value = {0.1f, 0, 0, 0};
        c.count = 1;
        pass.constants["alpha"] = c; // looked up ignoring case: the annotation says "Alpha"
        Constant bound;
        bound.user = "tintprop";
        bound.value = {0.2f, 0.2f, 0.2f, 0};
        bound.count = 3;
        pass.constants["tint"] = bound;
        Constant pair;
        pair.value = {7, 8, 0, 0};
        pair.count = 2;
        pass.constants["pair"] = pair;
        Constant count;
        count.value = {5, 0, 0, 0};
        count.count = 1;
        pass.constants["count"] = count;
        PropertyStore props;
        UniformContext ctx;
        ctx.material = &pass;
        ctx.properties = &props;

        std::set<std::string> unknown;
        auto b = fill(t, ctx, &unknown);
        QCOMPARE(f32(b, find(t, "g_Alpha").offset), 0.1f);       // the constant beats the annotation default
        QCOMPARE(f32(b, find(t, "g_Tint").offset), 0.2f);        // the unbound property: the constant's own value
        QCOMPARE(f32(b, find(t, "g_Pair").offset + 4), 8.0f);
        QCOMPARE(i32(b, find(t, "g_Count").offset), 5);          // an int uniform holds an integer
        QCOMPARE(f32(b, find(t, "g_Scalar").offset), 0.0f);      // no constant, no default: zero
        QVERIFY(unknown.count("g_Nobody"));                      // nothing knows this one
        QVERIFY(!unknown.count("g_Scalar"));

        // a property bound to the colour wins, and 0..255 is scaled
        props.set("tintprop", std::vector<double>{255, 128, 0});
        b = fill(t, ctx);
        QCOMPARE(f32(b, find(t, "g_Tint").offset), 1.0f);
        QVERIFY(std::abs(f32(b, find(t, "g_Tint").offset + 4) - 128.0f / 255.0f) < 1e-5f);

        // without the constant the annotation default applies
        pass.constants.erase("alpha");
        pass.constants.erase("count");
        b = fill(t, ctx);
        QCOMPARE(f32(b, find(t, "g_Alpha").offset), 0.75f);
        QCOMPARE(i32(b, find(t, "g_Count").offset), 2);

        // an override of the draw beats everything
        ctx.overrides["g_Alpha"] = {0.33f, 0, 0, 0};
        QCOMPARE(f32(fill(t, ctx), find(t, "g_Alpha").offset), 0.33f);
    }

    void aUserShaderValuePropertyReachesTheConstant()
    {
        const TranslatedShader t = shaderWith("uniform vec3 g_Flag; // {\"material\":\"flagcolor1\",\"default\":\"1 1 1\",\"type\":\"color\"}\n");
        const auto json = parseJson(R"({"shader":"flag","usershadervalues":{"flagcolor1":"color2"}})");
        QVERIFY(json.has_value());
        const MaterialPass pass = parseMaterialPass(*json);
        PropertyStore props;
        UniformContext ctx;
        ctx.material = &pass;
        ctx.properties = &props;
        QCOMPARE(f32(fill(t, ctx), find(t, "g_Flag").offset), 1.0f); // property not set: the shader's default
        props.set("color2", std::vector<double>{0.0, 0.5, 1.0});
        auto b = fill(t, ctx);
        QCOMPARE(f32(b, find(t, "g_Flag").offset), 0.0f);
        QCOMPARE(f32(b, find(t, "g_Flag").offset + 4), 0.5f);
        props.set("color2", std::vector<double>{1.0, 0.25, 0.0}); // live: read at each draw
        b = fill(t, ctx);
        QCOMPARE(f32(b, find(t, "g_Flag").offset + 4), 0.25f);
    }

    void arraysAndTheGarbageAroundThemAreZeroed()
    {
        const TranslatedShader t = shaderWith("uniform float g_AudioSpectrum16Left[16];\nuniform float g_Time;\nuniform vec3 g_V; // {\"material\":\"v\",\"default\":\"1 2 3\"}\n");
        UniformContext ctx;
        ctx.inputs.time = 3.0;
        const auto b = fill(t, ctx);
        const UniformInfo spectrum = find(t, "g_AudioSpectrum16Left");
        for (uint32_t i = 0; i < spectrum.size; ++i) {
            QCOMPARE(int(b[spectrum.offset + i]), 0); // zeros until audio exists, padding included
        }
        QCOMPARE(f32(b, find(t, "g_Time").offset), 3.0f);
        QCOMPARE(f32(b, find(t, "g_V").offset + 8), 3.0f);
        QCOMPARE(int(b[b.size() - 1]), 0); // the tail of the block was zeroed too
    }

    void eachLightTypeIsPackedInWorldSpace()
    {
        LightScene ls;
        const int parent = ls.group({100, 0, 0});
        SceneLight &point = ls.add(LightType::Point, {10, 20, 30}, {}, parent);
        point.color = {{1.0f, 0.5f, 0.0f}, {}};
        point.intensity = {2.0f, {}};
        point.radius = {100.0f, {}};
        point.exponent = {3.0f, {}};
        // a spot turned a quarter about y: its +x, along which it shines, points down -z
        SceneLight &spot = ls.add(LightType::Spot, {0, 0, 50}, {0, kHalfPi, 0});
        spot.innerCone = {60.0f, {}};
        spot.outerCone = {90.0f, {}};
        spot.exponent = {1.5f, {}};
        spot.radius = {40.0f, {}};
        // a tube from its origin to its control point, in its own space (turned a quarter about z: the point goes up y)
        SceneLight &tube = ls.add(LightType::Tube, {5, 5, 5}, {0, 0, kHalfPi});
        tube.controlPoint = Vec3{10, 0, 0};
        tube.radius = {7.0f, {}};
        SceneLight &sun = ls.add(LightType::Directional, {0, 0, 0}, {0, kHalfPi, 0});
        sun.intensity = {0.5f, {}};
        const LightUniforms u = packLights(ls.scene, PropertyStore{});
        QCOMPARE(u.counts.point, 1);
        QCOMPARE(u.counts.spot, 1);
        QCOMPARE(u.counts.tube, 1);
        QCOMPARE(u.counts.directional, 1);
        QCOMPARE(u.counts.legacy, 0);
        VERIFY_V4(u.pointColor[0], 2.0f, 1.0f, 0.0f, 100.0f); // colour times intensity, radius
        VERIFY_V4(u.pointOrigin[0], 110.0f, 20.0f, 30.0f, 3.0f); // under its parent; the exponent
        VERIFY_V4(u.spotColor[0], 1.0f, 1.0f, 1.0f, 40.0f);
        VERIFY_V4(u.spotOrigin[0], 0.0f, 0.0f, 50.0f, 0.5f);            // cos(inner 60 degrees)
        VERIFY_V4(u.spotDirection[0], 0.0f, 0.0f, -1.0f, 0.0f);         // where it shines, cos(outer 90 degrees)
        VERIFY_V4(u.spotExponent[0], 1.5f, 0.0f, 0.0f, 0.0f);
        VERIFY_V4(u.tubeColor[0], 1.0f, 1.0f, 1.0f, 7.0f);
        VERIFY_V4(u.tubeOriginA[0], 5.0f, 5.0f, 5.0f, 2.0f);
        VERIFY_V4(u.tubeOriginB[0], 5.0f, 15.0f, 5.0f, 0.0f);
        VERIFY_V4(u.directionalColor[0], 0.5f, 0.5f, 0.5f, 0.0f);
        VERIFY_V4(u.directionalDirection[0], 0.0f, 0.0f, 1.0f, 0.0f);   // towards the light: against where it shines

        // the combos: the counts, and SCENE_ORTHO in an orthographic scene
        const std::map<std::string, int> ortho = sceneLightCombos(ls.scene);
        QCOMPARE(ortho, (std::map<std::string, int>{{"LIGHTS_DIRECTIONAL", 1}, {"LIGHTS_POINT", 1}, {"LIGHTS_SPOT", 1}, {"LIGHTS_TUBE", 1}, {"SCENE_ORTHO", 1}}));
        ls.scene.perspective = true;
        QCOMPARE(sceneLightCombos(ls.scene).count("SCENE_ORTHO"), size_t(0));
        QCOMPARE(sceneLightCombos(Scene{}), (std::map<std::string, int>{{"SCENE_ORTHO", 1}})); // no lights: no counts
    }

    void aHiddenLightKeepsItsSlotAndGivesBlackAndBindingsAreResolved()
    {
        LightScene ls;
        SceneLight &first = ls.add(LightType::Point, {1, 0, 0});
        first.color = {{1, 1, 1}, "tint"};
        first.intensity = {1.0f, "power"};
        ls.add(LightType::Point, {2, 0, 0});
        ls.scene.nodes[0].visible = {false, {}};
        PropertyStore props;
        props.set("tint", std::vector<double>{0.0, 1.0, 0.0});
        props.set("power", 4.0);
        LightUniforms u = packLights(ls.scene, props);
        QCOMPARE(u.counts.point, 2);
        VERIFY_V4(u.pointColor[0], 0.0f, 0.0f, 0.0f, 1000.0f);
        VERIFY_V4(u.pointOrigin[0], 1.0f, 0.0f, 0.0f, 2.0f);
        VERIFY_V4(u.pointOrigin[1], 2.0f, 0.0f, 0.0f, 2.0f);
        ls.scene.nodes[0].visible = {true, {}};
        u = packLights(ls.scene, props);
        VERIFY_V4(u.pointColor[0], 0.0f, 4.0f, 0.0f, 1000.0f); // the bound colour times the bound intensity
        // a radius of zero does not reach the shaders (the falloff divides by it)
        ls.add(LightType::Spot, {0, 0, 0}).radius = {0.0f, {}};
        u = packLights(ls.scene, props);
        QVERIFY(u.spotColor[0].w > 0.0f);
    }

    void theOtherValuesOfALightFollowTheirBindings()
    {
        LightScene ls;
        SceneLight &spot = ls.add(LightType::Spot, {0, 0, 0});
        spot.exponent = {2.0f, "e"};
        spot.innerCone = {20.0f, "in"};
        spot.outerCone = {30.0f, "out"};
        PropertyStore props;
        props.set("e", 0.5);
        props.set("in", 60.0);
        props.set("out", 90.0);
        const LightUniforms u = packLights(ls.scene, props);
        VERIFY_V4(u.spotExponent[0], 0.5f, 0.0f, 0.0f, 0.0f);
        QVERIFY(std::fabs(u.spotOrigin[0].w - 0.5f) < 1e-4f);  // cos(60 degrees)
        QVERIFY(std::fabs(u.spotDirection[0].w) < 1e-4f);      // cos(90 degrees)
        // unset properties: the file's values
        const LightUniforms plain = packLights(ls.scene, PropertyStore{});
        VERIFY_V4(plain.spotExponent[0], 2.0f, 0.0f, 0.0f, 0.0f);
    }

    void anInnerConeWiderThanTheOuterIsKeptInsideIt()
    {
        // the shaders' smoothstep needs its edges in order: an inner cone of 50 under an outer one of 40 becomes (almost) 40 too
        LightScene ls;
        SceneLight &spot = ls.add(LightType::Spot, {0, 0, 0});
        spot.innerCone = {50.0f, {}};
        spot.outerCone = {40.0f, {}};
        const LightUniforms u = packLights(ls.scene, PropertyStore{});
        const float cosOuter = std::cos(40.0f * 3.14159265f / 180.0f);
        QVERIFY(u.spotOrigin[0].w > u.spotDirection[0].w);
        QVERIFY(std::fabs(u.spotDirection[0].w - cosOuter) < 1e-5f);
        QVERIFY(u.spotOrigin[0].w - cosOuter < 1e-3f);
    }

    void theCountsStopAtTheCapAndTheOlderPointLightsFillTheirOwnArrays()
    {
        LightScene ls;
        for (size_t i = 0; i < kMaxLightsPerType + 3; ++i) {
            ls.add(LightType::Spot, {float(i), 0, 0});
        }
        SceneLight &a = ls.add(LightType::LegacyPoint, {1, 2, 3});
        a.color = {{1.0f, 0.5f, 0.25f}, {}};
        a.intensity = {2.0f, {}};
        a.radius = {10.0f, {}};
        SceneLight &b = ls.add(LightType::LegacyPoint, {4, 5, 6});
        b.radius = {2.0f, {}};
        for (int i = 0; i < 3; ++i) {
            ls.add(LightType::LegacyPoint, {7, 8, 9}).radius = {3.0f, {}};
        }
        const LightUniforms u = packLights(ls.scene, PropertyStore{});
        QCOMPARE(u.counts.spot, int(kMaxLightsPerType));
        QCOMPARE(u.counts.legacy, int(kMaxLegacyLights));
        QCOMPARE(sceneLightCombos(ls.scene).at("LIGHTS_SPOT"), int(kMaxLightsPerType));
        QCOMPARE(sceneLightCombos(ls.scene).count("LIGHTS_POINT"), size_t(0)); // the older lights have arrays of a fixed size
        VERIFY_V4(u.spotOrigin[kMaxLightsPerType - 1], float(kMaxLightsPerType - 1), 0.0f, 0.0f, std::cos(20.0f * 3.14159265f / 180.0f));
        VERIFY_V4(u.legacyPosition[0], 1.0f, 2.0f, 3.0f, 0.0f);
        VERIFY_V4(u.legacyColorRadius[0], 2.0f, 1.0f, 0.5f, 10.0f);
        // premultiplied by the intensity: lights 0 to 2 in .rgb, light 3 over the three .w
        ls.scene.lights[kMaxLightsPerType + 6].intensity = {3.0f, {}}; // the fourth older light (after 35 spots and three others)
        const LightUniforms w = packLights(ls.scene, PropertyStore{});
        VERIFY_V4(w.legacyPremultiplied[0], 2.0f, 1.0f, 0.5f, 3.0f);
        VERIFY_V4(w.legacyPremultiplied[1], 1.0f, 1.0f, 1.0f, 3.0f);
        VERIFY_V4(w.legacyPremultiplied[2], 1.0f, 1.0f, 1.0f, 3.0f);
        // with fewer older lights the rest sit far away and give black
        LightScene one;
        one.add(LightType::LegacyPoint, {1, 1, 1});
        const LightUniforms v = packLights(one.scene, PropertyStore{});
        QVERIFY(v.legacyPosition[3].z > 1000.0f);
        VERIFY_V4(v.legacyColorRadius[3], 0.0f, 0.0f, 0.0f, 0.0f);
    }

    void theLightArraysAreFilledIntoTheirBlocks()
    {
        const TranslatedShader t = shaderWith(
            "uniform vec4 g_LPoint_Color[2];\nuniform vec4 g_LPoint_Origin[2];\nuniform vec4 g_LSpot_Direction[1];\nuniform vec4 g_LSpot_Exponent[1];\n"
            "uniform vec4 g_LTube_OriginB[1];\nuniform vec4 g_LDirectional_Direction[1];\nuniform vec3 g_LightsPosition[4];\n"
            "uniform vec4 g_LightsColorPremultiplied[3];\nuniform vec4 g_LightsColorRadius[4];\n");
        LightScene ls;
        ls.add(LightType::Point, {1, 2, 3}).radius = {5.0f, {}};
        ls.add(LightType::Point, {4, 5, 6}).radius = {6.0f, {}};
        ls.add(LightType::Spot, {0, 0, 0}).exponent = {0.5f, {}};
        ls.add(LightType::Tube, {0, 0, 0}).controlPoint = Vec3{3, 0, 0};
        ls.add(LightType::Directional, {0, 0, 0});
        ls.add(LightType::LegacyPoint, {7, 8, 9}).radius = {2.0f, {}};
        const LightUniforms lights = packLights(ls.scene, PropertyStore{});
        UniformContext ctx;
        ctx.inputs.lights = &lights;
        const auto b = fill(t, ctx);
        const auto at = [&](const char *name, int element, int component) { return f32(b, find(t, name).offset + uint32_t(element) * 16 + uint32_t(component) * 4); };
        QCOMPARE(find(t, "g_LPoint_Color").input, EngineInput::LPointColor);
        QCOMPARE(at("g_LPoint_Color", 1, 3), 6.0f);
        QCOMPARE(at("g_LPoint_Origin", 0, 2), 3.0f);
        QCOMPARE(at("g_LPoint_Origin", 1, 0), 4.0f);
        QCOMPARE(at("g_LSpot_Direction", 0, 0), 1.0f); // angles 0: along +x
        QCOMPARE(at("g_LSpot_Exponent", 0, 0), 0.5f);
        QCOMPARE(at("g_LTube_OriginB", 0, 0), 3.0f);
        QCOMPARE(at("g_LDirectional_Direction", 0, 0), -1.0f);
        QCOMPARE(at("g_LightsPosition", 0, 1), 8.0f); // vec3 elements take 16 bytes each too
        QVERIFY(at("g_LightsPosition", 1, 2) > 1000.0f);
        QCOMPARE(at("g_LightsColorRadius", 0, 3), 2.0f);
        QCOMPARE(at("g_LightsColorPremultiplied", 0, 0), 1.0f);
        QVERIFY(readsLight(find(t, "g_LightsColorRadius").input));
        QVERIFY(readsLight(EngineInput::ShadowCascadeTexel)); // the last of the light inputs
        QVERIFY(!readsLight(EngineInput::LightAmbientColor));
        // without lights (a pass the scene gives none) the arrays stay zero
        ctx.inputs.lights = nullptr;
        const auto none = fill(t, ctx);
        QCOMPARE(f32(none, find(t, "g_LPoint_Color").offset + 16 + 12), 0.0f);
    }

    void shadowedLightsTakeTheFirstSlotsOfTheirTypeAndSetTheShadowCombos()
    {
        LightScene ls;
        ls.add(LightType::Point, {1, 0, 0});
        ls.add(LightType::Point, {2, 0, 0}).castShadow = true;
        ls.add(LightType::Spot, {3, 0, 0}).castShadow = true;
        ls.add(LightType::Tube, {4, 0, 0}).castShadow = true; // no shadow for a tube
        // without a caster nothing is shadowed and the slots stay in file order
        QVERIFY(!sceneHasShadows(ls.scene));
        QCOMPARE(sceneLightCombos(ls.scene).count("LIGHTS_SHADOW_MAPPING"), size_t(0));
        QCOMPARE(packLights(ls.scene, PropertyStore{}).pointOrigin[0].x, 1.0f);
        addCaster(ls.scene, false);
        QVERIFY(!sceneHasShadows(ls.scene)); // a mesh that casts nothing
        addCaster(ls.scene);
        QVERIFY(sceneHasShadows(ls.scene));
        const auto combos = sceneLightCombos(ls.scene);
        QCOMPARE(combos.at("LIGHTS_SHADOW_MAPPING"), 1);
        QCOMPARE(combos.at("LIGHTS_SHADOW_MAPPING_QUALITY"), kShadowQuality);
        QCOMPARE(combos.at("LIGHTS_POINT_SHADOW"), 1);
        QCOMPARE(combos.at("LIGHTS_SPOT_SHADOW"), 1);
        QCOMPARE(combos.count("LIGHTS_DIRECTIONAL_SHADOW"), size_t(0));
        QCOMPARE(combos.at("LIGHTS_POINT"), 2);
        const LightUniforms u = packLights(ls.scene, PropertyStore{});
        QCOMPARE(u.counts.pointShadow, 1);
        QCOMPARE(u.pointOrigin[0].x, 2.0f); // the shadowed one first
        QCOMPARE(u.pointOrigin[1].x, 1.0f);
        // more shadowed lights than the atlas holds: the rest are lit without one, after the shadowed ones
        LightScene many;
        for (int i = 0; i < kMaxShadowDirectionalLights + 1; ++i) {
            many.add(LightType::Directional, {float(i), 0, 0}).castShadow = true;
        }
        addCaster(many.scene);
        QCOMPARE(packLights(many.scene, PropertyStore{}).counts.directionalShadow, kMaxShadowDirectionalLights);
        QCOMPARE(packLights(many.scene, PropertyStore{}).counts.directional, kMaxShadowDirectionalLights + 1);
    }

    void aPointLightsCasterPicturesAreWhereTheShippedLookupReadsThem()
    {
        LightScene ls;
        SceneLight &light = ls.add(LightType::Point, {10, 20, 30});
        light.castShadow = true;
        light.radius = {500.0f, {}};
        addCaster(ls.scene);
        const LightUniforms lights = packLights(ls.scene, PropertyStore{});
        ShadowBounds casters;
        casters.add({-50, -50, -50});
        casters.add({80, 90, 100});
        const ShadowPlan plan = planShadows(ls.scene, PropertyStore{}, lights, ShadowCamera{}, casters);
        QCOMPARE(plan.atlasWidth, kShadowAtlasWidth);
        QCOMPARE(plan.atlasHeight, 3 * kShadowCell);
        QCOMPARE(plan.lights.size(), size_t(1));
        QCOMPARE(plan.lights[0].views.size(), size_t(6));
        VERIFY_V4(plan.uniforms.pointTransform[0], 0.0f, 0.0f, 0.5f, 1.0f); // the first lane, two cells wide and three high
        const Vec4 info = plan.uniforms.pointProjection[0];
        QCOMPARE(info.z, -1.0f);
        // points in every direction from the light (each face, and near a face's edges): the lookup reads the pixel and depth the face's
        // caster draw wrote
        const Vec3 o{10, 20, 30};
        const std::vector<Vec3> offsets = {{40, 3, -5}, {-40, 10, 7},  {2, 35, -9}, {-6, -50, 4}, {9, -1, 60},
                                           {5, 8, -45}, {30, 29, -12}, {-20, 5, 19.5f}, {15, -14.9f, -15}};
        for (const Vec3 &d : offsets) {
            const Vec3 p{o.x + d.x, o.y + d.y, o.z + d.z};
            int face = -1;
            const AtlasHit read = pointLookup(p, o, info, plan.uniforms.pointTransform[0], float(plan.atlasWidth), float(plan.atlasHeight), &face);
            const AtlasHit wrote = casterHit(plan.lights[0].views[size_t(face)], p);
            QVERIFY2(nearHit(read, wrote), qPrintable(QStringLiteral("face %1: read %2, wrote %3").arg(face).arg(showHit(read), showHit(wrote))));
            const ShadowView &v = plan.lights[0].views[size_t(face)];
            QVERIFY(wrote.x > v.x && wrote.x < v.x + v.width && wrote.y > v.y && wrote.y < v.y + v.height); // inside its own face
            QVERIFY(wrote.depth > 0.0f && wrote.depth < 1.0f);
        }
        // the depth grows with the distance along the face's axis
        const AtlasHit nearer = pointLookup({o.x + 20, o.y, o.z}, o, info, plan.uniforms.pointTransform[0], 1, 1);
        const AtlasHit farther = pointLookup({o.x + 60, o.y, o.z}, o, info, plan.uniforms.pointTransform[0], 1, 1);
        QVERIFY(nearer.depth < farther.depth);
    }

    void aSpotAndTheCascadesAreReadWhereTheirCastersWereDrawn()
    {
        LightScene ls;
        SceneLight &spot = ls.add(LightType::Spot, {0, 0, 50}, {0, kHalfPi, 0}); // shines down -z
        spot.castShadow = true;
        spot.outerCone = {40.0f, {}};
        spot.innerCone = {30.0f, {}};
        spot.radius = {300.0f, {}};
        SceneLight &sun = ls.add(LightType::Directional, {0, 0, 0}, {0, 1.1071487f, 0}); // shines along (1, 0, -2)
        sun.castShadow = true;
        sun.cascadeDistances = {Bound<float>{20.0f, {}}, Bound<float>{60.0f, {}}, Bound<float>{200.0f, {}}};
        addCaster(ls.scene);
        const LightUniforms lights = packLights(ls.scene, PropertyStore{});
        ShadowBounds casters;
        casters.add({-30, -30, 0});
        casters.add({30, 30, 20});
        // a camera at (0, -100, 40) looking at the origin, fov 50 on 16:9, near 0.1, far 1000
        ShadowCamera camera;
        camera.viewProjection = perspectiveGl(50.0f * 3.14159265f / 180.0f, 16.0f / 9.0f, 0.1f, 1000.0f)
            * viewMatrix(lookAtBasis({0, -100, 40}, {0, 0, 0}, {0, 0, 1}));
        camera.nearDistance = 0.1f;
        camera.farDistance = 1000.0f;
        const ShadowPlan plan = planShadows(ls.scene, PropertyStore{}, lights, camera, casters);
        QCOMPARE(plan.lights.size(), size_t(2));
        QCOMPARE(plan.lights[0].views.size(), size_t(1));
        QCOMPARE(plan.lights[1].views.size(), size_t(3));
        // the spot's block, then the three cascades', each 2 x 2 cells, in the shorter lane: 4 cells high in all
        QCOMPARE(plan.atlasHeight, 4 * kShadowCell);
        const float aw = float(plan.atlasWidth), ah = float(plan.atlasHeight);
        for (const Vec3 &p : std::vector<Vec3>{{0, 0, 0}, {10, -5, 0}, {-20, 15, 10}, {0, 0, 25}}) {
            const AtlasHit read = projectedLookup(plan.uniforms.spotProjection[0], p, plan.uniforms.spotTransform[0], aw, ah);
            const AtlasHit wrote = casterHit(plan.lights[0].views[0], p);
            QVERIFY2(nearHit(read, wrote), qPrintable(QStringLiteral("spot: read %1, wrote %2").arg(showHit(read), showHit(wrote))));
        }
        // a point of the floor near the camera is in the first cascade's box, one far away only in the last one's; each is read where
        // its cascade's caster draw put it
        const auto cascadeOf = [&](Vec3 p) {
            for (int k = 0; k < kShadowCascades; ++k) {
                bool outside = true;
                const AtlasHit read = projectedLookup(plan.uniforms.cascadeProjection[size_t(k)], p, plan.uniforms.cascadeTransform[size_t(k)], aw, ah, &outside);
                if (!outside) {
                    const AtlasHit wrote = casterHit(plan.lights[1].views[size_t(k)], p);
                    return nearHit(read, wrote, 0.05f, 1e-4f) ? k : -2;
                }
            }
            return -1;
        };
        // (points along the line of sight, at a view depth of d)
        const Vec3 eye{0, -100, 40};
        const float len = std::sqrt(100.0f * 100.0f + 40.0f * 40.0f);
        const auto along = [&](float d) { return Vec3{0, eye.y + 100.0f / len * d, eye.z - 40.0f / len * d}; };
        QCOMPARE(cascadeOf(along(10)), 0);   // the first slice: depths up to 20
        QCOMPARE(cascadeOf(along(40)), 1);   // 20 to 60
        QCOMPARE(cascadeOf(along(150)), 2);  // 60 to 200
        QCOMPARE(cascadeOf(along(900)), -1); // beyond the last cascade: no shadow
        // every caster is inside each cascade's depth range (nothing between the box and the light is cut off)
        for (int k = 0; k < kShadowCascades; ++k) {
            for (int c = 0; c < 8; ++c) {
                const Vec4 v = times(plan.uniforms.cascadeProjection[size_t(k)], {(c & 1) ? 30.0f : -30.0f, (c & 2) ? 30.0f : -30.0f, (c & 4) ? 20.0f : 0.0f});
                QVERIFY(v.z / v.w >= 0.0f);
            }
        }
    }

    void theFarCascadeTakesOneCellAndTheNearOnesFour()
    {
        LightScene ls;
        SceneLight &sun = ls.add(LightType::Directional, {0, 0, 0}, {0, 1.1071487f, 0});
        sun.castShadow = true;
        sun.cascadeDistances = {Bound<float>{20.0f, {}}, Bound<float>{60.0f, {}}, Bound<float>{200.0f, {}}};
        addCaster(ls.scene);
        const LightUniforms lights = packLights(ls.scene, PropertyStore{});
        ShadowBounds casters;
        casters.add({-30, -30, 0});
        casters.add({30, 30, 20});
        ShadowCamera camera;
        camera.viewProjection = perspectiveGl(50.0f * 3.14159265f / 180.0f, 16.0f / 9.0f, 0.1f, 1000.0f)
            * viewMatrix(lookAtBasis({0, -100, 40}, {0, 0, 0}, {0, 0, 1}));
        camera.nearDistance = 0.1f;
        camera.farDistance = 1000.0f;
        const ShadowPlan plan = planShadows(ls.scene, PropertyStore{}, lights, camera, casters);
        QCOMPARE(plan.lights[0].views.size(), size_t(3));
        const ShadowView &v0 = plan.lights[0].views[0], &v1 = plan.lights[0].views[1], &v2 = plan.lights[0].views[2];
        QCOMPARE(v0.width, float(2 * kShadowCell));
        QCOMPARE(v0.height, float(2 * kShadowCell));
        QCOMPARE(v1.width, float(2 * kShadowCell));
        QCOMPARE(v2.width, float(kShadowCell)); // the far cascade: its texels are large anyway
        QCOMPARE(v2.height, float(kShadowCell));
        // the two big blocks side by side hold the near cascades, the far one sits in the first free corner under them
        QCOMPARE(v1.x, float(2 * kShadowCell));
        QCOMPARE(plan.atlasHeight, 3 * kShadowCell);
        QCOMPARE(v2.x, 0.0f);
        QCOMPARE(v2.y, float(2 * kShadowCell));
        // the world size of a texel a cascade writes for the normal offset: its box over its block's pixels, growing with the distance
        float last = 0.0f;
        for (int k = 0; k < kShadowCascades; ++k) {
            const float texel = plan.uniforms.cascadeTexel[size_t(k)].x;
            QVERIFY2(texel > last, qPrintable(QString::number(texel)));
            last = texel;
            const Mat4 &m = plan.uniforms.cascadeProjection[size_t(k)];
            // the box is 2 radius wide in the picture: the gradient of its x across the world is 1 / radius, and a texel is 2 radius over
            // the block's pixels
            const Vec4 o = times(m, {0, 0, 0});
            float gradient = 0.0f;
            for (const Vec3 &step : {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}}) {
                const Vec4 t = times(m, step);
                gradient += (t.x / t.w - o.x / o.w) * (t.x / t.w - o.x / o.w);
            }
            const float expected = 2.0f / std::sqrt(gradient) / plan.lights[0].views[size_t(k)].width;
            QVERIFY2(std::fabs(texel - expected) <= expected * 1e-3f, qPrintable(QStringLiteral("%1 vs %2").arg(texel).arg(expected)));
        }
        // the block the lighting function reads for each cascade is the one it was drawn into
        const float aw = float(plan.atlasWidth), ah = float(plan.atlasHeight);
        VERIFY_V4(plan.uniforms.cascadeTransform[2], v2.x / aw, v2.y / ah, v2.width / aw, v2.height / ah);
    }

    void theShadowSettingsSetTheCellAndTheQuality()
    {
        LightScene ls;
        SceneLight &light = ls.add(LightType::Point, {0, 0, 0});
        light.castShadow = true;
        light.radius = {500.0f, {}};
        addCaster(ls.scene);
        const LightUniforms lights = packLights(ls.scene, PropertyStore{});
        ShadowBounds casters;
        casters.add({-50, -50, -50});
        casters.add({50, 50, 50});
        // at a reduced render scale the engine halves the cell and asks for quality 1
        const ShadowSettings reduced{kShadowCell / 2, 1};
        const ShadowPlan plan = planShadows(ls.scene, PropertyStore{}, lights, ShadowCamera{}, casters, reduced);
        QCOMPARE(plan.atlasWidth, kShadowAtlasWidth / 2);
        QCOMPARE(plan.atlasHeight, 3 * kShadowCell / 2);
        VERIFY_V4(plan.uniforms.pointTransform[0], 0.0f, 0.0f, 0.5f, 1.0f); // shares of the atlas: the same
        QCOMPARE(plan.lights[0].views[0].width, float(kShadowCell / 2));
        QCOMPARE(shadowPointCompensation(1), shadowPointCompensation(2));
        QCOMPARE(sceneLightCombos(ls.scene, 1).at("LIGHTS_SHADOW_MAPPING_QUALITY"), 1);
        QCOMPARE(sceneLightCombos(ls.scene).at("LIGHTS_SHADOW_MAPPING_QUALITY"), kShadowQuality);
    }

    void aDriftingCasterKeepsAPointLightsDepthRange()
    {
        LightScene ls;
        SceneLight &light = ls.add(LightType::Point, {0, 0, 0});
        light.castShadow = true;
        light.radius = {500.0f, {}};
        addCaster(ls.scene);
        const LightUniforms lights = packLights(ls.scene, PropertyStore{});
        const auto projectionFor = [&](float half) {
            ShadowBounds casters;
            casters.add({-half, -half, -half});
            casters.add({half, half, half});
            return planShadows(ls.scene, PropertyStore{}, lights, ShadowCamera{}, casters).uniforms.pointProjection[0];
        };
        // the depth range is rounded outwards to steps of an eighth of a power of two: a caster that grows by half a percent leaves the
        // depth mapping (and so the pictures drawn with it) as it is; a doubled one does not
        const Vec4 base = projectionFor(50.0f);
        const Vec4 drift = projectionFor(50.3f);
        QVERIFY2(near4(base, drift, 0.0f), qPrintable(show4(base) + " vs " + show4(drift)));
        const Vec4 doubled = projectionFor(100.0f);
        QVERIFY(!near4(base, doubled));
    }

    void whatChangesFromFrameToFrame()
    {
        QVERIFY(changesEveryFrame("g_Time"));
        QVERIFY(changesEveryFrame("g_AudioSpectrum64Right"));
        QVERIFY(!changesEveryFrame("g_Texture0Resolution"));
        QVERIFY(dependsOnPointer("g_PointerPosition"));
        QVERIFY(dependsOnPointer("g_ParallaxPosition"));
        QVERIFY(!dependsOnPointer("g_Time"));
    }
};

QTEST_GUILESS_MAIN(TstSceneVkUniforms)
#include "tst_scenevk_uniforms.moc"
