// murasaki-scene-vk renderer: synthetic scenes drawn on the GPU and read back through the shared-memory path.
// Skips without a Vulkan 1.3 device. Small scenes at 1:1 make the expected pixels exact; the checks that involve
// filtering use a tolerance of a couple of levels.

#include "core/context.h"
#include "core/framebuffers.h"
#include "audio/backend.h"
#include "audiofixtures.h"
#include "render/scenerenderer.h"
#include "render/shaderlib.h"
#include "text/textraster.h"
#include "render/targets.h"
#include "scene/scene.h"
#include "puppetfixtures.h"
#include "support/meshwriter.h"
#include "scenevkfixtures.h"

#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cmath>
#include <functional>
#include <memory>
#include <set>

using namespace Qt::StringLiterals;
using namespace murasaki::scene;
using namespace scenevkfix;
namespace bridge = murasaki::bridge;

namespace {

using Px = std::array<int, 4>;

struct Frame {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba;
    Px at(uint32_t x, uint32_t y) const
    {
        const size_t i = (static_cast<size_t>(y) * width + x) * 4;
        return {rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]};
    }
};

bool near(const Px &a, const Px &b, int tolerance = 2)
{
    for (int i = 0; i < 4; ++i) {
        if (std::abs(a[static_cast<size_t>(i)] - b[static_cast<size_t>(i)]) > tolerance) {
            return false;
        }
    }
    return true;
}

QString show(const Px &p)
{
    return QStringLiteral("(%1,%2,%3,%4)").arg(p[0]).arg(p[1]).arg(p[2]).arg(p[3]);
}

#define VERIFY_PX(actual, expected, tol)                                                                                      \
    do {                                                                                                                      \
        const Px a_ = (actual);                                                                                               \
        const Px e_ = (expected);                                                                                             \
        QVERIFY2(near(a_, e_, tol), qPrintable(QStringLiteral("got %1, expected %2").arg(show(a_), show(e_))));                \
    } while (false)

QByteArray sceneText(int width, int height, const QByteArray &clear, const QByteArray &objects, const QByteArray &generalExtra = {})
{
    return "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},\"general\":{\"clearcolor\":\"" + clear
        + "\",\"orthogonalprojection\":{\"width\":" + QByteArray::number(width) + ",\"height\":" + QByteArray::number(height) + "}"
        + generalExtra + "},\"objects\":[" + objects + "]}";
}

QByteArray layer(int id, const char *name, const char *origin, const char *size, const QByteArray &extra = {})
{
    return "{\"id\":" + QByteArray::number(id) + ",\"name\":\"" + name + "\",\"image\":\"models/" + name + ".json\",\"origin\":\""
        + origin + "\",\"size\":\"" + size + "\"" + extra + "}";
}

// Texture of 2x2 pixels: red green / blue yellow, sampled without interpolation.
TexSpec quadrants()
{
    TexSpec s;
    s.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
    s.texW = s.imgW = 2;
    s.texH = s.imgH = 2;
    s.images = {{MipSpec{2, 2, {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 0, 255}, false}}};
    return s;
}

const char *kPassVert = R"(
uniform mat4 g_ModelViewProjectionMatrix;
attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec2 v_TexCoord;
void main() {
    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
    v_TexCoord = a_TexCoord;
}
)";

// An effect with one pass: `frag` is the pass's fragment shader, `effectJson` the effect (passes use the material of
// the same name unless the JSON says otherwise).
void addEffect(Item &item, const QString &name, const QByteArray &frag, const QByteArray &effectJson, const QByteArray &vert = kPassVert,
               const QByteArray &materialExtra = {})
{
    item.file("shaders/effects/" + name + ".vert", vert);
    item.file("shaders/effects/" + name + ".frag", frag);
    item.file("materials/effects/" + name + ".json",
              "{\"passes\":[{\"shader\":\"effects/" + name.toUtf8() + "\",\"blending\":\"normal\"" + materialExtra + "}]}");
    item.file("effects/" + name + "/effect.json", effectJson);
}

QByteArray simpleEffect(const QString &name)
{
    return "{\"passes\":[{\"material\":\"materials/effects/" + name.toUtf8() + ".json\"}]}";
}

const char *kInvertFrag = R"(
uniform sampler2D g_Texture0;
uniform float g_Amount; // {"material":"amount","label":"a","default":1}
varying vec2 v_TexCoord;
void main() {
    vec4 c = texSample2D(g_Texture0, v_TexCoord);
    c.rgb = lerp(c.rgb, 1.0 - c.rgb, g_Amount);
    gl_FragColor = c;
}
)";

QByteArray fxInstance(const QString &file, const QByteArray &passes = "[{}]", const QByteArray &visible = "true")
{
    return "{\"file\":\"effects/" + file.toUtf8() + "/effect.json\",\"visible\":" + visible + ",\"passes\":" + passes + "}";
}

QByteArray withEffects(const QByteArray &effects)
{
    return ",\"effects\":[" + effects + "]";
}

TexSpec solid(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
{
    TexSpec s;
    s.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
    s.texW = s.imgW = 1;
    s.texH = s.imgH = 1;
    s.images = {{MipSpec{1, 1, {r, g, b, a}, false}}};
    return s;
}


// A scene with one particle object. The item carries a minimal genericparticle (same vertex layout and names as
// WE's, no rotation or fog), a white dot texture and the system named by `body`.
const char *kParticleVert = R"(
uniform mat4 g_ModelViewProjectionMatrix;
uniform vec4 g_Texture0Resolution;
attribute vec3 a_Position;
attribute vec4 a_TexCoordVec4;
attribute vec4 a_Color;
attribute vec4 a_TexCoordVec4C1;
attribute vec2 a_TexCoordC2;
varying vec2 v_TexCoord;
varying vec4 v_Color;
void main() {
    float ratio = g_Texture0Resolution.y / g_Texture0Resolution.x;
    vec3 p = a_Position + a_TexCoordVec4.w * vec3(a_TexCoordVec4.x - 0.5, -(a_TexCoordVec4.y - 0.5) * ratio, 0.0);
    gl_Position = mul(vec4(p, 1.0), g_ModelViewProjectionMatrix);
    v_TexCoord = a_TexCoordVec4.xy;
    v_Color = a_Color;
}
)";
const char *kParticleFrag = R"(
uniform sampler2D g_Texture0;
varying vec2 v_TexCoord;
varying vec4 v_Color;
void main() { gl_FragColor = v_Color * texSample2D(g_Texture0, v_TexCoord); }
)";


// A rope shader with WE's vertex layout: each piece is a quad from its start to its end point, the width taken from the
// sizes at both ends, perpendicular to the piece in the plane.
const char *kRopeVert = R"(
uniform mat4 g_ModelViewProjectionMatrix;
attribute vec4 a_PositionVec4;
attribute vec4 a_TexCoordVec4;
attribute vec4 a_TexCoordVec4C1;
attribute vec4 a_TexCoordVec4C2;
attribute vec4 a_TexCoordVec4C3;
attribute vec2 a_TexCoordC4;
attribute vec4 a_Color;
varying vec4 v_Color;
varying vec2 v_TexCoord;
void main() {
    vec3 start = a_PositionVec4.xyz;
    vec3 end = a_TexCoordVec4.xyz;
    vec3 d = normalize(end - start);
    vec3 side = vec3(-d.y, d.x, 0.0);
    float size = mix(a_PositionVec4.w, a_TexCoordVec4C2.w, a_TexCoordC4.y);
    vec3 p = mix(start, end, a_TexCoordC4.y) + side * size * (a_TexCoordC4.x - 0.5);
    gl_Position = mul(vec4(p, 1.0), g_ModelViewProjectionMatrix);
    v_Color = mix(a_Color, a_TexCoordVec4C3, a_TexCoordC4.y);
    v_TexCoord = a_TexCoordC4;
}
)";

void particleItem(Item &item, const QByteArray &systemBody, const QByteArray &objectExtra = {}, int size = 200)
{
    item.file(u"shaders/genericparticle.vert"_s, kParticleVert);
    item.file(u"shaders/genericparticle.frag"_s, kParticleFrag);
    item.file(u"shaders/genericropeparticle.vert"_s, kRopeVert);
    item.file(u"shaders/genericropeparticle.frag"_s, kParticleFrag);
    item.tex(u"particle/dot"_s, solid(255, 255, 255));
    item.file(u"materials/presets/dot.json"_s, R"({"passes":[{"blending":"additive","shader":"genericparticle","textures":["particle/dot"]}]})");
    item.file(u"particles/p.json"_s, "{\"material\":\"materials/presets/dot.json\"," + systemBody + "}");
    item.file(u"scene.json"_s,
              sceneText(size, size, "0 0 0.2",
                        "{\"id\":2,\"name\":\"p\",\"particle\":\"particles/p.json\",\"origin\":\"" + QByteArray::number(size / 2) + " " + QByteArray::number(size / 2)
                            + " 0\",\"scale\":\"1 1 1\",\"angles\":\"0 0 0\"" + objectExtra + "}"));
}


// A scene with one puppet object (a 20 x 20 quad of the test model, red) at (50, 50); `layers` is the object's
// animationlayers JSON ("" for none); the puppet shader skins by g_Bones when the engine says SKINNING.
void puppetItem(Item &item, const std::vector<uint8_t> &mdl, const QByteArray &layers, const QByteArray &objectExtra = {})
{
    item.tex(u"red"_s, solid(255, 0, 0));
    item.file(u"shaders/puppetshader.vert"_s,
              "uniform mat4 g_ModelViewProjectionMatrix;\n#if SKINNING\nuniform mat4x3 g_Bones[BONECOUNT];\nattribute uvec4 a_BlendIndices;\nattribute vec4 a_BlendWeights;\n#endif\n"
              "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
              "void main() {\n  vec3 p = a_Position;\n#if SKINNING\n"
              "  p = mul(vec4(p, 1.0), g_Bones[a_BlendIndices.x] * a_BlendWeights.x + g_Bones[a_BlendIndices.y] * a_BlendWeights.y\n"
              "                      + g_Bones[a_BlendIndices.z] * a_BlendWeights.z + g_Bones[a_BlendIndices.w] * a_BlendWeights.w);\n#endif\n"
              "  gl_Position = mul(vec4(p, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n");
    item.file(u"shaders/puppetshader.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, vec2(0.5, 0.5)); }\n");
    item.file(u"materials/pup.json"_s, R"({"passes":[{"shader":"puppetshader","blending":"translucent","textures":["red"]}]})");
    item.file(u"models/pup.json"_s, R"({"material":"materials/pup.json","puppet":"models/pup.mdl"})");
    item.file(u"models/pup.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
    item.file(u"scene.json"_s,
              sceneText(100, 100, "0 0 0.2",
                        "{\"id\":2,\"name\":\"pup\",\"image\":\"models/pup.json\",\"origin\":\"50 50 0\",\"size\":\"40 40\"" + layers + objectExtra + "}"));
}

// 3D mesh objects: a shader with generic4's matrices (g_ModelMatrix, g_ViewProjectionMatrix, g_NormalModelMatrix) that shows its
// texture at the vertex's uv, or with NORMALCOLOR the world normal as a colour; a box model; and a material per colour.
void meshItem(Item &item, const QByteArray &objects, int size = 100)
{
    item.file(u"shaders/meshshader.vert"_s,
              "uniform mat4 g_ModelMatrix;\nuniform mat3 g_NormalModelMatrix;\nuniform mat4 g_ViewProjectionMatrix;\n"
              "attribute vec3 a_Position;\nattribute vec3 a_Normal;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvarying vec3 v_Normal;\n"
              "void main() {\n  vec4 world = mul(vec4(a_Position, 1.0), g_ModelMatrix);\n  gl_Position = mul(world, g_ViewProjectionMatrix);\n"
              "  v_TexCoord = a_TexCoord;\n  v_Normal = normalize(mul(a_Normal, g_NormalModelMatrix));\n}\n");
    item.file(u"shaders/meshshader.frag"_s,
              "// [COMBO] {\"material\":\"n\",\"combo\":\"NORMALCOLOR\",\"default\":0}\n"
              "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvarying vec3 v_Normal;\n"
              "void main() {\n#if NORMALCOLOR\n  gl_FragColor = vec4(normalize(v_Normal) * 0.5 + 0.5, 1.0);\n#else\n"
              "  gl_FragColor = texSample2D(g_Texture0, v_TexCoord);\n#endif\n}\n");
    // left half red, right half green: the box's +z face shows green, every other face red
    TexSpec halves;
    halves.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
    halves.texW = halves.imgW = 2;
    halves.texH = halves.imgH = 1;
    halves.images = {{MipSpec{2, 1, {255, 0, 0, 255, 0, 255, 0, 255}, false}}};
    item.tex(u"halves"_s, halves);
    item.tex(u"blue"_s, solid(0, 0, 255));
    item.tex(u"yellow"_s, solid(255, 255, 0));
    meshgen::Model m;
    m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/box.json"));
    const auto mdl = meshgen::write(m);
    item.file(u"models/box.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
    for (const char *colour : {"blue", "yellow"}) {
        meshgen::Model c;
        c.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, std::string("materials/models/") + colour + ".json"));
        const auto bytes = meshgen::write(c);
        item.file(u"models/"_s + QString::fromLatin1(colour) + u".mdl"_s, QByteArray(reinterpret_cast<const char *>(bytes.data()), static_cast<qsizetype>(bytes.size())));
        item.file(u"materials/models/"_s + QString::fromLatin1(colour) + u".json"_s,
                  QByteArray(R"({"passes":[{"shader":"meshshader","blending":"normal","textures":[")") + colour + "\"]}]}");
    }
    item.file(u"scene.json"_s, sceneText(size, size, "0 0 0.2", objects));
}

QByteArray meshObject(int id, const char *model, const char *origin, const QByteArray &extra = {})
{
    return "{\"id\":" + QByteArray::number(id) + ",\"name\":\"m" + QByteArray::number(id) + "\",\"model\":\"models/" + model + ".mdl\",\"origin\":\""
        + origin + "\",\"scale\":\"10 10 10\"" + extra + "}";
}


// Lit materials. The engine writes PerformLighting_V1 where a shader asks `#require LightingV1` and has it call the
// ComputePBRLightShadow / ComputePBRLightShadowInfinite of the common_pbr_2.h it includes. These tests bring their own header of that
// name (written here, much simpler than the shipped one): the light's colour times the albedo, the falloff (1 - distance / radius) ^
// exponent and the cosine to the normal, so the expected pixels can be worked out by hand.
const char *kTestPbrHeader = R"(
vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 lightColor, float radius, float exponent, vec3 specularTint,
                           vec3 baseReflectance, float roughness, float metallic, float shadowFactor)
{
    float d = length(L);
    float reach = clamp(1.0 - d / radius, 0.0, 1.0);
    return albedo * lightColor * pow(reach, exponent) * max(dot(N, L / d), 0.0) * shadowFactor;
}
vec3 ComputePBRLightShadowInfinite(vec3 N, vec3 L, vec3 V, vec3 albedo, vec3 lightColor, vec3 specularTint, vec3 baseReflectance,
                                   float roughness, float metallic, float shadowFactor)
{
    return albedo * lightColor * max(dot(N, L), 0.0) * shadowFactor;
}
// The shadow helpers the engine's lighting function calls, with the interface and arithmetic of the shipped ones (one comparison,
// no filter): a projection's x and y from -1..1 (y up) to the picture's 0..1 (top row first), placed in the atlas by (offset, size)
#ifdef SHADOW_ATLAS_SAMPLER
float PerformShadowMapping(vec3 coords, vec4 place)
{
    return texSample2DCompare(SHADOW_ATLAS_SAMPLER, coords.xy * place.zw + place.xy, coords.z).r;
}
float PerformPointShadowMapping(vec4 coords)
{
    return texSample2DCompare(SHADOW_ATLAS_SAMPLER, coords.xy, coords.z).r;
}
#endif
vec3 CalculateProjectedCoords(vec3 worldPos, mat4 m)
{
    vec4 c = mul(vec4(worldPos, 1.0), m);
    return vec3(c.x / c.w * 0.5 + 0.5, c.w > 0.0 ? 0.5 - c.y / c.w * 0.5 : 2.0, c.z / c.w);
}
vec4 CalculateProjectedCoordsCascades(vec3 worldPos, mat4 m)
{
    vec4 c = mul(vec4(worldPos, 1.0), m);
    vec3 n = c.xyz / c.w;
    float outside = (abs(n.x) >= 0.99 || abs(n.y) >= 0.99 || abs(n.z) >= 0.99) ? 1.0 : 0.0;
    return vec4(n.x * 0.5 + 0.5, 0.5 - n.y * 0.5, n.z, outside);
}
// a cube face by the dominant axis of the light to the point, its view (right, up, back), the projection in `info`, the face read at
// 0.47 of its half-size, the faces in 2 columns (+, -) and 3 rows (x, y, z) of the light's block
vec4 CalculateProjectedCoordsPoint(vec3 worldPos, vec3 origin, vec4 info, vec4 place)
{
    vec3 d = worldPos - origin;
    vec3 a = abs(d);
    vec3 r; vec3 u; vec3 b; vec2 cell;
    if (a.x >= a.y && a.x >= a.z) {
        float s = d.x >= 0.0 ? 1.0 : -1.0;
        r = vec3(0.0, 0.0, s); u = vec3(0.0, 1.0, 0.0); b = vec3(-s, 0.0, 0.0); cell = vec2(d.x >= 0.0 ? 0.0 : 1.0, 0.0);
    } else if (a.y >= a.z) {
        float s = d.y >= 0.0 ? 1.0 : -1.0;
        r = vec3(1.0, 0.0, 0.0); u = vec3(0.0, 0.0, s); b = vec3(0.0, -s, 0.0); cell = vec2(d.y >= 0.0 ? 0.0 : 1.0, 1.0);
    } else {
        float s = d.z >= 0.0 ? 1.0 : -1.0;
        r = vec3(-s, 0.0, 0.0); u = vec3(0.0, 1.0, 0.0); b = vec3(0.0, 0.0, -s); cell = vec2(d.z >= 0.0 ? 0.0 : 1.0, 2.0);
    }
    vec3 v = vec3(dot(r, d), dot(u, d), dot(b, d));
    float w = info.z * v.z + info.w;
    vec2 uv = vec2(v.x, v.y) / w * vec2(0.47, -0.47) + 0.5;
    return vec4((uv + cell) * place.zw * vec2(0.5, 0.3333) + place.xy, (info.x * v.z + info.y) / w, w);
}
)";

// A lit shader the way generic4 and genericimage4 are made: positions through g_ModelMatrix and g_ViewProjectionMatrix, the world
// position and normal handed on, the ambient colour plus PerformLighting_V1. With a texture in slot 1 (NORMALMAP) the normal is read
// from it (rgb * 2 - 1, the layer's own space is the world's here), else it is the vertex normal (a mesh) or +z (a layer).
void addLitShader(Item &item, const QString &name, bool mesh)
{
    item.file(u"shaders/common_pbr_2.h"_s, kTestPbrHeader);
    item.file(u"shaders/"_s + name + u".vert"_s,
              QByteArray("uniform mat4 g_ModelMatrix;\nuniform mat3 g_NormalModelMatrix;\nuniform mat4 g_ViewProjectionMatrix;\n"
                         "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n")
                  + (mesh ? "attribute vec3 a_Normal;\n" : "")
                  + "varying vec2 v_TexCoord;\nvarying vec3 v_Normal;\nvarying vec3 v_WorldPos;\n"
                    "void main() {\n  vec4 world = mul(vec4(a_Position, 1.0), g_ModelMatrix);\n  gl_Position = mul(world, g_ViewProjectionMatrix);\n"
                    "  v_WorldPos = world.xyz;\n  v_TexCoord = a_TexCoord;\n"
                  + (mesh ? "  v_Normal = normalize(mul(a_Normal, g_NormalModelMatrix));\n" : "  v_Normal = normalize(mul(vec3(0.0, 0.0, 1.0), g_NormalModelMatrix));\n")
                  + "}\n");
    item.file(u"shaders/"_s + name + u".frag"_s,
              "// [COMBO] {\"material\":\"l\",\"combo\":\"LIGHTING\",\"default\":1}\n"
              "#if LIGHTS_SHADOW_MAPPING\n#define SHADOW_ATLAS_SAMPLER g_Texture6\n"
              "uniform sampler2DComparison g_Texture6; // {\"hidden\":true,\"default\":\"_rt_shadowAtlas\"}\n#endif\n"
              "#include \"common_pbr_2.h\"\n"
              "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1; // {\"combo\":\"NORMALMAP\"}\nuniform vec3 g_LightAmbientColor;\n"
              "varying vec2 v_TexCoord;\nvarying vec3 v_Normal;\nvarying vec3 v_WorldPos;\n"
              "#require LightingV1\n"
              "void main() {\n  vec3 albedo = texSample2D(g_Texture0, v_TexCoord).rgb;\n"
              "#if NORMALMAP\n  vec3 normal = normalize(texSample2D(g_Texture1, v_TexCoord).rgb * 2.0 - 1.0);\n#else\n  vec3 normal = normalize(v_Normal);\n#endif\n"
              "  vec3 light = PerformLighting_V1(v_WorldPos, albedo, normal, vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.5, 0.0);\n"
              "  gl_FragColor = vec4(g_LightAmbientColor * albedo + light, 1.0);\n}\n");
}

// A white slab 100 x 100 whose front face (+z) lies on z = 0 and fills a 100 x 100 scene, drawn with the lit mesh shader, under
// `lights` (objects of the scene; the ambient colour is black).
void litModels(Item &item)
{
    addLitShader(item, u"litmesh"_s, true);
    item.tex(u"white"_s, solid(255, 255, 255));
    meshgen::Model m;
    m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/slab.json"));
    const auto mdl = meshgen::write(m);
    item.file(u"models/slab.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
    item.file(u"materials/models/slab.json"_s, R"({"passes":[{"shader":"litmesh","blending":"normal","textures":["white"]}]})");
}

void litSlabItem(Item &item, const QByteArray &lights)
{
    litModels(item);
    item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0",
                                         R"({"id":1,"name":"slab","model":"models/slab.mdl","origin":"50 50 -1","scale":"50 50 1"},)" + lights,
                                         R"(,"ambientcolor":"0 0 0")"));
}

// Shadows: the lit white slab of litSlabItem (its face on z = 0, filling the 100 x 100 scene) as the floor, a white cube 20 wide
// (the slab's model at scale 10) at `cube`, and `lights`; the ambient colour is black. Both meshes cast unless `cubeExtra` says.
void shadowItem(Item &item, const QByteArray &lights, const QByteArray &cube = "50 50 20", const QByteArray &cubeExtra = {})
{
    litSlabItem(item, R"({"id":5,"name":"cube","model":"models/slab.mdl","origin":")" + cube + R"(","scale":"10 10 10")" + cubeExtra + "}," + lights);
}

// Volumetric lights: the item carries its own volumetricsback / volumetricsfront (a short march of eight samples with the interface of
// the shipped pair: g_RenderVar1..4, the two depth images in slots 1 and 3, g_EffectModelMatrix, and with SHADOW the atlas in slot 0 and
// g_AltModelMatrix), so the test does not depend on a Wallpaper Engine install.
void volumetricShaders(Item &item)
{
    item.file(u"shaders/volumetricsback.vert"_s,
              "uniform mat4 g_ViewProjectionMatrix;\nuniform mat4 g_AltViewProjectionMatrix;\nattribute vec3 a_Position;\n"
              "void main() { gl_Position = mul(mul(vec4(a_Position, 1.0), g_AltViewProjectionMatrix), g_ViewProjectionMatrix); }\n");
    item.file(u"shaders/volumetricsback.frag"_s, "void main() {}\n");
    // the buffer's blur and the pass that adds it to the scene (the shipped blur_k3 and passthrough, in a few lines each)
    const char *quadVert = "uniform vec4 g_Texture0Resolution;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
                           "void main() { gl_Position = vec4(a_Position, 1.0); v_TexCoord.xy = a_TexCoord; v_TexCoord.zw = 1.0 / g_Texture0Resolution.xy; }\n";
    item.file(u"shaders/blur_k3.vert"_s, quadVert);
    item.file(u"shaders/blur_k3.frag"_s,
              "uniform sampler2D g_Texture0;\nvarying vec4 v_TexCoord;\n"
              "void main() {\n#if VERTICAL\n  vec2 step = vec2(0.0, v_TexCoord.w);\n#else\n  vec2 step = vec2(v_TexCoord.z, 0.0);\n#endif\n"
              "  vec3 c = texSample2D(g_Texture0, v_TexCoord.xy - step).rgb + texSample2D(g_Texture0, v_TexCoord.xy).rgb + texSample2D(g_Texture0, v_TexCoord.xy + step).rgb;\n"
              "  gl_FragColor = vec4(c / 3.0, 1.0);\n}\n");
    item.file(u"shaders/passthrough.vert"_s, quadVert);
    item.file(u"shaders/passthrough.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec4 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord.xy); }\n");
    item.file(u"shaders/volumetricsfront.vert"_s,
              "uniform mat4 g_ViewProjectionMatrix;\nuniform mat4 g_AltViewProjectionMatrix;\nattribute vec3 a_Position;\nvarying vec4 v_ScreenPos;\n"
              "void main() { gl_Position = mul(mul(vec4(a_Position, 1.0), g_AltViewProjectionMatrix), g_ViewProjectionMatrix); v_ScreenPos = gl_Position; }\n");
    item.file(u"shaders/volumetricsfront.frag"_s, R"(
uniform mat4 g_EffectModelMatrix;
uniform mat4 g_AltModelMatrix;
#if SHADOW
uniform sampler2DComparison g_Texture0; // {"hidden":true,"default":"_rt_shadowAtlas"}
#endif
uniform sampler2DBackBuffer g_Texture1; // {"hidden":true,"default":"_rt_volumetricsBack"}
uniform sampler2D g_Texture3; // {"hidden":true,"default":"_rt_volumetricsSingle"}
uniform vec4 g_RenderVar0;
uniform vec4 g_RenderVar1;
uniform vec4 g_RenderVar2;
uniform vec4 g_RenderVar3;
uniform vec4 g_RenderVar4;
uniform vec4 g_Texture1Resolution;
uniform vec4 g_Texture3Resolution;
varying vec4 v_ScreenPos;
void main() {
    vec3 ndc = v_ScreenPos.xyz / v_ScreenPos.w;
    vec2 uv = ndc.xy * vec2(0.5, -0.5) + vec2(0.5, 0.5);
    float backDepth = texSample2DBackBuffer(g_Texture1, uv, g_Texture1Resolution.xy).r;
    float limitDepth = texLoad2D(g_Texture3, uv, g_Texture3Resolution.xy).r;
    clip(backDepth - ndc.z);
    backDepth = min(backDepth, limitDepth);
    vec4 start = mul(vec4(ndc, 1.0), g_EffectModelMatrix);
    vec4 end = mul(vec4(ndc.xy, backDepth, 1.0), g_EffectModelMatrix);
    start.xyz /= start.w;
    end.xyz /= end.w;
    float sum = 0.0;
    for (int i = 0; i < 8; ++i) {
        vec3 p = mix(start.xyz, end.xyz, (float(i) + 0.5) / 8.0);
        vec3 delta = p - g_RenderVar2.xyz;
        float fall = pow(saturate(1.0 - length(delta) / g_RenderVar1.x), g_RenderVar4.w);
#if POINTLIGHT
        float cone = 1.0;
#else
        float cone = smoothstep(g_RenderVar1.z, g_RenderVar1.y, dot(normalize(delta), g_RenderVar3.xyz));
#endif
        float lit = 1.0;
#if SHADOW
        vec4 uvs = mul(vec4(p, 1.0), g_AltModelMatrix);
        uvs.xyz /= uvs.w;
        uvs.xy = uvs.xy * vec2(0.5, -0.5) + vec2(0.5, 0.5);
        uvs.xy = uvs.xy * g_RenderVar0.zw + g_RenderVar0.xy;
        lit = texSample2DCompare(g_Texture0, uvs.xy, uvs.z).r;
#endif
        sum += fall * cone * lit;
    }
    float reach = length(end.xyz - start.xyz) / g_RenderVar1.x;
    gl_FragColor = vec4(g_RenderVar2.w * g_RenderVar1.w * (sum / 8.0) * reach * g_RenderVar4.xyz * 0.5, 1.0);
}
)");
}

QByteArray beamLight(const char *extra = "")
{
    // a spot 60 above the middle shining down -z, a narrow cone (inner 10 degrees, outer 14: 15 wide where the light's radius ends)
    return QByteArray(R"({"id":2,"light":"lspot","origin":"50 50 60","angles":"0 1.5707963 0","color":"1 1 1","intensity":1,"radius":60,"exponent":0,)"
                      R"("volumetricsexponent":0,"innercone":10,"outercone":14,"density":2,"castvolumetrics":true)")
        + extra + "}";
}

// An audio backend driven by hand (the renderer's audio must not reach the machine's sound system in tests).
struct FakeAudio : AudioBackend {
    bool startPlayback(Pull pull, std::string *) override
    {
        m_pull = std::move(pull);
        ++playbacks;
        return true;
    }
    bool startCapture(Push push, std::string *) override
    {
        m_push = std::move(push);
        ++captures;
        return true;
    }
    void stopPlayback() override { m_pull = nullptr; }
    void stopCapture() override { m_push = nullptr; }
    Pull m_pull;
    Push m_push;
    int playbacks = 0, captures = 0;
};
FakeAudio *g_fakeAudio = nullptr;

float peakOf(const std::function<void(float *, size_t)> &pull, size_t frames)
{
    std::vector<float> out(frames * 2);
    pull(out.data(), frames);
    float peak = 0.0f;
    for (float v : out)
        peak = std::max(peak, std::fabs(v));
    return peak;
}

// Stand-ins for the shaders of WE's bloom chain, named as the engine asks for them (the real ones are WE's own).
void addBloomShaders(Item &item)
{
    const QByteArray vert =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nuniform vec2 g_TexelSize;\n"
        "void main() { gl_Position = vec4(a_Position, 1.0); v_TexCoord = a_TexCoord; }\n";
    item.file(u"shaders/downsample_quarter_bloom.vert"_s, vert);
    item.file(u"shaders/downsample_quarter_bloom.frag"_s,
              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
              "uniform float g_BloomStrength; // {\"material\":\"bloomstrength\",\"default\":2}\n"
              "uniform float g_BloomThreshold; // {\"material\":\"bloomthreshold\",\"default\":0.65}\n"
              "uniform vec3 g_BloomTint; // {\"material\":\"bloomtint\",\"default\":\"1 1 1\"}\n"
              "void main() { vec3 c = texSample2D(g_Texture0, v_TexCoord).rgb; float m = max(max(c.x, c.y), c.z);\n"
              "  gl_FragColor = vec4(max(vec3(0.0), c * saturate(m - g_BloomThreshold) * g_BloomStrength * g_BloomTint), 1.0); }\n");
    // the blurs: a box of the neighbours along one axis (the engine only has to run them in order into the right targets)
    const auto blur = [&](const char *name, bool x) {
        item.file(u"shaders/"_s + QString::fromLatin1(name) + u".vert"_s, vert);
        item.file(u"shaders/"_s + QString::fromLatin1(name) + u".frag"_s,
                  QByteArray("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform vec2 g_TexelSize;\n"
                             "void main() { vec3 sum = vec3(0.0);\n  for (int i = -4; i <= 4; ++i) { vec2 d = ")
                      + (x ? "vec2(float(i) * g_TexelSize.x * 8.0, 0.0)" : "vec2(0.0, float(i) * g_TexelSize.y * 8.0)")
                      + "; sum += texSample2D(g_Texture0, v_TexCoord + d).rgb; }\n  gl_FragColor = vec4(sum / 9.0, 1.0); }\n");
    };
    blur("downsample_eighth_blur_v", true);
    blur("blur_h_bloom", false);
    item.file(u"shaders/combine.vert"_s, vert);
    item.file(u"shaders/combine.frag"_s,
              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
              "void main() { gl_FragColor = vec4(texSample2D(g_Texture0, v_TexCoord).rgb + texSample2D(g_Texture1, v_TexCoord).rgb, 1.0); }\n");
}

// Stand-ins for the shaders of WE's HDR bloom chain (the engine runs WE's own; these follow their interface: g_RenderVar0 holds
// the four taps' offsets, BLOOM keeps what is above the threshold g_BloomBlendParams.x (with the soft knee of y, z, w below it)
// and scales by the strength, UPSAMPLE scales by the scatter,
// combine_hdr adds the bloom and clamps).
void addHdrBloomShaders(Item &item)
{
    const QByteArray vert =
        "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
        "void main() { gl_Position = vec4(a_Position, 1.0); v_TexCoord = a_TexCoord; }\n";
    item.file(u"shaders/hdr_downsample.vert"_s, vert);
    item.file(u"shaders/hdr_downsample.frag"_s,
              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform vec4 g_RenderVar0;\n"
              "#if BLOOM\nuniform float g_BloomStrength; // {\"material\":\"bloomstrength\",\"default\":2}\n"
              "uniform vec4 g_BloomBlendParams; // {\"material\":\"blend\",\"default\":\"1 1 0 1\"}\n#endif\n"
              "#if UPSAMPLE\nuniform float g_BloomScatter; // {\"material\":\"scatter\",\"default\":1}\n#endif\n"
              "void main() {\n"
              "  vec3 c = texSample2D(g_Texture0, v_TexCoord + g_RenderVar0.xy).rgb + texSample2D(g_Texture0, v_TexCoord + g_RenderVar0.zw).rgb\n"
              "         + texSample2D(g_Texture0, v_TexCoord + g_RenderVar0.xw).rgb + texSample2D(g_Texture0, v_TexCoord + g_RenderVar0.zy).rgb;\n"
              "  c *= 0.25;\n"
              "#if UPSAMPLE\n  c *= g_BloomScatter;\n#endif\n"
              "#if BLOOM\n  float m = max(max(c.x, c.y), c.z);\n"
              "  float soft = clamp(m - g_BloomBlendParams.y, 0.0, g_BloomBlendParams.z);\n"
              "  float pass = max(soft * soft * g_BloomBlendParams.w, m - g_BloomBlendParams.x) / max(m, 0.00001);\n"
              "  c *= pass * g_BloomStrength;\n#endif\n"
              "  gl_FragColor = vec4(c, 1.0);\n}\n");
    item.file(u"shaders/combine_hdr.vert"_s, vert);
    item.file(u"shaders/combine_hdr.frag"_s,
              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\n"
              "void main() { gl_FragColor = vec4(saturate(texSample2D(g_Texture0, v_TexCoord).rgb + texSample2D(g_Texture1, v_TexCoord).rgb), 1.0); }\n");
}

// An image shader with a fog interface like WE's genericimage4: the FOG combo (on by default) opts in, the engine's FOG_DIST,
// FOG_HEIGHT and FOG_COMPUTED switch the bands, and each band mixes towards its colour by start density + density range * state^2.
void addFogImageShader(Item &item)
{
    item.file(u"shaders/fogimage.vert"_s,
              "uniform mat4 g_ModelViewProjectionMatrix;\nuniform mat4 g_ModelMatrix;\nuniform vec3 g_EyePosition;\n"
              "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
              "#if FOG_COMPUTED && (FOG_DIST || FOG_HEIGHT)\nvarying vec4 v_ViewDir;\n#endif\n"
              "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n"
              "#if FOG_COMPUTED && (FOG_DIST || FOG_HEIGHT)\n  vec4 world = mul(vec4(a_Position, 1.0), g_ModelMatrix);\n"
              "  v_ViewDir = vec4(g_EyePosition - world.xyz, world.y);\n#endif\n}\n");
    item.file(u"shaders/fogimage.frag"_s,
              "// [COMBO] {\"material\":\"fog\",\"combo\":\"FOG\",\"default\":1}\n"
              "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
              "#if FOG_DIST\nuniform vec3 g_FogDistanceColor;\nuniform vec4 g_FogDistanceParams;\n#endif\n"
              "#if FOG_HEIGHT\nuniform vec3 g_FogHeightColor;\nuniform vec4 g_FogHeightParams;\n#endif\n"
              "#if FOG_COMPUTED && (FOG_DIST || FOG_HEIGHT)\nvarying vec4 v_ViewDir;\n#endif\n"
              "void main() {\n  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
              "#if FOG_COMPUTED && FOG_HEIGHT\n  float h = saturate((v_ViewDir.w - g_FogHeightParams.x) / g_FogHeightParams.y);\n"
              "  c.rgb = mix(c.rgb, g_FogHeightColor, g_FogHeightParams.z + g_FogHeightParams.w * h * h);\n#endif\n"
              "#if FOG_COMPUTED && FOG_DIST\n  float d = saturate((length(v_ViewDir.xyz) - g_FogDistanceParams.x) / g_FogDistanceParams.y);\n"
              "  c.rgb = mix(c.rgb, g_FogDistanceColor, g_FogDistanceParams.z + g_FogDistanceParams.w * d * d);\n#endif\n"
              "  gl_FragColor = c;\n}\n");
}

// sdrBloomIsThePreviousBuilds: what ea8fbf2 drew
constexpr const char *kSdrBloomPixels = "(200,200,146,255) (9,9,58,255) (8,8,57,255) (200,200,146,255) (0,0,51,255) ";

struct Bounds {
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
    int lit = 0;
};

// Pixels that differ from the background (0, 0, 51).
Bounds litBounds(const Frame &f)
{
    Bounds b;
    for (uint32_t y = 0; y < f.height; ++y) {
        for (uint32_t x = 0; x < f.width; ++x) {
            const Px p = f.at(x, y);
            if (std::abs(p[0]) > 8 || std::abs(p[1]) > 8 || std::abs(p[2] - 51) > 8) {
                b.x0 = std::min<int>(b.x0, static_cast<int>(x));
                b.y0 = std::min<int>(b.y0, static_cast<int>(y));
                b.x1 = std::max<int>(b.x1, static_cast<int>(x));
                b.y1 = std::max<int>(b.y1, static_cast<int>(y));
                ++b.lit;
            }
        }
    }
    return b;
}

// A text object (the font is whatever sans serif the machine has).
QByteArray textObject(int id, const char *text, const char *origin, const QByteArray &extra = {})
{
    return "{\"id\":" + QByteArray::number(id) + ",\"name\":\"t" + QByteArray::number(id) + "\",\"text\":" + text
        + ",\"font\":\"systemfont_sans\",\"origin\":\"" + origin + "\",\"pointsize\":8,\"color\":\"0 1 0\"" + extra + "}";
}

// Pixels that are not black.
Bounds brightBounds(const Frame &f, int threshold = 40)
{
    Bounds b;
    for (uint32_t y = 0; y < f.height; ++y) {
        for (uint32_t x = 0; x < f.width; ++x) {
            const Px p = f.at(x, y);
            if (p[0] + p[1] + p[2] > threshold) {
                b.x0 = std::min<int>(b.x0, static_cast<int>(x));
                b.y0 = std::min<int>(b.y0, static_cast<int>(y));
                b.x1 = std::max<int>(b.x1, static_cast<int>(x));
                b.y1 = std::max<int>(b.y1, static_cast<int>(y));
                ++b.lit;
            }
        }
    }
    return b;
}

} // namespace

class TstSceneVkRender : public QObject
{
    Q_OBJECT

    std::unique_ptr<VkContext> m_ctx;
    QString m_skip;
    std::string m_lastError;
    double m_frameStep = 0.25; // seconds between the frames render() draws
    QTemporaryDir m_cache;

    // Renders `frames` frames of the item at the given output size and returns the last one.
    Frame render(Item &item, uint32_t width, uint32_t height, bool *supported = nullptr, uint64_t *scenePasses = nullptr, int frames = 1,
                 bridge::msg::ScalingMode scaling = bridge::msg::ScalingMode::Stretch, PropertyStore *props = nullptr,
                 const std::function<void(SceneRenderer &, int)> &between = {})
    {
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string error;
        auto scene = loadScene(vfs, "scene.json", &error);
        if (!scene) {
            qFatal("loadScene: %s", error.c_str());
        }
        if (supported) {
            *supported = scene->supported();
        }
        PropertyStore local;
        PropertyStore &properties = props ? *props : local;
        SceneRenderer renderer(*m_ctx, std::move(*scene), vfs, properties);
        renderer.setScaling(scaling);
        auto set = FrameBufferSet::create(*m_ctx, width, height, Transport::Shm, error);
        if (!set) {
            qFatal("buffers: %s", error.c_str());
        }
        for (int f = 0; f < frames; ++f) {
            if (between) {
                between(renderer, f);
            }
            m_ctx->submitAndWait([&](VkCommandBuffer cb) {
                set->cmdAcquire(cb, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                FrameParams params;
                params.cb = cb;
                params.image = set->image(0);
                params.view = set->view(0);
                params.extent = set->extent();
                params.timeSeconds = f * m_frameStep;
                renderer.render(params);
                set->cmdRelease(cb, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            });
            set->finishFrame(0);
        }
        if (scenePasses) {
            *scenePasses = renderer.scenePasses();
        }
        Frame out;
        out.width = width;
        out.height = height;
        const auto bytes = set->ring().frame(0);
        out.rgba.assign(bytes.begin(), bytes.end());
        return out;
    }

private slots:
    void init() { m_frameStep = 0.25; }

    void initTestCase()
    {
        qputenv("XDG_CACHE_HOME", m_cache.path().toUtf8()); // shader cache of the test, not the user's
        qputenv("MURASAKI_VK_VALIDATE", "1");
        std::string error;
        m_ctx = VkContext::create(contextOptionsFromEnvironment(), error);
        if (!m_ctx)
            m_skip = QString::fromStdString(error);
    }

    void cleanupTestCase()
    {
        m_ctx.reset();
        QCOMPARE(validationMessageCount(), 0u);
    }

    void clearColourOnlyScene()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.file(u"scene.json"_s, sceneText(100, 50, "0.2 0.4 1.0", {}));
        const Frame f = render(item, 100, 50);
        VERIFY_PX(f.at(0, 0), (Px{51, 102, 255, 255}), 0);
        VERIFY_PX(f.at(99, 49), (Px{51, 102, 255, 255}), 0);
    }

    void pictureIsUprightAndNotMirrored()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        item.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "100 50")));
        bool supported = false;
        const Frame f = render(item, 100, 50, &supported);
        QVERIFY(supported);
        VERIFY_PX(f.at(25, 12), (Px{255, 0, 0, 255}), 0);   // top left of the picture: red
        VERIFY_PX(f.at(75, 12), (Px{0, 255, 0, 255}), 0);   // top right: green
        VERIFY_PX(f.at(25, 37), (Px{0, 0, 255, 255}), 0);   // bottom left: blue
        VERIFY_PX(f.at(75, 37), (Px{255, 255, 0, 255}), 0); // bottom right: yellow
    }

    void aScriptOnTheMaterialsColourDrivesTheLayer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a script on `constantshadervalues.color` of the material (2744921335's audio bars): it returns red from the second frame on
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        item.file(u"materials/w.json"_s, R"({"passes":[{"shader":"genericimage2","blending":"translucent","textures":["w"],"constantshadervalues":{)"
                                         R"("color":{"script":"export function update(v) { return new Vec3(1, 0, 0); }","value":"0 1 0"}}}]})");
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20")));
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 3).at(10, 10), (Px{255, 0, 0, 255}), 1);            // the script's colour, not the file's
        item.file(u"materials/w.json"_s, R"({"passes":[{"shader":"genericimage2","blending":"translucent","textures":["w"],"constantshadervalues":{"color":"0 1 0"}}]})");
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 3).at(10, 10), (Px{0, 255, 0, 255}), 1);            // without it, the file's
    }

    void aTargetBaseTextureBindsTheFilesBesideIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // 2244339517's clouds: slot 0 is the scene, slot 1 a file the shader reads as its own picture
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.tex(u"green"_s, solid(0, 255, 0));
        item.file(u"shaders/own.vert"_s, kPassVert);
        item.file(u"shaders/own.frag"_s, "uniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture1, v_TexCoord); }\n");
        item.file(u"models/util/fs.json"_s, R"({"material":"materials/util/fs.json","fullscreen":true})");
        item.file(u"materials/util/fs.json"_s, R"({"passes":[{"shader":"own","blending":"normal","textures":["_rt_FullFrameBuffer","green"]}]})");
        item.file(u"scene.json"_s, sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20") + R"(,{"id":2,"name":"fs","image":"models/util/fs.json"})"));
        const Frame f = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(f.at(20, 20), (Px{0, 255, 0, 255}), 2);
        VERIFY_PX(f.at(2, 2), (Px{0, 255, 0, 255}), 2);
    }

    void aMeshThatReadsTheReflectionTargetDrawsWithItEmpty()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // `_rt_Reflection` holds transparent black (nothing is `reflected`): the shader adds a quarter of red to it
        Item item;
        litModels(item);
        item.file(u"shaders/own.vert"_s, kPassVert);
        item.file(u"shaders/own.frag"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(texSample2D(g_Texture1, v_TexCoord).rgb + vec3(0.25, 0.0, 0.0), 1.0); }\n");
        item.file(u"materials/models/slab.json"_s, R"({"passes":[{"shader":"own","blending":"normal","textures":["white","_rt_Reflection"]}]})");
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", R"({"id":1,"name":"slab","model":"models/slab.mdl","origin":"50 50 -1","scale":"50 50 1"})"));
        bool supported = false;
        const Frame f = render(item, 100, 100, &supported, nullptr, 2);
        QVERIFY(supported);
        VERIFY_PX(f.at(50, 50), (Px{64, 0, 0, 255}), 2);
    }

    void aFrameWithMoreDrawsThanOneDescriptorPoolHolds()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // 2600 pictures, one draw each: past the 2048 texture sets of the first pool, so the frame chains a second one (the last
        // picture, the topmost, is the quadrants)
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        QByteArray layers;
        for (int i = 0; i < 2600; ++i)
            layers += QByteArray(i ? "," : "") + layer(i + 1, "q", "50 25 0", "100 50");
        item.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layers));
        bool supported = false;
        uint64_t firstFrameDraws = 0;
        const Frame f = render(item, 100, 50, &supported, nullptr, 2, bridge::msg::ScalingMode::Stretch, nullptr,
                               [&](SceneRenderer &r, int frame) { firstFrameDraws = frame == 1 ? r.draws() : firstFrameDraws; }); // twice: the pools are reset and reused
        QVERIFY(supported);
        QVERIFY2(firstFrameDraws > kPassTextureSets, qPrintable(QString::number(firstFrameDraws)));
        VERIFY_PX(f.at(25, 12), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(f.at(75, 37), (Px{255, 255, 0, 255}), 0);
    }

    void rotationAndScaleAboutTheOrigin()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        // 100x50 picture centred in a 100x100 scene, turned a quarter turn counter-clockwise (+z)
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "q", "50 50 0", "100 50", ",\"angles\":\"0 0 1.5707963\"")));
        const Frame f = render(item, 100, 100);
        // the red top left corner went to the lower left: x in [25,50], rows from the top in [50,100]
        VERIFY_PX(f.at(37, 75), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(f.at(37, 25), (Px{0, 255, 0, 255}), 0);
        VERIFY_PX(f.at(62, 75), (Px{0, 0, 255, 255}), 0);
        VERIFY_PX(f.at(62, 25), (Px{255, 255, 0, 255}), 0);
        VERIFY_PX(f.at(10, 50), (Px{0, 0, 0, 255}), 0); // outside the rotated picture

        // scale: half size around the origin
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "q", "50 50 0", "100 50", ",\"scale\":\"0.5 0.5 1\"")));
        const Frame g = render(item, 100, 100);
        VERIFY_PX(g.at(37, 44), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(g.at(10, 44), (Px{0, 0, 0, 255}), 0);
    }

    void alignmentMovesTheQuadAnchor()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"s"_s);
        item.tex(u"s"_s, solid(255, 255, 255));
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "s", "0 0 0", "50 50", ",\"alignment\":\"bottomleft\"")));
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(10, 90), (Px{255, 255, 255, 255}), 0); // lower left corner of the scene
        VERIFY_PX(f.at(80, 10), (Px{0, 0, 0, 255}), 0);
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "s", "100 100 0", "50 50", ",\"alignment\":\"topright\"")));
        const Frame g = render(item, 100, 100);
        VERIFY_PX(g.at(10, 10), (Px{0, 0, 0, 255}), 0);
        VERIFY_PX(g.at(90, 10), (Px{255, 255, 255, 255}), 0); // the quad's top right corner sits on the origin: it fills [50,100]
        VERIFY_PX(g.at(90, 90), (Px{0, 0, 0, 255}), 0);
        // centred default for comparison: only a quarter of the quad is inside the scene
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "s", "100 100 0", "50 50")));
        const Frame h = render(item, 100, 100);
        VERIFY_PX(h.at(90, 10), (Px{255, 255, 255, 255}), 0);
    }

    void blendingColourAlphaAndBrightness()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // translucent half alpha over a mid grey
        item.file(u"scene.json"_s, sceneText(20, 20, "0.5 0.5 0.5", layer(1, "w", "10 10 0", "20 20", ",\"alpha\":0.5")));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{191, 191, 191, 255}), 2); // 0.5*1 + 0.5*0.5
        // colour tint and brightness
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", ",\"color\":\"1 0.5 0\",\"brightness\":0.5")));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{128, 64, 0, 255}), 2);
        // additive
        item.file(u"materials/w.json"_s,
                  "{\"passes\":[{\"blending\":\"additive\",\"shader\":\"genericimage2\",\"textures\":[\"w\"],\"constantshadervalues\":{\"alpha\":0.5}}]}");
        item.file(u"scene.json"_s, sceneText(20, 20, "0.25 0.25 0.25", layer(1, "w", "10 10 0", "20 20")));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{191, 191, 191, 255}), 2); // 0.25 + 0.5*1
        // normal blending ignores the picture's alpha
        item.tex(u"w"_s, solid(255, 0, 0, 10));
        item.file(u"materials/w.json"_s, "{\"passes\":[{\"blending\":\"normal\",\"shader\":\"genericimage2\",\"textures\":[\"w\"]}]}");
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{255, 0, 0, 255}), 0);
        // translucent respects it
        item.file(u"materials/w.json"_s, "{\"passes\":[{\"blending\":\"translucent\",\"shader\":\"genericimage2\",\"textures\":[\"w\"]}]}");
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{71, 61, 61, 255}), 2); // 10/255 red over the 0.25 grey of the scene
    }

    void drawOrderIsFileOrder()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"a"_s);
        item.tex(u"a"_s, solid(255, 0, 0));
        item.image(u"b"_s);
        item.tex(u"b"_s, solid(0, 0, 255));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "a", "10 10 0", "20 20") + "," + layer(2, "b", "10 10 0", "10 10")));
        const Frame f = render(item, 20, 20);
        VERIFY_PX(f.at(2, 2), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(f.at(10, 10), (Px{0, 0, 255, 255}), 0); // the later object is on top
    }

    void onlyTheVisiblePartOfTheStorageIsShown()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // storage 4x4: the left 2x2 texels are the picture (red), the padding is green
        TexSpec s;
        s.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        s.texW = s.texH = 4;
        s.imgW = s.imgH = 2;
        Bytes px;
        for (int y = 0; y < 4; ++y)
            for (int x = 0; x < 4; ++x)
                for (uint8_t v : (x < 2 && y < 2) ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 255, 0, 255})
                    px.push_back(v);
        s.images = {{MipSpec{4, 4, px, false}}};
        Item item;
        item.image(u"p"_s);
        item.tex(u"p"_s, s);
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "p", "10 10 0", "20 20")));
        const Frame f = render(item, 20, 20);
        VERIFY_PX(f.at(1, 1), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(f.at(18, 18), (Px{255, 0, 0, 255}), 0); // the whole quad shows only the picture, never the padding
        VERIFY_PX(f.at(18, 1), (Px{255, 0, 0, 255}), 0);
    }

    void blockCompressedTexturesDraw()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        TexSpec s;
        s.format = 7; // DXT1
        s.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        s.texW = s.texH = s.imgW = s.imgH = 4;
        s.images = {{MipSpec{4, 4, {0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0}, false}}}; // both endpoints red, all indices 0
        Item item;
        item.image(u"d"_s);
        item.tex(u"d"_s, s);
        item.file(u"scene.json"_s, sceneText(8, 8, "0 0 0", layer(1, "d", "4 4 0", "8 8")));
        VERIFY_PX(render(item, 8, 8).at(4, 4), (Px{255, 0, 0, 255}), 0);
    }

    void scalingModes()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "q", "50 50 0", "100 100")));
        // output twice as wide as tall
        const Frame stretch = render(item, 200, 100, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch);
        VERIFY_PX(stretch.at(50, 25), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(stretch.at(150, 25), (Px{0, 255, 0, 255}), 0); // x stretched over the whole width
        const Frame fit = render(item, 200, 100, nullptr, nullptr, 1, bridge::msg::ScalingMode::Fit);
        VERIFY_PX(fit.at(10, 50), (Px{0, 0, 0, 255}), 0); // black bar on the left
        VERIFY_PX(fit.at(190, 50), (Px{0, 0, 0, 255}), 0);
        VERIFY_PX(fit.at(75, 25), (Px{255, 0, 0, 255}), 0); // the 100x100 scene sits in x [50,150]
        VERIFY_PX(fit.at(125, 75), (Px{255, 255, 0, 255}), 0);
        const Frame fill = render(item, 200, 100, nullptr, nullptr, 1, bridge::msg::ScalingMode::Fill);
        VERIFY_PX(fill.at(10, 10), (Px{255, 0, 0, 255}), 0); // the scene covers the output, cropped top and bottom
        VERIFY_PX(fill.at(190, 90), (Px{255, 255, 0, 255}), 0);
        VERIFY_PX(fill.at(90, 5), (Px{255, 0, 0, 255}), 0);
    }

    // One picture across two outputs: each renderer draws its half of the canvas, and side by side the halves are
    // the picture a single renderer draws over the whole canvas (golden image), whatever the scaling mode.
    void spanHalvesMakeTheWholePicture()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "q", "50 50 0", "100 100")));
        for (const auto mode : {bridge::msg::ScalingMode::Fill, bridge::msg::ScalingMode::Fit, bridge::msg::ScalingMode::Stretch}) {
            // Canvas of 400x100 logical units: two outputs of 200x100, the second one drawn at twice the pixel density.
            const Frame whole = render(item, 400, 100, nullptr, nullptr, 1, mode);
            const auto half = [&](float x, uint32_t pixelsPerUnit) {
                return render(item, 200 * pixelsPerUnit, 100 * pixelsPerUnit, nullptr, nullptr, 1, mode, nullptr,
                              [&](SceneRenderer &r, int) {
                                  r.setSpanView(bridge::msg::makeSpanView(true, 400, 100, x, 0, 200, 100));
                              });
            };
            const Frame left = half(0.0f, 1);
            const Frame right = half(200.0f, 1);
            for (uint32_t y = 0; y < 100; y += 7) {
                for (uint32_t x = 0; x < 200; x += 5) {
                    VERIFY_PX(left.at(x, y), whole.at(x, y), 3);
                    VERIFY_PX(right.at(x, y), whole.at(200 + x, y), 3);
                }
            }
            // At twice the density the same half is the same picture, only sharper: compare the flat quadrants.
            const Frame dense = half(200.0f, 2);
            for (uint32_t y = 4; y < 100; y += 24) {
                for (uint32_t x = 4; x < 200; x += 24)
                    VERIFY_PX(dense.at(x * 2, y * 2), whole.at(200 + x, y), 3);
            }
        }

        // Fill on a canvas: the centre of the picture is the seam, so the left half is red over green's side.
        const Frame fillLeft = render(item, 200, 100, nullptr, nullptr, 1, bridge::msg::ScalingMode::Fill, nullptr,
                                      [&](SceneRenderer &r, int) { r.setSpanView(bridge::msg::makeSpanView(true, 400, 100, 0, 0, 200, 100)); });
        VERIFY_PX(fillLeft.at(100, 25), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(fillLeft.at(100, 75), (Px{0, 0, 255, 255}), 0);
        // Fit leaves the outer quarters of the canvas black: the left output starts in the left band.
        const Frame fitLeft = render(item, 200, 100, nullptr, nullptr, 1, bridge::msg::ScalingMode::Fit, nullptr,
                                     [&](SceneRenderer &r, int) { r.setSpanView(bridge::msg::makeSpanView(true, 400, 100, 0, 0, 200, 100)); });
        VERIFY_PX(fitLeft.at(10, 50), (Px{0, 0, 0, 255}), 0);
        VERIFY_PX(fitLeft.at(150, 25), (Px{255, 0, 0, 255}), 0); // the 100x100 picture sits in x [150,250] of the canvas
        // Turning it off again draws the whole picture in the output.
        const Frame off = render(item, 200, 100, nullptr, nullptr, 2, bridge::msg::ScalingMode::Fit, nullptr, [&](SceneRenderer &r, int f) {
            r.setSpanView(bridge::msg::makeSpanView(f == 0, 400, 100, 0, 0, 200, 100));
        });
        VERIFY_PX(off.at(75, 25), (Px{255, 0, 0, 255}), 0);
    }

    void downscalingKeepsSolidAreasSolid()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        item.file(u"scene.json"_s, sceneText(1000, 1000, "0 0 0", layer(1, "q", "500 500 0", "1000 1000")));
        const Frame f = render(item, 100, 100, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch);
        VERIFY_PX(f.at(25, 25), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(f.at(75, 75), (Px{255, 255, 0, 255}), 0);
    }

    void sceneRendersOnlyWhenSomethingChanged()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", ",\"visible\":{\"user\":\"show\",\"value\":true}")));
        uint64_t passes = 0;
        PropertyStore props;
        render(item, 20, 20, nullptr, &passes, 5, bridge::msg::ScalingMode::Stretch, &props);
        QCOMPARE(passes, uint64_t{1}); // five frames, one scene pass

        // hiding the layer through a property redraws exactly once more
        const Frame hidden = render(item, 20, 20, nullptr, &passes, 4, bridge::msg::ScalingMode::Stretch, &props,
                                    [&](SceneRenderer &, int frame) {
                                        if (frame == 2)
                                            props.set("show", false);
                                    });
        QCOMPARE(passes, uint64_t{2});
        VERIFY_PX(hidden.at(5, 5), (Px{0, 0, 0, 255}), 0);

        // changing the scaling mode also redraws
        render(item, 20, 20, nullptr, &passes, 3, bridge::msg::ScalingMode::Fill, &props,
               [](SceneRenderer &r, int frame) {
                   if (frame == 1)
                       r.setScaling(bridge::msg::ScalingMode::Fit);
               });
        QCOMPARE(passes, uint64_t{2});
    }

    // ---------------- effects ----------------

    void layersWithoutTimeOrPointerKeepTheirBuffers()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"a"_s);
        item.image(u"b"_s);
        item.tex(u"a"_s, solid(255, 0, 0));
        item.tex(u"b"_s, solid(255, 255, 255));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addEffect(item, u"clock"_s,
                  "uniform sampler2D g_Texture0;\nuniform float g_Time;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(fract(g_Time), 0.0, 0.0, 1.0); }\n",
                  simpleEffect(u"clock"_s));
        // a: still, with an effect, on the left; b: reads the clock, on the right
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "a", "5 10 0", "10 20", withEffects(fxInstance(u"invert"_s) + QByteArray()))
                                + "," + layer(2, "b", "15 10 0", "10 20", withEffects(fxInstance(u"clock"_s)))));
        uint64_t passes = 0;
        uint64_t hits = 0;
        const Frame f = render(item, 20, 20, nullptr, &passes, 5, bridge::msg::ScalingMode::Stretch, nullptr,
                               [&](SceneRenderer &r, int) { hits = r.layerCacheHits(); });
        QCOMPARE(passes, uint64_t{5});            // the clock draws every frame ...
        QVERIFY2(hits >= 3, qPrintable(QString::number(hits))); // ... but the still layer is drawn once
        VERIFY_PX(f.at(2, 10), (Px{0, 255, 255, 255}), 1);
    }

    void aPropertyRedrawsACachedLayer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"a"_s);
        item.image(u"b"_s);
        item.tex(u"a"_s, solid(255, 0, 0));
        item.tex(u"b"_s, solid(255, 255, 255));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addEffect(item, u"clock"_s,
                  "uniform sampler2D g_Texture0;\nuniform float g_Time;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(fract(g_Time), 0.0, 0.0, 1.0); }\n",
                  simpleEffect(u"clock"_s));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "a", "5 10 0", "10 20", withEffects(fxInstance(u"invert"_s, "[{\"constantshadervalues\":{\"amount\":{\"user\":\"amt\",\"value\":1}}}]")))
                                + "," + layer(2, "b", "15 10 0", "10 20", withEffects(fxInstance(u"clock"_s)))));
        PropertyStore props;
        const Frame f = render(item, 20, 20, nullptr, nullptr, 5, bridge::msg::ScalingMode::Stretch, &props,
                               [&](SceneRenderer &, int frame) {
                                   if (frame == 3)
                                       props.set("amt", 0.0f);
                               });
        // amount 0 leaves the picture as it is: red, not the inverted cyan the cache held
        VERIFY_PX(f.at(2, 10), (Px{255, 0, 0, 255}), 1);
    }

    // A solid layer whose colour script reacts to the pointer: red, green while hovered, blue after a click.
    static void pointerItem(Item &item)
    {
        solidItem(item, ",\"solid\":true,\"color\":{\"value\":\"1 0 0\",\"script\":\"export function cursorEnter(e) { thisLayer.color = new Vec3(0, 1, 0); }"
                        " export function cursorLeave(e) { thisLayer.color = new Vec3(1, 0, 0); }"
                        " export function cursorClick(e) { thisLayer.color = new Vec3(0, 0, 1); }\"}");
    }

    void thePointerHoversAndClicksTheLayerUnderIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto run = [&](int x, int y, const std::vector<unsigned> &buttons) -> Px {
            Item item;
            pointerItem(item);
            bool supported = false;
            const Frame f = render(item, 100, 100, &supported, nullptr, static_cast<int>(buttons.size()) + 1, bridge::msg::ScalingMode::Stretch, nullptr,
                                   [&](SceneRenderer &r, int frame) {
                                       if (frame > 0) {
                                           r.setPointer(x, y, buttons[static_cast<size_t>(frame) - 1], {100, 100});
                                       }
                                   });
            if (!supported)
                qFatal("the pointer scene is not supported");
            return f.at(50, 50);
        };
        VERIFY_PX(run(10, 10, {0, 0}), (Px{255, 0, 0, 255}), 1);   // away from it: red
        VERIFY_PX(run(50, 50, {0, 0}), (Px{0, 255, 0, 255}), 1);   // over it (the 40 x 40 square at the middle): green
        VERIFY_PX(run(50, 50, {1, 0}), (Px{0, 0, 255, 255}), 1);   // pressed, then released over it: a click, blue
        VERIFY_PX(run(31, 69, {0, 0}), (Px{0, 255, 0, 255}), 1);   // the corner is inside
        VERIFY_PX(run(28, 50, {0, 0}), (Px{255, 0, 0, 255}), 1);   // just outside is not
    }

    void aChangedUserPropertyReachesApplyUserProperties()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        solidItem(item, ",\"color\":{\"value\":\"1 0 0\",\"script\":\"export function applyUserProperties(changed) { if (changed.paint) { thisLayer.color = new Vec3(0, 1, 0); } }\"}");
        PropertyStore props;
        const Frame f = render(item, 100, 100, nullptr, nullptr, 3, bridge::msg::ScalingMode::Stretch, &props,
                               [&](SceneRenderer &, int frame) {
                                   if (frame == 1)
                                       props.set("paint", 1.0);
                               });
        VERIFY_PX(f.at(50, 50), (Px{0, 255, 0, 255}), 1);
        // it is also called once when the scripts start, with whatever there is (nothing, here): WE does the same
        Item untouched;
        solidItem(untouched, ",\"color\":{\"value\":\"1 0 0\",\"script\":\"export function applyUserProperties(changed) { if (changed.paint) { thisLayer.color = new Vec3(0, 1, 0); } }\"}");
        VERIFY_PX(render(untouched, 100, 100, nullptr, nullptr, 3).at(50, 50), (Px{255, 0, 0, 255}), 1); // no `paint`: it did not act
    }

    void anFboWithFitIsItsLongSideInPixelsAtTheLayersAspect()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a 100 x 50 layer; the second pass reads the fbo and paints its size (g_Texture1Resolution.xy) as colour / 255
        const char *copy = "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n";
        const char *probe = "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nuniform vec4 g_Texture1Resolution;\nvarying vec2 v_TexCoord;\n"
                            "void main() { gl_FragColor = vec4(g_Texture1Resolution.x / 255.0, g_Texture1Resolution.y / 255.0, 0.0, 1.0); }\n";
        const auto sceneFor = [&](const QByteArray &fbo) {
            Item item;
            item.image(u"w"_s);
            item.tex(u"w"_s, solid(255, 255, 255));
            addEffect(item, u"fitfill"_s, copy, simpleEffect(u"fitfill"_s));
            addEffect(item, u"fitprobe"_s, probe, simpleEffect(u"fitprobe"_s));
            item.file(u"effects/fitprobe/effect.json"_s,
                      "{\"passes\":[{\"material\":\"materials/effects/fitfill.json\",\"target\":\"_rt_Fit\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
                      "{\"material\":\"materials/effects/fitprobe.json\",\"bind\":[{\"name\":\"previous\",\"index\":0},{\"name\":\"_rt_Fit\",\"index\":1}]}],"
                      "\"fbos\":[" + fbo + "]}");
            item.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layer(1, "w", "50 25 0", "100 50", withEffects(fxInstance(u"fitprobe"_s)))));
            return item;
        };
        bool supported = false;
        Item fitted = sceneFor(R"({"name":"_rt_Fit","fit":64,"format":"rgba8888"})");
        VERIFY_PX(render(fitted, 100, 50, &supported).at(50, 25), (Px{64, 32, 0, 255}), 1); // the long side is 64, the short one keeps the aspect
        QVERIFY(supported);
        Item scaled = sceneFor(R"({"name":"_rt_Fit","scale":2,"format":"rgba8888"})");
        VERIFY_PX(render(scaled, 100, 50).at(50, 25), (Px{50, 25, 0, 255}), 1); // without fit: the layer's size over the scale
    }

    void anFboWithUvsRepeatIsSampledWithWrappingCoordinates()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the first pass copies the 2 x 2 picture into an fbo, the second reads it at u + 0.5: at u = 0.75 that is 1.25, which clamps to
        // the right column (green) or wraps to u = 0.25 (red)
        const char *copy = "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n";
        const char *shift = "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                            "void main() { gl_FragColor = texSample2D(g_Texture1, v_TexCoord + vec2(0.5, 0.0)); }\n";
        const auto sceneFor = [&](const QByteArray &uvs) {
            Item item;
            item.image(u"q"_s);
            item.tex(u"q"_s, quadrants());
            addEffect(item, u"wrapfill"_s, copy, simpleEffect(u"wrapfill"_s));
            addEffect(item, u"wrapread"_s, shift, simpleEffect(u"wrapread"_s));
            item.file(u"effects/wrapread/effect.json"_s,
                      "{\"passes\":[{\"material\":\"materials/effects/wrapfill.json\",\"target\":\"_rt_Wrap\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
                      "{\"material\":\"materials/effects/wrapread.json\",\"bind\":[{\"name\":\"previous\",\"index\":0},{\"name\":\"_rt_Wrap\",\"index\":1}]}],"
                      "\"fbos\":[{\"name\":\"_rt_Wrap\",\"format\":\"rgba8888\"" + uvs + "}]}");
            item.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "100 50", withEffects(fxInstance(u"wrapread"_s)))));
            return item;
        };
        Item clamped = sceneFor({});
        const Frame c = render(clamped, 100, 50);
        VERIFY_PX(c.at(75, 12), (Px{0, 255, 0, 255}), 2);
        Item wrapped = sceneFor(R"(,"uvs":"repeat")");
        VERIFY_PX(render(wrapped, 100, 50).at(75, 12), (Px{255, 0, 0, 255}), 2);
    }

    void reflectionSamplesTheSceneCopyAtTheLodTheMipMapInfoGives()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a 16 x 16 checkerboard of one pixel squares below a layer whose shader reads the scene so far through its slot 1 default
        // (_rt_MipMappedFrameBuffer) at lod = amount * g_Texture1MipMapInfo, the way REFLECTION reads it: at amount 0 the checkerboard
        // shows, at amount 1 the top of the chain, the average, a grey
        TexSpec checker;
        checker.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        checker.texW = checker.imgW = 16;
        checker.texH = checker.imgH = 16;
        Bytes pixels;
        for (int y = 0; y < 16; ++y) {
            for (int x = 0; x < 16; ++x) {
                const uint8_t v = (x + y) % 2 ? 255 : 0;
                pixels.insert(pixels.end(), {v, v, v, 255});
            }
        }
        checker.images = {{MipSpec{16, 16, pixels, false}}};
        Item item;
        item.image(u"chk"_s);
        item.tex(u"chk"_s, checker);
        item.tex(u"white"_s, solid(255, 255, 255));
        item.file(u"shaders/mirror.vert"_s, kPassVert);
        item.file(u"shaders/mirror.frag"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1; // {\"hidden\":true,\"default\":\"_rt_MipMappedFrameBuffer\"}\n"
                  "uniform float g_Texture1MipMapInfo;\nuniform float g_Amount; // {\"material\":\"amount\",\"default\":0}\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(texSample2DLod(g_Texture1, v_TexCoord, g_Amount * g_Texture1MipMapInfo).rgb, 1.0); }\n");
        item.file(u"models/mirror.json"_s, R"({"autosize":true,"material":"materials/mirror.json"})");
        const auto sceneFor = [&](const char *amount) {
            item.file(u"materials/mirror.json"_s,
                      R"({"passes":[{"blending":"translucent","shader":"mirror","textures":["white"],"constantshadervalues":{"amount":)" + QByteArray(amount) + "}}]}");
            item.file(u"scene.json"_s, sceneText(16, 16, "0 0 0", layer(1, "chk", "8 8 0", "16 16") + "," + layer(2, "mirror", "8 8 0", "16 16")));
        };
        sceneFor("0");
        bool supported = false;
        const Frame sharp = render(item, 16, 16, &supported);
        QVERIFY(supported);
        QVERIFY(sharp.at(4, 4)[0] != sharp.at(5, 4)[0]); // neighbours differ: the checkerboard
        sceneFor("1");
        const Frame blurred = render(item, 16, 16);
        VERIFY_PX(blurred.at(4, 4), (Px{128, 128, 128, 255}), 3);
        VERIFY_PX(blurred.at(5, 4), (Px{128, 128, 128, 255}), 3);
    }

    void aSceneWithoutMediaSlotsDoesNotWantTheMedia()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        item.file(u"scene.json"_s, sceneText(20, 10, "0 0 0", layer(1, "w", "10 5 0", "20 10")));
        bool wants = true;
        render(item, 20, 10, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, nullptr, [&](SceneRenderer &r, int) { wants = r.wantsMedia(); });
        QVERIFY(!wants);
    }

    void theMediaCoverFillsAUserTextureSlotAndTheOneBeforeItsPreviousSlot()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // two layers over a red placeholder, one showing the cover of the track and one the cover before: red until a cover comes
        Item item;
        item.image(u"cur"_s, R"(,"usertextures":[{"name":"$mediaThumbnail","type":"system"}])");
        item.image(u"prev"_s, R"(,"usertextures":[{"name":"$mediaPreviousThumbnail","type":"system"}])");
        item.tex(u"cur"_s, solid(255, 0, 0));
        item.tex(u"prev"_s, solid(255, 0, 0));
        item.file(u"scene.json"_s, sceneText(20, 10, "0 0 0", layer(1, "cur", "5 5 0", "10 10") + "," + layer(2, "prev", "15 5 0", "10 10")));
        const auto cover = [](uint8_t r, uint8_t g, uint8_t b) {
            std::vector<uint8_t> px;
            for (int i = 0; i < 4; ++i)
                px.insert(px.end(), {r, g, b, uint8_t{255}});
            return px;
        };
        const std::vector<uint8_t> green = cover(0, 255, 0), blue = cover(0, 0, 255);
        const auto shown = [&](int frames, const std::function<void(SceneRenderer &, int)> &between) {
            Item &i = item;
            return render(i, 20, 10, nullptr, nullptr, frames, bridge::msg::ScalingMode::Stretch, nullptr, between);
        };
        bool wants = false;
        const Frame none = shown(1, [&](SceneRenderer &r, int) { wants = r.wantsMedia(); });
        QVERIFY(wants); // a scene with a slot the cover fills says so
        VERIFY_PX(none.at(5, 5), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(none.at(15, 5), (Px{255, 0, 0, 255}), 0);
        const Frame one = shown(2, [&](SceneRenderer &r, int f) {
            if (f == 1)
                r.setMediaThumbnail(2, 2, green);
        });
        VERIFY_PX(one.at(5, 5), (Px{0, 255, 0, 255}), 0);  // the cover
        VERIFY_PX(one.at(15, 5), (Px{255, 0, 0, 255}), 0); // none before it: the placeholder
        const Frame two = shown(3, [&](SceneRenderer &r, int f) {
            if (f == 1)
                r.setMediaThumbnail(2, 2, green);
            if (f == 2)
                r.setMediaThumbnail(2, 2, blue);
        });
        VERIFY_PX(two.at(5, 5), (Px{0, 0, 255, 255}), 0);
        VERIFY_PX(two.at(15, 5), (Px{0, 255, 0, 255}), 0); // the one before
        const Frame gone = shown(3, [&](SceneRenderer &r, int f) {
            if (f == 1)
                r.setMediaThumbnail(2, 2, green);
            if (f == 2)
                r.setMediaThumbnail(0, 0, {}); // the track has no cover
        });
        VERIFY_PX(gone.at(5, 5), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(gone.at(15, 5), (Px{0, 255, 0, 255}), 0);
        // a picture of the wrong size is no cover
        const Frame wrong = shown(2, [&](SceneRenderer &r, int f) {
            if (f == 1)
                r.setMediaThumbnail(3, 3, green);
        });
        VERIFY_PX(wrong.at(5, 5), (Px{255, 0, 0, 255}), 0);
    }

    void theIconOfAnApplicationFillsAShortcutSlotUntilItIsCleared()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // two slots over a red placeholder: launcher1 and launcher2. The host sends the icon of launcher1 only
        Item item;
        item.image(u"one"_s, R"(,"usertextures":[{"name":"launcher1","type":"usershortcut"}])");
        item.image(u"two"_s, R"(,"usertextures":[{"name":"launcher2","type":"usershortcut"}])");
        item.tex(u"one"_s, solid(255, 0, 0));
        item.tex(u"two"_s, solid(255, 0, 0));
        item.file(u"scene.json"_s, sceneText(20, 10, "0 0 0", layer(1, "one", "5 5 0", "10 10") + "," + layer(2, "two", "15 5 0", "10 10")));
        std::vector<uint8_t> green;
        for (int i = 0; i < 4; ++i)
            green.insert(green.end(), {0, 255, 0, 255});
        const auto shown = [&](int frames, const std::function<void(SceneRenderer &, int)> &between) {
            return render(item, 20, 10, nullptr, nullptr, frames, bridge::msg::ScalingMode::Stretch, nullptr, between);
        };
        bool wants = true;
        const Frame none = shown(1, [&](SceneRenderer &r, int) { wants = r.wantsMedia(); });
        QVERIFY(!wants); // a shortcut slot is no media
        VERIFY_PX(none.at(5, 5), (Px{255, 0, 0, 255}), 0); // the file's own texture
        const Frame icon = shown(2, [&](SceneRenderer &r, int f) {
            if (f == 1)
                r.setShortcutIcon("launcher1", 2, 2, green);
        });
        VERIFY_PX(icon.at(5, 5), (Px{0, 255, 0, 255}), 0);   // the icon of its application
        VERIFY_PX(icon.at(15, 5), (Px{255, 0, 0, 255}), 0);  // the other slot has none
        const Frame cleared = shown(3, [&](SceneRenderer &r, int f) {
            if (f == 1)
                r.setShortcutIcon("launcher1", 2, 2, green);
            if (f == 2)
                r.setShortcutIcon("launcher1", 0, 0, {}); // the user cleared it
        });
        VERIFY_PX(cleared.at(5, 5), (Px{255, 0, 0, 255}), 0);
        // a picture of the wrong size and a name that is no slot change nothing
        const Frame wrong = shown(2, [&](SceneRenderer &r, int f) {
            if (f == 1) {
                r.setShortcutIcon("launcher1", 3, 3, green);
                r.setShortcutIcon("nobody", 2, 2, green);
            }
        });
        VERIFY_PX(wrong.at(5, 5), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(wrong.at(15, 5), (Px{255, 0, 0, 255}), 0);
    }

    void keyframesOnAnEffectConstantAnimateIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        // the constant is 1 at frame 0 and 0 from frame 1 on, at 1 frame a second, and it stays at the last key (an intro fade runs so)
        const QByteArray animated =
            "[{\"constantshadervalues\":{\"amount\":{\"value\":1,\"animation\":{\"c0\":["
            "{\"frame\":0,\"value\":1,\"front\":{\"enabled\":false,\"x\":1,\"y\":0},\"back\":{\"enabled\":false,\"x\":-1,\"y\":0}},"
            "{\"frame\":1,\"value\":0,\"front\":{\"enabled\":false,\"x\":1,\"y\":0},\"back\":{\"enabled\":false,\"x\":-1,\"y\":0}}],"
            "\"options\":{\"fps\":1,\"length\":10,\"mode\":\"single\"}}}}}]";
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"invert"_s, animated)))));
        bool supported = false;
        VERIFY_PX(render(item, 20, 20, &supported, nullptr, 1).at(5, 5), (Px{0, 255, 255, 255}), 2); // time 0: inverted
        QVERIFY(supported);
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 6).at(5, 5), (Px{255, 0, 0, 255}), 2);       // 1.25 s on: as it is
    }

    void keyframesThatHaveEndedLeaveTheSceneStill()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        // the constant runs from 1 to 0 in one second and holds the last key: an intro that is over (a fade that has faded) is no reason to
        // draw 30 frames a second for the rest of the scene's life
        const QByteArray animated =
            "[{\"constantshadervalues\":{\"amount\":{\"value\":1,\"animation\":{\"c0\":["
            "{\"frame\":0,\"value\":1,\"front\":{\"enabled\":false,\"x\":1,\"y\":0},\"back\":{\"enabled\":false,\"x\":-1,\"y\":0}},"
            "{\"frame\":1,\"value\":0,\"front\":{\"enabled\":false,\"x\":1,\"y\":0},\"back\":{\"enabled\":false,\"x\":-1,\"y\":0}}],"
            "\"options\":{\"fps\":1,\"length\":10,\"mode\":\"single\"}}}}}]";
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"invert"_s, animated)))));
        bool stillAtTheEnd = false, movingAtTheStart = false;
        uint64_t passes = 0;
        const Frame f = render(item, 20, 20, nullptr, &passes, 14, bridge::msg::ScalingMode::Stretch, nullptr, [&](SceneRenderer &r, int frame) {
            movingAtTheStart = movingAtTheStart || (frame == 1 && r.needsFrame());
            stillAtTheEnd = frame == 13 && !r.needsFrame() && !r.animated();
        });
        VERIFY_PX(f.at(5, 5), (Px{255, 0, 0, 255}), 2); // the final value was drawn
        QVERIFY(movingAtTheStart);
        QVERIFY(stillAtTheEnd);
        QVERIFY2(passes <= 7, qPrintable(QString::number(passes))); // 4 frames of the second and the one that passes the key; not all 14
    }

    void scriptsWithNothingToDoLeaveTheSceneStill()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        const auto needsFrameWith = [&](const char *script) {
            item.file(u"scene.json"_s,
                      sceneText(20, 20, "0 0 0",
                                layer(1, "w", "10 10 0", "20 20",
                                      withEffects(fxInstance(u"invert"_s, QByteArray("[{\"constantshadervalues\":{\"amount\":{\"value\":1,\"script\":\"") + script + "\"}}}]")))));
            bool needs = true;
            render(item, 20, 20, nullptr, nullptr, 6, bridge::msg::ScalingMode::Stretch, nullptr, [&](SceneRenderer &r, int frame) {
                if (frame == 5)
                    needs = r.needsFrame();
            });
            return needs;
        };
        // a script that has done its init and has no update, no timer and no hook is done: the scene is as still as one without scripts
        QVERIFY(!needsFrameWith("export function init(v) { return v; }"));
        // one that throws in its update stops, and then it is done too
        QVERIFY(!needsFrameWith("export function update(v) { throw new Error('no'); }"));
        // an update, a timer or a cursor hook keep the frames coming
        QVERIFY(needsFrameWith("export function update(v) { return v; }"));
        QVERIFY(needsFrameWith("export function init(v) { engine.setInterval(() => {}, 100); return v; }"));
        QVERIFY(needsFrameWith("export function cursorMove(e) {}"));
    }

    void aScriptOnAnEffectConstantDrivesTheConstant()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        const auto sceneFor = [&](const char *script) {
            item.file(u"scene.json"_s,
                      sceneText(20, 20, "0 0 0",
                                layer(1, "w", "10 10 0", "20 20",
                                      withEffects(fxInstance(u"invert"_s, QByteArray("[{\"constantshadervalues\":{\"amount\":{\"value\":1,\"script\":\"") + script + "\"}}}]")))));
        };
        // the script returns what it was given (the constant's own value, 1): inverted
        sceneFor("export function update(v) { return v; }");
        bool supported = false;
        VERIFY_PX(render(item, 20, 20, &supported, nullptr, 2).at(5, 5), (Px{0, 255, 255, 255}), 1);
        QVERIFY(supported);
        // the script sets it to 0: the picture as it is
        sceneFor("export function update(v) { return 0; }");
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 2).at(5, 5), (Px{255, 0, 0, 255}), 1);
        // and one that changes it over time (the scene clock: 0.25 s a frame): half way at the third frame
        sceneFor("export function update(v) { return Math.min(1, engine.runtime * 2); }");
        const Px mid = render(item, 20, 20, nullptr, nullptr, 3).at(5, 5); // runtime 0.5 s -> amount 1; frame 2 (0.5 s)
        QVERIFY2(mid[0] <= 3, qPrintable(QString("%1").arg(mid[0])));
    }

    void aScriptOnTheClearColourPaintsTheBackground()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        solidItem(item, ",\"alpha\":0");
        item.file(u"scene.json"_s,
                  "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},\"general\":{\"orthogonalprojection\":{\"width\":20,\"height\":20},"
                  "\"clearcolor\":{\"value\":\"0 0 0.2\",\"script\":\"export function update(v) { return new Vec3(1, 0, 0); }\"}},"
                  "\"objects\":[{\"id\":2,\"name\":\"s\",\"image\":\"models/util/solidlayer.json\",\"origin\":\"10 10 0\",\"size\":\"2 2\",\"alpha\":0}]}");
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 2).at(2, 2), (Px{255, 0, 0, 255}), 1);
    }

    void withClearEnabledOffTheLastFrameStaysUnderTheNext()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        // red for the first frame (the scene clock: 0.25 s a frame), then not drawn at all
        const QByteArray blink =
            ",\"alpha\":{\"value\":1,\"script\":\"export function update(v) { return engine.runtime < 0.3 ? 1 : 0; }\"}";
        const auto lastFrame = [&](const QByteArray &general) {
            item.file(u"scene.json"_s, sceneText(20, 20, "0 0 1", layer(1, "w", "10 10 0", "20 20", blink), general));
            return render(item, 20, 20, nullptr, nullptr, 4).at(5, 5);
        };
        VERIFY_PX(lastFrame({}), (Px{0, 0, 255, 255}), 1);                                // cleared every frame: the clear colour
        VERIFY_PX(lastFrame(",\"clearenabled\":true"), (Px{0, 0, 255, 255}), 1);
        VERIFY_PX(lastFrame(",\"clearenabled\":false"), (Px{255, 0, 0, 255}), 1);         // never cleared again: the red is still there
    }

    void aScriptOnAnEffectsVisibilityShowsAndHidesIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        const auto sceneFor = [&](const char *script) {
            item.file(u"scene.json"_s,
                      sceneText(20, 20, "0 0 0",
                                layer(1, "w", "10 10 0", "20 20",
                                      withEffects(fxInstance(u"invert"_s, "[{}]", QByteArray("{\"value\":true,\"script\":\"") + script + "\"}")))));
        };
        sceneFor("export function update(v) { return v; }");
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 2).at(5, 5), (Px{0, 255, 255, 255}), 1); // visible: inverted
        sceneFor("export function update(v) { return false; }");
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 2).at(5, 5), (Px{255, 0, 0, 255}), 1);    // the script hid it
    }

    void anInstancesTexturesAndConstantsDrawTheLayer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.tex(u"util/white"_s, solid(255, 255, 255));
        item.tex(u"icon"_s, solid(255, 0, 0));
        item.file(u"models/solid.json"_s, R"({"instanced":true,"material":"materials/solid.json","solidlayer":true})");
        item.file(u"materials/solid.json"_s,
                  R"({"passes":[{"shader":"genericimage2","blending":"translucent","textures":["util/white"],"combos":{"version":2}}]})");
        const auto draw = [&](const QByteArray &instance) {
            item.file(u"scene.json"_s,
                      sceneText(20, 20, "0 0 0", R"({"id":1,"name":"s","image":"models/solid.json","origin":"10 10 0","size":"20 20","instance":)" + instance + "}"));
            bool supported = false;
            const Frame f = render(item, 20, 20, &supported);
            if (!supported) qFatal("scene not supported");
            return f.at(5, 5);
        };
        // the placeholder is white; the instance's texture is the layer's picture; a user texture that nothing fills keeps it
        VERIFY_PX(draw(R"({"combos":{"version":2},"id":2,"textures":["util/white"]})"), (Px{255, 255, 255, 255}), 1);
        VERIFY_PX(draw(R"({"combos":{"version":2},"id":2,"textures":["icon"]})"), (Px{255, 0, 0, 255}), 1);
        VERIFY_PX(draw(R"({"combos":{"version":2},"id":2,"textures":["icon"],"usertextures":[{"name":"s01c","type":"usershortcut"}]})"), (Px{255, 0, 0, 255}), 1);
        VERIFY_PX(draw(R"({"id":2,"textures":["icon"],"usertextures":[{"name":"$mediaThumbnail","type":"system"}]})"), (Px{255, 0, 0, 255}), 1);
        // the instance's constants replace the material's (version 2 tints with g_Color4)
        VERIFY_PX(draw(R"({"id":2,"textures":["icon"],"constantshadervalues":{"alpha":0.5}})"), (Px{128, 0, 0, 255}), 2);
    }

    void aUserTexturePropertyThatIsSetIsReportedOnce()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.tex(u"util/white"_s, solid(255, 255, 255));
        item.tex(u"icon"_s, solid(255, 0, 0));
        item.file(u"models/solid.json"_s, R"({"instanced":true,"material":"materials/solid.json","solidlayer":true})");
        item.file(u"materials/solid.json"_s, R"({"passes":[{"shader":"genericimage2","blending":"translucent","textures":["util/white"]}]})");
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            R"({"id":1,"name":"s","image":"models/solid.json","origin":"10 10 0","size":"20 20","instance":{"id":2,"textures":["icon"],"usertextures":["mine"]}})"));
        PropertyStore props;
        std::vector<std::set<std::string>> seen;
        const Frame f = render(item, 20, 20, nullptr, nullptr, 5, bridge::msg::ScalingMode::Stretch, &props, [&](SceneRenderer &r, int frame) {
            seen.push_back(r.reportedUserTextures());
            if (frame == 1) props.set("mine", std::string()); // the default: nothing to say
            if (frame == 2) props.set("mine", std::string("/home/u/me.png"));
            if (frame == 3) props.set("mine", std::string("/home/u/other.png"));
            if (frame == 3) props.set("unrelated", std::string("x"));
        });
        QVERIFY(seen[0].empty());
        QVERIFY(seen[2].empty());                         // set before frame 2 draws: reported as it draws
        QCOMPARE(seen[3], (std::set<std::string>{"mine"}));
        QCOMPARE(seen[4], (std::set<std::string>{"mine"})); // still once
        VERIFY_PX(f.at(5, 5), (Px{255, 0, 0, 255}), 1);     // the default picture is drawn whatever the property says
    }

    void aMediaThumbnailSlotShowsTheTexturesTheFileListsThere()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"cover"_s, R"(,"usertextures":[{"name":"$mediaThumbnail","type":"system"}])");
        item.tex(u"cover"_s, solid(0, 0, 255));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "cover", "10 10 0", "20 20")));
        bool supported = false;
        VERIFY_PX(render(item, 20, 20, &supported).at(5, 5), (Px{0, 0, 255, 255}), 1);
        QVERIFY(supported);
        // an effect's second picture: the instance lists the default for a thumbnail slot
        item.tex(u"w"_s, solid(255, 0, 0));
        item.tex(u"fallback"_s, solid(0, 255, 0));
        addEffect(item, u"mix"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(texSample2D(g_Texture0, v_TexCoord).r, texSample2D(g_Texture1, v_TexCoord).g, 0.0, 1.0); }\n",
                  simpleEffect(u"mix"_s));
        item.image(u"w"_s);
        const QByteArray fx = R"([{"textures":[null,"fallback"],"usertextures":[null,{"name":"$mediaThumbnail","type":"system"}]}])";
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"mix"_s, fx)))));
        VERIFY_PX(render(item, 20, 20, &supported).at(5, 5), (Px{255, 255, 0, 255}), 1);
        QVERIFY(supported);
    }

    void aSimpleEffectChangesThePicture()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"invert"_s)))));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{0, 255, 255, 255}), 1); // amount defaults to 1: inverted
        // the instance overrides the constant (by the uniform's `material` key)
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"invert"_s, "[{\"constantshadervalues\":{\"amount\":0.5}}]")))));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{128, 128, 128, 255}), 2);
        VERIFY_PX(render(item, 20, 20).at(15, 15), (Px{128, 128, 128, 255}), 2);
        // the layer still has its place in the scene: half the size leaves the clear colour around it
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 1", layer(1, "w", "10 10 0", "10 10", withEffects(fxInstance(u"invert"_s)))));
        const Frame f = render(item, 20, 20);
        VERIFY_PX(f.at(10, 10), (Px{0, 255, 255, 255}), 1);
        VERIFY_PX(f.at(1, 1), (Px{0, 0, 255, 255}), 0);
    }

    void effectsKeepTheOrientationOfThePicture()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        addEffect(item, u"copy"_s,
                  "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n",
                  simpleEffect(u"copy"_s));
        // two passes in a row: the picture goes through both buffers
        item.file(u"scene.json"_s,
                  sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "100 50", withEffects(fxInstance(u"copy"_s) + "," + fxInstance(u"copy"_s)))));
        const Frame f = render(item, 100, 50);
        VERIFY_PX(f.at(25, 12), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(f.at(75, 12), (Px{0, 255, 0, 255}), 0);
        VERIFY_PX(f.at(25, 37), (Px{0, 0, 255, 255}), 0);
        VERIFY_PX(f.at(75, 37), (Px{255, 255, 0, 255}), 0);
        // and a rotated layer with an effect is placed like one without
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0", layer(1, "q", "50 50 0", "100 50", ",\"angles\":\"0 0 1.5707963\"" + withEffects(fxInstance(u"copy"_s)))));
        const Frame g = render(item, 100, 100);
        VERIFY_PX(g.at(37, 75), (Px{255, 0, 0, 255}), 0);
        VERIFY_PX(g.at(62, 25), (Px{255, 255, 0, 255}), 0);
    }

    void textureCoordinatesStartAtTheTopLeft()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        addEffect(item, u"grad"_s, "varying vec2 v_TexCoord;\nvoid main() { gl_FragColor = vec4(v_TexCoord.x, v_TexCoord.y, 0.0, 1.0); }\n",
                  simpleEffect(u"grad"_s));
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "w", "50 50 0", "100 100", withEffects(fxInstance(u"grad"_s)))));
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(2, 2), (Px{5, 5, 0, 255}), 6);    // u = v = 0 at the top left
        VERIFY_PX(f.at(97, 2), (Px{250, 5, 0, 255}), 6); // u grows to the right
        VERIFY_PX(f.at(2, 97), (Px{5, 250, 0, 255}), 6); // v grows downwards
    }

    void effectVisibilityFollowsTheProperty()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "w", "10 10 0", "20 20",
                                  withEffects(fxInstance(u"invert"_s, "[{}]", "{\"user\":\"fx\",\"value\":false}")))));
        PropertyStore props;
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, &props).at(5, 5), (Px{255, 0, 0, 255}), 0); // off
        props.set("fx", true);
        VERIFY_PX(render(item, 20, 20, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, &props).at(5, 5), (Px{0, 255, 255, 255}), 1);
        // a constant bound to a property follows it, live
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "w", "10 10 0", "20 20",
                                  withEffects(fxInstance(u"invert"_s, "[{\"constantshadervalues\":{\"amount\":{\"user\":\"amt\",\"value\":0}}}]")))));
        PropertyStore props2;
        const Frame f = render(item, 20, 20, nullptr, nullptr, 3, bridge::msg::ScalingMode::Stretch, &props2,
                               [&](SceneRenderer &, int frame) {
                                   if (frame == 2)
                                       props2.set("amt", 1.0);
                               });
        VERIFY_PX(f.at(5, 5), (Px{0, 255, 255, 255}), 1);
    }

    void namedTargetsPreviousAndCopy()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        // pass 1: invert `previous` into the half-size target A; pass 2: copy A to B; pass 3: mix B (slot 0) with the
        // untouched `previous` (slot 1) into the layer's own buffer
        item.file("shaders/effects/chain_mix.vert", kPassVert);
        item.file("shaders/effects/chain_mix.frag",
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = (texSample2D(g_Texture0, v_TexCoord) + texSample2D(g_Texture1, v_TexCoord)) * 0.5; }\n");
        item.file("materials/effects/chain_mix.json", "{\"passes\":[{\"shader\":\"effects/chain_mix\",\"blending\":\"normal\"}]}");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file("effects/chain/effect.json",
                  "{\"passes\":["
                  "{\"material\":\"materials/effects/invert.json\",\"target\":\"_rt_A\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
                  "{\"command\":\"copy\",\"source\":\"_rt_A\",\"target\":\"_rt_B\"},"
                  "{\"material\":\"materials/effects/chain_mix.json\",\"bind\":[{\"name\":\"_rt_B\",\"index\":0},{\"name\":\"previous\",\"index\":1}]}],"
                  "\"fbos\":[{\"name\":\"_rt_A\",\"scale\":2,\"format\":\"rgba8888\"},{\"name\":\"_rt_B\",\"scale\":2,\"format\":\"rgba8888\"}]}");
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"chain"_s, "[{},{},{}]")))));
        bool supported = false;
        const Frame f = render(item, 20, 20, &supported);
        VERIFY_PX(f.at(5, 5), (Px{128, 128, 128, 255}), 2); // (cyan + red) / 2
    }

    void swapExchangesTwoBuffers()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        // as the copy test, but _rt_A and _rt_B trade places instead: B then is what A held (the inverted picture), A what B held
        item.file("shaders/effects/chain_mix.vert", kPassVert);
        item.file("shaders/effects/chain_mix.frag",
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = (texSample2D(g_Texture0, v_TexCoord) + texSample2D(g_Texture1, v_TexCoord)) * 0.5; }\n");
        item.file("materials/effects/chain_mix.json", "{\"passes\":[{\"shader\":\"effects/chain_mix\",\"blending\":\"normal\"}]}");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file("effects/chain/effect.json",
                  "{\"passes\":["
                  "{\"material\":\"materials/effects/invert.json\",\"target\":\"_rt_A\",\"bind\":[{\"name\":\"previous\",\"index\":0}]},"
                  "{\"command\":\"swap\",\"source\":\"_rt_A\",\"target\":\"_rt_B\"},"
                  "{\"material\":\"materials/effects/chain_mix.json\",\"bind\":[{\"name\":\"_rt_B\",\"index\":0},{\"name\":\"previous\",\"index\":1}]}],"
                  "\"fbos\":[{\"name\":\"_rt_A\",\"scale\":2,\"format\":\"rgba8888\"},{\"name\":\"_rt_B\",\"scale\":2,\"format\":\"rgba8888\"}]}");
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"chain"_s, "[{},{},{}]")))));
        bool supported = false;
        const Frame f = render(item, 20, 20, &supported);
        VERIFY_PX(f.at(5, 5), (Px{128, 128, 128, 255}), 2); // (cyan + red) / 2; without the swap B is empty: (128, 0, 0)
    }

    void aSamplersComboFollowsItsTexture()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        item.tex(u"nomask"_s, solid(0, 0, 0));
        addEffect(item, u"masked"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1; // {\"label\":\"mask\",\"combo\":\"MASK\"}\nvarying vec2 v_TexCoord;\n"
                  "void main() {\n  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n  float m = 1.0;\n#if MASK\n  m = texSample2D(g_Texture1, v_TexCoord).r;\n#endif\n"
                  "  c.rgb = lerp(c.rgb, 1.0 - c.rgb, m);\n  gl_FragColor = c;\n}\n",
                  simpleEffect(u"masked"_s));
        // no mask texture: inverted everywhere; a black mask: untouched
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"masked"_s)))));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{0, 255, 255, 255}), 1);
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"masked"_s, "[{\"textures\":[null,\"nomask\"]}]")))));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{255, 0, 0, 255}), 1);
    }

    void shadersSeeTheFormatOfTheirTextures()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // an R8 mask: WE's shaders learn it from TEX1FORMAT (9) and rebuild (1, 1, 1, r) out of it
        TexSpec r8;
        r8.format = 9;
        r8.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        r8.texW = r8.imgW = 1;
        r8.texH = r8.imgH = 1;
        r8.images = {{MipSpec{1, 1, {64}, false}}};
        item.tex(u"mask"_s, r8);
        addEffect(item, u"fmt"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                  "void main() {\n"
                  "#if TEX1FORMAT == 9\n  gl_FragColor = vec4(0.0, 1.0, 0.0, 1.0);\n#else\n  gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);\n#endif\n}\n",
                  simpleEffect(u"fmt"_s));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"fmt"_s, "[{\"textures\":[null,\"mask\"]}]")))));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{0, 255, 0, 255}), 1);
        // an ordinary RGBA mask leaves the combo unset
        item.tex(u"mask"_s, solid(10, 10, 10));
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{255, 0, 0, 255}), 1);
    }

    void oneAndTwoChannelTexturesKeepTheirChannels()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        TexSpec rg;
        rg.format = 8;
        rg.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        rg.texW = rg.imgW = 1;
        rg.texH = rg.imgH = 1;
        rg.images = {{MipSpec{1, 1, {200, 100}, false}}};
        item.tex(u"rg"_s, rg);
        addEffect(item, u"raw"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
                  "void main() { vec4 s = texSample2D(g_Texture1, v_TexCoord); gl_FragColor = vec4(s.r, s.g, s.b, 1.0); }\n",
                  simpleEffect(u"raw"_s));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"raw"_s, "[{\"textures\":[null,\"rg\"]}]")))));
        // RG88: (r, g, 0, 1), not grey plus alpha
        VERIFY_PX(render(item, 20, 20).at(5, 5), (Px{200, 100, 0, 255}), 1);
    }

    void engineInputsReachTheShaders()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // the clock, the pointer and the size of texture slot 0, each in its own channel
        addEffect(item, u"inputs"_s,
                  "uniform sampler2D g_Texture0;\nuniform float g_Time;\nuniform vec2 g_PointerPosition;\nuniform vec4 g_Texture0Resolution;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(fract(g_Time), g_PointerPosition.x * 0.5 + g_PointerPosition.y * 0.25, g_Texture0Resolution.x / 255.0, 1.0); }\n",
                  simpleEffect(u"inputs"_s));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"inputs"_s)))));
        // time: frame f is drawn at 0.25 * f seconds
        uint64_t passes = 0;
        bool animated = false;
        const Frame f = render(item, 20, 20, nullptr, &passes, 3, bridge::msg::ScalingMode::Stretch, nullptr,
                               [&](SceneRenderer &r, int) { animated = r.animated(); });
        QVERIFY(animated);
        QCOMPARE(passes, uint64_t{3}); // the clock makes every frame draw the scene
        // 0.5 s into the clock; the pointer at its default (0.5, 0.5) gives 0.5*0.5 + 0.5*0.25 = 0.375 (96); the layer buffer is 20 wide
        VERIFY_PX(f.at(5, 5), (Px{128, 96, 20, 255}), 2);
        // the pointer moves the output, and the layer buffer is 20 x 20
        Item item2;
        item2.image(u"w"_s);
        item2.tex(u"w"_s, solid(255, 255, 255));
        addEffect(item2, u"ptr"_s,
                  "uniform float g_Time0;\nuniform vec2 g_PointerPosition;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(g_PointerPosition.x, g_PointerPosition.y, 0.0, 1.0); }\n",
                  simpleEffect(u"ptr"_s));
        item2.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"ptr"_s)))));
        const Frame g = render(item2, 20, 20, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, nullptr,
                               [&](SceneRenderer &r, int frame) {
                                   QVERIFY(r.usesPointer());
                                   if (frame == 1)
                                       r.setPointer(15, 5, 0, {20, 20}); // x = 75 %, y = 25 % from the top left
                               });
        VERIFY_PX(g.at(5, 5), (Px{191, 64, 0, 255}), 2);
    }

    void pointerUnprojectsIntoTheLayersCoordinates()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // The vertex stage unprojects the pointer the way WE's xray does; the layer is a 20 x 20 quad in a 40 x 40 scene
        addEffect(item, u"unproject"_s,
                  "varying vec4 v_P;\nvarying vec2 v_Q;\nvoid main() { gl_FragColor = vec4(v_P.xy / v_P.z + 0.5, v_Q.x + 0.5, 1.0); }\n",
                  simpleEffect(u"unproject"_s),
                  "uniform mat4 g_ModelViewProjectionMatrix;\nuniform mat4 g_ModelViewProjectionMatrixInverse;\n"
                  "uniform mat4 g_EffectTextureProjectionMatrixInverse;\nuniform vec2 g_PointerPosition;\nuniform vec4 g_Texture0Resolution;\n"
                  "attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_P;\nvarying vec2 v_Q;\n"
                  "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
                  "  vec2 pointer = g_PointerPosition;\n  pointer.y = 1.0 - pointer.y;\n"
                  "  v_P.xyz = mul(vec4(pointer * 2.0 - 1.0, 0.0, 1.0), g_EffectTextureProjectionMatrixInverse).xyw;\n"
                  "  v_P.xy *= 0.5;\n  v_P.w = 0.0;\n"
                  "  v_Q = mul(vec4(pointer * 2.0 - 1.0, 0.0, 1.0), g_ModelViewProjectionMatrixInverse).xy / g_Texture0Resolution.xy;\n}\n");
        item.file(u"scene.json"_s, sceneText(40, 40, "0 0 0", layer(1, "w", "20 20 0", "20 20", withEffects(fxInstance(u"unproject"_s)))));
        const auto at = [&](double x, double y) {
            return render(item, 40, 40, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, nullptr,
                          [&](SceneRenderer &r, int frame) {
                              if (frame == 1)
                                  r.setPointer(x, y, 0, {40, 40});
                          });
        };
        // pointer 25,15 from the top left: 75 % along the layer, 75 % up from its bottom edge; the shader sees it from the
        // layer's centre (0.25), which the test shifts back
        const Frame f = at(25, 15);
        VERIFY_PX(f.at(20, 20), (Px{191, 191, 191, 255}), 3); // blue: the same through the MVP inverse and the buffer's size
        // pointer 15,25: 25 % along, 25 % up
        const Frame g = at(15, 25);
        VERIFY_PX(g.at(20, 20), (Px{64, 64, 64, 255}), 3);
    }

    void theReductionScaleAndTheEffectMatrixReachTheShaders()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // g_TextureReductionScale is 1; g_EffectModelViewProjectionMatrix takes the effect's clip square onto the layer's place
        // on the screen: the vertex shader writes the screen x of the corner it draws (0 at the left edge of the layer, 1 at the right)
        addEffect(item, u"fxin"_s,
                  "uniform float g_TextureReductionScale;\nvarying vec3 v_ScreenCoord;\n"
                  "void main() { vec2 s = v_ScreenCoord.xy / v_ScreenCoord.z * 0.5 + 0.5; gl_FragColor = vec4(s.x, g_TextureReductionScale * 0.5, 0.0, 1.0); }\n",
                  simpleEffect(u"fxin"_s),
                  "uniform mat4 g_ModelViewProjectionMatrix;\nuniform mat4 g_EffectModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
                  "varying vec3 v_ScreenCoord;\n"
                  "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n"
                  "  v_ScreenCoord = mul(vec4(a_Position, 1.0), g_EffectModelViewProjectionMatrix).xyw; }\n");
        // a layer over the right half of a 40 wide scene: screen x runs from 0.5 to 1
        item.file(u"scene.json"_s, sceneText(40, 40, "0 0 0", layer(1, "w", "30 20 0", "20 40", withEffects(fxInstance(u"fxin"_s)))));
        const Frame f = render(item, 40, 40);
        VERIFY_PX(f.at(21, 20), (Px{135, 128, 0, 255}), 12); // just right of the middle: screen x about 0.52
        VERIFY_PX(f.at(38, 20), (Px{245, 128, 0, 255}), 12); // near the right edge: about 0.97
    }

    void effectQuadsAreInTheLayersOwnUnits()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // WE's effect shaders move vertices by pixel amounts (foliagesway adds up to 40): a_Position runs from -size / 2 to size / 2.
        // Moving the quad of a 20 wide layer by 10 shows the right half of the picture on the right half of the layer's own buffer
        addEffect(item, u"slide"_s, kInvertFrag, simpleEffect(u"slide"_s),
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                  "void main() { vec3 p = a_Position; p.x += 10.0; gl_Position = mul(vec4(p, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord; }\n");
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"slide"_s, "[{\"constantshadervalues\":{\"amount\":0}}]")))));
        const Frame f = render(item, 20, 20);
        VERIFY_PX(f.at(15, 10), (Px{255, 255, 255, 255}), 2);
        VERIFY_PX(f.at(4, 10), (Px{0, 0, 0, 255}), 2); // the quad left this half: nothing was drawn there
    }

    void effectsDoNotBlendIntoTheirClearedTargets()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // half-transparent red, straight alpha: the layer's pixels must stay (255,0,0) with alpha 128 before the composite
        addEffect(item, u"half"_s, "varying vec2 v_TexCoord;\nvoid main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 0.5); }\n", simpleEffect(u"half"_s));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"half"_s)))));
        const Frame f = render(item, 20, 20, nullptr, nullptr, 2);
        // composited once over black: 128 red, not a squared alpha (64)
        VERIFY_PX(f.at(10, 10), (Px{128, 0, 0, 255}), 3);
    }

    void aResizedTargetStaysAliveUntilItsFrameIsRetired()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        TargetPool pool(*m_ctx);
        RenderTarget &small = pool.get("k", {8, 8}, VK_FORMAT_R8G8B8A8_UNORM);
        const VkImage smallImage = small.handle();
        RenderTarget &big = pool.get("k", {16, 16}, VK_FORMAT_R8G8B8A8_UNORM);
        QVERIFY(&big != &small);
        QCOMPARE(small.handle(), smallImage); // still the same live image, a recorded command may use it
        QCOMPARE(pool.retired(), size_t{1});
        for (uint64_t i = 0; i < TargetPool::kRetireFrames; ++i)
            pool.beginFrame();
        QCOMPARE(pool.retired(), size_t{0});
        QCOMPARE(pool.size(), size_t{1});
        // an unused target goes after the idle window, one marked `keep` stays
        pool.get("kept", {4, 4}, VK_FORMAT_R8G8B8A8_UNORM, true);
        for (uint64_t i = 0; i <= TargetPool::kIdleFrames + 1; ++i)
            pool.beginFrame();
        QVERIFY(!pool.has("k"));
        QVERIFY(pool.has("kept"));
    }

    void aParticleDrawsAsASquareWhereItIsAndInItsColour()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "1");
        Item item;
        // one particle at the origin of the object (100, 100), 20 units wide, red, never fading
        particleItem(item, R"("maxcount":10,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"sizerandom","min":40,"max":40},{"name":"colorrandom","min":"255 0 0","max":"255 0 0"}])");
        const Frame f = render(item, 200, 200, nullptr, nullptr, 3);
        VERIFY_PX(f.at(100, 100), (Px{255, 0, 51, 255}), 3); // additive red over the dark blue
        VERIFY_PX(f.at(80, 100), (Px{0, 0, 51, 255}), 3);
        const Bounds b = litBounds(f);
        QVERIFY2(b.x0 >= 89 && b.x0 <= 91 && b.x1 >= 108 && b.x1 <= 110, qPrintable(QString("x %1..%2").arg(b.x0).arg(b.x1)));
        QVERIFY2(b.y0 >= 89 && b.y0 <= 91 && b.y1 >= 108 && b.y1 <= 110, qPrintable(QString("y %1..%2").arg(b.y0).arg(b.y1)));
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void bigParticleSystemsDrawTheSameAtHalfSize()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "1");
        Item item;
        // three additive red sprites, each 400 wide on a 200 wide scene: fill bound, so drawn at half size into a buffer
        particleItem(item, R"("maxcount":10,"emitter":[{"name":"sphererandom","instantaneous":3,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"sizerandom","min":400,"max":400},{"name":"colorrandom","min":"80 0 0","max":"80 0 0"}])");
        qunsetenv("MURASAKI_SCENE_VK_HALF_PARTICLES");
        const Frame half = render(item, 200, 200, nullptr, nullptr, 3);
        qputenv("MURASAKI_SCENE_VK_HALF_PARTICLES", "0");
        const Frame full = render(item, 200, 200, nullptr, nullptr, 3);
        qunsetenv("MURASAKI_SCENE_VK_HALF_PARTICLES");
        qunsetenv("MURASAKI_SCENE_VK_SEED");
        QVERIFY(full.at(100, 100)[0] > 150); // three overlapping sprites add up (and the test is not trivially black)
        for (const auto &[x, y] : {std::pair{100u, 100u}, {20u, 30u}, {170u, 150u}, {101u, 99u}}) {
            VERIFY_PX(half.at(x, y), full.at(x, y), 4);
        }
    }

    void particleSystemsFillTheirEmitterAndStaySeeded()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "7");
        Item item;
        // 40 particles of 8 units inside a 60 x 40 box around the object (the box is +-30, +-20)
        particleItem(item, R"("maxcount":40,"emitter":[{"name":"boxrandom","instantaneous":40,"distancemax":"30 20 0"}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"sizerandom","min":16,"max":16},{"name":"colorrandom","min":"0 255 0","max":"0 255 0"}])");
        const Frame a = render(item, 200, 200, nullptr, nullptr, 3);
        const Bounds b = litBounds(a);
        QVERIFY(b.lit > 400); // forty squares of 64 px, overlapping a little
        QVERIFY2(b.x0 >= 100 - 30 - 5 && b.x1 <= 100 + 30 + 5 && b.y0 >= 100 - 20 - 5 && b.y1 <= 100 + 20 + 5,
                 qPrintable(QString("%1,%2 .. %3,%4").arg(b.x0).arg(b.y0).arg(b.x1).arg(b.y1)));
        QVERIFY(b.x1 - b.x0 > 40 && b.y1 - b.y0 > 20); // and it covers most of the box
        // the same seed draws the same picture
        const Frame again = render(item, 200, 200, nullptr, nullptr, 3);
        QCOMPARE(again.rgba, a.rgba);
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void aPointerControlPointMovesWhereParticlesAppear()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "3");
        Item item;
        particleItem(item, R"("maxcount":30,"controlpoint":[{"id":0,"flags":1,"offset":"0 0 0"}],"emitter":[{"name":"sphererandom","rate":300,"distancemax":10,"controlpoint":0}],)"
                           R"("initializer":[{"name":"lifetimerandom","min":50,"max":50},{"name":"sizerandom","min":12,"max":12}],"operator":[{"name":"movement"}])");
        const Frame f = render(item, 200, 200, nullptr, nullptr, 12, bridge::msg::ScalingMode::Stretch, nullptr, [&](SceneRenderer &r, int frame) {
            if (frame >= 0)
                r.setPointer(150, 50, 0, {200, 200}); // right and up
        });
        const Bounds b = litBounds(f);
        QVERIFY(b.lit > 50);
        // the centre of what is lit is near the pointer (150, 50 from the top left), not the object's origin
        QVERIFY2((b.x0 + b.x1) / 2 > 135 && (b.x0 + b.x1) / 2 < 165 && (b.y0 + b.y1) / 2 > 35 && (b.y0 + b.y1) / 2 < 65,
                 qPrintable(QString("%1,%2 .. %3,%4").arg(b.x0).arg(b.y0).arg(b.x1).arg(b.y1)));
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void aRopeDrawsARibbonThroughItsParticlesInOrder()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "5");
        Item item;
        // five particles a second leave the origin at 20 units a second to the right: the rope runs from the oldest
        // (the farthest right) to the newest (at x = 100) and is as thick as the particles are big
        particleItem(item, R"("maxcount":50,"emitter":[{"name":"sphererandom","rate":5,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"sizerandom","min":20,"max":20},{"name":"velocityrandom","min":"20 0 0","max":"20 0 0"}],"operator":[{"name":"movement"}],)"
                           R"("renderer":[{"name":"rope","subdivision":3}])");
        const Frame f = render(item, 200, 200, nullptr, nullptr, 28);
        // 28 frames; the simulation takes at most 0.1 s of each 0.25 s step, so the oldest particle is about 2.8 s old
        const Bounds b = litBounds(f);
        QVERIFY2(b.lit > 100, qPrintable(QString::number(b.lit)));
        QVERIFY2(b.x1 - b.x0 > 35, qPrintable(QString("%1..%2").arg(b.x0).arg(b.x1))); // long along x
        QVERIFY2(b.y1 - b.y0 < 30, qPrintable(QString("%1..%2").arg(b.y0).arg(b.y1))); // and thin across
        QVERIFY(std::abs((b.y0 + b.y1) / 2 - 100) < 8);                                // on the object's y
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void aFollowerChildDrawsAroundItsParentParticle()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "9");
        Item item;
        // the parent: one particle that leaves the origin to the right and is itself invisible (alpha 0); the child
        // follows it, emitting small green squares at its position
        particleItem(item, R"("maxcount":1,"emitter":[{"name":"sphererandom","instantaneous":1,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"alpharandom","min":0,"max":0},{"name":"velocityrandom","min":"30 0 0","max":"30 0 0"}],"operator":[{"name":"movement"}],)"
                           R"("children":[{"name":"particles/c.json","type":"eventfollow","maxcount":4}])");
        item.file(u"particles/c.json"_s,
                  R"({"material":"materials/presets/dot.json","maxcount":40,"emitter":[{"name":"sphererandom","rate":400,"distancemax":2}],"initializer":[{"name":"lifetimerandom","min":0.1,"max":0.1},)"
                  R"({"name":"sizerandom","min":12,"max":12},{"name":"colorrandom","min":"0 255 0","max":"0 255 0"}]})");
        const Frame f = render(item, 200, 200, nullptr, nullptr, 10);
        // 10 frames of at most 0.1 s: the parent is about 30 units to the right of the origin (100, 100)
        const Bounds b = litBounds(f);
        QVERIFY2(b.lit > 20, qPrintable(QString::number(b.lit)));
        const int cx = (b.x0 + b.x1) / 2;
        QVERIFY2(cx > 118 && cx < 140, qPrintable(QString("x %1..%2").arg(b.x0).arg(b.x1)));
        QVERIFY2(std::abs((b.y0 + b.y1) / 2 - 100) < 6, qPrintable(QString("y %1..%2").arg(b.y0).arg(b.y1)));
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void anEventChildBurstsWhereAParentParticleIsBornOrDies()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "9");
        const auto child = R"({"material":"materials/presets/dot.json","maxcount":40,"emitter":[{"name":"sphererandom","instantaneous":20,"rate":0,"distancemax":2}],)"
                           R"("initializer":[{"name":"lifetimerandom","min":50,"max":50},{"name":"sizerandom","min":10,"max":10},{"name":"colorrandom","min":"0 255 0","max":"0 255 0"}]})";
        // the parent: invisible particles that leave the origin to the right at 100 units a second and live 0.3 s
        const auto parent = [&](const char *type) {
            return QByteArray(R"("maxcount":4,"emitter":[{"name":"sphererandom","rate":2,"distancemax":0}],"initializer":[{"name":"lifetimerandom","min":0.3,"max":0.3},)"
                              R"({"name":"alpharandom","min":0,"max":0},{"name":"velocityrandom","min":"100 0 0","max":"100 0 0"}],"operator":[{"name":"movement"}],)"
                              R"("children":[{"name":"particles/c.json","type":")") + type + "\"}]";
        };
        // eventspawn: the burst is where the particle was born, the origin (100, 100), and stays there
        Item born;
        particleItem(born, parent("eventspawn"));
        born.file(u"particles/c.json"_s, child);
        const Bounds atBirth = litBounds(render(born, 200, 200, nullptr, nullptr, 10));
        QVERIFY2(atBirth.lit > 30, qPrintable(QString::number(atBirth.lit)));
        QVERIFY2(std::abs((atBirth.x0 + atBirth.x1) / 2 - 100) < 8, qPrintable(QString("x %1..%2").arg(atBirth.x0).arg(atBirth.x1)));
        // eventdeath: 0.3 s later and 30 units to the right
        Item died;
        particleItem(died, parent("eventdeath"));
        died.file(u"particles/c.json"_s, child);
        const Bounds atDeath = litBounds(render(died, 200, 200, nullptr, nullptr, 14));
        QVERIFY2(atDeath.lit > 30, qPrintable(QString::number(atDeath.lit)));
        QVERIFY2((atDeath.x0 + atDeath.x1) / 2 > 118, qPrintable(QString("x %1..%2").arg(atDeath.x0).arg(atDeath.x1)));
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void sharedBuffersGrowAcrossSystemsWithinOneFrame()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "5");
        Item item;
        // the parent draws first with a handful of quads, then its child needs a far bigger index buffer: the draw
        // already recorded must keep its own (the shared buffer is replaced in the middle of the frame)
        particleItem(item, R"("maxcount":10,"emitter":[{"name":"sphererandom","instantaneous":10,"distancemax":3}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"sizerandom","min":8,"max":8},{"name":"colorrandom","min":"255 0 0","max":"255 0 0"}],)"
                           R"("children":[{"name":"particles/c.json","origin":"45 0 0"}])", {}, 300);
        item.file(u"particles/c.json"_s,
                  R"({"material":"materials/presets/dot.json","maxcount":3000,"emitter":[{"name":"boxrandom","instantaneous":3000,"distancemax":"20 20 0"}],)"
                  R"("initializer":[{"name":"lifetimerandom","min":50,"max":50},{"name":"sizerandom","min":6,"max":6},{"name":"colorrandom","min":"0 255 0","max":"0 255 0"}]})");
        const Frame f = render(item, 300, 300, nullptr, nullptr, 3);
        int red = 0, green = 0;
        for (int y = 0; y < 300; ++y) {
            for (int x = 0; x < 300; ++x) {
                const Px p = f.at(x, y);
                red += p[0] > 200 && p[1] < 80;
                green += p[1] > 200 && p[0] < 80;
            }
        }
        QVERIFY2(red > 20, qPrintable(QString::number(red)));       // the parent's squares near (150, 150)
        QVERIFY2(green > 500, qPrintable(QString::number(green)));  // the child's three thousand around (195, 150)
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void hugePoolsAndTrailsStayWithinTheMemoryBudget()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "6");
        // a pool of a million and a rate that fills it at once: capped at the global limit, drawn, and the frame finishes
        Item pool;
        particleItem(pool, R"("maxcount":1000000,"emitter":[{"name":"boxrandom","rate":1000000,"distancemax":"90 90 0"}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"sizerandom","min":2,"max":2},{"name":"colorrandom","min":"0 255 0","max":"0 255 0"}])");
        QVERIFY(litBounds(render(pool, 200, 200, nullptr, nullptr, 3)).lit > 1000);
        // the same with a rope trail of 64 points per particle: the history is cut to its budget and a frame that would
        // need more vertices than the budget leaves the system out instead of failing
        Item trails;
        particleItem(trails, R"("maxcount":1000000,"emitter":[{"name":"boxrandom","rate":1000000,"distancemax":"90 90 0"}],"initializer":[{"name":"lifetimerandom","min":50,"max":50}],)"
                             R"("renderer":[{"name":"ropetrail","segments":64,"length":2}])");
        const Frame f = render(trails, 200, 200, nullptr, nullptr, 3);
        QCOMPARE(f.width, 200u); // it rendered a frame at all
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void aFollowerChildWithAHugeMaxCountIsCapped()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "8");
        Item item;
        particleItem(item, R"("maxcount":2000,"emitter":[{"name":"boxrandom","rate":5000,"distancemax":"80 80 0"}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},)"
                           R"({"name":"alpharandom","min":0,"max":0}],"children":[{"name":"particles/c.json","type":"eventfollow","maxcount":1000000000}])");
        item.file(u"particles/c.json"_s,
                  R"({"material":"materials/presets/dot.json","maxcount":20,"emitter":[{"name":"sphererandom","rate":50,"distancemax":2}],"initializer":[{"name":"lifetimerandom","min":0.2,"max":0.2},)"
                  R"({"name":"sizerandom","min":6,"max":6},{"name":"colorrandom","min":"0 255 0","max":"0 255 0"}]})");
        QElapsedTimer timer;
        timer.start();
        const Frame f = render(item, 200, 200, nullptr, nullptr, 8);
        QVERIFY(litBounds(f).lit > 20);
        QVERIFY2(timer.elapsed() < 20000, qPrintable(QString::number(timer.elapsed())));
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void perspectiveParticlesGrowTowardsTheCamera()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "2");
        const auto width = [&](const char *z) {
            Item item;
            // flag 4: a camera 1000 units away looks at the scene's middle; the particle sits z units in front of it
            particleItem(item, "\"flags\":4,\"maxcount\":1,\"emitter\":[{\"name\":\"boxrandom\",\"instantaneous\":1,\"distancemax\":0,\"origin\":\"0 0 " + QByteArray(z)
                                   + "\"}],\"initializer\":[{\"name\":\"lifetimerandom\",\"min\":50,\"max\":50},{\"name\":\"sizerandom\",\"min\":400,\"max\":400}]");
            const Bounds b = litBounds(render(item, 200, 200, nullptr, nullptr, 2));
            return b.x1 - b.x0 + 1;
        };
        const int flat = width("0");
        const int near = width("500");
        const int far = width("-1000");
        // 200 units wide at 1000 away with a 50 degree view of a 200 pixel tall scene: about 43 pixels
        QVERIFY2(flat > 38 && flat < 48, qPrintable(QString::number(flat)));
        QVERIFY2(std::abs(near - 2 * flat) <= 4, qPrintable(QString("%1 vs %2").arg(near).arg(flat))); // 1000 / (1000 - 500)
        QVERIFY2(std::abs(far * 2 - flat) <= 4, qPrintable(QString("%1 vs %2").arg(far).arg(flat)));  // 1000 / (1000 + 1000)
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void hiddenParticleObjectsStandStillAndCostNothing()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        particleItem(item, R"("maxcount":10,"emitter":[{"name":"sphererandom","rate":50}],"initializer":[{"name":"lifetimerandom","min":5,"max":5}])", ",\"visible\":false");
        uint64_t passes = 0;
        const Frame f = render(item, 200, 200, nullptr, &passes, 6);
        QCOMPARE(passes, uint64_t{1}); // the first frame only: nothing moves while the object is hidden
        QCOMPARE(litBounds(f).lit, 0);
        // shown, the scene is drawn every frame
        Item shown;
        particleItem(shown, R"("maxcount":10,"emitter":[{"name":"sphererandom","rate":50}],"initializer":[{"name":"lifetimerandom","min":5,"max":5}])");
        uint64_t shownPasses = 0;
        render(shown, 200, 200, nullptr, &shownPasses, 6);
        QCOMPARE(shownPasses, uint64_t{6});
    }

    void soundObjectsPlayAtTheirVolumeTimesTheBridgesAndFollowProperties()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        SceneRenderer::setAudioBackendFactory([] {
            auto fake = std::make_unique<FakeAudio>();
            g_fakeAudio = fake.get();
            return std::unique_ptr<AudioBackend>(std::move(fake));
        });
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(10, 10, 10));
        const auto mp3 = audiofix::sineMp3();
        item.file(u"sounds/tone.mp3"_s, QByteArray(reinterpret_cast<const char *>(mp3.data()), static_cast<qsizetype>(mp3.size())));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0",
                            layer(1, "w", "10 10 0", "20 20") + ",{\"id\":5,\"name\":\"music\",\"sound\":[\"sounds/tone.mp3\"],\"playbackmode\":\"loop\","
                            "\"volume\":{\"user\":\"vol\",\"value\":1.0}}"));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        std::string error;
        auto scene = loadScene(vfs, "scene.json", &error);
        QVERIFY2(scene && scene->supported(), error.c_str());
        QCOMPARE(scene->sounds.size(), size_t{1});
        PropertyStore props;
        {
            SceneRenderer renderer(*m_ctx, std::move(*scene), vfs, props);
            QVERIFY(g_fakeAudio);
            QCOMPARE(g_fakeAudio->playbacks, 1);
            QCOMPARE(g_fakeAudio->captures, 0); // no shader reads the spectrum
            renderer.setVolume(128);
            QTest::qWait(150);
            float full = 0.0f;
            for (int i = 0; i < 20; ++i) {
                full = std::max(full, peakOf(g_fakeAudio->m_pull, 1024));
                QTest::qWait(10);
            }
            QVERIFY2(full > 0.1f && full < 0.14f, qPrintable(QString::number(full))); // the clip peaks at 0.125
            renderer.setVolume(0);
            float quiet = 1.0f;
            for (int i = 0; i < 80; ++i) {
                quiet = peakOf(g_fakeAudio->m_pull, 1024);
                QTest::qWait(5);
            }
            QVERIFY2(quiet < 0.02f, qPrintable(QString::number(quiet)));
            renderer.setPaused(true);
            QCOMPARE(peakOf(g_fakeAudio->m_pull, 256), 0.0f);
        }
        g_fakeAudio = nullptr;
        SceneRenderer::setAudioBackendFactory({});
    }

    void shadersThatReadTheSpectrumStartTheCaptureAndSeeIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        SceneRenderer::setAudioBackendFactory([] {
            auto fake = std::make_unique<FakeAudio>();
            g_fakeAudio = fake.get();
            return std::unique_ptr<AudioBackend>(std::move(fake));
        });
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        addEffect(item, u"bars"_s,
                  "uniform float g_AudioSpectrum16Left[16];\nuniform float g_AudioSpectrum16Right[16];\nvarying vec2 v_TexCoord;\n"
                  "void main() {\n  int band = int(v_TexCoord.x * 16.0);\n"
                  "  gl_FragColor = vec4(g_AudioSpectrum16Left[band], g_AudioSpectrum16Right[band], 0.0, 1.0);\n}\n",
                  simpleEffect(u"bars"_s));
        item.file(u"scene.json"_s, sceneText(32, 32, "0 0 0", layer(1, "w", "16 16 0", "32 32", withEffects(fxInstance(u"bars"_s)))));
        int loudBand = -1;
        Frame f;
        {
            Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            std::string error;
            auto scene = loadScene(vfs, "scene.json", &error);
            QVERIFY2(scene, error.c_str());
            PropertyStore props;
            SceneRenderer renderer(*m_ctx, std::move(*scene), vfs, props);
            QVERIFY(g_fakeAudio);
            QCOMPARE(g_fakeAudio->captures, 1);
            QVERIFY(renderer.animated());
            // a 1 kHz tone in the left channel only
            std::vector<float> tone(4096 * 2, 0.0f);
            for (size_t i = 0; i < 4096; ++i)
                tone[i * 2] = static_cast<float>(std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(i) / 48000.0));
            g_fakeAudio->m_push(tone.data(), 4096);
            auto set = FrameBufferSet::create(*m_ctx, 32, 32, Transport::Shm, error);
            QVERIFY2(set, error.c_str());
            for (int frame = 0; frame < 3; ++frame) {
                m_ctx->submitAndWait([&](VkCommandBuffer cb) {
                    set->cmdAcquire(cb, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                    FrameParams params;
                    params.cb = cb;
                    params.image = set->image(0);
                    params.view = set->view(0);
                    params.extent = set->extent();
                    params.timeSeconds = frame * 0.25;
                    renderer.render(params);
                    set->cmdRelease(cb, 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                });
                set->finishFrame(0);
            }
            const auto bytes = set->ring().frame(0);
            f.width = 32;
            f.height = 32;
            f.rgba.assign(bytes.begin(), bytes.end());
        }
        // 1 kHz on a log axis of 16 bands from 30 Hz to 16 kHz: band 16 * log(1000/30) / log(16000/30) = 8.9
        int best = 0;
        for (int x = 0; x < 32; ++x)
            if (f.at(static_cast<uint32_t>(x), 16)[0] > f.at(static_cast<uint32_t>(best), 16)[0])
                best = x;
        loudBand = best / 2; // two pixels a band
        QVERIFY2(loudBand >= 8 && loudBand <= 9, qPrintable(QString::number(loudBand)));
        QVERIFY(f.at(static_cast<uint32_t>(best), 16)[0] > 200); // the left channel is bright
        QVERIFY(f.at(static_cast<uint32_t>(best), 16)[1] < 20);  // the right one is silent
        g_fakeAudio = nullptr;
        SceneRenderer::setAudioBackendFactory({});
    }

    void fogMixesFarLayersTowardsTheFogColour()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a perspective scene seen from z = 10 (fov 50, 160 x 90): red layers with the fog image shader at distances 10, 60 and about
        // 117 (and one as far whose material turns FOG off); the distance fog starts at 20 and ends at 100, its density going from 0 to 1, green
        Item item;
        addFogImageShader(item);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.file(u"materials/fogged.json"_s, R"({"passes":[{"shader":"fogimage","blending":"translucent","textures":["red"]}]})");
        item.file(u"materials/clear.json"_s, R"({"passes":[{"shader":"fogimage","blending":"translucent","textures":["red"],"combos":{"FOG":0}}]})");
        item.file(u"models/fogged.json"_s, R"({"material":"materials/fogged.json"})");
        item.file(u"models/clear.json"_s, R"({"material":"materials/clear.json"})");
        const auto object = [](int id, const char *model, const char *origin, const char *size) {
            return "{\"id\":" + QByteArray::number(id) + ",\"name\":\"o" + QByteArray::number(id) + "\",\"image\":\"models/" + model + ".json\",\"origin\":\""
                + origin + "\",\"size\":\"" + size + "\"}";
        };
        const QByteArray objects = object(2, "fogged", "0 0 -50", "10 10") + "," + object(3, "fogged", "-3 0 0", "2 2") + ","
            + object(4, "fogged", "40 0 -100", "20 20") + "," + object(5, "clear", "40 -30 -100", "20 20");
        const auto sceneWith = [&](const QByteArray &fog, const QByteArray &projection = "null") {
            item.file(u"scene.json"_s, R"({"camera":{"center":"0 0 0","eye":"0 0 10","up":"0 1 0"},"general":{"clearcolor":"0 0 0","fov":50,"nearz":0.1,"farz":1000,)"
                                           R"("orthogonalprojection":)" + projection + fog + "},\"objects\":[" + objects + "]}");
        };
        const QByteArray distanceFog = R"(,"fogdistance":true,"fogdistancecolor":"0 1 0","fogdistancestart":20,"fogdistanceend":100,)"
                                       R"("fogdistancestartdensity":0,"fogdistanceenddensity":1)";
        sceneWith(distanceFog);
        bool supported = false;
        {
            Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            const auto scene = loadScene(vfs, "scene.json");
            QVERIFY(scene->supported());
            QVERIFY(scene->degraded.empty()); // the fog is drawn
        }
        Frame f = render(item, 160, 90, &supported);
        QVERIFY(supported);
        VERIFY_PX(f.at(51, 45), (Px{255, 0, 0, 255}), 2);   // 10 away, before the fog starts: red
        VERIFY_PX(f.at(80, 45), (Px{191, 64, 0, 255}), 3);   // 60 away: halfway, density 0.5^2 = 0.25 of the fog
        VERIFY_PX(f.at(115, 45), (Px{0, 255, 0, 255}), 2);  // past the end: all fog
        VERIFY_PX(f.at(115, 71), (Px{255, 0, 0, 255}), 2);  // as far, with its FOG combo off: red
        // without the fog the same layers are plain red
        sceneWith("");
        f = render(item, 160, 90);
        VERIFY_PX(f.at(115, 45), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(80, 45), (Px{255, 0, 0, 255}), 2);
        // a band bound to a user property follows it while running
        sceneWith(R"(,"fogdistance":{"user":"mist","value":false},"fogdistancecolor":"0 1 0","fogdistancestart":20,"fogdistanceend":100,)"
                  R"("fogdistancestartdensity":0,"fogdistanceenddensity":1)");
        PropertyStore props;
        props.set("mist", false);
        f = render(item, 160, 90, nullptr, nullptr, 3, bridge::msg::ScalingMode::Stretch, &props, [&](SceneRenderer &, int frame) {
            if (frame == 2)
                props.set("mist", true);
        });
        VERIFY_PX(f.at(115, 45), (Px{0, 255, 0, 255}), 2);
        // a band whose end is its start is a step: nothing before it, all the end density past it
        sceneWith(R"(,"fogdistance":true,"fogdistancecolor":"0 1 0","fogdistancestart":30,"fogdistanceend":30,)"
                  R"("fogdistancestartdensity":0,"fogdistanceenddensity":1)");
        f = render(item, 160, 90);
        VERIFY_PX(f.at(51, 45), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(80, 45), (Px{0, 255, 0, 255}), 2);
        VERIFY_PX(f.at(115, 45), (Px{0, 255, 0, 255}), 2);
        // densities outside 0..1 are kept in it (an end density of 3 is all fog, not past it)
        sceneWith(R"(,"fogdistance":true,"fogdistancecolor":"0 1 0","fogdistancestart":20,"fogdistanceend":100,)"
                  R"("fogdistancestartdensity":-1,"fogdistanceenddensity":3)");
        f = render(item, 160, 90);
        VERIFY_PX(f.at(51, 45), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(115, 45), (Px{0, 255, 0, 255}), 2);
        // height fog: below 0 the colour thickens towards y = -10 (start 0, end -10: the state grows downwards)
        sceneWith(R"(,"fogheight":true,"fogheightcolor":"0 0 1","fogheightstart":0,"fogheightend":-10,"fogheightstartdensity":0,"fogheightenddensity":1)");
        f = render(item, 160, 90);
        VERIFY_PX(f.at(51, 40), (Px{255, 0, 0, 255}), 2); // the near layer's upper half is above the start
        QVERIFY2(f.at(115, 50)[2] > f.at(115, 40)[2], qPrintable(show(f.at(115, 50)) + show(f.at(115, 40)))); // further down: bluer
        // an orthographic scene has no eye distance or height to fog by: not drawn, named
        sceneWith(distanceFog, R"({"width":160,"height":90})");
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        const auto ortho = loadScene(vfs, "scene.json");
        QCOMPARE(ortho->degraded, (std::vector<std::string>{"general fog"}));
    }

    void switchingTheFogRebuildsOnlyFogShaders()
    {
        // the shader library alone (no GPU): the scene's fog combos enter the key of a shader whose FOG is on and of no other
        Item item;
        addFogImageShader(item);
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        ShaderLibrary shaders(vfs);
        MaterialPass fogged;
        fogged.shader = "fogimage";
        MaterialPass clear = fogged;
        clear.combos["fog"] = 0;
        MaterialPass plain;
        plain.shader = "effects/invert";
        const auto getAll = [&] {
            std::string error;
            for (const MaterialPass *pass : {&fogged, &clear, &plain}) {
                QVERIFY2(shaders.get(*pass, {0}, &error), error.c_str());
            }
        };
        getAll();
        QCOMPARE(shaders.all().size(), size_t{3});
        shaders.setSceneCombos({{"FOG_DIST", 1}, {"FOG_COMPUTED", 1}});
        getAll();
        QCOMPARE(shaders.all().size(), size_t{4}); // only the fogged variant is new
        shaders.setSceneCombos({{"FOG_DIST", 1}, {"FOG_HEIGHT", 1}, {"FOG_COMPUTED", 1}, {"HDR", 1}});
        getAll();
        QCOMPARE(shaders.all().size(), size_t{5}); // the second band: again only the fogged one; nothing here names HDR
        shaders.setSceneCombos({});
        getAll();
        QCOMPARE(shaders.all().size(), size_t{5}); // back off: the first variants again
        // the light combos follow the same rule: none of these reads the lights, so their keys stay
        shaders.setLightCombos({{"LIGHTS_POINT", 2}, {"SCENE_ORTHO", 1}});
        getAll();
        QCOMPARE(shaders.all().size(), size_t{5});
    }

    void hdrKeepsValuesAboveOneUntilTheCombine()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a white layer at brightness 2 or 4 through an effect that halves it; the HDR buffers keep 2.0 through the effect (half of it
        // is 1: white), the 8 bit ones clip it to 1 first (half of it: grey)
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        addHdrBloomShaders(item);
        addEffect(item, u"halve"_s,
                  "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\n"
                  "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord); gl_FragColor = vec4(c.rgb * 0.5, c.a); }\n",
                  simpleEffect(u"halve"_s));
        size_t degradedNames = 0;
        const auto scene = [&](const char *brightness, const QByteArray &general) {
            item.file(u"scene.json"_s, sceneText(128, 128, "0 0 0",
                                                 layer(1, "w", "64 64 0", "32 32", QByteArray(",\"brightness\":") + brightness + withEffects(fxInstance(u"halve"_s))),
                                                 general));
            Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            const auto loaded = loadScene(vfs, "scene.json");
            degradedNames += loaded->degraded.size(); // hdr and its bloom are drawn: nothing named
            return render(item, 128, 128, nullptr, nullptr, 2);
        };
        const Frame sdr = scene("2", "");
        VERIFY_PX(sdr.at(64, 64), (Px{128, 128, 128, 255}), 2);
        const Frame hdr = scene("2", ",\"hdr\":true");
        VERIFY_PX(hdr.at(64, 64), (Px{255, 255, 255, 255}), 1);
        VERIFY_PX(hdr.at(64, 40), (Px{0, 0, 0, 255}), 1);
        // the HDR bloom: 1.0 stays under a threshold of 1.5 (no glow), 2.0 passes it and glows around the square
        const QByteArray bloom = ",\"hdr\":true,\"bloom\":true,\"bloomhdrstrength\":1,\"bloomhdrthreshold\":1.5,\"bloomhdrfeather\":0,"
                                 "\"bloomhdrscatter\":1,\"bloomhdriterations\":4";
        const Frame under = scene("2", bloom);
        VERIFY_PX(under.at(64, 64), (Px{255, 255, 255, 255}), 1);
        VERIFY_PX(under.at(64, 44), (Px{0, 0, 0, 255}), 1);
        const Frame over = scene("4", bloom);
        QVERIFY2(over.at(64, 44)[0] > 10, qPrintable(show(over.at(64, 44)))); // 4 units above the square: the glow
        QVERIFY(over.at(64, 44)[0] > over.at(64, 30)[0] || over.at(64, 30)[0] == 255); // weakening with distance
        VERIFY_PX(over.at(1, 1), (Px{0, 0, 0, 255}), 3);
        // a feather softens the threshold: 1.0 under a threshold of 1.5 with a knee of 0.75 glows a little ((1.0 - 0.75)^2 / 3 of it)
        const QByteArray knee = ",\"hdr\":true,\"bloom\":true,\"bloomhdrstrength\":40,\"bloomhdrthreshold\":1.5,\"bloomhdrfeather\":0.5,"
                                "\"bloomhdrscatter\":1,\"bloomhdriterations\":4";
        const Frame soft = scene("2", knee);
        QVERIFY2(soft.at(64, 46)[0] > 3, qPrintable(show(soft.at(64, 46))));
        const Frame hard = scene("2", QByteArray(knee).replace("\"bloomhdrfeather\":0.5", "\"bloomhdrfeather\":0"));
        VERIFY_PX(hard.at(64, 46), (Px{0, 0, 0, 255}), 1);
        // the 8 bit path is untouched: the same bloom keys without hdr are the plain bloom's (stand-ins missing: no glow, the scene)
        const Frame plain = scene("4", ",\"bloom\":false");
        VERIFY_PX(plain.at(64, 64), (Px{128, 128, 128, 255}), 2);
        QCOMPARE(degradedNames, size_t{0});
    }

    void sdrBloomIsThePreviousBuilds()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // an 8 bit scene with a bloom and an effect, pixel for pixel what the build before HDR and fog drew (ea8fbf2; values read
        // from that build with this test): the HDR work must not touch an ordinary scene
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 200, 100));
        addBloomShaders(item);
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file(u"scene.json"_s, sceneText(128, 128, "0 0 0.2",
                                             layer(1, "w", "64 64 0", "32 32", ",\"brightness\":1.5" + withEffects(fxInstance(u"invert"_s, "[{\"constantshadervalues\":{\"amount\":0.25}}]"))),
                                             ",\"bloom\":true,\"bloomstrength\":1.0,\"bloomthreshold\":0.5"));
        const Frame f = render(item, 128, 128, nullptr, nullptr, 2);
        const std::array<std::array<int, 2>, 5> at{{{64, 64}, {64, 46}, {64, 40}, {50, 50}, {2, 2}}};
        QByteArray seen;
        for (const auto &p : at) {
            seen += show(f.at(static_cast<uint32_t>(p[0]), static_cast<uint32_t>(p[1]))).toUtf8() + " ";
        }
        qInfo("sdr bloom pixels: %s", seen.constData());
        QCOMPARE(seen, QByteArray(kSdrBloomPixels));
    }

    void bloomLetsTheBrightPartsGlow()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        addBloomShaders(item);
        const auto scene = [&](const QByteArray &bloom) {
            // a white square of 32 units in the middle of a 128 unit scene
            item.file(u"scene.json"_s, sceneText(128, 128, "0 0 0", layer(1, "w", "64 64 0", "32 32"), bloom));
            return render(item, 128, 128, nullptr, nullptr, 2);
        };
        const Frame plain = scene("");
        const Frame glowing = scene(",\"bloom\":true,\"bloomstrength\":1.0,\"bloomthreshold\":0.5");
        VERIFY_PX(plain.at(64, 64), (Px{255, 255, 255, 255}), 1);
        VERIFY_PX(plain.at(64, 40), (Px{0, 0, 0, 255}), 1);        // 8 units above the square: nothing
        QVERIFY2(glowing.at(64, 40)[0] > 10, qPrintable(show(glowing.at(64, 40)))); // the glow reaches it
        QVERIFY(glowing.at(64, 40)[0] < glowing.at(64, 46)[0] || glowing.at(64, 46)[0] == 255); // and weakens with distance
        VERIFY_PX(glowing.at(2, 2), (Px{0, 0, 0, 255}), 2);         // far away it is dark
        // a threshold above the picture's brightest value: no glow
        const Frame none = scene(",\"bloom\":true,\"bloomstrength\":1.0,\"bloomthreshold\":1.0");
        VERIFY_PX(none.at(64, 40), (Px{0, 0, 0, 255}), 1);
        // a bound property switches it on and off while running
        PropertyStore props;
        props.set("glow", false);
        item.file(u"scene.json"_s, sceneText(128, 128, "0 0 0", layer(1, "w", "64 64 0", "32 32"),
                                             ",\"bloom\":{\"user\":\"glow\",\"value\":false},\"bloomstrength\":1.0,\"bloomthreshold\":0.5"));
        uint64_t passes = 0;
        const Frame off = render(item, 128, 128, nullptr, &passes, 2, bridge::msg::ScalingMode::Stretch, &props);
        VERIFY_PX(off.at(64, 40), (Px{0, 0, 0, 255}), 1);
        const Frame on = render(item, 128, 128, nullptr, &passes, 3, bridge::msg::ScalingMode::Stretch, &props,
                                [&](SceneRenderer &, int frame) {
                                    if (frame == 1)
                                        props.set("glow", true);
                                });
        QVERIFY(on.at(64, 40)[0] > 10);
    }

    void textObjectsAreDrawnAtTheirPixelSizeAndHungOnTheirOrigin()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        if (!textSupported() || systemFontBytes("systemfont_sans").empty())
            QSKIP("no text support or no font");
        Item item;
        const auto draw = [&](const QByteArray &extra) {
            item.file(u"scene.json"_s, sceneText(200, 200, "0 0 0", textObject(1, "\"HHH\"", "100 100 0", extra)));
            return brightBounds(render(item, 200, 200, nullptr, nullptr, 2));
        };
        // 8 points: an em of 32 pixels; "HHH" is about 60 wide and its caps about 23 tall
        const Bounds centred = draw(",\"horizontalalign\":\"center\",\"verticalalign\":\"center\"");
        QVERIFY2(centred.lit > 150, qPrintable(QString::number(centred.lit)));
        const int w = centred.x1 - centred.x0;
        QVERIFY2(w > 40 && w < 100, qPrintable(QString::number(w)));
        QVERIFY(std::abs((centred.x0 + centred.x1) / 2 - 100) < 8);
        QVERIFY(std::abs((centred.y0 + centred.y1) / 2 - 100) < 14);
        // left: the box starts at the origin and grows to the right; right: it ends there
        const Bounds left = draw(",\"horizontalalign\":\"left\",\"verticalalign\":\"center\"");
        QVERIFY2(left.x0 >= 96 && left.x0 <= 106, qPrintable(QString::number(left.x0)));
        const Bounds right = draw(",\"horizontalalign\":\"right\",\"verticalalign\":\"center\"");
        QVERIFY2(right.x1 >= 94 && right.x1 <= 104, qPrintable(QString::number(right.x1)));
        // top: the box hangs below the origin (scene y is up, so lower on screen = larger row); bottom stands on it
        const Bounds top = draw(",\"horizontalalign\":\"center\",\"verticalalign\":\"top\"");
        const Bounds bottom = draw(",\"horizontalalign\":\"center\",\"verticalalign\":\"bottom\"");
        QVERIFY2(top.y0 >= 98, qPrintable(QString::number(top.y0)));
        QVERIFY2(bottom.y1 <= 102, qPrintable(QString::number(bottom.y1)));
        // the scale scales the glyphs (the bitmap is made at the scaled size, so it stays sharp)
        const Bounds big = draw(",\"scale\":\"2 2 1\"");
        QVERIFY2(big.x1 - big.x0 > static_cast<int>(w * 1.7), qPrintable(QString("%1 vs %2").arg(big.x1 - big.x0).arg(w)));
        // the colour: green
        item.file(u"scene.json"_s, sceneText(200, 200, "0 0 0", textObject(1, "\"HHH\"", "100 100 0")));
        const Frame f = render(item, 200, 200, nullptr, nullptr, 2);
        Px best{};
        for (uint32_t y = 80; y < 120; ++y)
            for (uint32_t x = 70; x < 130; ++x)
                if (f.at(x, y)[1] > best[1])
                    best = f.at(x, y);
        QVERIFY2(best[1] > 200 && best[0] < 10 && best[2] < 10, qPrintable(show(best)));
    }

    void textLayersRunTheirEffects()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        if (!textSupported() || systemFontBytes("systemfont_sans").empty())
            QSKIP("no text support or no font");
        Item item;
        addEffect(item, u"red"_s, R"(
uniform sampler2D g_Texture0;
varying vec2 v_TexCoord;
void main() {
    vec4 c = texSample2D(g_Texture0, v_TexCoord);
    gl_FragColor = vec4(1.0, 0.0, 0.0, c.a);
}
)", simpleEffect(u"red"_s));
        // white, because the layer's colour multiplies what the effects made
        const QByteArray text = QByteArray(R"({"id":1,"name":"t","text":"HHH","font":"systemfont_sans","origin":"100 100 0","pointsize":8,"color":"1 1 1")")
            + withEffects(fxInstance(u"red"_s)) + "}";
        item.file(u"scene.json"_s, sceneText(200, 200, "0 0 0", text));
        const Frame f = render(item, 200, 200, nullptr, nullptr, 2);
        int red = 0, other = 0;
        for (uint32_t y = 0; y < f.height; ++y) {
            for (uint32_t x = 0; x < f.width; ++x) {
                const Px p = f.at(x, y);
                if (p[0] > 200 && p[1] < 20 && p[2] < 20)
                    ++red;
                else if (p[1] > 30 || p[2] > 30) // (the rims are darker reds, never green or blue)
                    ++other;
            }
        }
        QVERIFY2(red > 150, qPrintable(QString::number(red)));
        QCOMPARE(other, 0);
        // and the glyphs are where the text is, not over the whole layer
        const Bounds b = brightBounds(f);
        QVERIFY(b.x1 - b.x0 < 100 && b.y1 - b.y0 < 50);
    }

    void textPaddingAddsRoomAroundTheGlyphsForEffects()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        if (!textSupported() || systemFontBytes("systemfont_sans").empty())
            QSKIP("no text support or no font");
        Item item;
        // an effect that paints the whole layer, so the lit area is the layer's box
        addEffect(item, u"fill"_s, R"(
varying vec2 v_TexCoord;
void main() {
    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)", simpleEffect(u"fill"_s));
        const auto box = [&](const char *padding) {
            const QByteArray text = QByteArray(R"({"id":1,"name":"t","text":"HHH","font":"systemfont_sans","origin":"100 100 0","pointsize":8,"color":"1 1 1","padding":)")
                + padding + withEffects(fxInstance(u"fill"_s)) + "}";
            item.file(u"scene.json"_s, sceneText(200, 200, "0 0 0", text));
            return brightBounds(render(item, 200, 200, nullptr, nullptr, 2));
        };
        const Bounds none = box("0");
        const Bounds padded = box("20");
        QVERIFY(none.x1 > none.x0);
        // 2 x 20 units of padding at scale 1 and one pixel per unit are 40 pixels; a few more or less for the rounding of the
        // box and of the edges the threshold finds
        QVERIFY2(std::abs((padded.x1 - padded.x0) - (none.x1 - none.x0) - 41) <= 3, qPrintable(QString("%1 vs %2").arg(padded.x1 - padded.x0).arg(none.x1 - none.x0)));
        QVERIFY2(std::abs((padded.y1 - padded.y0) - (none.y1 - none.y0) - 41) <= 3, qPrintable(QString("%1 vs %2").arg(padded.y1 - padded.y0).arg(none.y1 - none.y0)));
    }

    void shapeLayersDrawTheirEffects()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.tex(u"util/white"_s, solid(255, 255, 255));
        addEffect(item, u"red"_s, R"(
varying vec2 v_TexCoord;
void main() {
    gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0);
}
)", simpleEffect(u"red"_s));
        // a 100 x 60 quad at (100, 100) with an effect that paints it red; nothing else is in the scene
        const QByteArray quad = R"({"id":1,"name":"q","shape":"quad","size":"100 60","origin":"100 100 0")" + withEffects(fxInstance(u"red"_s)) + "}";
        item.file(u"scene.json"_s, sceneText(200, 200, "0 0 0", quad));
        const Frame f = render(item, 200, 200, nullptr, nullptr, 2);
        const Px centre = f.at(100, 100);
        QVERIFY2(centre[0] > 240 && centre[1] < 10 && centre[2] < 10, qPrintable(show(centre)));
        const Bounds b = brightBounds(f);
        QVERIFY2(std::abs((b.x1 - b.x0 + 1) - 100) <= 2 && std::abs((b.y1 - b.y0 + 1) - 60) <= 2, qPrintable(QString("%1 x %2").arg(b.x1 - b.x0 + 1).arg(b.y1 - b.y0 + 1)));
        QCOMPARE(f.at(20, 20)[0], uint8_t(0));
    }

    void scriptsDriveTextAndTransformsAndMakeLayers()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        if (!textSupported() || systemFontBytes("systemfont_sans").empty())
            QSKIP("no text support or no font");
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // the text becomes wider when the script returns more letters; a second script moves a layer; a third makes a text layer
        const QByteArray script = R"("script":"export function update(v) { return v + 'H'; }")";
        const QByteArray move = R"("script":"export function update(v) { thisLayer.origin = new Vec3(150, 50, 0); return v; }")";
        const QByteArray make = R"("script":"export function init(v) { const t = thisScene.createLayer({text: 'Hi', pointsize: 8, font: 'systemfont_sans', color: '1 0 0'}); t.origin = new Vec3(40, 160, 0); }")";
        item.file(u"scene.json"_s,
                  sceneText(200, 200, "0 0 0",
                            "{\"id\":1,\"name\":\"clock\",\"text\":{" + script + ",\"value\":\"H\"},\"font\":\"systemfont_sans\",\"origin\":\"60 100 0\",\"pointsize\":8,\"color\":\"0 1 0\",\"horizontalalign\":\"left\"}"
                            + "," + layer(2, "w", "20 20 0", "10 10", ",\"visible\":{" + move + ",\"value\":true}")
                            + ",{\"id\":3,\"name\":\"maker\",\"origin\":\"0 0 0\",\"visible\":{" + make + ",\"value\":true}}"));
        uint64_t passes = 0;
        const Frame f1 = render(item, 200, 200, nullptr, &passes, 1);
        const Frame f4 = render(item, 200, 200, nullptr, nullptr, 4);
        // the clock string grew by one letter a frame: "H" on the first frame's update... wider after four frames
        const Bounds a = brightBounds(f1);
        const Bounds b = brightBounds(f4);
        QVERIFY(b.x1 > a.x1);
        // the white square moved from (20, 20) to (150, 50): bottom-up scene coordinates, so row 150
        VERIFY_PX(f4.at(150, 150), (Px{255, 255, 255, 255}), 2);
        // the layer the maker created is red, at (40, 160) from the bottom: row 40
        bool red = false;
        for (uint32_t y = 20; y < 70 && !red; ++y)
            for (uint32_t x = 20; x < 80 && !red; ++x)
                red = f4.at(x, y)[0] > 200 && f4.at(x, y)[1] < 30;
        QVERIFY(red);
    }

    void aBaseMaterialBindsItsOtherTextureSlots()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.tex(u"green"_s, solid(0, 255, 0));
        item.tex(u"blue"_s, solid(0, 0, 255));
        item.file(u"shaders/three.vert"_s, kPassVert);
        item.file(u"shaders/three.frag"_s,
                  "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nuniform sampler2D g_Texture2;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(texSample2D(g_Texture0, v_TexCoord).r, texSample2D(g_Texture1, v_TexCoord).g, texSample2D(g_Texture2, v_TexCoord).b, 1.0); }\n");
        item.file(u"materials/red.json"_s, R"({"passes":[{"shader":"three","blending":"normal","textures":["red","green","blue"]}]})");
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "red", "10 10 0", "20 20")));
        VERIFY_PX(render(item, 20, 20).at(10, 10), (Px{255, 255, 255, 255}), 1); // the three channels come from three pictures
    }

    void aSpotsBeamLightsTheAirInsideItsConeAndNotOutside()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        volumetricShaders(item);
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", beamLight()));
        const Frame f = render(item, 100, 100);
        QVERIFY2(f.at(50, 50)[0] > 40, qPrintable(show(f.at(50, 50))));   // on the axis
        QVERIFY2(f.at(50, 50)[1] == f.at(50, 50)[0] && f.at(50, 50)[2] == f.at(50, 50)[0], qPrintable(show(f.at(50, 50)))); // white light
        VERIFY_PX(f.at(80, 50), (Px{0, 0, 0, 255}), 1);                    // beside the cone (15 wide at most)
        VERIFY_PX(f.at(50, 85), (Px{0, 0, 0, 255}), 1);
        VERIFY_PX(f.at(5, 5), (Px{0, 0, 0, 255}), 1);
        // the beam is the same either side of the axis (the volume is placed, not mirrored)
        QVERIFY(std::abs(int(f.at(45, 50)[0]) - int(f.at(54, 50)[0])) <= 6);
        QVERIFY(std::abs(int(f.at(50, 45)[0]) - int(f.at(50, 54)[0])) <= 6);
    }

    void aBeamOfDensityZeroOrAHiddenLightDrawsNothing()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const QByteArray flat = R"({"id":2,"light":"lspot","origin":"50 50 60","angles":"0 1.5707963 0","color":"1 1 1","intensity":1,"radius":60,)"
                                R"("exponent":0,"innercone":10,"outercone":14,"castvolumetrics":true)";
        for (const QByteArray &extra : {QByteArray(R"(,"density":0})"), QByteArray(R"(,"density":2,"visible":false})")}) {
            Item item;
            volumetricShaders(item);
            item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", flat + extra));
            const Frame f = render(item, 100, 100);
            VERIFY_PX(f.at(50, 50), (Px{0, 0, 0, 255}), 1);
        }
        // and a light that does not cast volumetrics draws none
        Item plain;
        volumetricShaders(plain);
        plain.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", QByteArray(flat).replace(",\"castvolumetrics\":true", "") + R"(,"density":2})"));
        VERIFY_PX(render(plain, 100, 100).at(50, 50), (Px{0, 0, 0, 255}), 1);
    }

    void aPointLightsBeamIsASphereOfLight()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        volumetricShaders(item);
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0",
                            R"({"id":2,"light":"lpoint","origin":"50 50 0","color":"0 1 0","intensity":1,"radius":30,"exponent":0,)"
                            R"("volumetricsexponent":1,"density":2,"castvolumetrics":true})"));
        const Frame f = render(item, 100, 100);
        QVERIFY2(f.at(50, 50)[1] > 40, qPrintable(show(f.at(50, 50))));
        QCOMPARE(int(f.at(50, 50)[0]), 0);
        QCOMPARE(int(f.at(50, 50)[2]), 0);
        QVERIFY(f.at(50, 50)[1] > f.at(65, 50)[1]);              // thinner towards the radius
        VERIFY_PX(f.at(85, 50), (Px{0, 0, 0, 255}), 1);          // beyond it
    }

    void aSceneThatIsNotClearedDoesNotCollectBeams()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // clearenabled off keeps the picture from frame to frame: the beam is added to a copy, not to what is kept
        Item item;
        volumetricShaders(item);
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", beamLight(), R"(,"clearenabled":false)"));
        PropertyStore props;
        const auto frames = [&](int count) {
            return render(item, 100, 100, nullptr, nullptr, count, bridge::msg::ScalingMode::Stretch, &props,
                          [&](SceneRenderer &, int frame) { props.set("tick", frame % 2 == 0); }); // (a property change draws the scene again)
        };
        const Frame one = frames(1);
        const Frame many = frames(6);
        QVERIFY2(one.at(50, 50)[0] > 40, qPrintable(show(one.at(50, 50))));
        QVERIFY2(std::abs(int(one.at(50, 50)[0]) - int(many.at(50, 50)[0])) <= 2, qPrintable(show(one.at(50, 50)) + show(many.at(50, 50))));
    }

    void wildBeamValuesAreHeldToTheirRanges()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto frame = [&](const char *radius, const char *density, const char *intensity, const char *exponent) {
            Item item;
            volumetricShaders(item);
            item.file(u"scene.json"_s,
                      sceneText(100, 100, "0 0 0",
                                QByteArray(R"({"id":2,"light":"lspot","origin":"50 50 60","angles":"0 1.5707963 0","color":"1 1 1","intensity":)") + intensity
                                    + R"(,"radius":)" + radius + R"(,"exponent":0,"volumetricsexponent":)" + exponent
                                    + R"(,"innercone":10,"outercone":14,"density":)" + density + R"(,"castvolumetrics":true})"));
            return render(item, 100, 100);
        };
        // 1e30 everywhere is the same picture as the ranges' ends, and a zero exponent as the smallest one (pow(0, 0) is undefined)
        const Frame wild = frame("1e30", "1e30", "1e30", "1e30");
        const Frame held = frame("100000", "1000", "1000", "1000");
        QVERIFY(wild.rgba == held.rgba);
        const Frame zero = frame("60", "2", "1", "0");
        const Frame tiny = frame("60", "2", "1", "0.001");
        QVERIFY(zero.rgba == tiny.rgba);
    }

    void aCasterBlocksTheBeam()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the cube (10..30 high, 40 to 60 wide) under a spot 60 above: at (35, 49) the light's rays pass through it between z 20 and 30. Four
        // frames separate the beam from the lit floor: shadow and beam, shadow alone, beam alone, neither
        const auto frame = [&](bool shadow, bool beam) {
            Item item;
            volumetricShaders(item);
            shadowItem(item, QByteArray(R"({"id":2,"light":"lspot","origin":"50 50 60","angles":"0 1.5707963 0","color":"1 1 1","intensity":0.3,"radius":400,)"
                                        R"("exponent":0,"innercone":50,"outercone":60,"density":10,"volumetricsexponent":0)")
                               + (shadow ? R"(,"castshadow":true)" : "") + (beam ? R"(,"castvolumetrics":true)" : "") + "}");
            return render(item, 100, 100);
        };
        const auto beamAt = [&](bool shadow, uint32_t x, uint32_t y) {
            return int(frame(shadow, true).at(x, y)[0]) - int(frame(shadow, false).at(x, y)[0]);
        };
        const int unshadowed = beamAt(false, 35, 49);
        const int shadowed = beamAt(true, 35, 49);
        QVERIFY2(unshadowed > 10, qPrintable(QString::number(unshadowed)));
        QVERIFY2(shadowed < unshadowed - 4, qPrintable(QString::number(shadowed) + " vs " + QString::number(unshadowed)));
    }

    void aPointLightLightsAMeshNearItAndNothingBeyondItsRadius()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        litSlabItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 10","color":"1 0 0","intensity":3,"radius":40,"exponent":1})");
        const Frame f = render(item, 100, 100);
        // under the light: 3 * (1 - 10 / 40) = 2.25, all red
        VERIFY_PX(f.at(20, 49), (Px{255, 0, 0, 255}), 2);
        // 20 to the side: 3 * (1 - 22.4 / 40) * (10 / 22.4) = 0.59
        VERIFY_PX(f.at(40, 49), (Px{150, 0, 0, 255}), 6);
        // beyond the radius: nothing (the ambient colour is black)
        VERIFY_PX(f.at(70, 49), (Px{0, 0, 0, 255}), 1);
        VERIFY_PX(f.at(20, 5), (Px{0, 0, 0, 255}), 1);
    }

    void aHiddenLightGivesNothingAndASecondLightAdds()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        litSlabItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 10","color":"1 0 0","intensity":3,"radius":40,"exponent":1,"visible":false},)"
                          R"({"id":3,"light":"lpoint","origin":"80 50 10","color":"0 0 1","intensity":3,"radius":40,"exponent":1})");
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(20, 49), (Px{0, 0, 0, 255}), 1);
        VERIFY_PX(f.at(80, 49), (Px{0, 0, 255, 255}), 2);
    }

    void aSpotLightsInsideItsConeOnly()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        // turned a quarter about y, its +x shines down -z onto the slab from 40 above (50, 50): the inner cone (20 degrees) is 14.6
        // wide there, the outer one (30 degrees) 23.1
        litSlabItem(item, R"({"id":2,"light":"lspot","origin":"50 50 40","angles":"0 1.5707963 0","color":"0 1 0","intensity":1,"radius":200,"exponent":0,)"
                          R"("innercone":20,"outercone":30})");
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(50, 49), (Px{0, 255, 0, 255}), 2);
        VERIFY_PX(f.at(60, 49), (Px{0, 247, 0, 255}), 4); // inside the inner cone: only the cosine to the normal (0.97)
        const Px edge = f.at(69, 49);                        // between the two cones, half way: 0.5 * 0.9
        QVERIFY2(edge[1] > 10 && edge[1] < 250, qPrintable(show(edge)));
        VERIFY_PX(f.at(80, 49), (Px{0, 0, 0, 255}), 1);   // outside the outer cone
        VERIFY_PX(f.at(50, 15), (Px{0, 0, 0, 255}), 1);
    }

    void aTubeLightsAlongItsSegment()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        // from (10, 80, 5) to its control point 80 further along x
        litSlabItem(item, R"({"id":2,"light":"ltube","origin":"10 80 5","controlpoint":"80 0 0","color":"1 1 1","intensity":2,"radius":20,"exponent":1})");
        const Frame f = render(item, 100, 100);
        // 5 above any point of the segment: 2 * (1 - 5 / 20) = 1.5 all along it
        VERIFY_PX(f.at(15, 19), (Px{255, 255, 255, 255}), 2);
        VERIFY_PX(f.at(50, 19), (Px{255, 255, 255, 255}), 2);
        VERIFY_PX(f.at(85, 19), (Px{255, 255, 255, 255}), 2);
        VERIFY_PX(f.at(50, 70), (Px{0, 0, 0, 255}), 1);   // 50 away from it
        VERIFY_PX(f.at(99, 70), (Px{0, 0, 0, 255}), 1);
    }

    void aDirectionalLightLightsWhatFacesItWhereverItIs()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // shining down -z (its +x turned a quarter about y): the slab faces it everywhere, far from the light's origin too
        Item down;
        litSlabItem(down, R"({"id":2,"light":"ldirectional","origin":"-5000 0 0","angles":"0 1.5707963 0","color":"1 0.5 0","intensity":0.5})");
        const Frame f = render(down, 100, 100);
        VERIFY_PX(f.at(10, 10), (Px{128, 64, 0, 255}), 2);
        VERIFY_PX(f.at(90, 90), (Px{128, 64, 0, 255}), 2);
        // shining up +z: the slab's front faces away
        Item up;
        litSlabItem(up, R"({"id":2,"light":"ldirectional","angles":"0 -1.5707963 0","color":"1 0.5 0","intensity":0.5})");
        VERIFY_PX(render(up, 100, 100).at(50, 50), (Px{0, 0, 0, 255}), 1);
    }

    void aCubeBetweenAPointLightAndTheFloorShadowsIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the light 60 above the floor's middle, the cube from 10 to 30 above it, 40 to 60 across: its shadow on the floor runs from 30
        // to 70 (seen from above the cube hides 40 to 60 of it)
        const QByteArray light = R"({"id":2,"light":"lpoint","origin":"50 50 60","color":"1 1 1","intensity":1,"radius":400,"exponent":0,"castshadow":true})";
        Item item;
        shadowItem(item, light);
        uint64_t passes = 0, shadowPasses = 0, firstPictures = 0, pictures = 0;
        const Frame f = render(item, 100, 100, nullptr, nullptr, 3, bridge::msg::ScalingMode::Stretch, nullptr, [&](SceneRenderer &r, int frame) {
            if (frame > 0) {
                r.setScaling(frame == 1 ? bridge::msg::ScalingMode::Fill : bridge::msg::ScalingMode::Stretch); // another scene pass
            }
            passes = r.scenePasses();
            shadowPasses = r.shadowPasses();
            if (frame == 1) { // (the callback runs before a frame is drawn)
                firstPictures = r.shadowPicturesDrawn();
            }
            pictures = r.shadowPicturesDrawn();
        });
        VERIFY_PX(f.at(35, 49), (Px{0, 0, 0, 255}), 12);        // in the shadow: black (the ambient colour)
        VERIFY_PX(f.at(65, 35), (Px{0, 0, 0, 255}), 12);
        VERIFY_PX(f.at(15, 49), (Px{220, 220, 220, 255}), 6);   // lit: the cosine to the light, 0.86
        VERIFY_PX(f.at(85, 85), (Px{198, 198, 198, 255}), 6);   // 0.78
        VERIFY_PX(f.at(50, 49), (Px{255, 255, 255, 255}), 2);   // the cube's top faces the light
        // the scene was drawn again, the shadows were not: nothing they depend on moved
        QCOMPARE(passes, uint64_t(2));
        QCOMPARE(shadowPasses, uint64_t(1));
        // each picture is kept on its own: the pictures drawn the first time (a point light's faces a receiver sees, six at most) are
        // all there are, the later scene passes draw none
        QVERIFY2(firstPictures >= 1 && firstPictures <= 6, qPrintable(QString::number(firstPictures)));
        QCOMPARE(pictures, firstPictures);
        // the cube casting nothing: lit under it too
        Item off;
        shadowItem(off, light, "50 50 20", R"(,"castshadow":false)");
        const Frame g = render(off, 100, 100);
        VERIFY_PX(g.at(35, 49), (Px{247, 247, 247, 255}), 6);
        VERIFY_PX(g.at(15, 49), (Px{220, 220, 220, 255}), 6);
    }

    void anAdditivePartCastsNoShadow()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the cube of the point light's test, its material additive (light added to the scene, a glow): the floor under it is lit
        const QByteArray light = R"({"id":2,"light":"lpoint","origin":"50 50 60","color":"1 1 1","intensity":1,"radius":400,"exponent":0,"castshadow":true})";
        Item item;
        litModels(item);
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0",
                                             R"({"id":1,"name":"slab","model":"models/slab.mdl","origin":"50 50 -1","scale":"50 50 1"},)"
                                             R"({"id":5,"name":"cube","model":"models/slabadd.mdl","origin":"50 50 20","scale":"10 10 10"},)" + light,
                                             R"(,"ambientcolor":"0 0 0")"));
        meshgen::Model m;
        m.parts.push_back(meshgen::box({-1, -1, -1}, {1, 1, 1}, "materials/models/slabadd.json"));
        const auto mdl = meshgen::write(m);
        item.file(u"models/slabadd.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
        item.file(u"materials/models/slabadd.json"_s, R"({"passes":[{"shader":"litmesh","blending":"additive","textures":["white"]}]})");
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(35, 49), (Px{247, 247, 247, 255}), 8); // as lit as with a cube that casts nothing
        VERIFY_PX(f.at(65, 35), (Px{247, 247, 247, 255}), 8);
    }

    void aDirectionalLightShadowsInAnOrthographicScene()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the scene's plane is 10000 units into the orthographic view's depth: the cascades start there, not at the near plane. The light
        // shines along (1, 0, -1): the cube (10 to 30 above the floor, 40 to 60 across) throws its shadow on the floor from x = 50 to 90
        const auto lightWith = [](const char *extra) {
            return QByteArray(R"({"id":2,"light":"ldirectional","angles":"0 0.7853982 0","color":"1 1 1","intensity":1)") + extra + "}";
        };
        Item lit, unlit;
        shadowItem(lit, lightWith(R"(,"castshadow":true)"));
        shadowItem(unlit, lightWith(""));
        const Frame f = render(lit, 100, 100);
        const Frame plain = render(unlit, 100, 100);
        QVERIFY2(plain.at(75, 50)[0] > 120, qPrintable(QString::number(plain.at(75, 50)[0])));
        VERIFY_PX(f.at(75, 50), (Px{0, 0, 0, 255}), 12);                       // in the shadow
        QVERIFY2(f.at(20, 50)[0] > 120, qPrintable(QString::number(f.at(20, 50)[0])));  // lit away from the cube
    }

    void aShadowGoesWhenItsOnlyCasterIsHidden()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const QByteArray light = R"({"id":2,"light":"lpoint","origin":"50 50 60","color":"1 1 1","intensity":1,"radius":400,"exponent":0,"castshadow":true})";
        Item item;
        // the floor does not cast: the cube is the one caster, and a property hides it after two frames
        litModels(item);
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0",
                                             R"({"id":1,"name":"slab","model":"models/slab.mdl","origin":"50 50 -1","scale":"50 50 1","castshadow":false},)"
                                             R"({"id":5,"name":"cube","model":"models/slab.mdl","origin":"50 50 20","scale":"10 10 10",)"
                                             R"("visible":{"user":"show","value":true}},)" + light,
                                             R"(,"ambientcolor":"0 0 0")"));
        PropertyStore props;
        const Frame shadowed = render(item, 100, 100, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, &props);
        VERIFY_PX(shadowed.at(35, 49), (Px{0, 0, 0, 255}), 12);
        const Frame gone = render(item, 100, 100, nullptr, nullptr, 3, bridge::msg::ScalingMode::Stretch, &props, [&](SceneRenderer &, int frame) {
            if (frame == 2)
                props.set("show", false);
        });
        VERIFY_PX(gone.at(35, 49), (Px{247, 247, 247, 255}), 8); // lit again: the last caster went, its shadow with it
    }

    void aPointLightsSideFacesShadowToo()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the light low beside the cube (2 to 22 high): the floor beyond it, seen through the light's +x face, is in its shadow
        Item item;
        shadowItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 15","color":"1 1 1","intensity":4,"radius":400,"exponent":0,"castshadow":true})",
                   "50 50 12");
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(80, 49), (Px{0, 0, 0, 255}), 12);
        VERIFY_PX(f.at(90, 90), (Px{186, 186, 186, 255}), 8); // +x face too, the ray passes beside the cube
        VERIFY_PX(f.at(10, 49), (Px{255, 255, 255, 255}), 2); // the -z face
    }

    void aSpotsShadowFallsInsideItsCone()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // shining down -z from 60 above the middle, the inner cone 50 degrees: the floor around the cube is fully lit but its shadow
        Item item;
        shadowItem(item, R"({"id":2,"light":"lspot","origin":"50 50 60","angles":"0 1.5707963 0","color":"1 1 1","intensity":1,"radius":400,)"
                         R"("exponent":0,"innercone":50,"outercone":60,"castshadow":true})");
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(35, 49), (Px{0, 0, 0, 255}), 12);
        VERIFY_PX(f.at(15, 49), (Px{220, 220, 220, 255}), 6);
        VERIFY_PX(f.at(65, 65), (Px{0, 0, 0, 255}), 12);
    }

    void aDirectionalLightsCascadesShadowNearAndFar()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a perspective view down a long floor (the slab stretched to 100 x 300) with a cube near the camera and one far from it, under a
        // light shining along (1, 0, -1): each throws a shadow towards +x. The cascades split the view at 80, 110 and 400: the near cube
        // (62 deep in the view) is in the first, the far one (235) in the last
        const auto sceneWith = [](Item &item, const char *light) {
            litModels(item);
            item.file(u"scene.json"_s,
                      QByteArray(R"({"camera":{"center":"50 60 0","eye":"50 -40 45","up":"0 0 1"},"general":{"clearcolor":"0 0 0","fov":50,"nearz":0.1,)"
                                 R"("farz":1000,"orthogonalprojection":null,"ambientcolor":"0 0 0"},"objects":[)"
                                 R"({"id":1,"name":"floor","model":"models/slab.mdl","origin":"50 150 -1","scale":"50 150 1"},)"
                                 R"({"id":3,"name":"near","model":"models/slab.mdl","origin":"50 10 5","scale":"5 5 5"},)"
                                 R"({"id":4,"name":"far","model":"models/slab.mdl","origin":"50 200 10","scale":"10 10 10"},)")
                          + R"({"id":2,"light":"ldirectional","angles":"0 0.7853982 0","color":"1 1 1","intensity":1)" + light + "}]}");
        };
        const auto darkened = [](const Frame &with, const Frame &without, uint32_t y0, uint32_t y1) {
            int n = 0;
            for (uint32_t y = y0; y < y1; ++y) {
                for (uint32_t x = 0; x < with.width; ++x) {
                    n += without.at(x, y)[0] - with.at(x, y)[0] > 100 ? 1 : 0;
                }
            }
            return n;
        };
        Item lit, unlit;
        sceneWith(lit, R"(,"castshadow":true,"cascadedistance0":80,"cascadedistance1":110,"cascadedistance2":400)");
        // the same light without its shadow: what the shadows take away is the difference
        sceneWith(unlit, "");
        const Frame f = render(lit, 320, 180);
        const Frame plain = render(unlit, 320, 180);
        const int nearShadow = darkened(f, plain, 90, 180); // the near cube is low in the picture, the far one high
        const int farShadow = darkened(f, plain, 0, 90);
        QVERIFY2(nearShadow > 160, qPrintable(QString::number(nearShadow)));
        QVERIFY2(farShadow > 25, qPrintable(QString::number(farShadow)));
        QVERIFY2(nearShadow + farShadow < 320 * 180 / 8, qPrintable(QString::number(nearShadow + farShadow))); // and nothing else darkens
        // cascades that end well before the cubes (their boxes reach a little beyond their slices): no shadow anywhere
        Item shortCascades;
        sceneWith(shortCascades, R"(,"castshadow":true,"cascadedistance0":5,"cascadedistance1":10,"cascadedistance2":15)");
        const int none = darkened(render(shortCascades, 320, 180), plain, 0, 180);
        QVERIFY2(none < 20, qPrintable(QString::number(none)));
    }

    void aLitLayersNormalMapAnswersALight()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        addLitShader(item, u"litimage"_s, false);
        item.tex(u"white"_s, solid(255, 255, 255));
        // left half tilted towards +x, right half towards -x
        TexSpec normals;
        normals.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        normals.texW = normals.imgW = 2;
        normals.texH = normals.imgH = 1;
        normals.images = {{MipSpec{2, 1, {204, 128, 230, 255, 51, 128, 230, 255}, false}}};
        item.tex(u"normals"_s, normals);
        item.file(u"models/lit.json"_s, R"({"material":"materials/lit.json"})");
        item.file(u"materials/lit.json"_s, R"({"passes":[{"shader":"litimage","blending":"normal","textures":["white","normals"],"combos":{"LIGHTING":1}}]})");
        // a light far to the right: the faces turned towards it are lit, the others not
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0",
                                             R"({"id":1,"name":"lit","image":"models/lit.json","origin":"50 50 0","size":"100 100"},)"
                                             R"({"id":2,"light":"lpoint","origin":"150 50 20","color":"1 1 1","intensity":3,"radius":200,"exponent":1})",
                                             R"(,"ambientcolor":"0 0 0")"));
        const Frame f = render(item, 100, 100);
        const Px left = f.at(25, 50);
        QVERIFY2(left[0] > 150 && left[0] == left[1] && left[1] == left[2], qPrintable(show(left)));
        VERIFY_PX(f.at(75, 50), (Px{0, 0, 0, 255}), 1);
    }

    void aLitLayerDrawnThroughItsBuffersIsLitWhereItIsInTheScene()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        addLitShader(item, u"litimage"_s, false);
        item.tex(u"white"_s, solid(255, 255, 255));
        item.file(u"models/lit.json"_s, R"({"material":"materials/lit.json"})");
        item.file(u"materials/lit.json"_s, R"({"passes":[{"shader":"litimage","blending":"normal","textures":["white"]}]})");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        for (const bool effect : {false, true}) {
            // a small light near the left edge of a layer that sits at the right of the scene: its world positions decide
            const QByteArray fx = effect ? withEffects(fxInstance(u"invert"_s)) : QByteArray();
            item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0",
                                                 R"({"id":1,"name":"lit","image":"models/lit.json","origin":"75 50 0","size":"50 100")" + fx + "},"
                                                 R"({"id":2,"light":"lpoint","origin":"55 50 5","color":"1 0 0","intensity":4,"radius":20,"exponent":1})",
                                                 R"(,"ambientcolor":"0 0 0")"));
            const Frame f = render(item, 100, 100);
            const Px lit = effect ? Px{0, 255, 255, 255} : Px{255, 0, 0, 255};
            const Px dark = effect ? Px{255, 255, 255, 255} : Px{0, 0, 0, 255};
            VERIFY_PX(f.at(55, 50), lit, 2);
            VERIFY_PX(f.at(90, 50), dark, 1);
            VERIFY_PX(f.at(55, 10), dark, 1);
        }
    }

    void aLitLayerKeptInItsBuffersFollowsTheParallaxUnderTheLights()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a lit layer with an effect (drawn through its buffers, which are kept between frames) and a parallax depth; the pointer moves
        // once and the camera eases after it for many frames without anything else changing. The light does not move with the
        // parallax, so its spot stays where it is on the screen while the layer slides 20 units left under it
        Item item;
        addLitShader(item, u"litimage"_s, false);
        item.tex(u"white"_s, solid(255, 255, 255));
        item.file(u"models/lit.json"_s, R"({"material":"materials/lit.json"})");
        item.file(u"materials/lit.json"_s, R"({"passes":[{"shader":"litimage","blending":"normal","textures":["white"]}]})");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0",
                                             R"({"id":1,"name":"lit","image":"models/lit.json","origin":"50 50 0","size":"100 100","parallaxDepth":"1 1")"
                                                 + withEffects(fxInstance(u"invert"_s)) + "},"
                                                 R"({"id":2,"light":"lpoint","origin":"30 50 3","color":"1 0 0","intensity":4,"radius":6,"exponent":1})",
                                             R"(,"ambientcolor":"0 0 0","cameraparallax":true,"cameraparallaxamount":0.4,"cameraparallaxdelay":1.5,)"
                                             R"("cameraparallaxmouseinfluence":1)"));
        m_frameStep = 0.02;
        const Frame f = render(item, 100, 100, nullptr, nullptr, 200, bridge::msg::ScalingMode::Stretch, nullptr, [](SceneRenderer &r, int frame) {
            if (frame == 0)
                r.setPointer(100, 50, 0, {100, 100});
        });
        VERIFY_PX(f.at(30, 50), (Px{0, 255, 255, 255}), 2);    // under the light (inverted red)
        VERIFY_PX(f.at(14, 50), (Px{255, 255, 255, 255}), 1);  // where the spot would be had the buffer kept its first lighting
        VERIFY_PX(f.at(95, 50), (Px{0, 0, 0, 255}), 1);        // the layer has moved: the clear colour shows at the right
    }

    void aPropertyScriptThatDoesNotCompileLeavesThePropertyAtItsValue()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // what 3815542599 holds: a pasted JSON fragment where the script should be
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "w", "50 50 0", "100 100",
                                                                     ",\"alpha\":{\"script\":\"export let x = 1;\\n\\\"properties\\\": {}\\n\",\"value\":0.5}")));
        bool supported = false;
        const Frame f = render(item, 100, 100, &supported, nullptr, 2);
        QVERIFY(supported);
        VERIFY_PX(f.at(50, 50), (Px{128, 128, 128, 255}), 2);
    }

    void aPuppetIsItsMeshInRestPoseUntilAnimationsPlay()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        using namespace puppetfix;
        // one root bone at the origin; animation 7 holds it 20 units to the right
        const auto mdl = makeMdl({{"root", -1, 0, 0}}, {{7, "slide", "loop", 30.0f, 1, {{{frame(20, 0), frame(20, 0)}}}}});
        const auto bounds = [&](const QByteArray &layers, const QByteArray &extra = {}) {
            Item item;
            puppetItem(item, mdl, layers, extra);
            return litBounds(render(item, 100, 100, nullptr, nullptr, 2));
        };
        // no animation layers: the mesh as authored, a 20 x 20 square centred on the object's origin (50, 50)
        Bounds rest = bounds("");
        QCOMPARE(rest.lit, 400);
        QVERIFY2(rest.x0 >= 39 && rest.x0 <= 41 && rest.x1 >= 59 && rest.x1 <= 61, qPrintable(QString("%1..%2").arg(rest.x0).arg(rest.x1)));
        // the object's scale scales the mesh
        QCOMPARE(bounds("", ",\"scale\":\"2 2 1\"").lit, 1600);
        // the animation layer moves it 20 units right (skinned through g_Bones)
        Bounds slid = bounds(",\"animationlayers\":[{\"animation\":7}]");
        QCOMPARE(slid.lit, 400);
        QVERIFY2(slid.x0 >= 59 && slid.x0 <= 61, qPrintable(QString("%1..%2").arg(slid.x0).arg(slid.x1)));
        // half a layer: half the way (the rest stays the bind pose)
        Bounds half = bounds(",\"animationlayers\":[{\"animation\":7,\"blend\":0.5}]");
        QVERIFY2(half.x0 >= 49 && half.x0 <= 51, qPrintable(QString("%1..%2").arg(half.x0).arg(half.x1)));
        // a layer that is not visible, or names an animation the model lacks, plays nothing
        QVERIFY(bounds(",\"animationlayers\":[{\"animation\":7,\"visible\":false}]").x0 <= 41);
        QVERIFY(bounds(",\"animationlayers\":[{\"animation\":99}]").x0 <= 41);
    }

    void aMeshIsDrawnWithDepthSoTheNearerOneWinsWhateverTheOrder()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // two boxes overlapping on screen: blue in front (z 50), yellow behind (z -50) and 6 units to the right
        for (bool blueFirst : {true, false}) {
            Item item;
            const QByteArray blue = meshObject(2, "blue", "50 50 50");
            const QByteArray yellow = meshObject(3, "yellow", "56 50 -50");
            meshItem(item, blueFirst ? blue + "," + yellow : yellow + "," + blue);
            bool supported = false;
            const Frame f = render(item, 100, 100, &supported);
            QVERIFY(supported);
            VERIFY_PX(f.at(50, 50), (Px{0, 0, 255, 255}), 2);   // the overlap: the nearer box
            VERIFY_PX(f.at(64, 50), (Px{255, 255, 0, 255}), 2); // yellow beyond blue's right edge (60)
            VERIFY_PX(f.at(20, 50), (Px{0, 0, 51, 255}), 2);    // the clear colour
        }
        // a 2D layer drawn after the mesh covers it (layers do not test depth), one drawn before is covered
        Item item;
        meshItem(item, layer(1, "plain", "50 50 0", "40 40") + "," + meshObject(2, "blue", "50 50 50"));
        item.image(u"plain"_s);
        item.tex(u"plain"_s, solid(0, 255, 255));
        VERIFY_PX(render(item, 100, 100).at(50, 50), (Px{0, 0, 255, 255}), 2);
        VERIFY_PX(render(item, 100, 100).at(35, 50), (Px{0, 255, 255, 255}), 2);
        Item over;
        meshItem(over, meshObject(2, "blue", "50 50 50") + "," + layer(3, "plain", "50 50 0", "40 40"));
        over.image(u"plain"_s);
        over.tex(u"plain"_s, solid(0, 255, 255));
        VERIFY_PX(render(over, 100, 100).at(50, 50), (Px{0, 255, 255, 255}), 2);
    }

    void aMeshCullsTheFacesSeenFromBehindAsItsMaterialSays()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // without the depth test the faces land in the model's order and the -z face (red) comes last; culling leaves the +z one (green)
        const auto centre = [&](const char *pass, const QByteArray &extra = {}) {
            Item item;
            meshItem(item, meshObject(2, "box", "50 50 0", extra));
            item.file(u"materials/models/box.json"_s, QByteArray(R"({"passes":[{"shader":"meshshader","blending":"normal","textures":["halves"],)") + pass + "}]}");
            return render(item, 100, 100).at(50, 50);
        };
        VERIFY_PX(centre(R"("depthtest":"disabled","cullmode":"normal")"), (Px{0, 255, 0, 255}), 2);
        VERIFY_PX(centre(R"("depthtest":"disabled","cullmode":"nocull")"), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(centre(R"("depthtest":"enabled","cullmode":"nocull")"), (Px{0, 255, 0, 255}), 2); // depth alone finds the front
        VERIFY_PX(centre(R"("depthtest":"disabled")"), (Px{0, 255, 0, 255}), 2);                  // culled unless the material says nocull
        // a mirroring scale turns the winding over, and the front is still the face toward the viewer
        VERIFY_PX(centre(R"("depthtest":"disabled","cullmode":"normal")", R"(,"scale":"-10 10 10")"), (Px{0, 255, 0, 255}), 2);
        // turned a quarter about y, the -x face looks at the viewer: red, and its world normal is +z
        VERIFY_PX(centre(R"("cullmode":"normal")", R"(,"angles":"0 1.5707963 0")"), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(centre(R"("cullmode":"normal","combos":{"NORMALCOLOR":1})", R"(,"angles":"0 1.5707963 0","scale":"10 30 5")"),
                  (Px{128, 128, 255, 255}), 3);
    }

    void aMovedMeshLeavesNoDepthBehind()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // frame 2 moves the box behind and partly beside where it was: the depth of frame 1 must be gone
        Item item;
        meshItem(item, meshObject(2, "blue", "50 50 50", R"(,"origin":{"user":"pos","value":"50 50 50"})").replace("\"origin\":\"50 50 50\",", ""));
        PropertyStore props;
        uint64_t passes = 0;
        const Frame f = render(item, 100, 100, nullptr, &passes, 2, bridge::msg::ScalingMode::Stretch, &props, [&](SceneRenderer &, int frame) {
            if (frame == 1)
                props.set("pos", std::string("55 50 -50"));
        });
        QCOMPARE(passes, uint64_t{2});
        VERIFY_PX(f.at(58, 50), (Px{0, 0, 255, 255}), 2); // where both were: drawn, though farther than the last frame's
        VERIFY_PX(f.at(42, 50), (Px{0, 0, 51, 255}), 2);  // where only the old one was: the clear colour
    }

    void theDepthIsClearedOnceAFrameAndSurvivesALayerWithEffectsBetweenMeshes()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        meshItem(item, meshObject(2, "blue", "50 50 50") + "," + layer(3, "plain", "10 10 0", "10 10", withEffects(fxInstance(u"invert"_s))) + ","
                     + meshObject(4, "yellow", "56 50 -50"));
        item.image(u"plain"_s);
        item.tex(u"plain"_s, solid(0, 255, 255));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(50, 50), (Px{0, 0, 255, 255}), 2);   // the far box drawn last still loses to the near one
        VERIFY_PX(f.at(64, 50), (Px{255, 255, 0, 255}), 2);
        VERIFY_PX(f.at(10, 90), (Px{255, 0, 0, 255}), 2);   // the layer between them, inverted by its effect
    }

    void aReflectiveMeshReadsTheSceneDrawnBeforeIt()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        meshItem(item, layer(1, "plain", "50 50 0", "100 100") + "," + meshObject(2, "box", "50 50 0"));
        item.image(u"plain"_s);
        item.tex(u"plain"_s, solid(255, 0, 255));
        item.file(u"shaders/reflshader.vert"_s,
                  "uniform mat4 g_ModelMatrix;\nuniform mat4 g_ViewProjectionMatrix;\nattribute vec3 a_Position;\n"
                  "void main() { gl_Position = mul(mul(vec4(a_Position, 1.0), g_ModelMatrix), g_ViewProjectionMatrix); }\n");
        item.file(u"shaders/reflshader.frag"_s,
                  "uniform sampler2D g_Texture3; // {\"hidden\":true,\"default\":\"_rt_MipMappedFrameBuffer\"}\n"
                  "void main() { gl_FragColor = texSample2D(g_Texture3, vec2(0.5, 0.5)); }\n");
        item.file(u"materials/models/box.json"_s, R"({"passes":[{"shader":"reflshader","blending":"normal","textures":["halves"]}]})");
        const Frame f = render(item, 100, 100);
        VERIFY_PX(f.at(50, 50), (Px{255, 0, 255, 255}), 2); // the copy of the scene under the mesh, not the white fallback
    }

    // A skinned 3D mesh (a `.mdl` with a skeleton as a model object): two stacked unit boxes, the lower one on the root bone, the
    // upper one on a child bone at the top of the lower; animation 5 turns the child a quarter about z. With the animation layer the
    // upper box swings over to -x (bent at the joint), without one the mesh stands as authored.
    void aSkinnedMeshBendsWhenItsBoneRotates()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        meshgen::Model m;
        m.skinned = true;
        meshgen::Part bar = meshgen::box({-0.5f, 0, -0.5f}, {0.5f, 1, 0.5f}, "materials/models/blue.json");
        meshgen::Part upper = meshgen::box({-0.5f, 1, -0.5f}, {0.5f, 2, 0.5f});
        for (meshgen::Vertex v : upper.vertices) {
            v.bones = {1, 0, 0, 0};
            bar.vertices.push_back(v);
        }
        for (uint32_t i : upper.indices) {
            bar.indices.push_back(i + 24);
        }
        m.parts.push_back(bar);
        mdlgen::Pose joint;
        joint.position = {0, 1, 0};
        m.bones = {{"root", -1, {}}, {"joint", 0, joint}};
        mdlgen::Pose bent = joint;
        bent.angles = {0, 0, 1.5707963f};
        mdlgen::Animation anim;
        anim.id = 5;
        anim.mode = "single";
        anim.length = 1;
        anim.tracks = {{mdlgen::Pose{}, mdlgen::Pose{}}, {bent, bent}};
        m.animations = {anim};
        const auto mdl = meshgen::write(m);
        bool keepsDrawing = true;
        const auto bounds = [&](const QByteArray &extra) {
            Item item;
            meshItem(item, meshObject(2, "bar", "50 30 0", extra));
            item.file(u"models/bar.mdl"_s, QByteArray(reinterpret_cast<const char *>(mdl.data()), static_cast<qsizetype>(mdl.size())));
            item.file(u"shaders/skinshader.vert"_s,
                      "uniform mat4 g_ModelMatrix;\nuniform mat4 g_ViewProjectionMatrix;\n#if SKINNING\nuniform mat4x3 g_Bones[BONECOUNT];\n"
                      "attribute uvec4 a_BlendIndices;\nattribute vec4 a_BlendWeights;\n#endif\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
                      "varying vec2 v_TexCoord;\nvoid main() {\n  vec3 p = a_Position;\n#if SKINNING\n"
                      "  p = mul(vec4(p, 1.0), g_Bones[a_BlendIndices.x] * a_BlendWeights.x + g_Bones[a_BlendIndices.y] * a_BlendWeights.y\n"
                      "                      + g_Bones[a_BlendIndices.z] * a_BlendWeights.z + g_Bones[a_BlendIndices.w] * a_BlendWeights.w);\n#endif\n"
                      "  gl_Position = mul(mul(vec4(p, 1.0), g_ModelMatrix), g_ViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n");
            item.file(u"shaders/skinshader.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
            item.file(u"materials/models/blue.json"_s, R"({"passes":[{"shader":"skinshader","blending":"normal","textures":["blue"]}]})");
            bool supported = false;
            // the shader never reads g_Time: the bones alone must keep the frames coming
            keepsDrawing = true;
            const Bounds b = litBounds(render(item, 100, 100, &supported, nullptr, 4, bridge::msg::ScalingMode::Stretch, nullptr,
                                              [&](SceneRenderer &r, int frame) { keepsDrawing = keepsDrawing && (frame == 0 || r.needsFrame()); }));
            if (!supported) {
                return Bounds{};
            }
            return b;
        };
        // as authored: a 10 x 20 bar from x 45 to 55
        const Bounds rest = bounds({});
        QVERIFY2(rest.x0 >= 44 && rest.x0 <= 46 && rest.x1 >= 53 && rest.x1 <= 55, qPrintable(QString("%1..%2").arg(rest.x0).arg(rest.x1)));
        QVERIFY2(rest.y1 - rest.y0 >= 18 && rest.y1 - rest.y0 <= 21, qPrintable(QString("%1..%2").arg(rest.y0).arg(rest.y1)));
        // bent: the upper box now lies from x 40 to 50, beside the joint, and the bar is shorter
        const Bounds bentBar = bounds(R"(,"animationlayers":[{"animation":5}])");
        QVERIFY(keepsDrawing);
        QVERIFY2(bentBar.x0 >= 39 && bentBar.x0 <= 41 && bentBar.x1 >= 53 && bentBar.x1 <= 55, qPrintable(QString("%1..%2").arg(bentBar.x0).arg(bentBar.x1)));
        QVERIFY2(bentBar.y1 - bentBar.y0 >= 13 && bentBar.y1 - bentBar.y0 <= 16, qPrintable(QString("%1..%2").arg(bentBar.y0).arg(bentBar.y1)));
        // half the animation: half the angle, an eighth of a turn (the upper box's far corner swings out to x 39.4 and up to y 50.6)
        const Bounds half = bounds(R"(,"animationlayers":[{"animation":5,"blend":0.5}])");
        QVERIFY2(half.x0 >= 38 && half.x0 <= 41, qPrintable(QString("%1..%2").arg(half.x0).arg(half.x1)));
        QVERIFY2(half.y1 - half.y0 >= 19 && half.y1 - half.y0 <= 22, qPrintable(QString("%1..%2").arg(half.y0).arg(half.y1)));
    }

    // A mesh whose material samples another layer's composite (`_rt_imageLayerComposite_<id>_a`): the layer's picture after its
    // effects, rendered for the mesh even though the layer is hidden and comes later in the file.
    void aMeshSamplingAnotherLayersCompositeShowsThatLayer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto centre = [&](const QByteArray &layerExtra, Px *corner = nullptr) {
            Item item;
            meshItem(item, meshObject(2, "box", "50 50 0") + "," + layer(7, "plain", "10 10 0", "10 10", ",\"visible\":false" + layerExtra));
            item.image(u"plain"_s);
            item.tex(u"plain"_s, solid(255, 0, 255));
            addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
            item.file(u"materials/models/box.json"_s, R"({"passes":[{"shader":"meshshader","blending":"normal","textures":["_rt_imageLayerComposite_7_a"]}]})");
            bool supported = false;
            const Frame f = render(item, 100, 100, &supported);
            if (!supported) {
                return Px{-1, -1, -1, -1};
            }
            if (corner) {
                *corner = f.at(10, 90);
            }
            return f.at(50, 50);
        };
        Px corner{};
        VERIFY_PX(centre({}, &corner), (Px{255, 0, 255, 255}), 2);
        VERIFY_PX(corner, (Px{0, 0, 51, 255}), 2); // the hidden layer itself is not drawn
        // with an effect, the composite is the picture after it
        VERIFY_PX(centre(withEffects(fxInstance(u"invert"_s))), (Px{0, 255, 0, 255}), 2);
        // two meshes sampling one visible layer with an effect, one before and one after it in the file: the layer is rendered
        // once in the frame and every reader (both meshes, the layer's own place in the scene) shows its finished picture
        Item item;
        meshItem(item, meshObject(2, "box", "30 50 0") + "," + layer(7, "plain", "10 10 0", "10 10", withEffects(fxInstance(u"invert"_s))) + ","
                     + meshObject(3, "box", "70 50 0"));
        item.image(u"plain"_s);
        item.tex(u"plain"_s, solid(255, 0, 255));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        item.file(u"materials/models/box.json"_s, R"({"passes":[{"shader":"meshshader","blending":"normal","textures":["_rt_imageLayerComposite_7_b"]}]})");
        const Frame f = render(item, 100, 100, nullptr, nullptr, 2);
        VERIFY_PX(f.at(30, 50), (Px{0, 255, 0, 255}), 2);
        VERIFY_PX(f.at(70, 50), (Px{0, 255, 0, 255}), 2); // (_b names the finished picture too)
        VERIFY_PX(f.at(10, 90), (Px{0, 255, 0, 255}), 2); // the layer in its own place
    }

    // `transparentsorting`: the opaque meshes first in file order, then the layers (and translucent meshes, particles) back to front
    // by the view depth of their origins; without it, file order.
    void transparentSortingDrawsTheFarthestFirst()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto centre = [&](bool sorting, bool nearFirst) {
            Item item;
            item.image(u"near"_s);
            item.tex(u"near"_s, solid(255, 0, 0));
            item.image(u"far"_s);
            item.tex(u"far"_s, solid(0, 0, 255));
            const QByteArray nearLayer = layer(1, "near", "50 50 10", "40 40"), farLayer = layer(2, "far", "50 50 -10", "40 40");
            item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0.2", nearFirst ? nearLayer + "," + farLayer : farLayer + "," + nearLayer,
                                                 sorting ? ",\"transparentsorting\":true" : ""));
            return render(item, 100, 100).at(50, 50);
        };
        // sorting: the near layer (z 10, nearer the eye looking down -z) is drawn last whatever the file says
        VERIFY_PX(centre(true, true), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(centre(true, false), (Px{255, 0, 0, 255}), 2);
        // without it the file's order: the last one wins
        VERIFY_PX(centre(false, true), (Px{0, 0, 255, 255}), 2);
        VERIFY_PX(centre(false, false), (Px{255, 0, 0, 255}), 2);
        // an opaque mesh goes first: a layer farther than the mesh and before it in the file is drawn after it, and covers it (layers
        // do not test depth); in file order the mesh would cover the layer
        Item unsorted;
        meshItem(unsorted, layer(1, "plain", "50 50 -40", "40 40") + "," + meshObject(2, "blue", "50 50 50"));
        unsorted.image(u"plain"_s);
        unsorted.tex(u"plain"_s, solid(0, 255, 255));
        VERIFY_PX(render(unsorted, 100, 100).at(50, 50), (Px{0, 0, 255, 255}), 2);
        Item item;
        meshItem(item, layer(1, "plain", "50 50 -40", "40 40") + "," + meshObject(2, "blue", "50 50 50"));
        item.image(u"plain"_s);
        item.tex(u"plain"_s, solid(0, 255, 255));
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0.2", layer(1, "plain", "50 50 -40", "40 40") + "," + meshObject(2, "blue", "50 50 50"),
                                             ",\"transparentsorting\":true"));
        VERIFY_PX(render(item, 100, 100).at(50, 50), (Px{0, 255, 255, 255}), 2);
    }

    // A solid layer: the `flat` shader, a colour and an alpha, no texture. The fixture carries its own flat shader.
    static void solidItem(Item &item, const QByteArray &objectExtra)
    {
        item.file(u"shaders/flat.vert"_s, "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec4 a_Color;\n#ifdef VERTEXCOLOR\nvarying vec4 v_Color;\n#endif\n"
                                          "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n#ifdef VERTEXCOLOR\n  v_Color = a_Color;\n#endif\n}\n");
        item.file(u"shaders/flat.frag"_s, "uniform float g_Alpha;\nuniform vec3 g_Color;\nvoid main() { gl_FragColor = vec4(g_Color, g_Alpha); }\n");
        item.file(u"materials/util/solidlayer.json"_s, R"({"passes":[{"shader":"flat","blending":"translucent","cullmode":"nocull"}]})");
        item.file(u"models/util/solidlayer.json"_s, R"({"material":"materials/util/solidlayer.json","solidlayer":true})");
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0.2",
                            "{\"id\":2,\"name\":\"solid\",\"image\":\"models/util/solidlayer.json\",\"origin\":\"50 50 0\",\"size\":\"40 40\"" + objectExtra + "}"));
    }

    void aSolidLayerIsItsColourOverItsSize()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        solidItem(item, ",\"color\":\"1 0 0\"");
        bool supported = false;
        const Frame f = render(item, 100, 100, &supported);
        QVERIFY(supported);
        VERIFY_PX(f.at(50, 50), (Px{255, 0, 0, 255}), 1);
        VERIFY_PX(f.at(5, 5), (Px{0, 0, 51, 255}), 1); // outside the 40 x 40: the clear colour
        Bounds b = litBounds(f);
        QVERIFY2(b.x0 >= 29 && b.x0 <= 31 && b.x1 >= 69 && b.x1 <= 71, qPrintable(QString("%1..%2").arg(b.x0).arg(b.x1)));
        // half alpha over the clear colour
        Item half;
        solidItem(half, ",\"color\":\"1 0 0\",\"alpha\":0.5");
        const Px mid = render(half, 100, 100).at(50, 50);
        QVERIFY2(mid[0] >= 125 && mid[0] <= 131, qPrintable(QString("%1").arg(mid[0])));
    }

    void userPropertiesMoveScaleAndSizeALayerAtRuntime()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto itemWith = [&](const QByteArray &extra) {
            auto item = std::make_unique<Item>();
            solidItem(*item, ",\"color\":\"1 0 0\"");
            item->file(u"scene.json"_s,
                       sceneText(100, 100, "0 0 0.2",
                                 "{\"id\":2,\"name\":\"solid\",\"image\":\"models/util/solidlayer.json\",\"color\":\"1 0 0\"," + extra + "}"));
            return item;
        };
        const auto boundsAfter = [&](Item &item, const char *key, const PropertyValue &value, int frames) {
            PropertyStore props;
            const Frame f = render(item, 100, 100, nullptr, nullptr, frames, bridge::msg::ScalingMode::Stretch, &props,
                                   [&](SceneRenderer &, int frame) {
                                       if (frame == 1)
                                           props.set(key, value);
                                   });
            return litBounds(f);
        };
        // scale: 40 wide at 1, 80 at "2 2 1", 20 at 0.5; set while running (frame 1), no reload
        auto scaled = itemWith("\"origin\":\"50 50 0\",\"size\":\"40 40\",\"scale\":{\"user\":\"s\",\"value\":\"1 1 1\"}");
        Bounds b = boundsAfter(*scaled, "unrelated", 1.0, 2);
        QCOMPARE(b.x1 - b.x0 + 1, 40);
        b = boundsAfter(*scaled, "s", std::string("2 2 1"), 2);
        QCOMPARE(b.x1 - b.x0 + 1, 80);
        b = boundsAfter(*scaled, "s", 0.5, 2);
        QCOMPARE(b.x1 - b.x0 + 1, 20);
        // origin
        auto moved = itemWith("\"origin\":{\"user\":\"o\",\"value\":\"50 50 0\"},\"size\":\"20 20\"");
        b = boundsAfter(*moved, "o", std::string("30 50 0"), 2);
        QVERIFY2(b.x0 >= 19 && b.x0 <= 21, qPrintable(QString::number(b.x0)));
        // size
        auto sized = itemWith("\"origin\":\"50 50 0\",\"size\":{\"user\":\"z\",\"value\":\"40 40\"}");
        b = boundsAfter(*sized, "z", std::string("60 20"), 2);
        QCOMPARE(b.x1 - b.x0 + 1, 60);
        QCOMPARE(b.y1 - b.y0 + 1, 20);
        // angles: a quarter turn makes a 60 x 20 bar upright (the file gives radians)
        auto turned = itemWith("\"origin\":\"50 50 0\",\"size\":\"60 20\",\"angles\":{\"user\":\"a\",\"value\":\"0 0 0\"}");
        b = boundsAfter(*turned, "a", std::string("0 0 1.5707963"), 2);
        QVERIFY2(b.x1 - b.x0 + 1 >= 19 && b.x1 - b.x0 + 1 <= 21, qPrintable(QString::number(b.x1 - b.x0 + 1)));
        QVERIFY2(b.y1 - b.y0 + 1 >= 59 && b.y1 - b.y0 + 1 <= 61, qPrintable(QString::number(b.y1 - b.y0 + 1)));
    }

    void cameraObjectsZoomAndMoveTheOrthographicView()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a 40 x 40 box at the centre of a 100 x 100 scene, and camera objects
        const auto itemWith = [&](const QByteArray &cameras) {
            auto item = std::make_unique<Item>();
            solidItem(*item, ",\"color\":\"1 0 0\"");
            item->file(u"scene.json"_s,
                       sceneText(100, 100, "0 0 0.2",
                                 "{\"id\":2,\"name\":\"solid\",\"image\":\"models/util/solidlayer.json\",\"color\":\"1 0 0\","
                                 "\"origin\":\"50 50 0\",\"size\":\"40 40\"}" + cameras));
            return item;
        };
        const auto bounds = [&](Item &item, PropertyStore *props = nullptr, const std::function<void(SceneRenderer &, int)> &between = {}) {
            return litBounds(render(item, 100, 100, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, props, between));
        };
        const auto width = [](const Bounds &b) { return b.x1 - b.x0 + 1; };
        // no camera object: 30..69
        auto none = itemWith({});
        QCOMPARE(width(bounds(*none)), 40);
        // zoom 2 shows half the width and height around the centre: the box is twice as wide, still centred
        auto zoomed = itemWith(R"(,{"id":3,"name":"cam","camera":"default","zoom":2,"origin":"0 0 500"})");
        Bounds b = bounds(*zoomed);
        QCOMPARE(width(b), 80);
        QVERIFY2(b.x0 >= 9 && b.x0 <= 11, qPrintable(QString::number(b.x0)));
        // zoom 0.5 shows twice as much
        auto out = itemWith(R"(,{"id":3,"name":"cam","camera":"default","zoom":0.5,"origin":"0 0 500"})");
        QCOMPARE(width(bounds(*out)), 20);
        // an origin offset moves the view's centre: the picture shifts the other way (10 to the right of the centre: 10 left)
        auto offset = itemWith(R"(,{"id":3,"name":"cam","camera":"default","origin":"10 0 500"})");
        b = bounds(*offset);
        QCOMPARE(width(b), 40);
        QVERIFY2(b.x0 >= 19 && b.x0 <= 21, qPrintable(QString::number(b.x0)));
        // a zoom bound to a user property follows it while running
        auto bound = itemWith(R"(,{"id":3,"name":"cam","camera":"default","zoom":{"user":"lens","value":1},"origin":"0 0 500"})");
        PropertyStore props;
        b = bounds(*bound, &props, [&](SceneRenderer &, int frame) {
            if (frame == 1)
                props.set("lens", 2.0);
        });
        QCOMPARE(width(b), 80);
        // a scripted origin whose script properties are bound to user properties (the library's lens position sliders): the
        // property's value at the start, the file's value when it was never set
        auto scripted = itemWith(R"(,{"id":3,"name":"cam","camera":"default","origin":{"value":"0 0 500","script":"export var scriptProperties = createScriptProperties().addSlider({name: 'x', value: 0.5, min: -1, max: 1}).finish(); export function update(v) { v.x = scriptProperties.x * 100; return v; }","scriptproperties":{"x":{"user":"lensx","value":0.5}}}})");
        PropertyStore lens;
        lens.set("lensx", 0.1);
        b = bounds(*scripted, &lens);
        QVERIFY2(b.x0 >= 19 && b.x0 <= 21, qPrintable(QString::number(b.x0))); // offset 10: 30 - 10
        b = bounds(*scripted, &lens, [&](SceneRenderer &, int frame) { // and live: the property changes while it runs
            if (frame == 1)
                lens.set("lensx", 0.2);
        });
        QVERIFY2(b.x0 >= 9 && b.x0 <= 11, qPrintable(QString::number(b.x0))); // offset 20: 30 - 20
        b = bounds(*scripted);
        QVERIFY2(b.x0 <= 0, qPrintable(QString::number(b.x0))); // offset 50: 30 - 50, the box is cut at the edge
        // two cameras switched by a user property: the first visible one counts
        auto two = itemWith(R"(,{"id":3,"name":"a","camera":"default","zoom":2,"origin":"0 0 500","visible":{"user":{"name":"mode","condition":"1"},"value":false}})"
                            R"(,{"id":4,"name":"b","camera":"default","zoom":0.5,"origin":"0 0 500","visible":{"user":{"name":"mode","condition":"2"},"value":false}})");
        PropertyStore first;
        first.set("mode", 1.0);
        QCOMPARE(width(bounds(*two, &first)), 80);
        PropertyStore second;
        second.set("mode", 2.0);
        QCOMPARE(width(bounds(*two, &second)), 20);
        PropertyStore switching;
        switching.set("mode", 1.0);
        b = bounds(*two, &switching, [&](SceneRenderer &, int frame) {
            if (frame == 1)
                switching.set("mode", 2.0);
        });
        QCOMPARE(width(b), 20);
    }

    void zoomedCameraObjectsKeepBlendModesAndFullscreenLayersOnTheirPixels()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const QByteArray zoom2 = R"(,{"id":9,"name":"cam","camera":"default","zoom":2,"origin":"0 0 500"})";
        // a colour blend mode: the pass finds the scene under the layer from the layer's screen position
        Item item;
        item.image(u"red"_s);
        item.image(u"grey"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.tex(u"grey"_s, solid(128, 128, 128));
        item.file(u"shaders/genericimage3.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenPos;\n"
                  "void main() {\n    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n    v_TexCoord = a_TexCoord;\n"
                  "    v_ScreenPos = gl_Position.xyw;\n#ifdef HLSL\n    v_ScreenPos.y = -v_ScreenPos.y;\n#endif\n}\n");
        item.file(u"shaders/genericimage3.frag"_s,
                  "// [COMBO] {\"material\":\"blend\",\"combo\":\"BLENDMODE\",\"default\":0}\n"
                  "uniform sampler2D g_Texture0;\nuniform vec4 g_Color4;\nuniform sampler2D g_Texture4;\n"
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenPos;\n"
                  "void main() {\n    vec4 color = texSample2D(g_Texture0, v_TexCoord) * g_Color4;\n"
                  "    vec2 screenUV = (v_ScreenPos.xy / v_ScreenPos.z) * 0.5 + 0.5;\n"
                  "    vec4 screen = texSample2D(g_Texture4, screenUV);\n"
                  "#if BLENDMODE == 2\n    color.rgb = screen.rgb * color.rgb;\n#endif\n"
                  "    gl_FragColor = vec4(color.rgb, screen.a);\n}\n");
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                                + "," + layer(2, "grey", "20 20 0", "40 40", ",\"colorBlendMode\":2") + zoom2));
        // the view is scene x 10..30, y 10..30: the red square (scene 0..20) fills the lower left quarter of the picture
        const Frame multiply = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(multiply.at(5, 30), (Px{128, 0, 0, 255}), 3);
        VERIFY_PX(multiply.at(15, 25), (Px{128, 0, 0, 255}), 3);
        VERIFY_PX(multiply.at(25, 30), (Px{0, 0, 26, 255}), 3);
        VERIFY_PX(multiply.at(5, 10), (Px{0, 0, 26, 255}), 3);
        // a fullscreen layer is the whole viewport and starts from the zoomed picture
        Item fs;
        fs.image(u"red"_s);
        fs.tex(u"red"_s, solid(255, 0, 0));
        fs.file(u"shaders/passthrough.vert"_s, kPassVert);
        fs.file(u"shaders/passthrough.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
        fs.file(u"models/util/fs.json"_s, R"({"material":"materials/util/fs.json","fullscreen":true,"passthrough":true})");
        fs.file(u"materials/util/fs.json"_s, R"({"passes":[{"shader":"passthrough","blending":"translucent","textures":["_rt_FullFrameBuffer"]}]})");
        addEffect(fs, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        fs.file(u"scene.json"_s,
                sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                              + ",{\"id\":2,\"name\":\"fs\",\"image\":\"models/util/fs.json\"" + withEffects(fxInstance(u"invert"_s)) + "}" + zoom2));
        const Frame f = render(fs, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(f.at(5, 30), (Px{0, 255, 255, 255}), 2);    // the red inverted, on the zoomed square
        VERIFY_PX(f.at(30, 30), (Px{255, 255, 204, 255}), 2); // the clear colour inverted, beside it
        VERIFY_PX(f.at(5, 10), (Px{255, 255, 204, 255}), 2);
        int inverted = 0;
        for (uint32_t y = 0; y < 40; ++y)
            for (uint32_t x = 0; x < 40; ++x)
                inverted += f.at(x, y)[0] < 50;
        QCOMPARE(inverted, 400); // the square's 20 x 20 pixels, once
    }

    void scriptsSeeTheCursorInTheZoomedView()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the 40 x 40 solid layer is scene 30..70; the camera zoomed by 2 and moved 10 right shows scene x 35..85, y 25..75
        const auto hovered = [&](int x, int y) {
            Item item;
            pointerItem(item);
            item.file(u"scene.json"_s,
                      sceneText(100, 100, "0 0 0.2", "{\"id\":2,\"name\":\"solid\",\"image\":\"models/util/solidlayer.json\",\"origin\":\"50 50 0\",\"size\":\"40 40\","
                                "\"solid\":true,\"color\":{\"value\":\"1 0 0\",\"script\":\"export function cursorEnter(e) { thisLayer.color = new Vec3(0, 1, 0); }"
                                " export function cursorLeave(e) { thisLayer.color = new Vec3(1, 0, 0); }\"}},"
                                "{\"id\":3,\"name\":\"cam\",\"camera\":\"default\",\"zoom\":2,\"origin\":\"10 0 500\"}"));
            const Frame f = render(item, 100, 100, nullptr, nullptr, 3, bridge::msg::ScalingMode::Stretch, nullptr,
                                   [&](SceneRenderer &r, int frame) {
                                       if (frame > 0)
                                           r.setPointer(x, y, 0, {100, 100});
                                   });
            return f.at(30, 50); // scene (50, 50): the layer's colour
        };
        VERIFY_PX(hovered(5, 50), (Px{0, 255, 0, 255}), 1);   // scene x 37.5: over the layer (unmapped it would be 5, outside)
        VERIFY_PX(hovered(60, 50), (Px{0, 255, 0, 255}), 1);  // scene 65
        VERIFY_PX(hovered(90, 50), (Px{255, 0, 0, 255}), 1);  // scene 80: outside
        VERIFY_PX(hovered(5, 5), (Px{255, 0, 0, 255}), 1);    // scene y 72.5: above the layer
    }

    void aRelativeCameraIntroEndsCentred()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        solidItem(item, ",\"color\":\"1 0 0\"");
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0.2",
                            "{\"id\":2,\"name\":\"solid\",\"image\":\"models/util/solidlayer.json\",\"origin\":\"50 50 0\",\"size\":\"40 40\",\"color\":\"1 0 0\"},"
                            // zoom 3 -> 1 over 36 frames at 12 fps; the relative origin starts 20 left of centre and ends at base + 20 = centred
                            "{\"id\":3,\"camera\":\"default\",\"zoom\":{\"value\":3,\"animation\":{\"c0\":[{\"frame\":0,\"value\":3},{\"frame\":36,\"value\":1}],"
                            "\"options\":{\"fps\":12,\"length\":60,\"mode\":\"single\"}}},"
                            "\"origin\":{\"value\":\"-20 30 500\",\"animation\":{\"relative\":true,\"c0\":[{\"frame\":0,\"value\":0},{\"frame\":36,\"value\":20}],"
                            "\"c1\":[{\"frame\":0,\"value\":0},{\"frame\":36,\"value\":-30}],\"options\":{\"fps\":12,\"length\":60,\"mode\":\"single\",\"parent\":{\"key\":\"zoom\"}}}}}"));
        m_frameStep = 2.0; // frames at 0, 2 and 4 s
        const Bounds end = litBounds(render(item, 100, 100, nullptr, nullptr, 3));
        QCOMPARE(end.x1 - end.x0 + 1, 40); // zoom 1
        QVERIFY2(end.x0 >= 29 && end.x0 <= 31, qPrintable(QString::number(end.x0))); // centred
        QVERIFY2(end.y0 >= 29 && end.y0 <= 31, qPrintable(QString::number(end.y0)));
        // at the start the view is zoomed in and moved left of the box
        m_frameStep = 0.25;
        const Bounds start = litBounds(render(item, 100, 100, nullptr, nullptr, 1));
        QVERIFY(start.x0 >= 45 && start.x1 - start.x0 + 1 > 45 && start.x1 - start.x0 + 1 < 60); // view x 13..47 shows the box from 30 on the right half, 17 units at zoom 3
    }

    void aKeyframedAlphaFadesWithTheSceneClock()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // 30 fps, 30 frames: 1 s from opaque to clear (single: it then holds the last value)
        Item item;
        solidItem(item, ",\"color\":\"1 0 0\",\"alpha\":{\"value\":1,\"animation\":{\"c0\":[{\"frame\":0,\"value\":1},{\"frame\":30,\"value\":0}],"
                        "\"options\":{\"fps\":30,\"length\":30,\"mode\":\"single\"}}}");
        m_frameStep = 0.5; // frames at 0 s and 0.5 s
        const Px start = render(item, 100, 100, nullptr, nullptr, 1).at(50, 50);
        const Px half = render(item, 100, 100, nullptr, nullptr, 2).at(50, 50);
        m_frameStep = 2.0;
        const Px end = render(item, 100, 100, nullptr, nullptr, 2).at(50, 50);
        QVERIFY2(start[0] >= 250, qPrintable(QString("%1").arg(start[0])));
        QVERIFY2(half[0] >= 120 && half[0] <= 135, qPrintable(QString("%1").arg(half[0])));
        QVERIFY2(end[0] <= 3, qPrintable(QString("%1").arg(end[0])));
        // an animated origin moves the layer: 40 units to the right over the same second
        Item moving;
        solidItem(moving, ",\"color\":\"1 0 0\"");
        moving.file(u"scene.json"_s,
                    sceneText(100, 100, "0 0 0.2",
                              "{\"id\":2,\"name\":\"solid\",\"image\":\"models/util/solidlayer.json\",\"size\":\"40 40\",\"color\":\"1 0 0\","
                              "\"origin\":{\"value\":\"30 50 0\",\"animation\":{\"c0\":[{\"frame\":0,\"value\":30},{\"frame\":30,\"value\":70}],"
                              "\"c1\":[{\"frame\":0,\"value\":50}],\"c2\":[{\"frame\":0,\"value\":0}],"
                              "\"options\":{\"fps\":30,\"length\":30,\"mode\":\"single\"}}}}"));
        m_frameStep = 2.0;
        const Bounds after = litBounds(render(moving, 100, 100, nullptr, nullptr, 2));
        QVERIFY2(after.x0 >= 49 && after.x0 <= 51, qPrintable(QString("%1..%2").arg(after.x0).arg(after.x1))); // 70 - 20
    }

    void aPuppetWithEffectsDrawsIntoItsBufferAndStaysWhereItIs()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        using namespace puppetfix;
        const auto mdl = makeMdl({{"root", -1, 0, 0}}, {});
        Item item;
        puppetItem(item, mdl, "", withEffects(fxInstance(u"invert"_s)));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        const Frame f = render(item, 100, 100, nullptr, nullptr, 2);
        // the layer's buffer is 40 x 40 around the origin; the quad inside it is 20 x 20: inverted to cyan on the red's place,
        // and the buffer's transparent surround stays transparent (the effect inverts colour only)
        VERIFY_PX(f.at(50, 50), (Px{0, 255, 255, 255}), 2);
        VERIFY_PX(f.at(45, 45), (Px{0, 255, 255, 255}), 2);
        VERIFY_PX(f.at(35, 50), (Px{0, 0, 51, 255}), 3);
        VERIFY_PX(f.at(50, 70), (Px{0, 0, 51, 255}), 3);
    }

    void aPuppetPieceThatIsTransparentWhereItCoversAnotherLeavesItAloneInItsBuffer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        using namespace puppetfix;
        // two quads, the second over the first and transparent all over (u = 1): the pieces of a character overlap by their
        // rectangles. Straight into the scene the second adds nothing; into the buffer an effect needs it must add nothing either
        const auto mdl = makeMdl({{"root", -1, 0, 0}}, {}, false, "MDLV0023", "MDLS0004", "MDLA0006", false, true);
        Item item;
        puppetItem(item, mdl, "", withEffects(fxInstance(u"invert"_s)));
        item.file(u"shaders/puppetshader.frag"_s,
                  "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = vec4(1.0, 0.0, 0.0, v_TexCoord.x < 0.5 ? 1.0 : 0.0); }\n");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        const Frame f = render(item, 100, 100, nullptr, nullptr, 2);
        VERIFY_PX(f.at(50, 50), (Px{0, 255, 255, 255}), 2);
        VERIFY_PX(f.at(45, 45), (Px{0, 255, 255, 255}), 2);
    }

    void aPuppetsTextureCoordinatesRunOverThePictureNotThePaddedFile()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        using namespace puppetfix;
        // the file is 8 x 8 with a 4 x 4 picture in its corner (red; the padding is green): the mesh's u and v of 0..1 are the picture's,
        // so the whole quad is red. Read as the file's, its right half would show the padding.
        const auto mdl = makeMdl({{"root", -1, 0, 0}}, {});
        Item item;
        puppetItem(item, mdl, "");
        TexSpec padded;
        padded.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        padded.texW = padded.texH = 8;
        padded.imgW = padded.imgH = 4;
        Bytes pixels;
        for (uint32_t y = 0; y < 8; ++y) {
            for (uint32_t x = 0; x < 8; ++x) {
                const bool picture = x < 4 && y < 4;
                for (const uint8_t c : {picture ? 255 : 0, picture ? 0 : 255, 0, 255}) {
                    pixels.push_back(c);
                }
            }
        }
        padded.images = {{MipSpec{8, 8, pixels, false}}};
        item.tex(u"red"_s, padded);
        item.file(u"shaders/puppetshader.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
        const Frame f = render(item, 100, 100, nullptr, nullptr, 2);
        VERIFY_PX(f.at(43, 50), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(57, 50), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(57, 43), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(43, 57), (Px{255, 0, 0, 255}), 2);
    }

    void aPuppetDeepInZStillDrawsIntoItsBuffer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        using namespace puppetfix;
        // the pieces of a character are layered in z (the library's ones run over +-25): the buffer's projection must not clip them
        for (const float depth : {-5.0f, 25.0f}) {
            const auto mdl = makeMdl({{"root", -1, 0, 0}}, {}, false, "MDLV0023", "MDLS0004", "MDLA0006", false, false, depth);
            Item item;
            puppetItem(item, mdl, "", withEffects(fxInstance(u"invert"_s)));
            addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
            const Frame f = render(item, 100, 100, nullptr, nullptr, 2);
            VERIFY_PX(f.at(50, 50), (Px{0, 255, 255, 255}), 2);
        }
    }

    void aSkinnedPuppetLayerIsNeverCachedAndAStillLayerStillIs()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        using namespace puppetfix;
        // the animation holds the root bone 20 units to the right; the puppet has an effect, so it draws through its buffers
        const auto mdl = makeMdl({{"root", -1, 0, 0}}, {{7, "slide", "loop", 30.0f, 1, {{{frame(20, 0), frame(20, 0)}}}}});
        Item item;
        puppetItem(item, mdl, ",\"animationlayers\":[{\"animation\":7}]", withEffects(fxInstance(u"invert"_s)));
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addEffect(item, u"clock"_s,
                  "uniform sampler2D g_Texture0;\nuniform float g_Time;\nvarying vec2 v_TexCoord;\n"
                  "void main() { gl_FragColor = vec4(fract(g_Time), 0.0, 0.0, 1.0); }\n",
                  simpleEffect(u"clock"_s));
        // a still layer with an effect (id 1), the skinned puppet (id 2), a layer that reads the clock (id 3, keeps the scene drawing)
        const QByteArray puppet = "{\"id\":2,\"name\":\"pup\",\"image\":\"models/pup.json\",\"origin\":\"50 50 0\",\"size\":\"40 40\",\"animationlayers\":[{\"animation\":7}]"
            + withEffects(fxInstance(u"invert"_s)) + "}";
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0.2",
                            layer(1, "w", "20 80 0", "20 20", withEffects(fxInstance(u"invert"_s))) + "," + puppet + ","
                                + layer(3, "w", "80 80 0", "20 20", withEffects(fxInstance(u"clock"_s)))));
        uint64_t hits = 0;
        const Frame f = render(item, 100, 100, nullptr, nullptr, 5, bridge::msg::ScalingMode::Stretch, nullptr,
                               [&](SceneRenderer &r, int) { hits = r.layerCacheHits(); });
        // hits are read before the fifth frame is drawn: four frames reused the still layer's buffer (3) and the clock layer's
        // still base picture (3); a puppet that was cached would add 3 more
        QCOMPARE(hits, uint64_t{6});
        VERIFY_PX(f.at(65, 50), (Px{0, 255, 255, 255}), 2); // the puppet slid 20 right (the buffer ends at 70), inverted
        VERIFY_PX(f.at(20, 20), (Px{0, 255, 255, 255}), 2); // the still layer (scene y 80 is row 20), inverted
    }

    void aWorkshopScriptFindsItsModelUnderTheWorkshopFolder()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"workshop/123/w"_s);
        item.tex(u"workshop/123/w"_s, solid(255, 255, 255));
        // the script says models/w.json; the package has models/workshop/123/w.json
        const QByteArray make = R"("script":"export let __workshopId = '123';\nexport function init(v) { const l = thisScene.createLayer('models/w.json'); l.origin = new Vec3(30, 30, 0); l.size = new Vec2(20, 20); }")";
        item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", "{\"id\":3,\"name\":\"maker\",\"origin\":\"0 0 0\",\"visible\":{" + make + ",\"value\":true}}"));
        const Frame f = render(item, 100, 100, nullptr, nullptr, 2);
        VERIFY_PX(f.at(30, 70), (Px{255, 255, 255, 255}), 2); // scene y 30 is row 70
        // and the same scene refuses nothing at --check time: the model resolved
    }

    void aScriptCannotFloodTheSceneWithLayersOrText()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        if (!textSupported() || systemFontBytes("systemfont_sans").empty())
            QSKIP("no text support or no font");
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // a thousand image layers and a thousand text layers in init, and a text of a megabyte every frame
        const QByteArray flood = R"("script":"export function init(v) { for (let i = 0; i < 1000; ++i) { thisScene.createLayer('models/w.json'); thisScene.createLayer({text: 'T' + i, pointsize: 4, font: 'systemfont_sans'}); } }")";
        const QByteArray huge = R"("script":"export function update(v) { return 'word '.repeat(200000); }")";
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0",
                            layer(1, "w", "10 10 0", "20 20")
                            + ",{\"id\":3,\"name\":\"flood\",\"origin\":\"0 0 0\",\"visible\":{" + flood + ",\"value\":true}}"
                            + ",{\"id\":4,\"name\":\"big\",\"text\":{" + huge + ",\"value\":\"x\"},\"font\":\"systemfont_sans\",\"origin\":\"50 50 0\",\"pointsize\":8}"));
        QElapsedTimer timer;
        timer.start();
        const Frame f = render(item, 100, 100, nullptr, nullptr, 3);
        QCOMPARE(f.width, 100u);
        QVERIFY2(timer.elapsed() < 30000, qPrintable(QString::number(timer.elapsed())));
        VERIFY_PX(f.at(10, 90), (Px{255, 255, 255, 255}), 2); // the real layer still draws
    }

    void aParticleRateFollowsAScript()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        qputenv("MURASAKI_SCENE_VK_SEED", "4");
        const auto lit = [&](const char *rate) {
            Item item;
            particleItem(item, R"("maxcount":400,"emitter":[{"name":"sphererandom","rate":300,"distancemax":30}],"initializer":[{"name":"lifetimerandom","min":50,"max":50},{"name":"sizerandom","min":10,"max":10}])",
                         std::string(",\"instanceoverride\":{\"rate\":{\"script\":\"export function update(v) { return ").append(rate).append("; }\",\"value\":1}}").c_str());
            return litBounds(render(item, 200, 200, nullptr, nullptr, 8)).lit;
        };
        const int normal = lit("1");
        const int none = lit("0");
        QVERIFY2(normal > 100, qPrintable(QString::number(normal)));
        QVERIFY2(none == 0, qPrintable(QString::number(none)));
        qunsetenv("MURASAKI_SCENE_VK_SEED");
    }

    void fullscreenLayersStartFromTheSceneDrawnSoFar()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.file(u"shaders/passthrough.vert"_s, kPassVert);
        item.file(u"shaders/passthrough.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
        item.file(u"models/util/fs.json"_s, R"({"material":"materials/util/fs.json","fullscreen":true,"passthrough":true})");
        item.file(u"materials/util/fs.json"_s, R"({"passes":[{"shader":"passthrough","blending":"translucent","textures":["_rt_FullFrameBuffer"]}]})");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        // a red square on the left half under a fullscreen layer with an effect: the effect sees the square and the clear colour
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 20 0", "20 40")
                                + ",{\"id\":2,\"name\":\"fs\",\"image\":\"models/util/fs.json\",\"origin\":\"5 5 0\"" + withEffects(fxInstance(u"invert"_s)) + "}"));
        const Frame f = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(f.at(5, 20), (Px{0, 255, 255, 255}), 2);   // red inverted
        VERIFY_PX(f.at(30, 20), (Px{255, 255, 204, 255}), 2); // the clear colour inverted: (0, 0, 0.2) -> (1, 1, 0.8)
        // without the fullscreen layer the picture is as drawn
        item.file(u"scene.json"_s, sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 20 0", "20 40")));
        VERIFY_PX(render(item, 40, 40).at(5, 20), (Px{255, 0, 0, 255}), 2);
    }

    void colourBlendModeCombinesTheLayerWithTheSceneUnderneath()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.image(u"grey"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.tex(u"grey"_s, solid(128, 128, 128));
        // the blend pass is WE's genericimage3 with a BLENDMODE combo; a small stand-in with the same screen position and
        // g_Texture4 conventions (the screen position flips on HLSL, as the shipped vertex shader does)
        item.file(u"shaders/genericimage3.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\n"
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenPos;\n"
                  "void main() {\n    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n    v_TexCoord = a_TexCoord;\n"
                  "    v_ScreenPos = gl_Position.xyw;\n#ifdef HLSL\n    v_ScreenPos.y = -v_ScreenPos.y;\n#endif\n}\n");
        item.file(u"shaders/genericimage3.frag"_s,
                  "// [COMBO] {\"material\":\"blend\",\"combo\":\"BLENDMODE\",\"default\":0}\n"
                  "uniform sampler2D g_Texture0;\nuniform vec4 g_Color4;\nuniform sampler2D g_Texture4;\n"
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenPos;\n"
                  "void main() {\n    vec4 color = texSample2D(g_Texture0, v_TexCoord) * g_Color4;\n"
                  "    vec2 screenUV = (v_ScreenPos.xy / v_ScreenPos.z) * 0.5 + 0.5;\n"
                  "    vec4 screen = texSample2D(g_Texture4, screenUV);\n"
                  "#if BLENDMODE == 2\n    color.rgb = screen.rgb * color.rgb;\n#endif\n"
                  "#if BLENDMODE == 9\n    color.rgb = min(screen.rgb + color.rgb, vec3(1.0));\n#endif\n"
                  "    gl_FragColor = vec4(color.rgb, screen.a);\n}\n");
        const auto scene = [&](int mode) {
            item.file(u"scene.json"_s,
                      sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                                    + "," + layer(2, "grey", "20 20 0", "40 40", QByteArray(",\"colorBlendMode\":") + QByteArray::number(mode))));
            return render(item, 40, 40, nullptr, nullptr, 2);
        };
        // multiply (2): red x grey, and the clear colour x grey; the layer's own colour never shows
        const Frame multiply = scene(2);
        // (the red square sits in the lower-left quarter: scene y runs up, the picture's rows run down)
        VERIFY_PX(multiply.at(5, 30), (Px{128, 0, 0, 255}), 3);
        VERIFY_PX(multiply.at(5, 10), (Px{0, 0, 26, 255}), 3);
        VERIFY_PX(multiply.at(30, 30), (Px{0, 0, 26, 255}), 3);
        // add (9): the grey lifts every channel
        const Frame add = scene(9);
        VERIFY_PX(add.at(5, 30), (Px{255, 128, 128, 255}), 3);
        VERIFY_PX(add.at(30, 10), (Px{128, 128, 179, 255}), 3);
    }

    void aFullscreenLayerWithWEsPlainPassthroughCoversTheWholeScene()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        // the shipped passthrough: without TRANSFORM its vertices are clip positions, not scene ones
        item.file(u"shaders/passthrough.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                  "void main() {\n#ifdef TRANSFORM\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n#else\n  gl_Position = vec4(a_Position, 1.0);\n#endif\n"
                  "  v_TexCoord.xy = a_TexCoord;\n}\n");
        item.file(u"shaders/passthrough.frag"_s, "uniform sampler2D g_Texture0;\nvarying vec2 v_TexCoord;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord); }\n");
        item.file(u"models/util/fs.json"_s, R"({"material":"materials/util/fs.json","fullscreen":true,"passthrough":true})");
        item.file(u"materials/util/fs.json"_s, R"({"passes":[{"shader":"passthrough","blending":"translucent","textures":["_rt_FullFrameBuffer"]}]})");
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20") + ",{\"id\":2,\"name\":\"fs\",\"image\":\"models/util/fs.json\"}"));
        const Frame f = render(item, 40, 40, nullptr, nullptr, 2);
        // the square is in the lower left once (rows 20..40) and the layer added no copy of the scene in a corner
        VERIFY_PX(f.at(5, 30), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(35, 5), (Px{0, 0, 51, 255}), 3);   // top right: the clear colour, not a shrunken copy
        VERIFY_PX(f.at(25, 10), (Px{0, 0, 51, 255}), 3);
        VERIFY_PX(f.at(35, 30), (Px{0, 0, 51, 255}), 3);
        int reds = 0;
        for (uint32_t y = 0; y < 40; ++y)
            for (uint32_t x = 0; x < 40; ++x)
                reds += f.at(x, y)[0] > 200;
        QCOMPARE(reds, 400); // exactly the one 20 x 20 square
    }

    void aComposeLayerShowsTheSceneBehindItsOwnQuadWithItsEffects()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        // WE's composelayer shader: the quad comes from the texture coordinates, the picture from the layer's screen position
        item.file(u"shaders/composelayer.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvarying vec3 v_ScreenCoord;\n"
                  "void main() {\n  v_ScreenCoord = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix).xyw;\n  vec3 position = vec3(a_TexCoord, 0.0);\n"
                  "#ifdef HLSL\n  position.y = 1.0 - position.y;\n  v_ScreenCoord.y = -v_ScreenCoord.y;\n#endif\n"
                  "  position.xy = position.xy * 2.0 - 1.0;\n  gl_Position = vec4(position, 1.0);\n  v_TexCoord = a_TexCoord;\n}\n");
        item.file(u"shaders/composelayer.frag"_s,
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenCoord;\nuniform sampler2D g_Texture0;\n"
                  "void main() {\n  vec2 texCoord = v_ScreenCoord.xy / v_ScreenCoord.z * vec2(0.5, 0.5) + 0.5;\n  gl_FragColor = texSample2D(g_Texture0, texCoord);\n}\n");
        item.file(u"models/util/compose.json"_s, R"({"material":"materials/util/compose.json","passthrough":true})");
        item.file(u"materials/util/compose.json"_s,
                  R"({"passes":[{"shader":"composelayer","blending":"translucent","textures":["_rt_FullFrameBuffer"]}]})");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        // a red square over the lower left quarter of a 40 x 40 scene; the compose layer is 10 x 10 around (10, 10), so it
        // inverts the part of the square it covers (rows 25..35, columns 5..15) and nothing else
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                                + ",{\"id\":2,\"name\":\"c\",\"image\":\"models/util/compose.json\",\"origin\":\"10 10 0\",\"size\":\"10 10\"" + withEffects(fxInstance(u"invert"_s)) + "}"));
        const Frame f = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(f.at(10, 30), (Px{0, 255, 255, 255}), 2); // inside the compose layer: the red, inverted
        VERIFY_PX(f.at(17, 30), (Px{255, 0, 0, 255}), 2);   // red square outside it: untouched
        VERIFY_PX(f.at(10, 22), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(30, 10), (Px{0, 0, 51, 255}), 3);    // and no copy of the scene anywhere else
        VERIFY_PX(f.at(30, 30), (Px{0, 0, 51, 255}), 3);
    }

    void aMirroredComposeLayerStillShowsTheSceneWhereItIs()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        // WE's composelayer shader: the quad comes from the texture coordinates, the picture from the layer's screen position
        item.file(u"shaders/composelayer.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvarying vec3 v_ScreenCoord;\n"
                  "void main() {\n  v_ScreenCoord = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix).xyw;\n  vec3 position = vec3(a_TexCoord, 0.0);\n"
                  "#ifdef HLSL\n  position.y = 1.0 - position.y;\n  v_ScreenCoord.y = -v_ScreenCoord.y;\n#endif\n"
                  "  position.xy = position.xy * 2.0 - 1.0;\n  gl_Position = vec4(position, 1.0);\n  v_TexCoord = a_TexCoord;\n}\n");
        item.file(u"shaders/composelayer.frag"_s,
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenCoord;\nuniform sampler2D g_Texture0;\n"
                  "void main() {\n  vec2 texCoord = v_ScreenCoord.xy / v_ScreenCoord.z * vec2(0.5, 0.5) + 0.5;\n  gl_FragColor = texSample2D(g_Texture0, texCoord);\n}\n");
        item.file(u"models/util/compose.json"_s, R"({"material":"materials/util/compose.json","passthrough":true})");
        item.file(u"materials/util/compose.json"_s,
                  R"({"passes":[{"shader":"composelayer","blending":"translucent","textures":["_rt_FullFrameBuffer"]}]})");
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        // a red square over the lower left quarter of a 40 x 40 scene; the compose layer is 10 x 10 around (10, 10), so it
        // inverts the part of the square it covers (rows 25..35, columns 5..15) and nothing else
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                                + ",{\"id\":2,\"name\":\"c\",\"image\":\"models/util/compose.json\",\"origin\":\"10 10 0\",\"size\":\"10 10\",\"scale\":\"-1 -1 1\"" + withEffects(fxInstance(u"invert"_s)) + "}"));
        const Frame f = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(f.at(10, 30), (Px{0, 255, 255, 255}), 2); // inside the compose layer: the red, inverted
        VERIFY_PX(f.at(17, 30), (Px{255, 0, 0, 255}), 2);   // red square outside it: untouched
        VERIFY_PX(f.at(10, 22), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(30, 10), (Px{0, 0, 51, 255}), 3);    // and no copy of the scene anywhere else
        VERIFY_PX(f.at(30, 30), (Px{0, 0, 51, 255}), 3);
    }

    void aComposeLayerWithoutEffectsLeavesTheSceneAsItIs()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"red"_s);
        item.tex(u"red"_s, solid(255, 0, 0));
        item.file(u"shaders/composelayer.vert"_s,
                  "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\nvarying vec3 v_ScreenCoord;\n"
                  "void main() {\n  v_ScreenCoord = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix).xyw;\n  vec3 position = vec3(a_TexCoord, 0.0);\n"
                  "#ifdef HLSL\n  position.y = 1.0 - position.y;\n  v_ScreenCoord.y = -v_ScreenCoord.y;\n#endif\n"
                  "  position.xy = position.xy * 2.0 - 1.0;\n  gl_Position = vec4(position, 1.0);\n  v_TexCoord = a_TexCoord;\n}\n");
        item.file(u"shaders/composelayer.frag"_s,
                  "varying vec2 v_TexCoord;\nvarying vec3 v_ScreenCoord;\nuniform sampler2D g_Texture0;\n"
                  "void main() {\n  vec2 texCoord = v_ScreenCoord.xy / v_ScreenCoord.z * vec2(0.5, 0.5) + 0.5;\n  gl_FragColor = texSample2D(g_Texture0, texCoord);\n}\n");
        item.file(u"models/util/compose.json"_s, R"({"material":"materials/util/compose.json","passthrough":true})");
        item.file(u"materials/util/compose.json"_s,
                  R"({"passes":[{"shader":"composelayer","blending":"translucent","textures":["_rt_FullFrameBuffer"]}]})");
        // the project layers some scenes end with: a whole-scene sized compose layer, off to the side, with nothing on it. Drawn
        // straight into the scene, its shader would paint the quad it writes from the texture coordinates (the whole viewport)
        // with the scene sampled at the layer's own, shifted, position
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                                + ",{\"id\":2,\"name\":\"c\",\"image\":\"models/util/compose.json\",\"origin\":\"50 20 0\",\"size\":\"40 40\"}"));
        const Frame f = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(f.at(10, 30), (Px{255, 0, 0, 255}), 2);
        VERIFY_PX(f.at(30, 10), (Px{0, 0, 51, 255}), 3);
        VERIFY_PX(f.at(30, 30), (Px{0, 0, 51, 255}), 3);
        VERIFY_PX(f.at(2, 2), (Px{0, 0, 51, 255}), 3);
        // a tinted one is not nothing: it still goes through its own buffer and shows the tint where it sits (rows 25..35)
        item.file(u"scene.json"_s,
                  sceneText(40, 40, "0 0 0.2", layer(1, "red", "10 10 0", "20 20")
                                + ",{\"id\":2,\"name\":\"c\",\"image\":\"models/util/compose.json\",\"origin\":\"10 10 0\",\"size\":\"10 10\",\"color\":\"0 1 0\"}"));
        const Frame g = render(item, 40, 40, nullptr, nullptr, 2);
        VERIFY_PX(g.at(10, 30), (Px{0, 0, 0, 255}), 3);
        VERIFY_PX(g.at(17, 30), (Px{255, 0, 0, 255}), 2);
    }

    void perspectiveScenesAreSeenThroughTheCamera()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a perspective scene (no orthogonal projection) of 2 x 2 squares; fov 50 on a 16:9 picture of 160 x 90: at distance d the
        // picture is 2 d tan 25 = 0.9326 d high, so a square 10 away is 90 * 2 / 9.326 = 19.3 pixels high and wide
        const auto itemWith = [&](const QByteArray &camera, const QByteArray &objects, bool *supported = nullptr) {
            auto item = std::make_unique<Item>();
            solidItem(*item, {});
            item->file(u"scene.json"_s, "{\"camera\":" + camera + ",\"general\":{\"clearcolor\":\"0 0 0.2\",\"fov\":50,\"nearz\":0.1,\"farz\":1000,"
                                        "\"orthogonalprojection\":null},\"objects\":[" + objects + "]}");
            if (supported) {
                Vfs vfs = Vfs::forItem(item->root().toStdString(), {});
                *supported = loadScene(vfs, "scene.json")->supported();
            }
            return item;
        };
        const QByteArray front = R"({"center":"0 0 0","eye":"0 0 10","up":"0 1 0"})";
        const auto square = [](const char *origin, const char *extra = "") {
            return QByteArray(R"({"id":2,"name":"solid","image":"models/util/solidlayer.json","color":"1 0 0","size":"2 2","origin":")") + origin + "\"" + extra + "}";
        };
        const auto bounds = [&](Item &item, int frames = 1) {
            return litBounds(render(item, 160, 90, nullptr, nullptr, frames, bridge::msg::ScalingMode::Stretch));
        };
        const auto width = [](const Bounds &b) { return b.x1 - b.x0 + 1; };
        const auto height = [](const Bounds &b) { return b.y1 - b.y0 + 1; };
        const auto centred = [](const Bounds &b) { return std::abs((b.x0 + b.x1) - 159) <= 2 && std::abs((b.y0 + b.y1) - 89) <= 2; };
        // the camera's centre lands in the middle, at the size the fov gives
        bool supported = false;
        auto near = itemWith(front, square("0 0 0"), &supported);
        QVERIFY(supported);
        Bounds b = bounds(*near);
        QVERIFY2(centred(b), qPrintable(QStringLiteral("%1..%2 x %3..%4").arg(b.x0).arg(b.x1).arg(b.y0).arg(b.y1)));
        QVERIFY2(width(b) >= 18 && width(b) <= 21 && height(b) >= 18 && height(b) <= 21, qPrintable(QString::number(width(b))));
        // a layer twice as far is half the size
        auto far = itemWith(front, square("0 0 -10"));
        b = bounds(*far);
        QVERIFY(centred(b));
        QVERIFY2(width(b) >= 8 && width(b) <= 11, qPrintable(QString::number(width(b))));
        // off the line of sight it lands off the middle: 2 units up at distance 10 is 90 * 2 / 9.326 = 19 pixels above
        auto up = itemWith(front, square("0 2 0"));
        b = bounds(*up);
        QVERIFY2(std::abs((b.y0 + b.y1) / 2 - (45 - 19)) <= 2, qPrintable(QString::number((b.y0 + b.y1) / 2)));
        // a visible camera object is the camera (the block looks elsewhere): its origin, its own -z, its zoom
        const QByteArray away = R"({"center":"100 0 0","eye":"101 0 0","up":"0 1 0"})";
        auto zoomed = itemWith(away, square("0 0 0") + R"(,{"id":3,"name":"cam","camera":"default","zoom":2,"fov":50,"origin":"0 0 10"})");
        b = bounds(*zoomed);
        QVERIFY(centred(b));
        QVERIFY2(width(b) >= 36 && width(b) <= 41, qPrintable(QString::number(width(b))));
        // any orientation: a camera turned a quarter about y looks down -x, at a layer turned the same way (facing it)
        auto turned = itemWith(away, square("0 0 0", R"(,"angles":"0 1.5707963 0")") + R"(,{"id":3,"name":"cam","camera":"default","fov":50,"origin":"10 0 0","angles":"0 1.5707963 0"})");
        b = bounds(*turned);
        QVERIFY(centred(b));
        QVERIFY2(width(b) >= 18 && width(b) <= 21, qPrintable(QString::number(width(b))));
        // seen edge on (the layer not turned) it is all but gone
        auto edge = itemWith(away, square("0 0 0") + R"(,{"id":3,"name":"cam","camera":"default","fov":50,"origin":"10 0 0","angles":"0 1.5707963 0"})");
        QVERIFY(width(bounds(*edge)) <= 2);
        // a camera block that looks along +x with z up
        auto block = itemWith(R"({"center":"0 0 0","eye":"-10 0 0","up":"0 0 1"})", square("0 0 0", R"(,"angles":"0 -1.5707963 0")"));
        b = bounds(*block);
        QVERIFY(centred(b));
        QVERIFY2(width(b) >= 18 && width(b) <= 21, qPrintable(QString::number(width(b))));
    }

    void aTurnedOrthogonalCameraTurnsTheSceneAboutItsCentre()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the camera's up is the world's +x: the picture turns a quarter, the red top left corner goes to the bottom left
        Item item;
        item.image(u"q"_s);
        item.tex(u"q"_s, quadrants());
        QByteArray json = sceneText(100, 100, "0 0 0", layer(1, "q", "50 50 0", "50 50"));
        json.replace("\"up\":\"0 1 0\"", "\"up\":\"1 0 0\"");
        item.file(u"scene.json"_s, json);
        bool supported = false;
        const Frame f = render(item, 100, 100, &supported);
        QVERIFY(supported);
        VERIFY_PX(f.at(37, 62), (Px{255, 0, 0, 255}), 0);   // red: bottom left
        VERIFY_PX(f.at(37, 37), (Px{0, 255, 0, 255}), 0);   // green (top right): top left
        VERIFY_PX(f.at(62, 62), (Px{0, 0, 255, 255}), 0);   // blue (bottom left): bottom right
        VERIFY_PX(f.at(5, 5), (Px{0, 0, 0, 255}), 0);       // the turn is about the centre: the corners stay empty
    }

    void theOrthogonalEyeIsAboveTheMiddleOfTheView()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // g_EyePosition in an orthographic scene: the view's middle in scene units, 10000 up (lit materials see the view along -z)
        const auto eyeOf = [&](const QByteArray &cameras) {
            Item item;
            item.image(u"w"_s);
            item.tex(u"w"_s, solid(255, 255, 255));
            addEffect(item, u"eye"_s,
                      "uniform sampler2D g_Texture0;\nuniform vec3 g_EyePosition;\nvarying vec2 v_TexCoord;\n"
                      "void main() { gl_FragColor = vec4(g_EyePosition.x / 200.0, g_EyePosition.y / 200.0, g_EyePosition.z / 20000.0, 1.0); }\n",
                      simpleEffect(u"eye"_s));
            item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "w", "50 50 0", "400 400", withEffects(fxInstance(u"eye"_s))) + cameras));
            return render(item, 20, 20).at(10, 10);
        };
        VERIFY_PX(eyeOf({}), (Px{64, 64, 128, 255}), 2); // (50, 50, 10000)
        // a camera object moves the view, and the eye with it
        VERIFY_PX(eyeOf(R"(,{"id":3,"name":"cam","camera":"default","origin":"40 -20 500"})"), (Px{115, 38, 128, 255}), 2); // (90, 30, 10000)
    }

    void aDefaultOrthogonalSceneDrawsAsItAlwaysHas()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the stored expectation: the 2 x 2 quadrant picture over a 100 x 50 scene, every pixel its quadrant's colour (the default
        // camera, -z with y up, and a shifted eye are no turn and no move)
        for (const char *eye : {"0 0 0", "-5.5 -17.5 1"}) {
            Item item;
            item.image(u"q"_s);
            item.tex(u"q"_s, quadrants());
            QByteArray json = sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "100 50"));
            json.replace("\"eye\":\"0 0 0\"", QByteArray("\"eye\":\"") + eye + "\"");
            if (QByteArray(eye) != "0 0 0") {
                json.replace("\"center\":\"0 0 -1\"", "\"center\":\"-5.5 -17.5 0\"");
            }
            item.file(u"scene.json"_s, json);
            const Frame f = render(item, 100, 50);
            for (uint32_t y = 0; y < 50; ++y) {
                for (uint32_t x = 0; x < 100; ++x) {
                    const Px want = y < 25 ? (x < 50 ? Px{255, 0, 0, 255} : Px{0, 255, 0, 255}) : (x < 50 ? Px{0, 0, 255, 255} : Px{255, 255, 0, 255});
                    if (f.at(x, y) != want) {
                        QFAIL(qPrintable(QStringLiteral("pixel %1,%2: got %3, expected %4").arg(x).arg(y).arg(show(f.at(x, y)), show(want))));
                    }
                }
            }
        }
    }

    void cameraPathsMoveThePerspectiveCamera()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // one path of one second: the eye backs off from 10 to 20 in front of a 2 x 2 square, so it ends half the size
        Item item;
        solidItem(item, {});
        item.file(u"scripts/camera_paths_3.json"_s, R"({"paths":[{"center":{"c0":[{"frame":0,"value":0}],"c1":[{"frame":0,"value":0}],"c2":[{"frame":0,"value":0}]},
            "eye":{"c0":[{"frame":0,"value":0}],"c1":[{"frame":0,"value":0}],"c2":[{"frame":0,"value":10},{"frame":30,"value":20}]},"up":null,"fov":null,"zoom":null,
            "visible":true,"options":{"fps":30.0,"length":30,"mode":"single"}}]})");
        item.file(u"scene.json"_s, R"({"camera":{"center":"0 0 0","eye":"0 0 10","up":"0 1 0"},"general":{"clearcolor":"0 0 0.2","fov":50,"camerafade":false},"objects":[)"
                                   R"({"id":2,"name":"solid","image":"models/util/solidlayer.json","color":"1 0 0","size":"2 2","origin":"0 0 0"},)"
                                   R"({"id":3,"name":"cam","camera":"default","fov":50,"origin":"0 0 500","path":"scripts/camera_paths_3.json","queuemode":"sequential"}]})");
        const auto width = [](const Bounds &b) { return b.x1 - b.x0 + 1; };
        // at the start the path, not the object's origin, places the camera
        Bounds b = litBounds(render(item, 160, 90, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch));
        QVERIFY2(width(b) >= 18 && width(b) <= 21, qPrintable(QString::number(width(b))));
        // at its end it has reached the second key
        m_frameStep = 0.33;
        b = litBounds(render(item, 160, 90, nullptr, nullptr, 4, bridge::msg::ScalingMode::Stretch));
        QVERIFY2(width(b) >= 8 && width(b) <= 11, qPrintable(QString::number(width(b))));
        // halfway, in between
        m_frameStep = 0.5;
        b = litBounds(render(item, 160, 90, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch));
        QVERIFY2(width(b) > 11 && width(b) < 18, qPrintable(QString::number(width(b))));
        // a camera that hangs on a moved and turned group: the path is still in world units
        item.file(u"scene.json"_s, R"({"camera":{"center":"0 0 0","eye":"0 0 10","up":"0 1 0"},"general":{"clearcolor":"0 0 0.2","fov":50,"camerafade":false},"objects":[)"
                                   R"({"id":2,"name":"solid","image":"models/util/solidlayer.json","color":"1 0 0","size":"2 2","origin":"0 0 0"},)"
                                   R"({"id":4,"name":"rig","origin":"30 -7 4","angles":"0.3 1.1 -0.2","scale":"2 2 2"},)"
                                   R"({"id":3,"name":"cam","camera":"default","fov":50,"parent":4,"origin":"0 0 500","path":"scripts/camera_paths_3.json","queuemode":"sequential"}]})");
        m_frameStep = 0.25;
        b = litBounds(render(item, 160, 90, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch));
        QVERIFY2(width(b) >= 18 && width(b) <= 21, qPrintable(QString::number(width(b))));
        QVERIFY2(std::abs((b.x0 + b.x1) - 159) <= 2 && std::abs((b.y0 + b.y1) - 89) <= 2, qPrintable(QStringLiteral("%1..%2 x %3..%4").arg(b.x0).arg(b.x1).arg(b.y0).arg(b.y1)));
    }

    void theScenesOwnZoomEnlargesTheView()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a picture 40 wide in a scene 100 wide: at zoom 2 it fills 80 of the 100 pixels, about the centre
        const auto frame = [&](const QByteArray &general) {
            Item item;
            item.image(u"q"_s);
            item.tex(u"q"_s, quadrants());
            item.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "40 20"), general));
            return render(item, 100, 50);
        };
        VERIFY_PX(frame("").at(20, 25), (Px{0, 0, 0, 255}), 1);                          // outside the picture
        VERIFY_PX(frame(R"(,"zoom":2)").at(20, 25), (Px{0, 0, 255, 255}), 1);            // inside it now: the lower left quadrant
        VERIFY_PX(frame(R"(,"zoom":2)").at(80, 12), (Px{0, 255, 0, 255}), 1);            // the upper right one
        VERIFY_PX(frame(R"(,"zoom":2)").at(5, 25), (Px{0, 0, 0, 255}), 1);               // and the edge of it
        // bound to a user property it follows the property
        Item bound;
        bound.image(u"q"_s);
        bound.tex(u"q"_s, quadrants());
        bound.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "40 20"), R"(,"zoom":{"user":"z","value":1})"));
        PropertyStore zoomProps;
        VERIFY_PX(render(bound, 100, 50, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, &zoomProps).at(20, 25), (Px{0, 0, 0, 255}), 1);
        zoomProps.set("z", 2.0);
        VERIFY_PX(render(bound, 100, 50, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, &zoomProps).at(20, 25), (Px{0, 0, 255, 255}), 1);
        // a script that sets the camera's zoom (setCameraTransforms) does the same
        Item scripted;
        scripted.image(u"q"_s);
        scripted.tex(u"q"_s, quadrants());
        scripted.file(u"scene.json"_s, sceneText(100, 50, "0 0 0", layer(1, "q", "50 25 0", "40 20"),
                                                 R"(,"zoom":{"script":"export function update(v) { return 2; }","value":1})"));
        VERIFY_PX(render(scripted, 100, 50, nullptr, nullptr, 3).at(20, 25), (Px{0, 0, 255, 255}), 1);
    }

    void aLightsValuesFollowTheirScriptsAndKeyframes()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto lit = [&](const QByteArray &intensity, int frames) {
            Item item;
            litSlabItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 10","color":"1 0 0","intensity":)" + intensity + R"(,"radius":40,"exponent":1})");
            const Frame f = render(item, 100, 100, nullptr, nullptr, frames);
            return f.at(20, 49)[0];
        };
        QVERIFY(lit("3", 3) > 150);
        // a script on the intensity switches the light off; it runs from the second frame on
        QCOMPARE(int(lit(R"({"script":"export function update(v) { return 0; }","value":3})", 3)), 0);
        // a script that writes 1e30 leaves the image finite: the same picture as the top of the range (intensity and colour 1e3)
        const Frame huge = [&] {
            Item item;
            litSlabItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 10","color":"1 0 0","intensity":{"script":"export function update(v) { return 1e30; }","value":3},"radius":40,"exponent":1})");
            return render(item, 100, 100, nullptr, nullptr, 3);
        }();
        const Frame top = [&] {
            Item item;
            litSlabItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 10","color":"1 0 0","intensity":1000,"radius":40,"exponent":1})");
            return render(item, 100, 100, nullptr, nullptr, 3);
        }();
        QVERIFY(huge.rgba == top.rgba);
        // keyframes on the radius play: from 0.001 to 40 over a second, the floor beside the light is dark at first and lit at the end
        const auto radius = [&](int frames) {
            Item item;
            litSlabItem(item, R"({"id":2,"light":"lpoint","origin":"20 50 10","color":"1 0 0","intensity":3,"exponent":1,"radius":{"animation":{"c0":[{"frame":0,"value":0.001},{"frame":30,"value":40}],)"
                              R"("options":{"fps":30,"length":30,"mode":"single"}},"value":40}})");
            m_frameStep = 0.5;
            return render(item, 100, 100, nullptr, nullptr, frames).at(20, 49)[0];
        };
        QVERIFY(radius(1) < 20);
        QVERIFY(radius(3) > 150);
    }

    void aCameraFadeDarkensTheEndsOfAPath()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto scene = [&](bool fade) {
            Item item;
            solidItem(item, {});
            item.file(u"scripts/camera_paths_3.json"_s, R"({"paths":[{"center":{"c0":[{"frame":0,"value":0}],"c1":[{"frame":0,"value":0}],"c2":[{"frame":0,"value":0}]},
                "eye":{"c0":[{"frame":0,"value":0}],"c1":[{"frame":0,"value":0}],"c2":[{"frame":0,"value":10}]},"up":null,"fov":null,"zoom":null,
                "visible":true,"options":{"fps":30.0,"length":30,"mode":"single"}}]})");
            item.file(u"scene.json"_s, QByteArray(R"({"camera":{"center":"0 0 0","eye":"0 0 10","up":"0 1 0"},"general":{"clearcolor":"0 0 0.2","fov":50,"camerafade":)")
                                           + (fade ? "true" : "false") + R"(},"objects":[)"
                                           R"({"id":2,"name":"solid","image":"models/util/solidlayer.json","color":"1 0 0","size":"2 2","origin":"0 0 0"},)"
                                           R"({"id":3,"name":"cam","camera":"default","fov":50,"origin":"0 0 500","path":"scripts/camera_paths_3.json","queuemode":"sequential"}]})");
            return item;
        };
        const auto red = [&](bool fade, double step, int frames) {
            m_frameStep = step;
            Item item = scene(fade);
            return render(item, 160, 90, nullptr, nullptr, frames).at(80, 45)[0];
        };
        QVERIFY(red(false, 0.9, 2) > 250);                 // no fade: the red square at 0.9 seconds of a second-long path
        QVERIFY(red(true, 0.5, 2) > 250);                  // fade, but half way through the path: untouched
        const int atEnd = red(true, 0.9, 2);               // 0.1 s from the end: most of the way to black
        QVERIFY2(atEnd > 40 && atEnd < 120, qPrintable(QString::number(atEnd)));
        QVERIFY2(red(true, 0.9, 1) < 5, "the start of a path is black");
        // the switch may be bound to a user property, followed as it changes
        Item bound = scene(false);
        bound.file(u"scene.json"_s, QByteArray(R"({"camera":{"center":"0 0 0","eye":"0 0 10","up":"0 1 0"},"general":{"clearcolor":"0 0 0.2","fov":50,"camerafade":{"user":"fade","value":false}},"objects":[)"
                                               R"({"id":2,"name":"solid","image":"models/util/solidlayer.json","color":"1 0 0","size":"2 2","origin":"0 0 0"},)"
                                               R"({"id":3,"name":"cam","camera":"default","fov":50,"origin":"0 0 500","path":"scripts/camera_paths_3.json","queuemode":"sequential"}]})"));
        PropertyStore props;
        m_frameStep = 0.9;
        QVERIFY(render(bound, 160, 90, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, &props).at(80, 45)[0] > 250);
        props.set("fade", true);
        QVERIFY(render(bound, 160, 90, nullptr, nullptr, 1, bridge::msg::ScalingMode::Stretch, &props).at(80, 45)[0] < 5);
    }

    void cameraShakeMovesTheWholePictureAndOnlyWhileItIsOn()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        const auto edge = [&](const QByteArray &general, int frames) {
            item.file(u"scene.json"_s, sceneText(100, 100, "0 0 0", layer(1, "w", "50 50 0", "40 40"), general));
            return brightBounds(render(item, 100, 100, nullptr, nullptr, frames), 100);
        };
        const Bounds still = edge("", 6);
        QCOMPARE(still.x0, 30);
        // amplitude 10: up to 10 units either way
        const QByteArray shake = ",\"camerashake\":true,\"camerashakeamplitude\":10,\"camerashakeroughness\":1,\"camerashakespeed\":3";
        std::set<int> positions;
        for (int frames = 2; frames <= 24; frames += 2)
            positions.insert(edge(shake, frames).x0);
        { QString all; for (int x : positions) all += QString::number(x) + " "; QVERIFY2(positions.size() >= 3, qPrintable("the picture wanders: " + all)); }
        for (int x : positions)
            QVERIFY2(x >= 30 - 11 && x <= 30 + 11, qPrintable(QString::number(x)));
        // off again: still
        QCOMPARE(edge(",\"camerashake\":false,\"camerashakeamplitude\":10", 6).x0, 30);
    }

    // ---------------- camera parallax and sprite sheets ----------------

    void disabledMouseAndParallaxKeepTheSceneStill()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0", layer(1, "w", "50 25 0", "50 20", ",\"parallaxDepth\":\"1 1\""),
                            ",\"cameraparallax\":true,\"cameraparallaxamount\":0.2,\"cameraparallaxdelay\":0,\"cameraparallaxmouseinfluence\":1"));
        const auto run = [&](bool mouse, bool parallax) {
            return render(item, 100, 100, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, nullptr, [&](SceneRenderer &r, int frame) {
                if (frame == 0) {
                    r.setPointerEnabled(mouse);
                    r.setParallaxEnabled(parallax);
                }
                if (frame == 1)
                    r.setPointer(100, 50, 0, {100, 100});
            });
        };
        // enabled: the layer moves 10 units left (covers x 15..65); either switch off: it stays at 25..75
        VERIFY_PX(run(true, true).at(20, 75), (Px{255, 255, 255, 255}), 0);
        VERIFY_PX(run(false, true).at(20, 75), (Px{0, 0, 0, 255}), 0);
        VERIFY_PX(run(true, false).at(20, 75), (Px{0, 0, 0, 255}), 0);
        VERIFY_PX(run(true, false).at(70, 75), (Px{255, 255, 255, 255}), 0);
    }

    void cameraParallaxMovesLayersAgainstThePointerByDepth()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        // two layers side by side vertically: depth 1 (moves) above depth 0 (stays)
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0",
                            layer(1, "w", "50 25 0", "50 20", ",\"parallaxDepth\":\"1 1\"") + "," + layer(2, "w", "50 75 0", "50 20", ",\"parallaxDepth\":\"0 0\""),
                            ",\"cameraparallax\":true,\"cameraparallaxamount\":0.2,\"cameraparallaxdelay\":0,\"cameraparallaxmouseinfluence\":1"));
        // pointer at the right edge, vertical centre: the layers move left by 0.5 * 0.2 * 100 = 10 units
        const Frame f = render(item, 100, 100, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, nullptr,
                               [](SceneRenderer &r, int frame) {
                                   QVERIFY(r.usesPointer());
                                   if (frame == 1)
                                       r.setPointer(100, 50, 0, {100, 100});
                               });
        // layer 1 (centre y = 25 from the bottom, so rows 65..85 from the top) covers x 15..65 now
        VERIFY_PX(f.at(20, 75), (Px{255, 255, 255, 255}), 0);
        VERIFY_PX(f.at(70, 75), (Px{0, 0, 0, 255}), 0);
        // layer 2 (rows 15..35) did not move: x 25..75
        VERIFY_PX(f.at(70, 25), (Px{255, 255, 255, 255}), 0);
        VERIFY_PX(f.at(20, 25), (Px{0, 0, 0, 255}), 0);
        // the pointer below the centre moves layers up (scene y grows): top left pointer moves them right and down
        const Frame g = render(item, 100, 100, nullptr, nullptr, 2, bridge::msg::ScalingMode::Stretch, nullptr,
                               [](SceneRenderer &r, int frame) {
                                   if (frame == 1)
                                       r.setPointer(0, 100, 0, {100, 100}); // bottom left
                               });
        // x target = +0.1 -> +10 units right; y target = +0.1 -> +10 units up (rows 55..75)
        VERIFY_PX(g.at(80, 60), (Px{255, 255, 255, 255}), 0);
        VERIFY_PX(g.at(60, 60), (Px{255, 255, 255, 255}), 0);
        VERIFY_PX(g.at(30, 60), (Px{0, 0, 0, 255}), 0);   // x now runs 35..85
        VERIFY_PX(g.at(60, 80), (Px{0, 0, 0, 255}), 0);   // and the rows 55..75 left this one empty
    }

    void parallaxEasesTowardsThePointerAndStops()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 255, 255));
        item.file(u"scene.json"_s,
                  sceneText(100, 100, "0 0 0", layer(1, "w", "50 50 0", "20 20"),
                            ",\"cameraparallax\":true,\"cameraparallaxamount\":1,\"cameraparallaxdelay\":1.5,\"cameraparallaxmouseinfluence\":1"));
        // delay 1.5 of 3 halves the rate to 5 per second; at 20 ms per frame each frame closes a tenth of the gap
        m_frameStep = 0.02;
        const auto movePointer = [](SceneRenderer &r, int frame) {
            if (frame == 0)
                r.setPointer(100, 50, 0, {100, 100});
        };
        uint64_t passes = 0;
        const Frame early = render(item, 100, 100, nullptr, &passes, 12, bridge::msg::ScalingMode::Stretch, nullptr, movePointer);
        QCOMPARE(passes, uint64_t{12}); // still easing on every frame
        // after 12 frames the layer has covered 1 - 0.9^12 = 72 % of the 50 units: it now spans x 4..24
        VERIFY_PX(early.at(10, 50), (Px{255, 255, 255, 255}), 0);
        VERIFY_PX(early.at(50, 50), (Px{0, 0, 0, 255}), 0);
        // given long enough it arrives and the scene passes stop
        const Frame late = render(item, 100, 100, nullptr, &passes, 200, bridge::msg::ScalingMode::Stretch, nullptr, movePointer);
        QVERIFY(passes < 200);
        QVERIFY(passes > 50);
        VERIFY_PX(late.at(5, 50), (Px{255, 255, 255, 255}), 0); // 50 units to the left it spans x -10..10
        VERIFY_PX(late.at(50, 50), (Px{0, 0, 0, 255}), 0);
    }

    void spriteSheetsPlayTheirFramesInOrderAndLoop()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a 4 x 2 sheet of two 2 x 2 frames: red, then green, a quarter second each
        TexSpec sheet;
        sheet.flags = TexFlags::IsGif | TexFlags::NoInterpolation | TexFlags::ClampUVs;
        sheet.texW = sheet.imgW = 4;
        sheet.texH = sheet.imgH = 2;
        Bytes px;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x)
                for (uint8_t v : (x < 2 ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 255, 0, 255}))
                    px.push_back(v);
        sheet.images = {{MipSpec{4, 2, px, false}}};
        sheet.sheetVersion = 2;
        sheet.frames = {{0, 0, 2, 0, 0, 2}, {2, 0, 2, 0, 0, 2}};
        Item item;
        item.image(u"s"_s, ",\"combos\":{\"spritesheet\":1}");
        item.tex(u"s"_s, sheet);
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "s", "10 10 0", "20 20")));
        uint64_t passes = 0;
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 1).at(5, 5), (Px{255, 0, 0, 255}), 0);  // t = 0
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 2).at(5, 5), (Px{0, 255, 0, 255}), 0);  // t = 0.25
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 3).at(5, 5), (Px{255, 0, 0, 255}), 0);  // t = 0.5: the loop restarts
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 4).at(15, 15), (Px{0, 255, 0, 255}), 0); // t = 0.75, whole frame
        QCOMPARE(passes, uint64_t{4}); // the sheet animates: a scene pass per frame
    }

    void spriteSheetFramesSampleTheImageTheyLieOn()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // image 0 is 4 x 2 (red | green); image 1 is a smaller 2 x 2 picture, all blue. Frames: red, green, then blue from image 1
        TexSpec sheet;
        sheet.flags = TexFlags::IsGif | TexFlags::NoInterpolation | TexFlags::ClampUVs;
        sheet.texW = sheet.imgW = 4;
        sheet.texH = sheet.imgH = 2;
        Bytes px;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 4; ++x)
                for (uint8_t v : (x < 2 ? std::array<uint8_t, 4>{255, 0, 0, 255} : std::array<uint8_t, 4>{0, 255, 0, 255}))
                    px.push_back(v);
        Bytes blue;
        for (int i = 0; i < 4; ++i)
            for (uint8_t v : std::array<uint8_t, 4>{0, 0, 255, 255})
                blue.push_back(v);
        sheet.images = {{MipSpec{4, 2, px, false}}, {MipSpec{2, 2, blue, false}}};
        sheet.sheetVersion = 3;
        sheet.frames = {{0, 0, 2, 0, 0, 2}, {2, 0, 2, 0, 0, 2}, {0, 0, 2, 0, 0, 2}};
        sheet.frameImages = {0, 0, 1};
        Item item;
        item.image(u"s"_s, ",\"combos\":{\"spritesheet\":1}");
        item.tex(u"s"_s, sheet);
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "s", "10 10 0", "20 20")));
        uint64_t passes = 0;
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 1).at(5, 5), (Px{255, 0, 0, 255}), 0);  // t = 0, image 0
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 2).at(5, 5), (Px{0, 255, 0, 255}), 0);  // t = 0.25, image 0
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 3).at(5, 5), (Px{0, 0, 255, 255}), 0);  // t = 0.5, image 1
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 3).at(15, 15), (Px{0, 0, 255, 255}), 0);
        VERIFY_PX(render(item, 20, 20, nullptr, &passes, 4).at(5, 5), (Px{255, 0, 0, 255}), 0);  // t = 0.75: the loop restarts
    }

    void shaderProblemsStopTheRendererWithAReason()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        const auto build = [&](Item &item) {
            Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
            auto scene = loadScene(vfs, "scene.json");
            QVERIFY(scene.has_value());
            PropertyStore props;
            try {
                SceneRenderer renderer(*m_ctx, std::move(*scene), vfs, props);
                QFAIL("the renderer should refuse this scene");
            } catch (const std::exception &e) {
                m_lastError = e.what();
            }
        };
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(255, 0, 0));
        // an effect whose shader is not there
        item.file("materials/effects/ghost.json", "{\"passes\":[{\"shader\":\"effects/ghost\"}]}");
        item.file("effects/ghost/effect.json", simpleEffect(u"ghost"_s));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"ghost"_s)))));
        build(item);
        QVERIFY2(m_lastError.find("ghost") != std::string::npos, m_lastError.c_str());
        // one that does not compile
        addEffect(item, u"broken"_s, "varying vec2 v_TexCoord;\nvoid main() { gl_FragColor = nothing_here; }\n", simpleEffect(u"broken"_s));
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20", withEffects(fxInstance(u"broken"_s)))));
        build(item);
        QVERIFY2(m_lastError.find("nothing_here") != std::string::npos, m_lastError.c_str());
    }

    void unsupportedSceneStillDrawsItsImageLayersWhenForced()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(0, 255, 0));
        item.file(u"scene.json"_s,
                  sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20") + ",{\"id\":2,\"particle\":\"particles/x.json\",\"image\":null}"));
        bool supported = true;
        const Frame f = render(item, 20, 20, &supported);
        QVERIFY(!supported);
        VERIFY_PX(f.at(5, 5), (Px{0, 255, 0, 255}), 0);
    }

    // ---- pass optimiser (render/passprobe.h) ----
    // A layer that reads the clock last, so it is redrawn every frame (an all-static layer is cached whole and never reaches the
    // optimiser), preceded by `first`.
    static QByteArray optimisedScene(int w, int h, const QByteArray &first)
    {
        return sceneText(w, h, "0 0 0",
                         layer(1, "w", (QByteArray::number(w / 2) + " " + QByteArray::number(h / 2) + " 0").constData(),
                               (QByteArray::number(w) + " " + QByteArray::number(h)).constData(),
                               withEffects(first + "," + fxInstance(u"shade"_s))));
    }
    static void addShade(Item &item)
    {
        addEffect(item, u"shade"_s,
                  "uniform sampler2D g_Texture0;\nuniform float g_Time;\nvarying vec2 v_TexCoord;\n"
                  "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord); c.rgb *= 0.5 + 0.25 * fract(g_Time); gl_FragColor = c; }\n",
                  simpleEffect(u"shade"_s));
    }
    // Renders with the optimiser on and off; returns the statistics of the run with it on.
    SceneRenderer::PassOptStats optimised(Item &item, int w, int h, Frame &on, Frame &off, const char *mode = "1", const char *scale = "1")
    {
        SceneRenderer::PassOptStats stats;
        qputenv("MURASAKI_SCENE_VK_RENDER_SCALE", scale);
        qputenv("MURASAKI_SCENE_VK_PASS_OPT", mode);
        on = render(item, w, h, nullptr, nullptr, 4, bridge::msg::ScalingMode::Stretch, nullptr,
                    [&](SceneRenderer &r, int) { stats = r.passOptStats(); });
        qputenv("MURASAKI_SCENE_VK_PASS_OPT", "0");
        off = render(item, w, h, nullptr, nullptr, 4);
        qunsetenv("MURASAKI_SCENE_VK_PASS_OPT");
        qunsetenv("MURASAKI_SCENE_VK_RENDER_SCALE");
        return stats;
    }

    void aPassThatChangesNothingIsNotDrawn()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addShade(item);
        // amount 0: the effect lerps to the input itself; derived from the shader and its constants, not from its name
        item.file(u"scene.json"_s, optimisedScene(64, 64, fxInstance(u"invert"_s, "[{\"constantshadervalues\":{\"amount\":0}}]")));
        Frame on, off;
        const auto stats = optimised(item, 64, 64, on, off);
        QVERIFY2(stats.skipped >= 1, "the identity pass was drawn");
        QCOMPARE(on.rgba, off.rgba); // lossless
    }

    void anHdrSceneJudgesNoPass()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the identity pass of aPassThatChangesNothingIsNotDrawn, in an HDR scene: the probe works in 8 bits, so nothing is judged there
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addShade(item);
        const QByteArray scene = optimisedScene(64, 64, fxInstance(u"invert"_s, "[{\"constantshadervalues\":{\"amount\":0}}]"));
        item.file(u"scene.json"_s, scene.left(scene.indexOf("},\"objects\"")) + ",\"hdr\":true" + scene.mid(scene.indexOf("},\"objects\"")));
        Frame on, off;
        const auto stats = optimised(item, 64, 64, on, off);
        QCOMPARE(stats.probes, uint64_t{0});
        QCOMPARE(stats.skipped, uint64_t{0});
        QCOMPARE(stats.reduced, uint64_t{0});
        QCOMPARE(stats.partial, uint64_t{0});
        QCOMPARE(on.rgba, off.rgba);
    }

    void aPassThatChangesThePictureIsDrawn()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addShade(item);
        item.file(u"scene.json"_s, optimisedScene(64, 64, fxInstance(u"invert"_s)));
        Frame on, off;
        const auto stats = optimised(item, 64, 64, on, off);
        QCOMPARE(stats.skipped, 0u);
        QCOMPARE(on.rgba, off.rgba);
    }

    void aMaskThatHidesTheEffectMakesItIdentity()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // the shader mixes by a mask texture: with an all-black mask nothing changes, with a white one it does
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        item.tex(u"black"_s, solid(0, 0, 0));
        item.tex(u"white"_s, solid(255, 255, 255));
        const QByteArray maskedFrag =
            "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
            "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord); float m = texSample2D(g_Texture1, v_TexCoord).r;"
            " c.rgb = lerp(c.rgb, 1.0 - c.rgb, m); gl_FragColor = c; }\n";
        addEffect(item, u"maskedblack"_s, maskedFrag, simpleEffect(u"maskedblack"_s), kPassVert, ",\"textures\":[null,\"black\"]");
        addEffect(item, u"maskedwhite"_s, maskedFrag, simpleEffect(u"maskedwhite"_s), kPassVert, ",\"textures\":[null,\"white\"]");
        addShade(item);
        Frame on, off;
        item.file(u"scene.json"_s, optimisedScene(64, 64, fxInstance(u"maskedblack"_s)));
        QVERIFY(optimised(item, 64, 64, on, off).skipped >= 1);
        QCOMPARE(on.rgba, off.rgba);
        item.file(u"scene.json"_s, optimisedScene(64, 64, fxInstance(u"maskedwhite"_s)));
        QCOMPARE(optimised(item, 64, 64, on, off).skipped, 0u);
        QCOMPARE(on.rgba, off.rgba);
    }

    void aPassThatChangesOnlyPartOfThePictureIsDrawnThereOnly()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        // a mask that is black on the left half and white on the right: the effect only changes the right half
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        TexSpec half;
        half.flags = TexFlags::NoInterpolation | TexFlags::ClampUVs;
        half.texW = half.imgW = 2;
        half.texH = half.imgH = 1;
        half.images = {{MipSpec{2, 1, {0, 0, 0, 255, 255, 255, 255, 255}, false}}};
        item.tex(u"halfmask"_s, half);
        const QByteArray maskedFrag =
            "uniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nvarying vec2 v_TexCoord;\n"
            "void main() { vec4 c = texSample2D(g_Texture0, v_TexCoord); float m = texSample2D(g_Texture1, v_TexCoord).r;"
            " c.rgb = lerp(c.rgb, 1.0 - c.rgb, m); gl_FragColor = c; }\n";
        addEffect(item, u"maskedhalf"_s, maskedFrag, simpleEffect(u"maskedhalf"_s), kPassVert, ",\"textures\":[null,\"halfmask\"]");
        addShade(item);
        item.file(u"scene.json"_s, optimisedScene(64, 64, fxInstance(u"maskedhalf"_s)));
        Frame on, off;
        const auto stats = optimised(item, 64, 64, on, off);
        QCOMPARE(stats.skipped, 0u);
        QVERIFY2(stats.partial >= 1, "the pass was drawn over the whole layer");
        QCOMPARE(on.rgba, off.rgba);
    }

    void aWideBlurRunsReducedAndStaysClose()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        // a 15x15 box blur, one texel a tap
        addEffect(item, u"boxblur"_s,
                  "uniform sampler2D g_Texture0;\nuniform vec4 g_Texture0Resolution;\nvarying vec2 v_TexCoord;\n"
                  "void main() { vec4 sum = vec4(0.0); for (int j = -7; j <= 7; ++j) { for (int i = -7; i <= 7; ++i) {"
                  " sum += texSample2D(g_Texture0, v_TexCoord + vec2(float(i), float(j)) / g_Texture0Resolution.xy); } }"
                  " gl_FragColor = sum / 225.0; }\n",
                  simpleEffect(u"boxblur"_s));
        addShade(item);
        item.file(u"scene.json"_s, optimisedScene(1280, 720, fxInstance(u"boxblur"_s)));
        Frame on, off;
        // below 100% the gate is looser and the reduction larger
        const auto stats = optimised(item, 1280, 720, on, off, "1", "0.75");
        QVERIFY2(stats.reduced >= 1, "the blur was drawn at full size");
        int worst = 0;
        for (size_t i = 0; i < on.rgba.size(); ++i)
            worst = std::max(worst, std::abs(int(on.rgba[i]) - int(off.rgba[i])));
        QVERIFY2(worst <= 6, qPrintable(QString("reduced blur differs by %1").arg(worst)));
    }

    void aPassThatKeepsDetailIsNeverReduced()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.tex(u"w"_s, solid(200, 60, 20));
        addEffect(item, u"invert"_s, kInvertFrag, simpleEffect(u"invert"_s));
        addShade(item);
        item.file(u"scene.json"_s, optimisedScene(1280, 720, fxInstance(u"invert"_s)));
        Frame on, off;
        for (const char *scale : {"1", "0.75"}) {
            const auto stats = optimised(item, 1280, 720, on, off, "1", scale);
            QCOMPARE(stats.reduced, 0u);
            QCOMPARE(on.rgba, off.rgba);
        }
    }

    void aMissingTextureStopsTheRenderer()
    {
        if (!m_ctx)
            QSKIP(qPrintable(m_skip));
        Item item;
        item.image(u"w"_s);
        item.file(u"scene.json"_s, sceneText(20, 20, "0 0 0", layer(1, "w", "10 10 0", "20 20")));
        Vfs vfs = Vfs::forItem(item.root().toStdString(), {});
        auto scene = loadScene(vfs, "scene.json");
        QVERIFY(scene.has_value());
        PropertyStore props;
        try {
            SceneRenderer renderer(*m_ctx, std::move(*scene), vfs, props);
            QFAIL("the renderer should refuse a scene whose texture is missing");
        } catch (const std::exception &e) {
            QVERIFY(std::string(e.what()).find("not found") != std::string::npos);
        }
    }
};

QTEST_GUILESS_MAIN(TstSceneVkRender)
#include "tst_scenevk_render.moc"
