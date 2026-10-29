// murasaki-scene-vk shader pipeline: sources with includes and annotations, the dialect translation, the std140
// layout against what the compiler produced, and the on-disk cache. No GPU involved. Shader text is synthetic.

#include "shaders/cache.h"
#include "shaders/glslcompiler.h"
#include "shaders/source.h"
#include "shaders/translate.h"

#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTest>

#include <sys/mman.h>

#include <algorithm>
#include <cstdio>
#include <functional>
#include <ctime>
#include <map>
#include <set>
#include <string>

using namespace murasaki::scene;

namespace {

IncludeResolver headers(const std::map<std::string, std::string> &files)
{
    return [files](const std::string &, const std::string &name) -> std::optional<std::string> {
        const auto it = files.find(name);
        if (it == files.end()) {
            return std::nullopt;
        }
        return it->second;
    };
}

ShaderSource source(const std::string &text, const std::map<std::string, std::string> &files = {})
{
    std::string error;
    auto s = parseShaderSource("test", text, headers(files), &error);
    if (!s) {
        qFatal("parseShaderSource: %s", error.c_str());
    }
    return *s;
}

const char *kVert = R"(
// [COMBO] {"material":"Mask","combo":"MASK","default":0}
uniform mat4 g_ModelViewProjectionMatrix;
uniform float g_Time;
#if MASK
uniform vec4 g_Texture1Resolution;
#endif
attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec4 v_TexCoord;
varying vec3 v_Extra;
void main() {
    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
    v_TexCoord = a_TexCoord.xyxy + g_Time;
    v_Extra = vec3(1.0);
}
)";

// A vertex shader that hands the fragment shader its texture coordinates, for tests about the fragment stage.
const char *kPlainVert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                         "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord; }\n";

const char *kFrag = R"(
varying vec4 v_TexCoord;
varying vec3 v_Extra;
uniform sampler2D g_Texture0; // {"hidden":true}
uniform sampler2D g_Texture1; // {"label":"mask","combo":"MASK"}
uniform float g_Strength; // {"material":"strength","label":"s","default":0.5,"range":[0,1]}
uniform vec3 g_Tint; // {"material":"tint","default":"1 0.5 0","type":"color"}
void main() {
    vec4 c = texSample2D(g_Texture0, v_TexCoord.xy);
#if MASK
    c.a *= texSample2D(g_Texture1, v_TexCoord.zw).r;
#endif
    c.rgb = lerp(c.rgb, g_Tint, g_Strength) + v_Extra * 0.0;
    gl_FragColor = c;
}
)";

// Offsets the compiler wrote into the uniform block, read from the SPIR-V (OpMemberDecorate ... Offset).
std::vector<uint32_t> blockOffsets(const std::vector<uint32_t> &spirv)
{
    std::map<uint32_t, std::map<uint32_t, uint32_t>> offsets; // struct id -> member -> offset
    std::set<uint32_t> blocks;
    for (size_t i = 5; i < spirv.size();) {
        const uint32_t word = spirv[i];
        const uint32_t count = word >> 16;
        const uint32_t op = word & 0xFFFF;
        if (count == 0) {
            break;
        }
        if (op == 72 && count >= 5 && spirv[i + 3] == 35) { // OpMemberDecorate, Offset
            offsets[spirv[i + 1]][spirv[i + 2]] = spirv[i + 4];
        } else if (op == 71 && count >= 3 && spirv[i + 2] == 2) { // OpDecorate, Block
            blocks.insert(spirv[i + 1]);
        }
        i += count;
    }
    std::vector<uint32_t> out;
    for (const auto &[id, members] : offsets) {
        if (blocks.count(id)) {
            for (const auto &[member, offset] : members) {
                out.push_back(offset);
            }
        }
    }
    return out;
}

} // namespace

class TstSceneVkShaders : public QObject
{
    Q_OBJECT

    QTemporaryDir m_cache;

private slots:
    void initTestCase() { qputenv("XDG_CACHE_HOME", m_cache.path().toUtf8()); }

    // ---------------- sandbox ----------------

    void aStrayEndifIsIgnoredLikeWEDoes()
    {
        // two Workshop effects ship with one `#endif` too many: glslang refuses it, WE's preprocessor does not
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                                 "#if A\nvoid unused() {}\n#endif\n#endif\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n";
        const std::string frag = "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nvoid main() {\n#if B\n#else\n#endif\n#else\n"
                                 "  gl_FragColor = texSample2D(g_Texture0, v_TexCoord);\n}\n";
        std::string error;
        const auto t = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        // a balanced shader is left alone, nested conditions included
        const auto ok = translateShader(source(kVert), source(kFrag), {}, &error);
        QVERIFY2(ok.has_value(), error.c_str());
    }

    void workshopHabitsTheShimTakes()
    {
        // a vec4 times a vec2 (HLSL cuts the longer), a write to a varying in the fragment shader, an int literal beside floats in max
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n";
        const std::string frag = "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform vec2 g_Scale;\nuniform float g_Lo;\n"
                                 "void main() {\n  vec4 p = vec4(v_TexCoord, 0.0, 1.0);\n  vec2 da = p * g_Scale * 0.001;\n  v_TexCoord = v_TexCoord + da;\n"
                                 "  float lo = max(0, min(g_Lo, v_TexCoord.x - 0.1));\n  gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * lo;\n}\n";
        std::string error;
        const auto t = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void moreWorkshopHabitsTheShimTakes()
    {
        // log10 and fmod of HLSL, mix and dot over vectors of different widths, `%=` on a float, a number as the condition of `?:`,
        // a float as an array index
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n";
        const std::string frag = "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float g_Time;\nuniform float g_Pick;\nuniform float g_Bands[4];\nuniform vec3 u_Offset3;\nuniform vec4 g_Texture0Resolution;\n"
                                 "float hashed(highp vec2 p) { return p.x; }\nfloat hashed(vec2 p, int unused) { return p.y; }\n"
                                 "float mod(float x, float y) { return x - y * floor(x / y); }\n"
                                 "#if g_Texture0Resolution.x < g_Texture0Resolution.y\n#define RATIO 2.0\n#else\n#define RATIO 1.0\n#endif\n"
                                 "void main() {\n  vec4 albedo = texSample2D(g_Texture0, v_TexCoord);\n  vec3 other = vec3(0.2, 0.4, 0.6);\n"
                                 "  albedo.rgb = mix(albedo, other, 0.5);\n"
                                 "  float luma = dot(1.0 - pow(albedo * 0.995, 2.2), vec3(0.2126, 0.7152, 0.0722));\n"
                                 "  float tone = log10(1.0 + luma) / log10(2.0);\n"
                                 "  vec2 wrapped = fmod(v_TexCoord * 10.0, 3.0);\n"
                                 "  float counter = g_Time;\n  counter %= 2;\n"
                                 "  float mask = 0.25;\n  mask = 0 ? 1 - mask : mask;\n"
                                 "  float band = g_Bands[g_Pick];\n"
                                 "  vec2 radial = 0.0, tangential = 0.0, center = (v_TexCoord - 0.5);\n  tangential.x = center.y;\n"
                                 "  float2 pair = float2(0.25, 0.75);\n  half level = pair.x; float3 triple = float3(level, pair.y, 1.0);\n  int2 cell = int2(1, 2);\n"
                                 "  vec2 offset2 = v_TexCoord * 2.0 - u_Offset3;\n"
                                 "  float eased = mix(0, 1, band * 100.0);\n  vec3 lifted = mix(luma, other, 0.25) + mix(other, 0.5, 0.5);\n"
                                 "  float twice = hashed(vec2(1.0, 2.0)) + hashed(highp vec2(3.0)) + mod(5.5, 2.0);\n"
                                 "  gl_FragColor = vec4(albedo.rgb * tone * band + wrapped.x * 0.0 + counter * 0.0 + mask + twice * 0.0 + offset2.x * 0.0 + eased * 0.0 + lifted.x * 0.0 + triple.x * 0.0 + float(cell.x) * 0.0 + radial.x * 0.0 + tangential.x * 0.0, 1.0);\n}\n";
        std::string error;
        const auto t = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void interpolatorsAndComparisonsTheShimReconciles()
    {
        // the fragment shader declares v_TexCoord narrower than the vertex shader writes it and reads `.zw` and passes it whole to a
        // texture; a vec4 interpolator meets vec2s inside one expression; a comparison is multiplied by a float
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = vec4(a_TexCoord, a_TexCoord);\n}\n";
        const std::string frag = "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform sampler2D g_Texture1;\nuniform vec2 u_Center;\n"
                                 "void main() {\n  vec4 scene = texSample2D(g_Texture0, v_TexCoord);\n  float mask = texSample2D(g_Texture1, v_TexCoord.zw).r;\n"
                                 "  vec2 uv = (v_TexCoord * 2.0 - 1.0 - (u_Center * 2.0 - 1.0)) / 2.0;\n"
                                 "  float depth = mask;\n  depth *= (depth < 0.6) * 6.0;\n  static const float kBase = 0.5;\n  vec3 powered = vec3(pow(abs(depth), vec3(2.0, 3.0, 4.0) * kBase));\n"
                                 "  gl_FragColor = vec4(scene.rgb * depth + uv.x * 0.0 + powered * 0.0, 1.0);\n}\n";
        std::string error;
        const auto t = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void aFunctionTheShaderDefinesTakesAWiderVectorThanItsParameter()
    {
        // color_grading's `float select(vec3 a, vec3 b)` is called with a vec4: HLSL cuts the extra component
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n";
        const std::string frag = "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                                 "float pick(vec3 first, vec3 second) {\n  return dot(first, second);\n}\n"
                                 "#define fract frac\n#define mix lerp\nvoid main() {\n  vec4 base = texSample2D(g_Texture0, v_TexCoord);\n  vec3 other = vec3(0.2, 0.4, 0.6);\n"
                                 "  bool left = base.y < 0.49;\n  bool right = base.y > 0.51;\n  float barLeft = 1.0;\n  float barRight = 1.0;\n  barLeft *= left; barRight *= right;\n"
                                 "  float picked = pick(base, other) + fract(base.x) + mix(0.0, 1.0, 0.5) + barLeft * barRight;\n  gl_FragColor = vec4(vec3(picked), 1.0);\n}\n";
        std::string error;
        const auto t = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void aVertexShaderMayWriteToItsInputs()
    {
        const auto vert = source("uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                                 "void main() {\n  a_TexCoord.x = a_TexCoord.x * 2.0;\n  a_TexCoord.y += 0.5;\n  a_Position.z++;\n"
                                 "  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_TexCoord = a_TexCoord;\n}\n");
        const auto frag = source("varying vec2 v_TexCoord;\nvoid main() { gl_FragColor = vec4(v_TexCoord, 0.0, 1.0); }\n");
        std::string error;
        TranslateOptions strict;
        const auto text = translateToGlsl(vert, frag, strict, &error);
        QVERIFY2(text.has_value(), error.c_str());
        // the later reads see the written value: the shader's name is a global copied from the input at the top of main
        QVERIFY(text->vertex.find("we_in_a_TexCoord;") != std::string::npos);
        QVERIFY(text->vertex.find("a_TexCoord = we_in_a_TexCoord;") != std::string::npos);
        std::string diagnostics;
        QVERIFY2(compileGlsl(ShaderStage::Vertex, text->vertex, diagnostics).size() > 0, diagnostics.c_str());
        const auto t = translateShader(vert, frag, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        bool named = false; // the engine still feeds the attribute by its own name
        for (const auto &a : t->attributes) {
            named = named || a.name == "a_TexCoord";
        }
        QVERIFY(named);
    }

    void aDeclarationWithASwizzleForANameDeclaresTheName()
    {
        // frame_builder's fragment shader says `varying vec4 v_Size.xy;` where its vertex shader says `varying vec4 v_Size;`
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nvarying vec4 v_Size;\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n  v_Size = vec4(1.0, 2.0, 3.0, 4.0);\n}\n";
        const std::string frag = "varying vec4 v_Size.xy;\nvoid main() { gl_FragColor = vec4(v_Size.xy, v_Size.zw); }\n";
        std::string error;
        const auto t = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void anAttributeTheBodyNeverNamesIsNotAnInput()
    {
        // the `flat` shader declares a_Color for VERTEXCOLOR only: without the combo the draw has no colour to feed
        const std::string vert = "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec4 a_Color;\n#ifdef VERTEXCOLOR\nvarying vec4 v_Color;\n#endif\n"
                                 "void main() {\n  gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);\n#ifdef VERTEXCOLOR\n  v_Color = a_Color;\n#endif\n}\n";
        const std::string frag = "uniform float g_Alpha;\nuniform vec3 g_Color;\nvoid main() { gl_FragColor = vec4(g_Color, g_Alpha); }\n";
        std::string error;
        const auto plain = translateShader(source(vert), source(frag), {}, &error);
        QVERIFY2(plain.has_value(), error.c_str());
        QCOMPARE(plain->attributes.size(), size_t{1});
        QCOMPARE(plain->attributes[0].name, std::string("a_Position"));
        // with the combo the colour is used, so it is an input
        TranslateOptions options;
        options.combos["VERTEXCOLOR"] = 1;
        const auto colored = translateShader(source(vert), source("varying vec4 v_Color;\n" + frag), options, &error);
        QVERIFY2(colored.has_value(), error.c_str());
        QCOMPARE(colored->attributes.size(), size_t{2});
    }

    void macroBombIsStoppedAndCached()
    {
#ifdef __SANITIZE_ADDRESS__
        QSKIP("the compiler's CPU and memory limits belong to the forked child, which the asan preset does not use");
#endif
        std::string bomb = "#define L0 1.0\n";
        for (int i = 1; i <= 22; ++i)
            bomb += "#define L" + std::to_string(i) + " (L" + std::to_string(i - 1) + "+L" + std::to_string(i - 1) + "+L" + std::to_string(i - 1) + ")\n";
        bomb += "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nvoid main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * L22; }\n";
        const auto vert = source("attribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
                                 "void main() { gl_Position = vec4(a_Position, 1.0); v_TexCoord = a_TexCoord; }\n");
        const auto frag = source(bomb);

        QElapsedTimer timer;
        timer.start();
        std::string error;
        QVERIFY(!translateShader(vert, frag, {}, &error).has_value());
        QVERIFY2(timer.elapsed() < 25000, "the compiler must be bounded");
        QVERIFY2(error.find("limits") != std::string::npos || error.find("too large") != std::string::npos, error.c_str());

        // the second attempt is answered by the cached failure, quickly
        timer.restart();
        std::string again;
        QVERIFY(!translateShader(vert, frag, {}, &again).has_value());
        QVERIFY(timer.elapsed() < 2000);
        QVERIFY(!again.empty());
    }

    void ordinaryShadersStillCompileInTheSandbox()
    {
        std::string error;
        const auto t = translateShader(source(kVert), source(kFrag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        QVERIFY(!t->vertex.spirv.empty());
    }

    // ---------------- the compatibility shim ----------------

    void theShimHoistsGlobalInitializersThatReadUniforms()
    {
        const auto v = source("uniform mat4 g_ModelViewProjectionMatrix;\nuniform float u_Balance;\nattribute vec3 a_Position;\n"
                              "const float kHalf = 0.5;\nconst float kScaled = kHalf * 2.0;\n"
                              "const vec2 kMix = vec2(u_Balance, 1.0 - u_Balance);\nconst float kRatio = kMix.x / kMix.y;\n"
                              "void main() { gl_Position = mul(vec4(a_Position * kRatio * kScaled, 1.0), g_ModelViewProjectionMatrix); }\n");
        const auto f = source("void main() { gl_FragColor = vec4(1.0); }\n// shim hoist\n");
        std::string error;
        // not valid as it stands (a global initializer reads a uniform), valid through the shim
        const auto t = translateShader(v, f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        QVERIFY(!t->vertex.spirv.empty());
    }

    void theShimReadsMarkedFloatUniformsAsIntegersAndNarrowsWiderTypes()
    {
        const auto v = source("uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
                              "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord.xyxy; }\n");
        const auto f = source("uniform sampler2D g_Texture0;\nuniform float u_Count; // {\"material\":\"count\",\"int\":true,\"default\":3}\n"
                              "uniform vec2 g_PointerPosition;\nvarying vec4 v_TexCoord;\n"
                              "void main() {\n"
                              "  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n"           // a vec4 coordinate
                              "  float p = g_PointerPosition * 0.5;\n"                     // a vec2 into a float
                              "  vec3 narrow = vec4(c.rgb, 1.0);\n"                        // a vec4 into a vec3
                              "  for (int i = u_Count; i < 6; ++i) { c.rgb += narrow * p; }\n"  // an int from a float uniform
                              "  gl_FragColor = vec4(max(0, c.rgb), c.a);\n"               // a scalar first operand
                              "}\n");
        std::string error;
        const auto t = translateShader(v, f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        QVERIFY(!t->fragment.spirv.empty());
    }

    void theStrictCompileFailsFirstAndTheShimIsWhatMakesItCompile()
    {
        const auto v = source("uniform mat4 g_ModelViewProjectionMatrix;\nuniform float u_Strict;\nattribute vec3 a_Position;\n"
                              "const float kFromUniform = u_Strict * 2.0;\n"
                              "void main() { gl_Position = mul(vec4(a_Position * kFromUniform, 1.0), g_ModelViewProjectionMatrix); }\n");
        const auto f = source("void main() { gl_FragColor = vec4(max(0, vec3(0.5)), 1.0); } // strict-first\n");
        std::string error;
        TranslateOptions strict;
        const auto strictText = translateToGlsl(v, f, strict, &error);
        QVERIFY2(strictText.has_value(), error.c_str());
        std::string diagnostics;
        QVERIFY(compileGlsl(ShaderStage::Vertex, strictText->vertex, diagnostics).empty()); // the strict vertex shader does not compile
        QVERIFY(diagnostics.find("ERROR") != std::string::npos);
        QVERIFY(compileGlsl(ShaderStage::Fragment, strictText->fragment, diagnostics).empty()); // nor the fragment one (max(0, vec3))
        TranslateOptions lenient;
        lenient.lenient = true;
        const auto shimmed = translateToGlsl(v, f, lenient, &error);
        QVERIFY2(shimmed.has_value(), error.c_str());
        QVERIFY(shimmed->vertex.find("we_to_") != std::string::npos);
        QVERIFY(!compileGlsl(ShaderStage::Vertex, shimmed->vertex, diagnostics).empty());
        QVERIFY2(!compileGlsl(ShaderStage::Fragment, shimmed->fragment, diagnostics).empty(), diagnostics.c_str());
        // and translateShader gets there by itself
        QVERIFY2(translateShader(v, f, {}, &error).has_value(), error.c_str());
    }

    void everyEngineInputSurvivesTheCacheRoundTrip()
    {
        // a puppet vertex shader: g_Bones (the last EngineInput) must read back from the cache as what it was
        const auto v = source("uniform mat4 g_ModelViewProjectionMatrix;\nuniform mat4x3 g_Bones[4];\nattribute vec3 a_Position;\nattribute uvec4 a_BlendIndices;\n"
                              "void main() { gl_Position = mul(vec4(mul(vec4(a_Position, 1.0), g_Bones[a_BlendIndices.x]), 1.0), g_ModelViewProjectionMatrix); } // cache round trip\n");
        const auto f = source("void main() { gl_FragColor = vec4(1.0); }\n");
        std::string error;
        const auto fresh = translateShader(v, f, {}, &error);
        QVERIFY2(fresh.has_value(), error.c_str());
        const auto cached = translateShader(v, f, {}, &error); // the second call is answered by the cache file
        QVERIFY2(cached.has_value(), error.c_str());
        QCOMPARE(cached->vertex.uniforms.size(), fresh->vertex.uniforms.size());
        for (size_t i = 0; i < fresh->vertex.uniforms.size(); ++i)
            QCOMPARE(static_cast<int>(cached->vertex.uniforms[i].input), static_cast<int>(fresh->vertex.uniforms[i].input));
        bool bones = false;
        for (const UniformInfo &u : cached->vertex.uniforms)
            bones = bones || (u.name == "g_Bones" && u.input == EngineInput::Bones && u.arrayCount == 4);
        QVERIFY(bones);
    }

    void theShimTruncatesTheArgumentsOfWEsOwnHelpers()
    {
        // HLSL passes a vec4 where a helper takes a vec2 or a vec3 (effects/shimmer calls rotateVec2(v_TexCoord, ...) with a vec4)
        const auto v = source("uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
                              "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord.xyxy; }\n");
        const auto f = source("#include \"common.h\"\nvarying vec4 v_TexCoord;\n"
                              "void main() { vec2 r = rotateVec2(v_TexCoord, 0.5); float g = greyscale(v_TexCoord); gl_FragColor = vec4(r, g, 1.0); } // helper args\n",
                              {{"common.h", "vec2 rotateVec2(vec2 v, float r) { vec2 cs = vec2(cos(r), sin(r)); return vec2(v.x * cs.x - v.y * cs.y, v.x * cs.y + v.y * cs.x); }\n"
                                            "float greyscale(vec3 color) { return dot(color, vec3(0.11, 0.59, 0.3)); }\n"}});
        std::string error;
        const auto t = translateShader(v, f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        QVERIFY(!t->fragment.spirv.empty());
    }

    void theShimLeavesValidShadersAlone()
    {
        // a shader that compiles strictly is the strict translation (the shim's helper functions are not in it)
        const auto v = source(kVert);
        const auto f = source(kFrag);
        TranslateOptions strict;
        std::string error;
        const auto text = translateToGlsl(v, f, strict, &error);
        QVERIFY2(text.has_value(), error.c_str());
        QVERIFY(text->fragment.find("we_to_") == std::string::npos);
        // and a shader the shim cannot rescue still fails with the compiler's message
        const auto broken = source("void main() { gl_FragColor = nothingDefined; }\n");
        QVERIFY(!translateShader(v, broken, {}, &error).has_value());
        QVERIFY(error.find("nothingDefined") != std::string::npos);
    }

    // the rules of the shim each have a shader of their own: one habit per shader, so a rule that stops working is named by its test

    void theShimDropsHlslLoopAttributesAndFunctionQualifiers()
    {
        // `[loop]`, `[unroll(4)]` and `[branch]` in front of statements (effects/audio_yuan), and `const` on a function's return type that
        // also redefines a built-in (effects/light_pillar)
        const auto f = source("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\nconst float fract(float x) {\n  return x - floor(x);\n}\n"
                              "inline float twice(float x) { return x * 2.0; }\n"
                              "void main() {\n  float sum = 0.0;\n  [loop]\n  for (int i = 0; i < 4; i++) { sum += fract(float(i) * 0.3); }\n"
                              "  [unroll(4)] for (int j = 0; j < 4; j++) { sum += 0.1; }\n  [branch] if (sum > 1.0) { sum = twice(sum); }\n"
                              "  gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * sum; // loop attributes\n}\n");
        std::string error;
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        // `const` on a variable stays: only a function loses it
        TranslateOptions lenient;
        lenient.lenient = true;
        const auto text = translateToGlsl(source(kPlainVert), source("const float kHalf = 0.5;\nvoid main() { gl_FragColor = vec4(kHalf); }\n"), lenient, &error);
        QVERIFY2(text.has_value(), error.c_str());
        QVERIFY(text->fragment.find("const float kHalf") != std::string::npos);
    }

    void theShimCutsTokensAfterAConditionalDirective()
    {
        // `#elif AUDIOSAMPLES == 32;` (effects/simple_gradient_audio_bar): WE's preprocessor ignores the semicolon, glslang does not
        const auto f = source("// [COMBO] {\"material\":\"n\",\"combo\":\"AUDIOSAMPLES\",\"default\":32}\n"
                              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                              "#if AUDIOSAMPLES == 16;\n#define BARS 16.0\n#elif AUDIOSAMPLES == 32; // the one\n#define BARS 32.0\n#else ;\n#define BARS 64.0\n#endif ;\n"
                              "#ifdef BARS extra\n#define HAVE_BARS 1\n#endif\n"
                              "void main() { gl_FragColor = texSample2D(g_Texture0, v_TexCoord) * BARS * float(HAVE_BARS); } // directive tails\n");
        std::string error;
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        TranslateOptions lenient;
        lenient.lenient = true;
        const auto text = translateToGlsl(source(kPlainVert), f, lenient, &error);
        QVERIFY2(text.has_value(), error.c_str());
        QVERIFY(text->fragment.find("32.0") != std::string::npos); // the #elif was taken, the condition was kept
        QVERIFY(text->fragment.find("64.0") == std::string::npos);
    }

    void theShimReadsASubscriptOfAScalarAsTheScalar()
    {
        // `g_AudioSpectrum64Left[barID / 4][barID % 4]` of a float array with a float index (effects/video), and a subscript of a plain float
        const auto f = source("varying vec2 v_TexCoord;\nuniform float g_AudioSpectrum64Left[64];\n"
                              "float volume(float barID) {\n  return clamp(0., 1., g_AudioSpectrum64Left[barID / 4][barID % 4]);\n}\n"
                              "void main() {\n  float level = v_TexCoord.x;\n  float picked = level[0];\n"
                              "  gl_FragColor = vec4(volume(level * 63.0) + picked); // scalar subscripts\n}\n");
        std::string error;
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        // and a vector's subscript is not touched by it: the shader still fails on what is really wrong
        const auto broken = source("varying vec2 v_TexCoord;\nvoid main() { vec4 c = vec4(1.0); gl_FragColor = vec4(c[0][1]) + nowhere; }\n");
        QVERIFY(!translateShader(source(kPlainVert), broken, {}, &error).has_value());
    }

    void theShimCutsAWiderParenthesisedOperand()
    {
        // `v_TexCoord.xy - (u_rOffset * timer + pointer)` where `timer` is a vec4 (effects/chromatic_aberration), and the call form
        const auto f = source("varying vec4 v_TexCoord;\nuniform sampler2D g_Texture0;\nuniform float u_rOffset;\nuniform vec2 g_PointerPosition;\n"
                              "vec4 wide(float x) { return vec4(x); }\n"
                              "void main() {\n  vec4 timer = texSample2D(g_Texture0, v_TexCoord.xy);\n  float pointer = g_PointerPosition.x;\n"
                              "  vec4 r = texSample2D(g_Texture0, v_TexCoord.xy - (u_rOffset * timer + pointer));\n"
                              "  vec2 shifted = v_TexCoord.xy + wide(0.5);\n"
                              "  gl_FragColor = r + vec4(shifted, 0.0, 0.0); // parenthesised operands\n}\n");
        const auto v = source("uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec4 v_TexCoord;\n"
                              "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord.xyxy; }\n");
        std::string error;
        const auto t = translateShader(v, f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void theShimConvertsAReturnedValueToTheFunctionsType()
    {
        // `vec3 drawScene(vec2 uv) { vec4 col = ...; return col; }` (effects/video): HLSL converts what a function returns to its type
        const auto f = source("varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                              "vec3 drawScene(vec2 uv)\n{\n  vec4 col = vec4(uv, 0.0, 1.0);\n  for (int i = 0; i < 2; ++i) {\n    col.r += 0.1;\n  }\n  return col;\n}\n"
                              "float level(vec2 uv) { vec2 both = uv * 2.0;\n  return both;\n}\n"
                              "void main() { gl_FragColor = vec4(drawScene(v_TexCoord) * level(v_TexCoord), 1.0); } // returns\n");
        std::string error;
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void theShimEndsAFunctionWhoseBodyIfsDownToNothing()
    {
        // every branch of the body is behind a combo that is off: HLSL accepts the function and nothing calls it
        const auto f = source("// [COMBO] {\"material\":\"Mask\",\"combo\":\"MASK\",\"type\":\"options\",\"default\":0,\"options\":{\"None\":0,\"On\":1}}\n"
                              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                              "vec3 pickMask(vec2 uv)\n{\n#if MASK == 1\n  return vec3(uv, 0.0);\n#endif\n}\n"
                              "void main() {\n  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
                              "#if MASK != 0\n  c.rgb *= pickMask(v_TexCoord);\n#endif\n  gl_FragColor = c;\n}\n");
        std::string error;
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void theShimEndsTheOverloadThatIsBrokenAndSkipsCommentedBraces()
    {
        // the first overload returns; the second one is empty under the combo, and a comment in it holds a stray brace
        const auto f = source("// [COMBO] {\"material\":\"Mask\",\"combo\":\"MASK\",\"type\":\"options\",\"default\":0,\"options\":{\"None\":0,\"On\":1}}\n"
                              "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
                              "float pick(float x)\n{\n  return x;\n}\n"
                              "float pick(vec2 uv)\n{\n  // a brace in a comment {\n#if MASK == 1\n  return uv.x;\n#endif\n}\n"
                              "void main() {\n  vec4 c = texSample2D(g_Texture0, v_TexCoord);\n"
                              "#if MASK != 0\n  c.r *= pick(v_TexCoord);\n#endif\n  gl_FragColor = c * pick(c.g);\n}\n");
        std::string error;
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
    }

    void aFragmentShaderReadsGlPositionAsItsWindowPosition()
    {
        // HLSL's pixel shader input SV_Position is the pixel's window position (effects/light_pillar reads `gl_Position.xy` for noise)
        const auto f = source("varying vec2 v_TexCoord;\nfloat noise(vec2 p) { return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453); }\n"
                              "void main() { gl_FragColor = vec4(noise(gl_Position.xy) + v_TexCoord.x); } // window position\n");
        std::string error;
        TranslateOptions strict;
        const auto text = translateToGlsl(source(kPlainVert), f, strict, &error);
        QVERIFY2(text.has_value(), error.c_str());
        QVERIFY(text->fragment.find("gl_FragCoord.xy") != std::string::npos);
        QVERIFY(text->vertex.find("gl_Position") != std::string::npos); // the vertex stage keeps its own
        QVERIFY2(translateShader(source(kPlainVert), f, {}, &error).has_value(), error.c_str());
    }

    void requireLightingV1WithoutThePbrHeaderIsTheSumOverNoLights()
    {
        // `#require LightingV1` is where the engine writes PerformLighting_V1; a shader that does not include common_pbr_2.h has no
        // light terms to add, so its lighting is its ambient colour, which the engine fills
        const auto f = source("varying vec2 v_TexCoord;\nuniform vec3 g_LightAmbientColor;\nuniform vec3 g_LightSkylightColor;\n#if LIGHTING\n#require LightingV1\n#endif\n"
                              "// [COMBO] {\"material\":\"l\",\"combo\":\"LIGHTING\",\"default\":1}\n"
                              "void main() {\n  vec3 light = PerformLighting_V1(vec3(0.0), vec3(1.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.0, 0.0);\n"
                              "  gl_FragColor = vec4(g_LightAmbientColor + g_LightSkylightColor * 0.0 + light, 1.0); // lighting v1\n}\n");
        std::string error;
        TranslateOptions strict;
        const auto text = translateToGlsl(source(kPlainVert), f, strict, &error);
        QVERIFY2(text.has_value(), error.c_str());
        QVERIFY(text->fragment.find("PerformLighting_V1(vec3 worldPos") != std::string::npos);
        std::string diagnostics;
        QVERIFY2(!compileGlsl(ShaderStage::Fragment, text->fragment, diagnostics).empty(), diagnostics.c_str()); // no shim needed
        const auto t = translateShader(source(kPlainVert), f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        bool ambient = false, skylight = false;
        for (const UniformInfo &u : t->fragment.uniforms) {
            ambient = ambient || (u.name == "g_LightAmbientColor" && u.input == EngineInput::LightAmbientColor);
            skylight = skylight || (u.name == "g_LightSkylightColor" && u.input == EngineInput::LightSkylightColor);
        }
        QVERIFY(ambient && skylight);
    }

    void requireLightingV1FindsThePbrHeaderThroughNestedIncludes()
    {
        // the shipped shaders reach common_pbr_2.h directly; one included by another header counts the same
        const std::map<std::string, std::string> headers = {
            {"common_pbr_2.h",
             "vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 a, vec3 c, float r, float e, vec3 t, vec3 f, float ro, float m, float s) { return c; }\n"
             "vec3 ComputePBRLightShadowInfinite(vec3 N, vec3 L, vec3 V, vec3 a, vec3 c, vec3 t, vec3 f, float ro, float m, float s) { return c; }\n"},
            {"outer.h", "#include \"common_pbr_2.h\"\n"}};
        std::string error;
        const auto f = parseShaderSource("t.frag",
                                         "#include \"outer.h\"\nvarying vec2 v_TexCoord;\n#require LightingV1\n"
                                         "void main() { gl_FragColor = vec4(PerformLighting_V1(vec3(0.0), vec3(1.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.5, 0.0), 1.0); }\n",
                                         [&](const std::string &, const std::string &name) -> std::optional<std::string> {
                                             const auto it = headers.find(name);
                                             return it == headers.end() ? std::nullopt : std::optional<std::string>(it->second);
                                         },
                                         &error);
        QVERIFY2(f.has_value(), error.c_str());
        TranslateOptions options;
        options.combos = {{"LIGHTS_SPOT", 2}, {"LIGHTS_TUBE", 1}};
        const auto t = translateShader(source(kPlainVert), *f, options, &error);
        QVERIFY2(t.has_value(), error.c_str());
        bool spots = false, tubes = false;
        for (const UniformInfo &u : t->fragment.uniforms) {
            spots = spots || (u.name == "g_LSpot_Exponent" && u.input == EngineInput::LSpotExponent && u.arrayCount == 2);
            tubes = tubes || (u.name == "g_LTube_OriginB" && u.input == EngineInput::LTubeOriginB && u.arrayCount == 1);
        }
        QVERIFY(spots && tubes); // the engine's function and its arrays, not the empty one
    }

    void aShadowCasterThatPicksItsViewportByInstanceTranslates()
    {
        // WE's shadow casters declare the built-ins they use and send each instance to its own viewport; the engine draws one viewport
        // at a time, the instance index naming the matrix
        const auto v = source("uniform mat4 g_ModelMatrix;\nuniform mat4 g_ViewportViewProjectionMatrices[6];\nin uint gl_InstanceID;\nvarying uint gl_ViewportIndex;\n"
                              "attribute vec3 a_Position;\n"
                              "void main() {\n  gl_Position = mul(mul(vec4(a_Position, 1.0), g_ModelMatrix), g_ViewportViewProjectionMatrices[gl_InstanceID]);\n"
                              "  gl_ViewportIndex = gl_InstanceID;\n}\n");
        const auto f = source("void main() {\n}\n");
        std::string error;
        const auto t = translateShader(v, f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        bool matrices = false;
        for (const UniformInfo &u : t->vertex.uniforms) {
            matrices = matrices || (u.name == "g_ViewportViewProjectionMatrices" && u.input == EngineInput::ViewportViewProjections && u.arrayCount == 6);
        }
        QVERIFY(matrices);
    }

    void theShadowCombosGiveTheLightingFunctionItsShadowArrays()
    {
        const std::map<std::string, std::string> headers = {
            {"common_pbr_2.h",
             "vec3 ComputePBRLightShadow(vec3 N, vec3 L, vec3 V, vec3 a, vec3 c, float r, float e, vec3 t, vec3 f, float ro, float m, float s) { return c * s; }\n"
             "vec3 ComputePBRLightShadowInfinite(vec3 N, vec3 L, vec3 V, vec3 a, vec3 c, vec3 t, vec3 f, float ro, float m, float s) { return c * s; }\n"
             "#ifdef SHADOW_ATLAS_SAMPLER\nfloat PerformShadowMapping(vec3 p, vec4 t) { return texSample2DCompare(SHADOW_ATLAS_SAMPLER, p.xy * t.zw + t.xy, p.z).r; }\n"
             "float PerformPointShadowMapping(vec4 p) { return texSample2DCompare(SHADOW_ATLAS_SAMPLER, p.xy, p.z).r; }\n#endif\n"
             "vec3 CalculateProjectedCoords(vec3 p, mat4 m) { vec4 c = mul(vec4(p, 1.0), m); return c.xyz / c.w; }\n"
             "vec4 CalculateProjectedCoordsCascades(vec3 p, mat4 m) { vec4 c = mul(vec4(p, 1.0), m); return vec4(c.xyz / c.w, 0.0); }\n"
             "vec4 CalculateProjectedCoordsPoint(vec3 p, vec3 o, vec4 i, vec4 t) { return vec4(p - o, 1.0) * i + t; }\n"}};
        std::string error;
        const auto f = parseShaderSource("t.frag",
                                         "#if LIGHTS_SHADOW_MAPPING\n#define SHADOW_ATLAS_SAMPLER g_Texture6\nuniform sampler2DComparison g_Texture6; // {\"default\":\"_rt_shadowAtlas\"}\n#endif\n"
                                         "#include \"common_pbr_2.h\"\nvarying vec2 v_TexCoord;\n#require LightingV1\n"
                                         "void main() { gl_FragColor = vec4(PerformLighting_V1(vec3(0.0), vec3(1.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.5, 0.0), 1.0); }\n",
                                         [&](const std::string &, const std::string &name) -> std::optional<std::string> {
                                             const auto it = headers.find(name);
                                             return it == headers.end() ? std::nullopt : std::optional<std::string>(it->second);
                                         },
                                         &error);
        QVERIFY2(f.has_value(), error.c_str());
        TranslateOptions options;
        options.combos = {{"LIGHTS_POINT", 2}, {"LIGHTS_SPOT", 1}, {"LIGHTS_DIRECTIONAL", 1}, {"LIGHTS_SHADOW_MAPPING", 1}, {"LIGHTS_SHADOW_MAPPING_QUALITY", 2},
                          {"LIGHTS_POINT_SHADOW", 1}, {"LIGHTS_SPOT_SHADOW", 1}, {"LIGHTS_DIRECTIONAL_SHADOW", 1}};
        const auto t = translateShader(source(kPlainVert), *f, options, &error);
        QVERIFY2(t.has_value(), error.c_str());
        std::map<std::string, std::pair<EngineInput, uint32_t>> found;
        for (const UniformInfo &u : t->fragment.uniforms) {
            found[u.name] = {u.input, u.arrayCount};
        }
        QVERIFY(found["g_LFeature_ShadowPointProjection"] == std::make_pair(EngineInput::ShadowPointProjection, 1u));
        QVERIFY(found["g_LFeature_ShadowPointProjectionTransform"] == std::make_pair(EngineInput::ShadowPointProjectionTransform, 1u));
        QVERIFY(found["g_LFeature_ShadowProjection"] == std::make_pair(EngineInput::ShadowProjection, 1u));
        QVERIFY(found["g_LFeature_ShadowCascadeProjection"] == std::make_pair(EngineInput::ShadowCascadeProjection, 6u));
        // the normal offset of the cascades: one world texel size a cascade, read by the lighting function (translator version 42)
        QVERIFY(found["g_LFeature_ShadowCascadeTexel"] == std::make_pair(EngineInput::ShadowCascadeTexel, 6u));
        QVERIFY(kTranslatorVersion >= 42);
        bool atlas = false;
        for (const SamplerInfo &sampler : t->samplers) {
            atlas = atlas || (sampler.binding == 6 && sampler.type == "sampler2DShadow" && sampler.defaultText == "_rt_shadowAtlas");
        }
        QVERIFY(atlas);
        // without the atlas's sampler (a shader that does not declare it) the same combos add no shadow arrays
        const auto plain = parseShaderSource("u.frag",
                                             "#include \"common_pbr_2.h\"\nvarying vec2 v_TexCoord;\n#require LightingV1\n"
                                             "void main() { gl_FragColor = vec4(PerformLighting_V1(vec3(0.0), vec3(1.0), vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0), vec3(1.0), vec3(0.04), 0.5, 0.0), 1.0); }\n",
                                             [&](const std::string &, const std::string &name) -> std::optional<std::string> {
                                                 const auto it = headers.find(name);
                                                 return it == headers.end() ? std::nullopt : std::optional<std::string>(it->second);
                                             },
                                             &error);
        QVERIFY2(plain.has_value(), error.c_str());
        const auto u = translateShader(source(kPlainVert), *plain, options, &error);
        QVERIFY2(u.has_value(), error.c_str());
        for (const UniformInfo &info : u->fragment.uniforms) {
            QVERIFY2(info.name.rfind("g_LFeature_", 0) != 0, info.name.c_str());
        }
    }

    void eachSourceIsCompiledOncePerProcess()
    {
        // a scene names one shader from many layers, and the shim retries a failing one: the compiler runs once per text, success or not
        const std::string good = "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0.125); } // once\n";
        const std::string bad = "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = notDeclaredAnywhere; } // once\n";
        std::string error;
        const uint64_t before = glslCompilerRuns();
        QVERIFY(!compileGlsl(ShaderStage::Fragment, good, error).empty());
        QVERIFY(compileGlsl(ShaderStage::Fragment, bad, error).empty());
        QCOMPARE(glslCompilerRuns(), before + 2);
        error.clear();
        QVERIFY(!compileGlsl(ShaderStage::Fragment, good, error).empty());
        QVERIFY(compileGlsl(ShaderStage::Fragment, bad, error).empty());
        QVERIFY(error.find("notDeclaredAnywhere") != std::string::npos); // the remembered failure keeps its message
        QCOMPARE(glslCompilerRuns(), before + 2);
        // the stage is part of what is remembered
        compileGlsl(ShaderStage::Vertex, good, error);
        QCOMPARE(glslCompilerRuns(), before + 3);
    }

    void theCompilerRunsInAProcessThatHasMappedMoreThanItsAllowance()
    {
#ifdef __SANITIZE_ADDRESS__
        QSKIP("the asan preset compiles in this process");
#endif
        // a scene with video textures decoded by VAAPI has over 3 GiB mapped before its first shader: the forked compiler's address
        // space limit is what it may add, not a total under what it already has (2907572744 failed every shader)
        const size_t reserve = size_t{3} << 30;
        void *reserved = ::mmap(nullptr, reserve, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        QVERIFY(reserved != MAP_FAILED);
        std::string error;
        const std::string text = "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(0.75); } // a big parent\n";
        const bool compiled = !compileGlsl(ShaderStage::Fragment, text, error).empty();
        const std::string preprocessed = preprocessGlsl(ShaderStage::Vertex, "void main() { gl_Position = vec4(0.25); } // a big parent\n", error);
        ::munmap(reserved, reserve);
        QVERIFY2(compiled, error.c_str());
        QVERIFY2(!preprocessed.empty(), error.c_str());
    }

    void aForkedCompileIsCheap()
    {
#ifdef __SANITIZE_ADDRESS__
        QSKIP("the asan preset compiles in this process");
#endif
        // the built-in symbol tables are built once in this process and inherited by every forked compiler: twenty distinct small
        // shaders take well under the second each one cost when the child built them
        QElapsedTimer timer;
        timer.start();
        std::string error;
        for (int i = 0; i < 20; ++i) {
            const std::string text = "#version 450\nlayout(location = 0) out vec4 o;\nvoid main() { o = vec4(" + std::to_string(i) + ".0 / 7.0); } // cheap\n";
            QVERIFY2(!compileGlsl(ShaderStage::Fragment, text, error).empty(), error.c_str());
        }
        QVERIFY2(timer.elapsed() < 6000, std::to_string(timer.elapsed()).c_str());
    }

    // ---------------- source ----------------

    void includesAreExpandedOnce()
    {
        const auto s = source("#include \"a.h\"\n#include \"b.h\"\n#include \"a.h\"\nmain\n",
                              {{"a.h", "AAA\n#include \"b.h\"\n"}, {"b.h", "BBB\n"}});
        QCOMPARE(std::count(s.text.begin(), s.text.end(), 'A'), 3 + 0); // "AAA" once; no other capital A in the text
        const size_t first = s.text.find("BBB");
        QVERIFY(first != std::string::npos);
        QCOMPARE(s.text.find("BBB", first + 1), std::string::npos); // b.h once, though included three times
        QCOMPARE(s.includes.size(), size_t{2});
        QVERIFY(s.text.find("already included") != std::string::npos);
    }

    void includeErrors()
    {
        std::string error;
        QVERIFY(!parseShaderSource("t", "#include \"missing.h\"\n", headers({}), &error).has_value());
        QVERIFY(error.find("missing.h") != std::string::npos);
        QVERIFY(!parseShaderSource("t", "#include broken\n", headers({}), &error).has_value());

        // a chain deeper than the limit
        std::map<std::string, std::string> chain;
        for (int i = 0; i < 40; ++i)
            chain["h" + std::to_string(i) + ".h"] = "#include \"h" + std::to_string(i + 1) + ".h\"\n";
        chain["h40.h"] = "end\n";
        QVERIFY(!parseShaderSource("t", "#include \"h0.h\"\n", headers(chain), &error).has_value());
        QVERIFY(error.find("deeper") != std::string::npos);

        // a self-including header is harmless (it is expanded once)
        QVERIFY(parseShaderSource("t", "#include \"loop.h\"\n", headers({{"loop.h", "#include \"loop.h\"\nx\n"}}), &error).has_value());

        // size limits
        const std::string big(2u << 20, 'x');
        QVERIFY(!parseShaderSource("t", big, headers({}), &error).has_value());
        QVERIFY(error.find("1 MiB") != std::string::npos);
    }

    void annotationsAreParsed()
    {
        const auto s = source(
            "// [COMBO] {\"material\":\"Lighting\",\"combo\":\"LIGHTING\",\"default\":1}\n"
            "//[COMBO_OFF] {\"combo\":\"SPEC\",\"default\":0}\n"
            "// [OFF_COMBO] {\"combo\":\"POS\",\"default\":2}\n"
            "// [COMBO_DISABLED] {\"combo\":\"DS\"}\n"
            "// [PASS] shadow shadowcaster\n"
            "uniform float g_A; // {\"material\":\"alpha\",\"default\":1,\"range\":[0,1]}\n"
            "uniform sampler2D g_Texture2; // {\"combo\":\"PBRMASKS\",\"label\":\"x\"}\n"
            "uniform vec3 g_Plain;\n"
            "#require LightingV1\n"
            "void main() {}\n");
        QCOMPARE(s.combos.size(), size_t{5}); // LIGHTING, SPEC, POS, DS and PBRMASKS from the sampler
        QCOMPARE(s.combos[0].name, std::string("LIGHTING"));
        QCOMPARE(s.combos[0].defaultValue, 1);
        QVERIFY(s.combos[0].exposed);
        QVERIFY(!s.combos[1].exposed);
        QCOMPARE(s.combos[2].defaultValue, 2);
        QCOMPARE(s.combos[4].name, std::string("PBRMASKS"));
        QCOMPARE(s.extraPasses.size(), size_t{1});
        QCOMPARE(s.requires_.size(), size_t{1});
        QCOMPARE(s.requires_[0], std::string("LightingV1"));
        QCOMPARE(s.text.find("\n#require"), std::string::npos); // not passed on as a directive
        QVERIFY(s.notes.count("g_A"));
        QCOMPARE(s.notes.at("g_A").stringOr("material"), std::string("alpha"));
        QVERIFY(!s.notes.count("g_Plain"));
        QVERIFY(s.notes.count("g_Texture2"));
    }

    // ---------------- translation ----------------

    void combosResolveInTheDocumentedOrder()
    {
        const ShaderSource v = source(kVert);
        const ShaderSource f = source(kFrag);
        auto combos = resolveCombos(v, f, {});
        QCOMPARE(combos.at("MASK"), 0);
        TranslateOptions bound;
        bound.boundTextures = {1};
        QCOMPARE(resolveCombos(v, f, bound).at("MASK"), 1); // a texture on g_Texture1 turns it on
        bound.combos["mask"] = 0;                           // an explicit value (any case) wins over the texture
        QCOMPARE(resolveCombos(v, f, bound).at("MASK"), 0);
        TranslateOptions extra;
        extra.combos["Custom"] = 7;
        QCOMPARE(resolveCombos(v, f, extra).at("CUSTOM"), 7);
    }

    void translationShapesTheInterface()
    {
        std::string error;
        TranslateOptions options;
        options.boundTextures = {0, 1};
        const auto t = translateToGlsl(source(kVert), source(kFrag), options, &error);
        QVERIFY2(t.has_value(), error.c_str());
        const std::string &v = t->vertex;
        const std::string &f = t->fragment;
        QVERIFY(v.rfind("#version 450", 0) == 0);
        QVERIFY(v.find("layout(std140, set = 0, binding = 0) uniform WeVertexUniforms") != std::string::npos);
        QVERIFY(v.find("void we_main()") != std::string::npos);
        QVERIFY(v.find("(gl_Position.z + gl_Position.w) * 0.5") != std::string::npos);
        QVERIFY(v.find("layout(location = 0) in vec3 a_Position") != std::string::npos);
        QVERIFY(v.find("layout(location = 1) in vec2 a_TexCoord") != std::string::npos);
        // the varyings: outputs in declaration order, inputs by name
        QVERIFY(v.find("layout(location = 0) out vec4 v_TexCoord") != std::string::npos);
        QVERIFY(v.find("layout(location = 1) out vec3 v_Extra") != std::string::npos);
        QVERIFY(f.find("layout(location = 0) in vec4 v_TexCoord") != std::string::npos);
        QVERIFY(f.find("layout(location = 1) in vec3 v_Extra") != std::string::npos);
        QVERIFY(f.find("layout(set = 1, binding = 0) uniform sampler2D g_Texture0") != std::string::npos);
        QVERIFY(f.find("layout(set = 1, binding = 1) uniform sampler2D g_Texture1") != std::string::npos);
        QVERIFY(f.find("layout(location = 0) out vec4 we_FragColor") != std::string::npos);
        QVERIFY(f.find("gl_FragColor") == std::string::npos);
        QVERIFY(f.find("texSample2D") == std::string::npos); // macros are gone
        QVERIFY(f.find("layout(std140, set = 0, binding = 1) uniform WeFragmentUniforms") != std::string::npos);
        // reflection of the dialect-level facts
        QCOMPARE(t->layout.attributes.size(), size_t{2});
        QCOMPARE(t->layout.samplers.size(), size_t{2});
        QCOMPARE(t->layout.samplers[1].comboName, std::string("MASK"));
        QVERIFY(t->layout.samplers[0].hidden);
        QCOMPARE(t->layout.combos.at("MASK"), 1);
    }

    void combosSelectLiveDeclarations()
    {
        std::string error;
        const auto off = translateToGlsl(source(kVert), source(kFrag), {}, &error);
        QVERIFY2(off.has_value(), error.c_str());
        QCOMPARE(off->layout.vertex.uniforms.size(), size_t{2}); // no g_Texture1Resolution
        QVERIFY(off->fragment.find("v_TexCoord.zw") == std::string::npos); // the masked branch is gone
        TranslateOptions on;
        on.combos["MASK"] = 1;
        const auto with = translateToGlsl(source(kVert), source(kFrag), on, &error);
        QVERIFY2(with.has_value(), error.c_str());
        QCOMPARE(with->layout.vertex.uniforms.size(), size_t{3});
    }

    void uniformAnnotationsReachTheLayout()
    {
        std::string error;
        const auto t = translateToGlsl(source(kVert), source(kFrag), {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        const auto &u = t->layout.fragment.uniforms;
        QCOMPARE(u.size(), size_t{2});
        QCOMPARE(u[0].name, std::string("g_Strength"));
        QCOMPARE(u[0].materialKey, std::string("strength"));
        QCOMPARE(u[0].defaultText, std::string("0.5"));
        QCOMPARE(u[1].name, std::string("g_Tint"));
        QCOMPARE(u[1].defaultText, std::string("1 0.5 0"));
        QCOMPARE(u[1].offset, 16u); // a vec3 is aligned to 16
    }

    void std140OffsetsMatchTheCompiler()
    {
        const ShaderSource v = source(
            "uniform float a;\nuniform vec3 b;\nuniform float c;\nuniform vec2 d;\nuniform vec4 e;\nuniform mat4 f;\n"
            "uniform float g[3];\nuniform mat4x3 h[2];\nuniform vec3 i;\nuniform mat3 j;\nuniform uint k;\nuniform vec4 l[2];\n"
            "attribute vec3 a_Position;\n"
            "void main() { gl_Position = vec4(a_Position, 1.0) * f * e + vec4(a, b.x, c, d.x) + vec4(g[2], h[1][0][0], i.x, j[0][0] + float(k) + l[1].x); }\n");
        const ShaderSource f = source("void main() { gl_FragColor = vec4(1.0); }\n");
        std::string error;
        const auto t = translateShader(v, f, {}, &error);
        QVERIFY2(t.has_value(), error.c_str());
        const auto compiled = blockOffsets(t->vertex.spirv);
        QCOMPARE(compiled.size(), t->vertex.uniforms.size());
        for (size_t i = 0; i < compiled.size(); ++i) {
            QVERIFY2(compiled[i] == t->vertex.uniforms[i].offset,
                     qPrintable(QString("%1: ours %2, compiler %3").arg(t->vertex.uniforms[i].name.c_str()).arg(t->vertex.uniforms[i].offset).arg(compiled[i])));
        }
        QCOMPARE(t->vertex.blockSize % 16, 0u);
        const UniformInfo &last = t->vertex.uniforms.back();
        QVERIFY(t->vertex.blockSize >= last.offset + last.size);
    }

    void std140Table()
    {
        QCOMPARE(std140Of("float").size, 4u);
        QCOMPARE(std140Of("vec3").size, 12u);
        QCOMPARE(std140Of("vec3").alignment, 16u);
        QCOMPARE(std140Of("mat3").size, 48u);
        QCOMPARE(std140Of("mat4x3").size, 64u);
        QCOMPARE(std140Of("sampler2D").size, 0u);
    }

    void dialectHelpersCompile()
    {
        std::string error;
        const ShaderSource v = source(
            "uniform mat4 g_ModelViewProjectionMatrix;\nattribute vec3 a_Position;\nattribute vec2 a_TexCoord;\nvarying vec2 v_TexCoord;\n"
            "void main() { gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix); v_TexCoord = a_TexCoord; }\n");
        const ShaderSource f = source(
            "varying vec2 v_TexCoord;\nuniform sampler2D g_Texture0;\n"
            "void main() {\n"
            "  vec3 sample = vec3(0.2);\n"                      // a reserved word in GLSL 450
            "  float a = saturate(frac(v_TexCoord.x * 3.0)) + atan2(1.0, 2.0);\n"
            "  vec3 p = pow(sample, 2.2) + pow(sample, 2) + lerp(vec3(0.0), vec3(1.0), 0.5);\n"
            "  vec4 c = texSample2D(g_Texture0, v_TexCoord) + vec4(CAST3(a), 1.0) + vec4(p, 0.0);\n"
            "  gl_FragColor = c + vec4(ddx(a), ddy(a), rsqrt(a + 1.0), float(CASTF(2)));\n"
            "}\n");
        QVERIFY2(translateShader(v, f, {}, &error).has_value(), error.c_str());
    }

    void failuresCarryTheCompilerMessage()
    {
        std::string error;
        const ShaderSource v = source("attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1.0); }\n");
        const ShaderSource bad = source("void main() { gl_FragColor = undefined_symbol; }\n");
        QVERIFY(!translateShader(v, bad, {}, &error).has_value());
        QVERIFY2(error.find("undefined_symbol") != std::string::npos, error.c_str());
        // unsupported uniform type, missing main, broken array size
        QVERIFY(!translateShader(v, source("uniform dvec3 b;\nvoid main() { gl_FragColor = vec4(b.x); }\n"), {}, &error).has_value());
        QVERIFY(error.find("unsupported uniform type") != std::string::npos);
        QVERIFY(!translateToGlsl(source("attribute vec3 a_Position;\n"), bad, {}, &error).has_value());
        QVERIFY(error.find("no main") != std::string::npos);
        QVERIFY(!translateToGlsl(v, source("uniform float g[N];\nvoid main() { gl_FragColor = vec4(g[0]); }\n"), {}, &error).has_value());
    }

    // ---------------- cache ----------------

    void translatedShadersAreCachedWithTheirReflection()
    {
        std::string error;
        const ShaderSource v = source(kVert);
        const ShaderSource f = source(kFrag);
        TranslateOptions options;
        options.boundTextures = {1};
        options.combos["x"] = 3;
        const std::string key = shaderCacheKey(v, f, options.combos, options.boundTextures);
        QVERIFY(!loadCachedShader(key, &error).hit);

        const auto first = translateShader(v, f, options, &error);
        QVERIFY2(first.has_value(), error.c_str());
        const auto cached = loadCachedShader(key, &error);
        QVERIFY(cached.hit && cached.shader.has_value());
        QCOMPARE(cached.shader->vertex.spirv, first->vertex.spirv);
        QCOMPARE(cached.shader->fragment.spirv, first->fragment.spirv);
        QCOMPARE(cached.shader->fragment.uniforms.size(), first->fragment.uniforms.size());
        QCOMPARE(cached.shader->fragment.uniforms[1].defaultText, std::string("1 0.5 0"));
        QCOMPARE(cached.shader->vertex.blockSize, first->vertex.blockSize);
        QCOMPARE(cached.shader->samplers.size(), first->samplers.size());
        QCOMPARE(cached.shader->samplers[1].comboName, std::string("MASK"));
        QCOMPARE(cached.shader->attributes.size(), first->attributes.size());
        QCOMPARE(cached.shader->combos, first->combos);
        // the second call is a hit (same result)
        const auto second = translateShader(v, f, options, &error);
        QVERIFY(second.has_value());
        QCOMPARE(second->fragment.spirv, first->fragment.spirv);
    }

    void keysSeparateWhatMatters()
    {
        const ShaderSource v = source(kVert);
        const ShaderSource f = source(kFrag);
        const auto base = shaderCacheKey(v, f, {}, {});
        QCOMPARE(base, shaderCacheKey(v, f, {}, {}));
        QVERIFY(base != shaderCacheKey(v, f, {{"A", 1}}, {}));
        QVERIFY(base != shaderCacheKey(v, f, {}, {1}));
        QCOMPARE(shaderCacheKey(v, f, {{"a", 1}}, {}), shaderCacheKey(v, f, {{"A", 1}}, {})); // case-insensitive names
        QCOMPARE(shaderCacheKey(v, f, {}, {1, 2}), shaderCacheKey(v, f, {}, {2, 1}));
        QVERIFY(base != shaderCacheKey(f, v, {}, {})); // stages are not interchangeable
        QVERIFY(base != shaderCacheKey(source(kVert), source("void main() { gl_FragColor = vec4(0.0); }\n"), {}, {}));
    }

    void failuresAreRemembered()
    {
        std::string error;
        const ShaderSource v = source("attribute vec3 a_Position;\nvoid main() { gl_Position = vec4(a_Position, 1.0); }\n");
        const ShaderSource bad = source("void main() { gl_FragColor = broken_symbol_xyz; }\n");
        const std::string key = shaderCacheKey(v, bad, {}, {});
        QVERIFY(!translateShader(v, bad, {}, &error).has_value());
        const std::string firstError = error;
        error.clear();
        const auto remembered = loadCachedShader(key, &error);
        QVERIFY(remembered.hit);
        QVERIFY(!remembered.shader.has_value());
        QCOMPARE(error, firstError);
        error.clear();
        QVERIFY(!translateShader(v, bad, {}, &error).has_value()); // served from the cache
        QCOMPARE(error, firstError);
    }

    void damagedCacheFilesAreIgnored()
    {
        std::string error;
        const ShaderSource v = source(kVert);
        const ShaderSource f = source(kFrag);
        const std::string key = shaderCacheKey(v, f, {{"D", 1}}, {});
        const std::string path = shaderCacheDirectory() + "/" + key + ".bin";
        for (const std::string &junk : {std::string("garbage"), std::string("WSC1"), std::string("WSC1\x01\x00\x00\x00", 8),
                                        std::string("WSC1\x00\x00\x00\x00\xff\xff\xff\xff", 12)}) {
            QDir().mkpath(QString::fromStdString(shaderCacheDirectory()));
            FILE *file = std::fopen(path.c_str(), "wb");
            QVERIFY(file);
            std::fwrite(junk.data(), 1, junk.size(), file);
            std::fclose(file);
            TranslateOptions options;
            options.combos["D"] = 1;
            QVERIFY2(translateShader(v, f, options, &error).has_value(), error.c_str()); // recompiled over the junk
            QVERIFY(loadCachedShader(key, &error).shader.has_value());
        }
    }

    void inconsistentEntriesAreRejected()
    {
        std::string error;
        const ShaderSource v = source(kVert);
        const ShaderSource f = source(kFrag);
        const auto good = translateShader(v, f, {}, &error);
        QVERIFY2(good.has_value(), error.c_str());
        QVERIFY(!good->vertex.uniforms.empty());

        // every damaged copy reads back as absent (hit == false), never as a shader
        const auto absent = [&](const char *name, const std::function<void(TranslatedShader &)> &damage) {
            TranslatedShader copy = *good;
            damage(copy);
            const std::string key = std::string("damage-") + name;
            storeCachedShader(key, &copy, {});
            const CachedShader loaded = loadCachedShader(key, &error);
            QVERIFY2(!loaded.hit, name);
        };
        absent("offset-past-block", [](TranslatedShader &t) { t.vertex.uniforms[0].offset = t.vertex.blockSize; });
        absent("block-over-limit", [](TranslatedShader &t) { t.vertex.blockSize = 1u << 20; });
        absent("bad-magic", [](TranslatedShader &t) { t.fragment.spirv[0] = 0x12345678; });
        absent("truncated", [](TranslatedShader &t) { t.fragment.spirv.resize(3); });
        absent("empty-spirv", [](TranslatedShader &t) { t.vertex.spirv.clear(); });
        // the undamaged copy is accepted
        storeCachedShader("damage-none", &*good, {});
        QVERIFY(loadCachedShader("damage-none", &error).shader.has_value());
    }

    void oldFailuresExpire()
    {
        const auto writeFailure = [&](const std::string &key, uint32_t storedAt) {
            QDir().mkpath(QString::fromStdString(shaderCacheDirectory()));
            std::string data = "WSC1";
            const auto put = [&](uint32_t v) { data.append(reinterpret_cast<const char *>(&v), 4); };
            put(0);
            put(storedAt);
            const std::string message = "boom";
            put(static_cast<uint32_t>(message.size()));
            data += message;
            FILE *file = std::fopen((shaderCacheDirectory() + "/" + key + ".bin").c_str(), "wb");
            QVERIFY(file);
            std::fwrite(data.data(), 1, data.size(), file);
            std::fclose(file);
        };
        std::string error;
        const uint32_t now = static_cast<uint32_t>(std::time(nullptr));
        writeFailure("fresh-failure", now - 60);
        const CachedShader fresh = loadCachedShader("fresh-failure", &error);
        QVERIFY(fresh.hit && !fresh.shader.has_value());
        QCOMPARE(error, std::string("boom"));
        writeFailure("stale-failure", now - 3 * 24 * 3600);
        QVERIFY(!loadCachedShader("stale-failure", &error).hit);
    }
};

QTEST_GUILESS_MAIN(TstSceneVkShaders)
#include "tst_scenevk_shaders.moc"
