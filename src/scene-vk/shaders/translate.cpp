#include "shaders/translate.h"
#include "util/log.h"

#include "shaders/cache.h"
#include "shaders/glslcompiler.h"

#include <algorithm>
#include <regex>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstdlib>
#include <fstream>
#include <set>
#include <sstream>

namespace murasaki::scene {

namespace {

// Helpers of the dialect that have no GLSL spelling, written for this engine from the way the shipped shaders use
// them. (texSample2DBackBuffer and texLoad2D are the volumetric shaders' reads of a depth image: its values are Vulkan's 0 to 1 and
// the depth those shaders compute is the clip z / w of OpenGL, -1 to 1, so the reads return that.) Macros, so they work on every vector width. `mul(v, M)` has its operands swapped: the shaders are written
// for row vectors times row-major matrices and the engine uploads column-major ones.
constexpr const char *kPrelude = R"(
#define mul(a, b) ((b) * (a))
#define texSample2D(s, uv) texture(s, uv)
#define texSample2DLod(s, uv, l) textureLod(s, uv, l)
#define texSample2DCompare(s, uv, ref) vec4(texture(s, vec3(uv, ref)))
#define texSample2DBackBuffer(s, uv, res) vec4(texture(s, uv).r * 2.0 - 1.0)
#define texLoad2D(s, uv, res) vec4(texelFetch(s, ivec2((uv) * (res)), 0).r * 2.0 - 1.0)
#define clip(x) do { if (we_clipped(x)) discard; } while (false)
#define saturate(x) clamp(x, 0.0, 1.0)
#define frac(x) fract(x)
#define lerp(a, b, t) mix(a, b, t)
#define atan2(y, x) atan(y, x)
#define ddx(x) dFdx(x)
#define ddy(x) dFdy(x)
#define rsqrt(x) inversesqrt(x)
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define half float
#define half2 vec2
#define half3 vec3
#define half4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define float2x2 mat2
#define float3x3 mat3
#define float4x4 mat4
#define CAST2(x) vec2(x)
#define CAST3(x) vec3(x)
#define CAST4(x) vec4(x)
#define CAST3X3(x) mat3(x)
#define CASTF(x) float(x)
#define CASTU(x) uint(x)
bool we_clipped(float x) { return x < 0.0; }
bool we_clipped(vec2 x) { return any(lessThan(x, vec2(0.0))); }
bool we_clipped(vec3 x) { return any(lessThan(x, vec3(0.0))); }
bool we_clipped(vec4 x) { return any(lessThan(x, vec4(0.0))); }
vec2 pow(vec2 x, float y) { return pow(x, vec2(y)); }
vec3 pow(vec3 x, float y) { return pow(x, vec3(y)); }
vec4 pow(vec4 x, float y) { return pow(x, vec4(y)); }
float pow(float x, int y) { return pow(x, float(y)); }
vec2 pow(vec2 x, int y) { return pow(x, vec2(float(y))); }
vec3 pow(vec3 x, int y) { return pow(x, vec3(float(y))); }
vec4 pow(vec4 x, int y) { return pow(x, vec4(float(y))); }
)";

bool isIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

std::string lowerCase(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string upper(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

// Replaces whole-word occurrences of `from`.
std::string replaceWord(const std::string &text, const std::string &from, const std::string &to)
{
    std::string out;
    out.reserve(text.size());
    size_t i = 0;
    while (i < text.size()) {
        if (text.compare(i, from.size(), from) == 0 && (i == 0 || !isIdentChar(text[i - 1]))
            && (i + from.size() >= text.size() || !isIdentChar(text[i + from.size()]))) {
            out += to;
            i += from.size();
        } else {
            out += text[i++];
        }
    }
    return out;
}

bool contains(const std::string &text, const std::string &word)
{
    return replaceWord(text, word, "\x01") != text;
}

struct Decl {
    std::string type;
    std::string name;
    uint32_t arrayCount = 0;
};

// Splits `qualifiers type name[N]` (the part after the storage keyword).
bool parseDecl(const std::string &text, Decl &out, std::string &error)
{
    std::vector<std::string> words;
    std::string current;
    std::string arrayText;
    bool inArray = false;
    for (char c : text) {
        if (inArray) {
            if (c == ']') {
                inArray = false;
            } else {
                arrayText += c;
            }
        } else if (c == '[') {
            inArray = true;
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (!current.empty()) {
                words.push_back(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.empty()) {
        words.push_back(current);
    }
    // drop precision and interpolation qualifiers
    static const std::set<std::string> kIgnored = {"highp", "mediump", "lowp", "flat", "smooth", "noperspective"};
    std::vector<std::string> kept;
    for (const std::string &w : words) {
        if (!kIgnored.count(w)) {
            kept.push_back(w);
        }
    }
    if (kept.size() != 2) {
        error = "cannot read the declaration '" + text + "'";
        return false;
    }
    out.type = kept[0];
    out.name = kept[1];
    if (!arrayText.empty()) {
        // after preprocessing the size is a number, perhaps parenthesised
        std::string digits;
        for (char c : arrayText) {
            if (!std::isspace(static_cast<unsigned char>(c)) && c != '(' && c != ')') {
                digits += c;
            }
        }
        uint32_t n = 0;
        const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
        if (ec != std::errc() || end != digits.data() + digits.size() || n == 0 || n > 1024) {
            error = "unsupported array size '" + arrayText + "' of " + out.name;
            return false;
        }
        out.arrayCount = n;
    }
    return true;
}

uint32_t roundUp(uint32_t v, uint32_t a)
{
    return (v + a - 1) / a * a;
}

// Locations a type takes in a stage interface.
uint32_t locationSlots(const std::string &type, uint32_t arrayCount)
{
    uint32_t slots = 1;
    if (type == "mat2") {
        slots = 2;
    } else if (type == "mat3") {
        slots = 3;
    } else if (type == "mat4") {
        slots = 4;
    }
    return slots * std::max(1u, arrayCount);
}

bool integerBased(const std::string &type)
{
    return type.rfind("uint", 0) == 0 || type.rfind("uvec", 0) == 0 || type.rfind("int", 0) == 0 || type.rfind("ivec", 0) == 0;
}

// `#define fract frac` and `#define mix lerp`: HLSL's names for GLSL's, which the prelude already maps the other way round
// (`frac(x)` is `fract(x)`); the two together expand to a name nothing defines, because an object-like macro that becomes a
// function-like one is not rescanned. The lenient attempt drops the aliases before the preprocessor sees them.
std::string dropHlslAliases(const std::string &text)
{
    static const std::regex alias(R"((^|\n)[ \t]*#[ \t]*define[ \t]+(fract[ \t]+frac|mix[ \t]+lerp)[ \t]*(?=\r?\n|$))");
    return std::regex_replace(text, alias, "$1");
}

// `#require LightingV1` (shaders/source.cpp keeps it as a comment where it stood): WE puts its lighting function there, and the
// shipped shaders call it as PerformLighting_V1(worldPos, albedo, normal, viewVector, specularTint, baseReflectance, roughness,
// metallic) for the light the scene's lights add at a point (the ambient term is the shader's own). This engine's version, written
// for it from the shader interface (docs/scene-vk/formats.md, "Lights"): it declares the light arrays the LIGHTS_* combos size
// (render/lights.h fills them) and adds one term of the shipped common_pbr_2.h per light, ComputePBRLightShadow for the lights
// with a position and ComputePBRLightShadowInfinite for the directional ones, unshadowed (a shadow factor of 1).
//   point: towards its origin, its radius and exponent;
//   spot: the same, the colour scaled by where the point lies between the outer and the inner cone (cosines in Direction.w and
//     Origin.w), the exponent in Exponent.x;
//   tube: towards the nearest point of the segment OriginA - OriginB, the exponent in OriginA.w;
//   directional: along Direction (towards the light), no falloff.
// A shader that does not include common_pbr_2.h has none of those terms: its sum is zero, so it shows its ambient term only.
// A point exactly at a light's origin (or on a tube's segment) gives a zero vector, which the shipped term normalises: NaN, as it
// would in WE, whose terms take the same vector. No library scene puts a light on a lit surface.
// Shadows (a scene with shadows sets LIGHTS_SHADOW_MAPPING and the shadowed counts per type; the shader declares the atlas as
// SHADOW_ATLAS_SAMPLER): the first LIGHTS_<TYPE>_SHADOW lights of a type take their shadow factor from the atlas through the shipped
// helpers (render/shadows.h has the values): a point light's cube face (CalculateProjectedCoordsPoint, PerformPointShadowMapping), a
// spot's projection (CalculateProjectedCoords, PerformShadowMapping), a directional light's first cascade whose box holds the point
// (CalculateProjectedCoordsCascades flags a point outside in .w; outside all three, no shadow), the point moved first along its normal
// by one and a half of the cascade's texels (a normal offset: the larger a cascade's texels, the more a lit slope would shadow itself). The depth looked up is kept at most 1:
// a point beyond the far end of a light's picture is lit unless a caster is in front of it (the comparison of a float depth does not
// clamp the reference).
constexpr const char *kLightingV1 = R"(
#if defined(SHADOW_ATLAS_SAMPLER) && LIGHTS_POINT_SHADOW
uniform vec4 g_LFeature_ShadowPointProjection[LIGHTS_POINT_SHADOW];
uniform vec4 g_LFeature_ShadowPointProjectionTransform[LIGHTS_POINT_SHADOW];
#endif
#if defined(SHADOW_ATLAS_SAMPLER) && LIGHTS_SPOT_SHADOW
uniform mat4 g_LFeature_ShadowProjection[LIGHTS_SPOT_SHADOW];
uniform vec4 g_LFeature_ShadowProjectionTransform[LIGHTS_SPOT_SHADOW];
#endif
#if defined(SHADOW_ATLAS_SAMPLER) && LIGHTS_DIRECTIONAL_SHADOW
uniform mat4 g_LFeature_ShadowCascadeProjection[6];
uniform vec4 g_LFeature_ShadowCascadeTransform[6];
uniform vec4 g_LFeature_ShadowCascadeTexel[6];
#endif
#if LIGHTS_POINT
uniform vec4 g_LPoint_Color[LIGHTS_POINT];
uniform vec4 g_LPoint_Origin[LIGHTS_POINT];
#endif
#if LIGHTS_SPOT
uniform vec4 g_LSpot_Color[LIGHTS_SPOT];
uniform vec4 g_LSpot_Origin[LIGHTS_SPOT];
uniform vec4 g_LSpot_Direction[LIGHTS_SPOT];
uniform vec4 g_LSpot_Exponent[LIGHTS_SPOT];
#endif
#if LIGHTS_TUBE
uniform vec4 g_LTube_Color[LIGHTS_TUBE];
uniform vec4 g_LTube_OriginA[LIGHTS_TUBE];
uniform vec4 g_LTube_OriginB[LIGHTS_TUBE];
#endif
#if LIGHTS_DIRECTIONAL
uniform vec4 g_LDirectional_Color[LIGHTS_DIRECTIONAL];
uniform vec4 g_LDirectional_Direction[LIGHTS_DIRECTIONAL];
#endif
vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewVector, vec3 specularTint, vec3 baseReflectance, float roughness,
                        float metallic)
{
    vec3 total = vec3(0.0);
#if LIGHTS_POINT
    for (int n = 0; n < LIGHTS_POINT; ++n) {
        vec4 tint = g_LPoint_Color[n];
        vec4 place = g_LPoint_Origin[n];
        float shadow = 1.0;
#if defined(SHADOW_ATLAS_SAMPLER) && LIGHTS_POINT_SHADOW
        if (n < LIGHTS_POINT_SHADOW) {
            vec4 coords = CalculateProjectedCoordsPoint(worldPos, place.xyz, g_LFeature_ShadowPointProjection[n],
                                                        g_LFeature_ShadowPointProjectionTransform[n]);
            coords.z = min(coords.z, 1.0);
            shadow = PerformPointShadowMapping(coords);
        }
#endif
        total += ComputePBRLightShadow(normal, place.xyz - worldPos, viewVector, albedo, tint.rgb, tint.w, place.w, specularTint,
                                       baseReflectance, roughness, metallic, shadow);
    }
#endif
#if LIGHTS_SPOT
    for (int n = 0; n < LIGHTS_SPOT; ++n) {
        vec4 tint = g_LSpot_Color[n];
        vec4 place = g_LSpot_Origin[n];
        vec4 aim = g_LSpot_Direction[n];
        vec3 towards = place.xyz - worldPos;
        float along = dot(normalize(-towards), aim.xyz);
        float cone = smoothstep(aim.w, place.w, along);
        float shadow = 1.0;
#if defined(SHADOW_ATLAS_SAMPLER) && LIGHTS_SPOT_SHADOW
        if (n < LIGHTS_SPOT_SHADOW) {
            vec3 coords = CalculateProjectedCoords(worldPos, g_LFeature_ShadowProjection[n]);
            coords.z = min(coords.z, 1.0);
            shadow = PerformShadowMapping(coords, g_LFeature_ShadowProjectionTransform[n]);
        }
#endif
        total += ComputePBRLightShadow(normal, towards, viewVector, albedo, tint.rgb * cone, tint.w, g_LSpot_Exponent[n].x, specularTint,
                                       baseReflectance, roughness, metallic, shadow);
    }
#endif
#if LIGHTS_TUBE
    for (int n = 0; n < LIGHTS_TUBE; ++n) {
        vec4 tint = g_LTube_Color[n];
        vec3 a = g_LTube_OriginA[n].xyz;
        vec3 ab = g_LTube_OriginB[n].xyz - a;
        float lengthSquared = dot(ab, ab);
        float t = lengthSquared > 0.0 ? clamp(dot(worldPos - a, ab) / lengthSquared, 0.0, 1.0) : 0.0;
        total += ComputePBRLightShadow(normal, a + ab * t - worldPos, viewVector, albedo, tint.rgb, tint.w, g_LTube_OriginA[n].w, specularTint,
                                       baseReflectance, roughness, metallic, 1.0);
    }
#endif
#if LIGHTS_DIRECTIONAL
    for (int n = 0; n < LIGHTS_DIRECTIONAL; ++n) {
        float shadow = 1.0;
#if defined(SHADOW_ATLAS_SAMPLER) && LIGHTS_DIRECTIONAL_SHADOW
        if (n < LIGHTS_DIRECTIONAL_SHADOW) {
            vec3 lift = normal * 1.5;
            vec4 p1 = CalculateProjectedCoordsCascades(worldPos + lift * g_LFeature_ShadowCascadeTexel[n * 3].x, g_LFeature_ShadowCascadeProjection[n * 3]);
            vec4 p2 = CalculateProjectedCoordsCascades(worldPos + lift * g_LFeature_ShadowCascadeTexel[n * 3 + 1].x,
                                                       g_LFeature_ShadowCascadeProjection[n * 3 + 1]);
            vec4 p3 = CalculateProjectedCoordsCascades(worldPos + lift * g_LFeature_ShadowCascadeTexel[n * 3 + 2].x,
                                                       g_LFeature_ShadowCascadeProjection[n * 3 + 2]);
            vec4 coords = mix(p1, mix(p2, p3, p2.w), p1.w);
            vec4 place = mix(g_LFeature_ShadowCascadeTransform[n * 3],
                             mix(g_LFeature_ShadowCascadeTransform[n * 3 + 1], g_LFeature_ShadowCascadeTransform[n * 3 + 2], p2.w), p1.w);
            coords.z = min(coords.z, 1.0);
            shadow = mix(PerformShadowMapping(coords.xyz, place), 1.0, coords.w);
        }
#endif
        total += ComputePBRLightShadowInfinite(normal, normalize(g_LDirectional_Direction[n].xyz), viewVector, albedo, g_LDirectional_Color[n].rgb,
                                               specularTint, baseReflectance, roughness, metallic, shadow);
    }
#endif
    return total;
}
)";

constexpr const char *kNoLighting =
    "vec3 PerformLighting_V1(vec3 worldPos, vec3 albedo, vec3 normal, vec3 viewVector, vec3 specularTint, vec3 baseReflectance, float roughness,"
    " float metallic) { return vec3(0.0); }";

std::string withRequiredFeatures(const ShaderSource &source)
{
    static const std::string marker = "// #require LightingV1";
    const bool pbr = std::any_of(source.includes.begin(), source.includes.end(), [](const std::string &name) {
        return name.size() >= 14 && name.compare(name.size() - 14, 14, "common_pbr_2.h") == 0;
    });
    const std::string lighting = pbr ? kLightingV1 : kNoLighting;
    std::string out = source.text;
    size_t at = out.find(marker);
    while (at != std::string::npos) {
        const size_t after = at + marker.size();
        if (after < out.size() && out[after] != '\n' && out[after] != '\r' && out[after] != ' ') {
            at = out.find(marker, after); // another feature whose name starts the same
            continue;
        }
        out.replace(at, marker.size(), lighting);
        at = out.find(marker, at + lighting.size());
    }
    return out;
}

// HLSL's spellings around statements and functions that GLSL has no place for, which the lenient attempt removes:
//   * the attributes in front of a loop or a branch (`[loop]`, `[unroll]`, `[unroll(4)]`, `[branch]`, `[flatten]`, ...): hints to
//     the HLSL compiler, without effect on the result;
//   * `const` or `inline` in front of a function's return type (`const float fract(float x) {`), which GLSL refuses as a
//     qualifier of a return value.
std::string dropHlslStatementSyntax(const std::string &body)
{
    static const std::regex attribute(
        R"(\[\s*(loop|unroll|branch|flatten|fastopt|allow_uav_condition|forcecase|call)\s*(\(\s*\w*\s*\))?\s*\](?=\s*(for|while|do|if|switch)\b))");
    static const std::regex qualifiedFunction(R"((^|\n)([ \t]*)(?:(?:const|inline)[ \t]+)+([A-Za-z_]\w*[ \t]+[A-Za-z_]\w*[ \t]*\())");
    return std::regex_replace(std::regex_replace(body, attribute, ""), qualifiedFunction, "$1$2$3");
}

// WE's own preprocessor ignores a `#endif`, `#else` or `#elif` that closes nothing; glslang refuses it. The lenient attempt blanks
// such lines (two Workshop effects ship with one extra `#endif`), and cuts what follows a directive's own tokens.
std::string dropStrayConditionals(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    int depth = 0;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end;
        std::string line = text.substr(at, end - at);
        size_t p = line.find_first_not_of(" \t");
        bool keep = true;
        if (p != std::string::npos && line[p] == '#') {
            p = line.find_first_not_of(" \t", p + 1);
            const auto isDirective = [&](const char *word) {
                const size_t n = std::strlen(word);
                return p != std::string::npos && line.compare(p, n, word) == 0 && (p + n >= line.size() || !std::isalnum(static_cast<unsigned char>(line[p + n])));
            };
            if (isDirective("if") && std::regex_search(line, std::regex(R"(\b[A-Za-z_]\w*\.[xyzwrgba]\b)"))) {
                // a condition that reads a uniform's component (`#if g_Texture0Resolution.x < g_Texture0Resolution.y`) is not a
                // preprocessor expression; WE's reads it as false
                out += "#if 0";
                if (end < text.size()) {
                    out += '\n';
                }
                ++depth;
                at = end + 1;
                continue;
            }
            // tokens after the directive's own, which WE's preprocessor ignores and glslang refuses: a `;` closing an `#if` or `#elif`
            // condition (`#elif AUDIOSAMPLES == 32;`), more than a name after `#ifdef`, anything after `#else` and `#endif`
            if (const size_t keyword = p; isDirective("if") || isDirective("elif")) {
                const size_t comment = line.find("//", keyword);
                size_t codeEnd = comment == std::string::npos ? line.size() : comment;
                const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
                while (codeEnd > keyword && blank(line[codeEnd - 1])) {
                    --codeEnd;
                }
                if (codeEnd > keyword && line[codeEnd - 1] == ';') {
                    while (codeEnd > keyword && (line[codeEnd - 1] == ';' || blank(line[codeEnd - 1]))) {
                        --codeEnd;
                    }
                    line = line.substr(0, codeEnd);
                }
            } else if (isDirective("ifdef") || isDirective("ifndef") || isDirective("else") || isDirective("endif")) {
                size_t q = line.find_first_of(" \t", keyword);
                if (isDirective("ifdef") || isDirective("ifndef")) {
                    q = q == std::string::npos ? q : line.find_first_not_of(" \t", q);
                    q = q == std::string::npos ? q : line.find_first_of(" \t", q);
                }
                const size_t extra = q == std::string::npos ? q : line.find_first_not_of(" \t\r", q);
                if (extra != std::string::npos && line.compare(extra, 2, "//") != 0 && line.compare(extra, 2, "/*") != 0) {
                    line = line.substr(0, q);
                }
            }
            if (isDirective("if") || isDirective("ifdef") || isDirective("ifndef")) {
                ++depth;
            } else if (isDirective("endif")) {
                if (depth == 0) {
                    keep = false;
                } else {
                    --depth;
                }
            } else if ((isDirective("else") || isDirective("elif")) && depth == 0) {
                keep = false;
            }
        }
        if (keep) {
            out += line;
        }
        if (end < text.size()) {
            out += '\n';
        }
        at = end + 1;
    }
    return out;
}

// True when `body` writes to `name` (plain or compound assignment or ++/--, to the whole value or to a swizzle or an element): a varying
// the fragment shader modifies or an attribute the vertex shader modifies, which GLSL does not allow of an input.
bool assignsTo(const std::string &body, const std::string &name)
{
    const auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t at = body.find(name); at != std::string::npos; at = body.find(name, at + 1)) {
        if ((at > 0 && isWord(body[at - 1])) || (at + name.size() < body.size() && isWord(body[at + name.size()]))) {
            continue;
        }
        {
            size_t b = at;
            while (b > 0 && std::isspace(static_cast<unsigned char>(body[b - 1]))) {
                --b;
            }
            if (b >= 2 && (body.compare(b - 2, 2, "++") == 0 || body.compare(b - 2, 2, "--") == 0)) {
                return true; // `++a_Position.x`
            }
        }
        size_t p = at + name.size();
        const auto skipSpace = [&] {
            while (p < body.size() && std::isspace(static_cast<unsigned char>(body[p]))) {
                ++p;
            }
        };
        skipSpace();
        if (p < body.size() && body[p] == '[') {
            const size_t close = body.find(']', p);
            if (close == std::string::npos) {
                continue;
            }
            p = close + 1;
            skipSpace();
        }
        if (p < body.size() && body[p] == '.') {
            ++p;
            while (p < body.size() && isWord(body[p])) {
                ++p;
            }
            skipSpace();
        }
        if (p >= body.size()) {
            continue;
        }
        if (body[p] == '=' && (p + 1 >= body.size() || body[p + 1] != '=')) {
            return true;
        }
        if (std::string("+-*/").find(body[p]) != std::string::npos && p + 1 < body.size() && body[p + 1] == '=') {
            return true;
        }
        if ((body[p] == '+' || body[p] == '-') && p + 1 < body.size() && body[p + 1] == body[p]) {
            return true; // `a_Position.x++`
        }
    }
    return false;
}

// True when `name` appears in `text` as a whole identifier.
bool mentions(const std::string &text, const std::string &name)
{
    const auto isWord = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
        const bool before = at == 0 || !isWord(text[at - 1]);
        const bool after = at + name.size() >= text.size() || !isWord(text[at + name.size()]);
        if (before && after) {
            return true;
        }
    }
    return false;
}

// Fixed vertex input locations by attribute name; others get the next free ones.
const std::map<std::string, uint32_t> kAttributeLocations = {
    {"a_Position", 0}, {"a_TexCoord", 1}, {"a_Normal", 2}, {"a_Tangent4", 3}, {"a_Color", 4}, {"a_BlendIndices", 5}, {"a_BlendWeights", 6},
};

struct StageText {
    std::string body;                    // everything except the declarations that were rewritten
    std::vector<Decl> uniforms;          // non-sampler
    std::vector<Decl> samplers;
    std::vector<Decl> attributes;
    std::vector<Decl> varyings;
};

// Walks the top-level items of preprocessed text. Function bodies and everything else stay in `body`.
bool splitStage(const std::string &text, StageText &out, std::string &error)
{
    int depth = 0;
    int parens = 0;
    std::string item;
    const auto flush = [&]() -> bool {
        std::string_view v(item);
        size_t b = 0;
        while (b < v.size() && std::isspace(static_cast<unsigned char>(v[b]))) {
            ++b;
        }
        const std::string_view trimmed = v.substr(b);
        const auto starts = [&](const char *kw) {
            const size_t n = std::char_traits<char>::length(kw);
            return trimmed.substr(0, n) == kw && trimmed.size() > n && std::isspace(static_cast<unsigned char>(trimmed[n]));
        };
        std::string_view rest;
        enum { Other, Uniform, Attribute, Varying, BuiltinIn } kind = Other;
        if (starts("uniform")) {
            kind = Uniform;
            rest = trimmed.substr(7);
        } else if (starts("attribute")) {
            kind = Attribute;
            rest = trimmed.substr(9);
        } else if (starts("varying")) {
            kind = Varying;
            rest = trimmed.substr(7);
        } else if (starts("in") && trimmed.find("gl_") != std::string_view::npos && trimmed.find('(') == std::string_view::npos) {
            kind = BuiltinIn; // `in uint gl_InstanceID;`: a declaration of a built-in, which Vulkan GLSL has already
        }
        if (kind == Other || trimmed.find('{') != std::string_view::npos) {
            out.body += item;
            item.clear();
            return true;
        }
        if (kind == BuiltinIn) {
            item.clear();
            return true;
        }
        std::string statement(rest);
        while (!statement.empty() && (statement.back() == ';' || std::isspace(static_cast<unsigned char>(statement.back())))) {
            statement.pop_back();
        }
        // a declaration with a swizzle for a name (`varying vec4 v_Size.xy;`, a typo WE's compiler ignores) declares the name
        static const std::regex swizzled(R"(^(.*[A-Za-z0-9_])\.[xyzwrgba]{1,4}$)");
        if (std::smatch m; std::regex_match(statement, m, swizzled)) {
            statement = m[1].str();
        }
        Decl decl;
        if (!parseDecl(statement, decl, error)) {
            return false;
        }
        const bool sampler = decl.type.rfind("sampler", 0) == 0;
        if (kind == Varying && decl.name.rfind("gl_", 0) == 0) {
            // `varying uint gl_ViewportIndex;` (the shipped shadow casters): a built-in output, which the assembly replaces
            item.clear();
            return true;
        }
        if (kind == Uniform) {
            (sampler ? out.samplers : out.uniforms).push_back(decl);
        } else if (kind == Attribute) {
            out.attributes.push_back(decl);
        } else {
            out.varyings.push_back(decl);
        }
        out.body += "\n"; // keep line numbers roughly aligned for compiler messages
        item.clear();
        return true;
    };
    for (char c : text) {
        item += c;
        if (c == '(') {
            ++parens;
        } else if (c == ')') {
            --parens;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && parens == 0) {
                if (!flush()) {
                    return false;
                }
            }
        } else if (c == ';' && depth == 0 && parens == 0) {
            if (!flush()) {
                return false;
            }
        }
    }
    out.body += item;
    return true;
}

std::string stripDirectives(const std::string &text)
{
    std::istringstream in(text);
    std::string out;
    std::string line;
    while (std::getline(in, line)) {
        size_t b = line.find_first_not_of(" \t");
        if (b != std::string::npos && line[b] == '#') {
            const std::string_view d = std::string_view(line).substr(b + 1);
            if (d.substr(0, 4) == "line" || d.substr(0, 7) == "version" || d.substr(0, 9) == "extension") {
                out += '\n';
                continue;
            }
        }
        out += line;
        out += '\n';
    }
    return out;
}

} // namespace

EngineInput classifyUniform(const std::string &n, int *aux)
{
    if (aux) {
        *aux = 0;
    }
    static const std::map<std::string, EngineInput> kByName = {
        {"g_Time", EngineInput::Time},
        {"g_Frametime", EngineInput::Frametime},
        {"g_Daytime", EngineInput::Daytime},
        {"g_PointerPosition", EngineInput::PointerPosition},
        {"g_PointerPositionLast", EngineInput::PointerPositionLast},
        {"g_PointerState", EngineInput::PointerState},
        {"g_ParallaxPosition", EngineInput::ParallaxPosition},
        {"g_Screen", EngineInput::Screen},
        {"g_EyePosition", EngineInput::EyePosition},
        {"g_ModelViewProjectionMatrix", EngineInput::ModelViewProjection},
        {"g_ModelViewProjectionMatrixInverse", EngineInput::ModelViewProjectionInverse},
        {"g_ModelMatrix", EngineInput::ModelMatrix},
        {"g_NormalModelMatrix", EngineInput::NormalModelMatrix},
        {"g_ViewProjectionMatrix", EngineInput::ViewProjection},
        {"g_EffectModelViewProjectionMatrix", EngineInput::EffectModelViewProjection},
        {"g_TextureReductionScale", EngineInput::TextureReductionScale},
        {"g_EffectTextureProjectionMatrix", EngineInput::EffectTextureProjection},
        {"g_EffectTextureProjectionMatrixInverse", EngineInput::EffectTextureProjectionInverse},
        {"g_Texture0Rotation", EngineInput::Texture0Rotation},
        {"g_Texture0Translation", EngineInput::Texture0Translation},
        {"g_ModelMatrixInverse", EngineInput::ModelMatrixInverse},
        {"g_TexelSize", EngineInput::TexelSize},
        {"g_TexelSizeHalf", EngineInput::TexelSizeHalf},
        {"g_OrientationUp", EngineInput::OrientationUp},
        {"g_OrientationRight", EngineInput::OrientationRight},
        {"g_OrientationForward", EngineInput::OrientationForward},
        {"g_ViewUp", EngineInput::ViewUp},
        {"g_ViewRight", EngineInput::ViewRight},
        {"g_ViewForward", EngineInput::ViewForward},
        {"g_AltViewProjectionMatrix", EngineInput::AltViewProjection},
        {"g_RenderVar0", EngineInput::RenderVar0},
        {"g_RenderVar1", EngineInput::RenderVar1},
        {"g_LightAmbientColor", EngineInput::LightAmbientColor},
        {"g_LightSkylightColor", EngineInput::LightSkylightColor},
        {"g_FogDistanceColor", EngineInput::FogDistanceColor},
        {"g_FogDistanceParams", EngineInput::FogDistanceParams},
        {"g_FogHeightColor", EngineInput::FogHeightColor},
        {"g_FogHeightParams", EngineInput::FogHeightParams},
        {"g_LPoint_Color", EngineInput::LPointColor},
        {"g_LPoint_Origin", EngineInput::LPointOrigin},
        {"g_LSpot_Color", EngineInput::LSpotColor},
        {"g_LSpot_Origin", EngineInput::LSpotOrigin},
        {"g_LSpot_Direction", EngineInput::LSpotDirection},
        {"g_LSpot_Exponent", EngineInput::LSpotExponent},
        {"g_LTube_Color", EngineInput::LTubeColor},
        {"g_LTube_OriginA", EngineInput::LTubeOriginA},
        {"g_LTube_OriginB", EngineInput::LTubeOriginB},
        {"g_LDirectional_Color", EngineInput::LDirectionalColor},
        {"g_LDirectional_Direction", EngineInput::LDirectionalDirection},
        {"g_LightsPosition", EngineInput::LightsPosition},
        {"g_LightsColorRadius", EngineInput::LightsColorRadius},
        {"g_LightsColorPremultiplied", EngineInput::LightsColorPremultiplied},
        {"g_LFeature_ShadowPointProjection", EngineInput::ShadowPointProjection},
        {"g_LFeature_ShadowPointProjectionTransform", EngineInput::ShadowPointProjectionTransform},
        {"g_LFeature_ShadowProjection", EngineInput::ShadowProjection},
        {"g_LFeature_ShadowProjectionTransform", EngineInput::ShadowProjectionTransform},
        {"g_LFeature_ShadowCascadeProjection", EngineInput::ShadowCascadeProjection},
        {"g_LFeature_ShadowCascadeTransform", EngineInput::ShadowCascadeTransform},
        {"g_LFeature_ShadowCascadeTexel", EngineInput::ShadowCascadeTexel},
        {"g_ViewportViewProjectionMatrices", EngineInput::ViewportViewProjections},
        {"g_Bones", EngineInput::Bones},
    };
    if (const auto it = kByName.find(n); it != kByName.end()) {
        return it->second;
    }
    if (n.rfind("g_AudioSpectrum", 0) == 0) {
        // g_AudioSpectrum<16|32|64><Left|Right>: aux = bands, plus 1000 for the right channel
        if (aux) {
            *aux = std::atoi(n.c_str() + 15) + (n.size() > 5 && n.compare(n.size() - 5, 5, "Right") == 0 ? 1000 : 0);
        }
        return EngineInput::AudioSpectrum;
    }
    // g_Texture<N>Texel
    if (n.rfind("g_Texture", 0) == 0 && n.size() > 14 && n.compare(n.size() - 5, 5, "Texel") == 0) {
        const std::string digits = n.substr(9, n.size() - 14);
        if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            if (aux) {
                *aux = std::atoi(digits.c_str());
            }
            return EngineInput::TextureTexel;
        }
    }
    // g_Texture<N>MipMapInfo
    if (n.rfind("g_Texture", 0) == 0 && n.size() > 19 && n.compare(n.size() - 10, 10, "MipMapInfo") == 0) {
        const std::string digits = n.substr(9, n.size() - 19);
        if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            if (aux) {
                *aux = std::atoi(digits.c_str());
            }
            return EngineInput::TextureMipMapInfo;
        }
    }
    // g_Texture<N>Resolution
    if (n.rfind("g_Texture", 0) == 0 && n.size() > 19 && n.compare(n.size() - 10, 10, "Resolution") == 0) {
        const std::string digits = n.substr(9, n.size() - 19);
        if (!digits.empty() && std::all_of(digits.begin(), digits.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); })) {
            if (aux) {
                *aux = std::atoi(digits.c_str());
            }
            return EngineInput::TextureResolution;
        }
    }
    return EngineInput::None;
}

Std140 std140Of(const std::string &t)
{
    if (t == "float" || t == "int" || t == "uint" || t == "bool") {
        return {4, 4};
    }
    if (t == "vec2" || t == "ivec2" || t == "uvec2") {
        return {8, 8};
    }
    if (t == "vec3" || t == "ivec3" || t == "uvec3") {
        return {12, 16};
    }
    if (t == "vec4" || t == "ivec4" || t == "uvec4") {
        return {16, 16};
    }
    // matCxR: C columns, each padded to a vec4 slot
    if (t == "mat2" || t == "mat2x2" || t == "mat2x3" || t == "mat2x4") {
        return {32, 16};
    }
    if (t == "mat3" || t == "mat3x2" || t == "mat3x3" || t == "mat3x4") {
        return {48, 16};
    }
    if (t == "mat4" || t == "mat4x2" || t == "mat4x3" || t == "mat4x4") {
        return {64, 16};
    }
    return {0, 0};
}

std::map<std::string, int> resolveCombos(const ShaderSource &vertex, const ShaderSource &fragment, const TranslateOptions &options)
{
    std::map<std::string, int> combos;
    for (const ShaderSource *s : {&vertex, &fragment}) {
        for (const ComboDecl &c : s->combos) {
            combos.emplace(c.name, c.defaultValue);
        }
    }
    // A texture bound to a sampler whose annotation names a combo turns the combo on.
    for (const ShaderSource *s : {&vertex, &fragment}) {
        for (const auto &[name, note] : s->notes) {
            const Json *combo = note.find("combo");
            if (!combo || !combo->isString() || name.rfind("g_Texture", 0) != 0) {
                continue;
            }
            const uint32_t slot = static_cast<uint32_t>(std::atoi(name.c_str() + 9));
            // A render target given as the sampler's default is always there for the engine to bind, so it implies the
            // combo like a bound texture does.
            const std::string def = note.stringOr("default");
            if (std::find(options.boundTextures.begin(), options.boundTextures.end(), slot) != options.boundTextures.end()
                || def.rfind("_rt_", 0) == 0) {
                combos[combo->asString()] = 1;
            }
        }
    }
    for (const auto &[name, value] : options.combos) {
        combos[upper(name)] = value;
    }
    // the scene's fog reaches a material that opts in with its FOG combo (declared on by default in WE's shaders), and no other
    const auto fog = combos.find("FOG");
    if (fog == combos.end() || fog->second == 0) {
        for (const char *name : {"FOG_DIST", "FOG_HEIGHT", "FOG_COMPUTED"}) {
            combos.erase(name);
        }
    }
    return combos;
}


bool isWordAt(const std::string &text, size_t at, size_t length)
{
    return (at == 0 || !isIdentChar(text[at - 1])) && (at + length >= text.size() || !isIdentChar(text[at + length]));
}

bool mentionsWord(const std::string &text, const std::string &word)
{
    for (size_t at = text.find(word); at != std::string::npos; at = text.find(word, at + 1)) {
        if (isWordAt(text, at, word.size())) {
            return true;
        }
    }
    return false;
}

// The compatibility shim's first rule: a global `const T name = expr;` whose expr reads a uniform (or a name moved before it)
// is not a constant expression in GLSL. It becomes `T name;` and the assignment runs first thing in main(). The statements
// are found by walking the top level of the body (depth 0, `;` outside parentheses). False when there was nothing to do.
bool hoistGlobalInitializers(std::string &body, const std::vector<Decl> &uniforms)
{
    std::vector<std::string> names;
    for (const Decl &u : uniforms) {
        names.push_back(u.name);
    }
    std::string out;
    std::string assignments;
    std::string item;
    int depth = 0, parens = 0;
    bool changed = false;
    const auto flush = [&] {
        std::string statement = item;
        const size_t first = statement.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && statement.compare(first, 6, "const ") == 0) {
            const size_t eq = statement.find('=');
            if (eq != std::string::npos) {
                const std::string expr = statement.substr(eq + 1, statement.rfind(';') == std::string::npos ? std::string::npos : statement.rfind(';') - eq - 1);
                bool dependent = false;
                for (const std::string &n : names) {
                    dependent = dependent || mentionsWord(expr, n);
                }
                size_t nameEnd = eq;
                while (nameEnd > 0 && std::isspace(static_cast<unsigned char>(statement[nameEnd - 1]))) {
                    --nameEnd;
                }
                size_t nameStart = nameEnd;
                while (nameStart > 0 && isIdentChar(statement[nameStart - 1])) {
                    --nameStart;
                }
                if (dependent && nameStart < nameEnd) {
                    const std::string name = statement.substr(nameStart, nameEnd - nameStart);
                    names.push_back(name); // what reads it is no longer constant either
                    assignments += "    " + name + " =" + expr + ";\n";
                    out += statement.substr(0, first) + statement.substr(first + 6, nameEnd - first - 6) + ";";
                    item.clear();
                    changed = true;
                    return;
                }
            }
        }
        out += item;
        item.clear();
    };
    for (char c : body) {
        item += c;
        if (c == '(') {
            ++parens;
        } else if (c == ')') {
            --parens;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && parens == 0) {
                out += item;
                item.clear();
            }
        } else if (c == ';' && depth == 0 && parens == 0) {
            flush();
        }
    }
    out += item;
    if (!changed) {
        return false;
    }
    const size_t mainAt = out.find("void main");
    const size_t brace = mainAt == std::string::npos ? mainAt : out.find('{', mainAt);
    if (brace == std::string::npos) {
        return false;
    }
    out.insert(brace + 1, "\n" + assignments);
    body = std::move(out);
    return true;
}


// Conversions HLSL does silently, as overloaded functions the shim wraps an assignment's right side in.
const char *const kShimConversions =
    "float we_mod(float a, float b) { return a - b * trunc(a / b); } vec2 we_mod(vec2 a, vec2 b) { return a - b * trunc(a / b); } "
    "vec3 we_mod(vec3 a, vec3 b) { return a - b * trunc(a / b); } vec4 we_mod(vec4 a, vec4 b) { return a - b * trunc(a / b); } "
    "int we_mod(int a, int b) { return b == 0 ? 0 : a % b; } uint we_mod(uint a, uint b) { return b == 0u ? 0u : a % b; } "
    "uint we_to_uint(float v) { return uint(v); } uint we_to_uint(int v) { return uint(v); } uint we_to_uint(uint v) { return v; } uint we_to_uint(bool v) { return v ? 1u : 0u; } "
    "float we_to_float(float v) { return v; } float we_to_float(bool v) { return v ? 1.0 : 0.0; } float we_to_float(int v) { return float(v); } float we_to_float(vec2 v) { return v.x; } "
    "float we_to_float(vec3 v) { return v.x; } float we_to_float(vec4 v) { return v.x; } int we_to_int(float v) { return int(v); } "
    "int we_to_int(int v) { return v; } int we_to_int(bool v) { return v ? 1 : 0; } vec2 we_to_vec2(float v) { return vec2(v); } vec2 we_to_vec2(vec2 v) { return v; } "
    "vec2 we_to_vec2(vec3 v) { return v.xy; } vec2 we_to_vec2(vec4 v) { return v.xy; } vec3 we_to_vec3(float v) { return vec3(v); } "
    "vec3 we_to_vec3(vec3 v) { return v; } vec3 we_to_vec3(vec4 v) { return v.xyz; } vec3 we_to_vec3(vec2 v) { return vec3(v, 0.0); } "
    "vec4 we_to_vec4(float v) { return vec4(v); } vec4 we_to_vec4(vec4 v) { return v; } vec4 we_to_vec4(vec3 v) { return vec4(v, 1.0); } "
    "vec4 we_to_vec4(vec2 v) { return vec4(v, 0.0, 1.0); } "
    // a scalar first operand broadcasts in HLSL: max(0, color)
    "vec2 max(float a, vec2 b) { return max(vec2(a), b); } vec3 max(float a, vec3 b) { return max(vec3(a), b); } vec4 max(float a, vec4 b) { return max(vec4(a), b); } "
    "vec2 min(float a, vec2 b) { return min(vec2(a), b); } vec3 min(float a, vec3 b) { return min(vec3(a), b); } vec4 min(float a, vec4 b) { return min(vec4(a), b); } "
    // an integer literal beside a float, which overload resolution next to the vector forms above no longer finds
    "float max(int a, float b) { return max(float(a), b); } float min(int a, float b) { return min(float(a), b); } "
    "float max(float a, int b) { return max(a, float(b)); } float min(float a, int b) { return min(a, float(b)); } "
    // HLSL's log10 and fmod (the remainder keeps the dividend's sign), and the functions that cut the longer vector of their operands
    "float log10(float x) { return log(x) * 0.43429448190325176; } vec2 log10(vec2 x) { return log(x) * 0.43429448190325176; } "
    "vec3 log10(vec3 x) { return log(x) * 0.43429448190325176; } vec4 log10(vec4 x) { return log(x) * 0.43429448190325176; } "
    "float fmod(float a, float b) { return a - b * trunc(a / b); } vec2 fmod(vec2 a, vec2 b) { return a - b * trunc(a / b); } "
    "vec3 fmod(vec3 a, vec3 b) { return a - b * trunc(a / b); } vec4 fmod(vec4 a, vec4 b) { return a - b * trunc(a / b); } "
    "vec2 fmod(vec2 a, float b) { return a - b * trunc(a / b); } vec3 fmod(vec3 a, float b) { return a - b * trunc(a / b); } "
    "vec4 fmod(vec4 a, float b) { return a - b * trunc(a / b); } "
    "vec3 mix(vec4 a, vec3 b, float t) { return mix(a.xyz, b, t); } vec3 mix(vec3 a, vec4 b, float t) { return mix(a, b.xyz, t); } "
    "vec2 mix(vec3 a, vec2 b, float t) { return mix(a.xy, b, t); } vec2 mix(vec2 a, vec3 b, float t) { return mix(a, b.xy, t); } "
    "float dot(vec4 a, vec3 b) { return dot(a.xyz, b); } float dot(vec3 a, vec4 b) { return dot(a, b.xyz); } "
    "float dot(vec3 a, vec2 b) { return dot(a.xy, b); } float dot(vec2 a, vec3 b) { return dot(a, b.xy); } "
    "float mix(int a, int b, float t) { return mix(float(a), float(b), t); } float mix(float a, int b, float t) { return mix(a, float(b), t); } "
    "float mix(int a, float b, float t) { return mix(float(a), b, t); } "
    "vec2 mix(float a, vec2 b, float t) { return mix(vec2(a), b, t); } vec3 mix(float a, vec3 b, float t) { return mix(vec3(a), b, t); } "
    "vec2 pow(float x, vec2 y) { return pow(vec2(x), y); } vec3 pow(float x, vec3 y) { return pow(vec3(x), y); } vec4 pow(float x, vec4 y) { return pow(vec4(x), y); } "
    "vec4 texture(sampler2D s, vec3 uv) { return texture(s, uv.xy); } vec4 texture(sampler2D s, vec4 uv) { return texture(s, uv.xy); } "
    "vec4 mix(float a, vec4 b, float t) { return mix(vec4(a), b, t); } vec2 mix(vec2 a, float b, float t) { return mix(a, vec2(b), t); } "
    "vec3 mix(vec3 a, float b, float t) { return mix(a, vec3(b), t); } vec4 mix(vec4 a, float b, float t) { return mix(a, vec4(b), t); }\n";

// The GLSL type name inside glslang's description of a type ("temp highp 2-component vector of float").
std::string typeFromDescription(const std::string &d)
{
    if (d.find("4-component vector of float") != std::string::npos) return "vec4";
    if (d.find("3-component vector of float") != std::string::npos) return "vec3";
    if (d.find("2-component vector of float") != std::string::npos) return "vec2";
    if (d.size() >= 5 && d.compare(d.size() - 5, 5, "float") == 0) return "float";
    if (d.size() >= 4 && d.compare(d.size() - 4, 4, "uint") == 0) return "uint";
    if (d.size() >= 3 && d.compare(d.size() - 3, 3, "int") == 0) return "int";
    return {};
}

// The compatibility shim's last rule, driven by the compiler: for "'=' : cannot convert from A to B" on a line, the right
// side of that line's first assignment goes through we_to_B (HLSL narrows, widens and truncates silently). Returns the lines
// it changed; the caller compiles again.
// `a % b` on floats (HLSL's fmod): the first `%` of a line becomes we_mod(a, b), the operands being an identifier or number with
// its member access, or a parenthesised expression (with the function name in front of it). False when it cannot find them.
bool rewriteModulo(std::string &line)
{
    size_t pos = line.find('%');
    if (pos == std::string::npos || pos == 0) {
        return false;
    }
    if (pos + 1 < line.size() && line[pos + 1] == '=') {
        // `a %= b;` is `a = a % b;`
        size_t l = pos;
        while (l > 0 && line[l - 1] == ' ') {
            --l;
        }
        size_t b = l;
        while (b > 0 && (std::isalnum(static_cast<unsigned char>(line[b - 1])) || line[b - 1] == '_' || line[b - 1] == '.')) {
            --b;
        }
        if (b == l) {
            return false;
        }
        const std::string lhs = line.substr(b, l - b);
        line.replace(pos, 2, "= " + lhs + " %");
        pos = line.find('%', b);
    }
    const auto isId = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; };
    // left operand
    size_t l = pos;
    while (l > 0 && line[l - 1] == ' ') {
        --l;
    }
    const size_t leftEnd = l;
    if (l > 0 && line[l - 1] == ')') {
        int depth = 0;
        while (l > 0) {
            --l;
            if (line[l] == ')') {
                ++depth;
            } else if (line[l] == '(' && --depth == 0) {
                break;
            }
        }
        while (l > 0 && isId(line[l - 1])) {
            --l; // a function call: its name belongs to the operand
        }
    } else {
        while (l > 0 && isId(line[l - 1])) {
            --l;
        }
    }
    // right operand
    size_t r = pos + 1;
    while (r < line.size() && line[r] == ' ') {
        ++r;
    }
    const size_t rightBegin = r;
    while (r < line.size() && isId(line[r])) {
        ++r;
    }
    if (r < line.size() && line[r] == '(') {
        int depth = 0;
        for (; r < line.size(); ++r) {
            if (line[r] == '(') {
                ++depth;
            } else if (line[r] == ')' && --depth == 0) {
                ++r;
                break;
            }
        }
    } else if (rightBegin == r && rightBegin < line.size() && line[rightBegin] == '(') {
        return false;
    }
    if (l == leftEnd || r == rightBegin) {
        return false;
    }
    line = line.substr(0, l) + "we_mod(" + line.substr(l, leftEnd - l) + ", " + line.substr(rightBegin, r - rightBegin) + ")" + line.substr(r);
    return true;
}

// WE's own helper functions (common.h) called with a wider vector than they take (HLSL truncates silently): the helper is
// renamed with a macro and the wider calls get overloads that take the leading components. Added the first time the compiler
// names the helper. Returns whether it added one.
bool addHelperOverloads(std::string &glsl, const std::string &name)
{
    struct Helper {
        const char *name;
        const char *block;
    };
    static const Helper helpers[] = {
        {"rotateVec2", "vec2 we_rotateVec2(vec2 v, float r);\nvec2 we_rotateVec2(vec3 v, float r) { return we_rotateVec2(v.xy, r); }\n"
                       "vec2 we_rotateVec2(vec4 v, float r) { return we_rotateVec2(v.xy, r); }\n"},
        {"greyscale", "float we_greyscale(vec3 c);\nfloat we_greyscale(vec4 c) { return we_greyscale(c.rgb); }\n"},
        {"hsv2rgb", "vec3 we_hsv2rgb(vec3 c);\nvec3 we_hsv2rgb(vec4 c) { return we_hsv2rgb(c.rgb); }\n"},
        {"rgb2hsv", "vec3 we_rgb2hsv(vec3 c);\nvec3 we_rgb2hsv(vec4 c) { return we_rgb2hsv(c.rgb); }\n"},
        {"ApplyBlending", "vec3 we_ApplyBlending(const int m, vec3 a, vec3 b, float o);\n"
                          "vec3 we_ApplyBlending(const int m, vec3 a, vec3 b, vec2 o) { return we_ApplyBlending(m, a, b, o.x); }\n"
                          "vec3 we_ApplyBlending(const int m, vec3 a, vec3 b, vec3 o) { return we_ApplyBlending(m, a, b, o.x); }\n"
                          "vec3 we_ApplyBlending(const int m, vec3 a, vec3 b, vec4 o) { return we_ApplyBlending(m, a, b, o.x); }\n"
                          "vec3 we_ApplyBlending(const int m, vec4 a, vec4 b, float o) { return we_ApplyBlending(m, a.rgb, b.rgb, o); }\n"},
    };
    for (const Helper &h : helpers) {
        if (name != h.name || glsl.find(std::string("#define ") + h.name + " we_") != std::string::npos) {
            continue;
        }
        const size_t eol = glsl.find('\n');
        if (eol == std::string::npos) {
            return false;
        }
        glsl.insert(eol + 1, std::string("#define ") + h.name + " we_" + h.name + "\n" + h.block);
        return true;
    }
    // any other function the shader defines itself, called with a wider vector than a parameter takes: the same, made from its own
    // signature (one overload per vector parameter that widens by one size, and one with every one of them widened)
    if (glsl.find(std::string("#define ") + name + " we_") != std::string::npos || name.empty() || !(std::isalpha(static_cast<unsigned char>(name[0])) || name[0] == '_')) {
        return false;
    }
    const std::regex definition(std::string(R"((^|\n)\s*(float|int|bool|void|vec2|vec3|vec4)\s+)") + name + R"(\s*\(([^)]*)\)\s*\{)");
    std::smatch m;
    if (!std::regex_search(glsl, m, definition)) {
        return false;
    }
    struct Param {
        std::string type, name;
    };
    std::vector<Param> params;
    std::stringstream list(m[3].str());
    for (std::string item; std::getline(list, item, ',');) {
        std::stringstream words(item);
        std::string word, type, pname;
        while (words >> word) {
            if (word == "in" || word == "const" || word == "out" || word == "inout") {
                continue;
            }
            (type.empty() ? type : pname) = word;
        }
        if (type.empty() || pname.empty() || pname.find('[') != std::string::npos) {
            return false;
        }
        params.push_back({type, pname});
    }
    const auto wider = [](const std::string &t) { return t == "vec2" ? "vec3" : t == "vec3" ? "vec4" : ""; };
    const auto cut = [](const std::string &t, const std::string &from) { return t == "vec2" ? from + ".xy" : t == "vec3" ? from + ".xyz" : from; };
    const std::string internal = "we_" + name;
    std::string block;
    const auto overload = [&](const std::vector<bool> &widen) {
        std::string signature, call;
        for (size_t i = 0; i < params.size(); ++i) {
            signature += (i ? ", " : "") + std::string(widen[i] ? wider(params[i].type) : params[i].type) + " " + params[i].name;
            call += (i ? ", " : "") + (widen[i] ? cut(params[i].type, params[i].name) : params[i].name);
        }
        block += m[2].str() + " " + internal + "(" + signature + ") { " + (m[2].str() == "void" ? "" : "return ") + internal + "(" + call + "); }\n";
    };
    bool any = false;
    std::vector<bool> all(params.size(), false);
    for (size_t i = 0; i < params.size(); ++i) {
        if (*wider(params[i].type)) {
            std::vector<bool> one(params.size(), false);
            one[i] = true;
            overload(one);
            all[i] = true;
            any = true;
        }
    }
    if (!any) {
        return false;
    }
    if (std::count(all.begin(), all.end(), true) > 1) {
        overload(all);
    }
    std::string prototype = m[2].str() + " " + internal + "(";
    for (size_t i = 0; i < params.size(); ++i) {
        prototype += (i ? ", " : "") + params[i].type + " " + params[i].name;
    }
    prototype += ");\n";
    const size_t eol = glsl.find('\n');
    if (eol == std::string::npos) {
        return false;
    }
    glsl.insert(eol + 1, "#define " + name + " " + internal + "\n" + prototype + block);
    return true;
}

// HLSL lets a function with a value type end without returning (a body that `#if`s down to nothing under one combo value; the value is
// undefined and the call is dead code): the definition of `name` that spans `line` (the line glslang names; any definition without a
// `return` when it names none) gets a `return T(0);` before its closing brace. The text is the preprocessor's output, so conditionals
// are resolved already; braces in comments are skipped. Only scalar, vector and matrix return types are handled.
bool addFinalReturn(std::string &glsl, const std::string &name, int line)
{
    static const std::regex type(R"(^(float|int|uint|bool|[biu]?vec[234]|mat[234](x[234])?)$)");
    const auto lineOf = [&](size_t at) { return 1 + static_cast<int>(std::count(glsl.begin(), glsl.begin() + static_cast<std::ptrdiff_t>(at), '\n')); };
    struct Definition {
        std::string returnType;
        size_t begin, open, close; // the name, and the body's braces
    };
    std::vector<Definition> definitions;
    size_t at = 0;
    while (!name.empty() && (at = glsl.find(name, at)) != std::string::npos) {
        const size_t nameAt = at;
        at += name.size();
        if (!isWordAt(glsl, nameAt, name.size())) {
            continue;
        }
        // `<type> name (` at the start of a line, then the parameter list and a body
        size_t lineStart = glsl.rfind('\n', nameAt);
        lineStart = lineStart == std::string::npos ? 0 : lineStart + 1;
        const std::string head = glsl.substr(lineStart, nameAt - lineStart);
        const size_t typeBegin = head.find_first_not_of(" \t");
        const size_t typeEnd = typeBegin == std::string::npos ? typeBegin : head.find_first_of(" \t", typeBegin);
        if (typeEnd == std::string::npos) {
            continue;
        }
        const std::string returnType = head.substr(typeBegin, typeEnd - typeBegin);
        if (!std::regex_match(returnType, type) || head.find_first_not_of(" \t", typeEnd) != std::string::npos) {
            continue;
        }
        size_t p = glsl.find_first_not_of(" \t", at);
        if (p == std::string::npos || glsl[p] != '(') {
            continue;
        }
        int depth = 0;
        for (; p < glsl.size(); ++p) {
            depth += glsl[p] == '(' ? 1 : glsl[p] == ')' ? -1 : 0;
            if (depth == 0) {
                break;
            }
        }
        p = p < glsl.size() ? glsl.find_first_not_of(" \t\r\n", p + 1) : std::string::npos;
        if (p == std::string::npos || glsl[p] != '{') {
            continue; // a prototype
        }
        const size_t open = p;
        depth = 0;
        bool closed = false;
        for (; p < glsl.size() && !closed; ++p) {
            if (glsl.compare(p, 2, "//") == 0) {
                p = std::min(glsl.find('\n', p), glsl.size()) - 1;
            } else if (glsl.compare(p, 2, "/*") == 0) {
                const size_t end = glsl.find("*/", p + 2);
                p = end == std::string::npos ? glsl.size() : end + 1;
            } else {
                depth += glsl[p] == '{' ? 1 : glsl[p] == '}' ? -1 : 0;
                closed = depth == 0;
                if (closed) {
                    definitions.push_back({returnType, nameAt, open, p});
                }
            }
        }
    }
    const auto returns = [&](const Definition &d) {
        const std::string body = glsl.substr(d.open, d.close - d.open);
        return mentionsWord(body, "return");
    };
    const Definition *chosen = nullptr;
    for (const Definition &d : definitions) {
        if (line > 0 && lineOf(d.begin) <= line && line <= lineOf(d.close)) {
            chosen = &d;
            break;
        }
    }
    for (const Definition &d : definitions) {
        if (!chosen && !returns(d)) {
            chosen = &d;
        }
    }
    if (!chosen) {
        return false;
    }
    glsl.insert(chosen->close, " return " + chosen->returnType + "(0); ");
    return true;
}

int fixConversions(std::string &glsl, const std::string &compileError)
{
    std::vector<std::pair<int, std::string>> fixes;
    std::vector<std::string> helpers; // helpers called with a wider vector, added after the line fixes (they shift the lines)
    std::set<std::pair<std::string, int>> unfinished; // functions that can run off their end without a value, and the line named
    size_t at = 0;
    while ((at = compileError.find("ERROR: 0:", at)) != std::string::npos) {
        at += 9;
        const int line = std::atoi(compileError.c_str() + at);
        const size_t eol = compileError.find('\n', at);
        const std::string message = compileError.substr(at, eol == std::string::npos ? std::string::npos : eol - at);
        if (message.find("no matching overloaded function found") != std::string::npos) {
            const size_t open = message.find('\'');
            const size_t close = open == std::string::npos ? open : message.find('\'', open + 1);
            if (close != std::string::npos) {
                helpers.push_back(message.substr(open + 1, close - open - 1));
            }
            continue;
        }
        if (message.find("function does not return a value") != std::string::npos) {
            // (the name comes after the text: `'' : function does not return a value: ShadowMask`)
            const size_t colon = message.rfind(':');
            const size_t first = message.find_first_not_of(' ', colon + 1);
            const size_t last = message.find_first_of(" \r", first == std::string::npos ? 0 : first);
            if (first != std::string::npos) {
                unfinished.emplace(message.substr(first, last == std::string::npos ? std::string::npos : last - first), line);
            }
            continue;
        }
        if ((message.find("'%' :") != std::string::npos || message.find("'%=' :") != std::string::npos) && line > 0) {
            fixes.emplace_back(line, "%"); // float operands of a remainder
            continue;
        }
        if (line > 0 && message.find("boolean expression expected") != std::string::npos) {
            fixes.emplace_back(line, "bool"); // a number as the condition of `? :`
            continue;
        }
        if (line > 0 && message.find("left of '[' is not of type array, matrix, or vector") != std::string::npos) {
            fixes.emplace_back(line, "scalarindex"); // a subscript of a scalar
            continue;
        }
        if (line > 0 && message.find("cannot convert return value to function return type") != std::string::npos) {
            fixes.emplace_back(line, "return"); // HLSL converts a returned value to the function's type
            continue;
        }
        if (line > 0 && message.find("scalar integer expression required") != std::string::npos) {
            fixes.emplace_back(line, "index"); // a float as an array index
            continue;
        }
        // arithmetic on a comparison (`depth *= (depth < 0.6) * 6.0;`): HLSL reads true as 1
        if (line > 0 && message.find("wrong operand types") != std::string::npos && message.find(" bool'") != std::string::npos) {
            fixes.emplace_back(line, "boolarith");
            continue;
        }
        // vectors of different sizes in one operation: HLSL drops the extra components of the longer one
        if (line > 0 && message.find("wrong operand types") != std::string::npos) {
            const auto width = [&](const std::string &word) {
                const size_t w = message.find(word + " operand of type '");
                if (w == std::string::npos) {
                    return 0;
                }
                const size_t v = message.find("-component vector", w);
                return v != std::string::npos && v > 0 && std::isdigit(static_cast<unsigned char>(message[v - 1])) ? message[v - 1] - '0' : 0;
            };
            const int left = width("left-hand"), right = width("right");
            const size_t op = message.find("'", 0);
            if (left > right && right >= 2 && op != std::string::npos && op + 1 < message.size()) {
                fixes.emplace_back(line, std::string("cut") + std::to_string(right) + message[op + 1]);
                fixes.emplace_back(line, std::string("cutv") + std::to_string(right)); // when the longer is inside a bigger expression
            } else if (right > left && left >= 2 && op != std::string::npos && op + 1 < message.size()) {
                fixes.emplace_back(line, std::string("cur") + std::to_string(left) + message[op + 1]); // the right operand is the longer
                fixes.emplace_back(line, std::string("cutv") + std::to_string(left));
            }
            continue;
        }
        const size_t from = message.find("cannot convert from '");
        const size_t to = message.find("' to '", from);
        if ((message.find("'=' :") == std::string::npos && message.find("'assign' :") == std::string::npos) || from == std::string::npos || to == std::string::npos) {
            continue;
        }
        const std::string target = typeFromDescription(message.substr(to + 6, message.rfind('\'') - to - 6));
        if (!target.empty() && line > 0) {
            fixes.emplace_back(line, target);
        }
    }
    int changed = 0;
    for (const auto &[line, target] : fixes) {
        size_t begin = 0;
        for (int l = 1; l < line && begin != std::string::npos; ++l) {
            begin = glsl.find('\n', begin);
            begin = begin == std::string::npos ? begin : begin + 1;
        }
        if (begin == std::string::npos) {
            continue;
        }
        const size_t end = glsl.find('\n', begin);
        std::string text = glsl.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
        if (target == "bool") {
            // the condition of the first `?` on the line, after the last assignment, `(` or `return` before it, goes through bool()
            const size_t q = text.find('?');
            if (q == std::string::npos) {
                continue;
            }
            size_t start = 0;
            for (size_t i = 0; i < q; ++i) {
                const bool assign = text[i] == '=' && i + 1 < text.size() && text[i + 1] != '=' && (i == 0 || std::string("=<>!").find(text[i - 1]) == std::string::npos);
                if (assign || text[i] == '(') {
                    start = i + 1;
                }
            }
            if (const size_t ret = text.rfind("return", q); ret != std::string::npos && ret + 6 > start && ret < q) {
                start = ret + 6;
            }
            const std::string condition = text.substr(start, q - start);
            if (condition.find_first_not_of(' ') == std::string::npos || condition.find("bool(") != std::string::npos) {
                continue;
            }
            text = text.substr(0, start) + " bool(" + condition + ") " + text.substr(q);
            glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
            ++changed;
            continue;
        }
        if (target == "boolarith") {
            // every parenthesised comparison of the line that is not a call's argument list or a condition becomes float(...)
            std::string out;
            bool touched = false;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '(' || (i > 0 && (std::isalnum(static_cast<unsigned char>(text[i - 1])) || text[i - 1] == '_'))) {
                    out += text[i];
                    continue;
                }
                int depth = 1;
                size_t j = i + 1;
                while (j < text.size() && depth > 0) {
                    depth += text[j] == '(' ? 1 : text[j] == ')' ? -1 : 0;
                    ++j;
                }
                if (depth != 0) {
                    out += text.substr(i);
                    break;
                }
                const std::string inner = text.substr(i + 1, j - i - 2);
                const bool comparison = inner.find_first_of("<>") != std::string::npos || inner.find("==") != std::string::npos || inner.find("!=") != std::string::npos;
                const bool nested = inner.find('(') != std::string::npos || inner.find("&&") != std::string::npos || inner.find("||") != std::string::npos;
                if (comparison && !nested) {
                    out += "float" + text.substr(i, j - i);
                    touched = true;
                    i = j - 1;
                } else {
                    out += text[i];
                }
            }
            if (touched) {
                glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, out);
                ++changed;
            }
            continue;
        }
        if (target == "index") {
            // every [expr] of the line that is not a plain number becomes [int(expr)]
            std::string out;
            bool touched = false;
            for (size_t i = 0; i < text.size(); ++i) {
                if (text[i] != '[') {
                    out += text[i];
                    continue;
                }
                int depth = 1;
                size_t j = i + 1;
                while (j < text.size() && depth > 0) {
                    depth += text[j] == '[' ? 1 : text[j] == ']' ? -1 : 0;
                    ++j;
                }
                if (depth != 0) {
                    out += text.substr(i);
                    break;
                }
                const std::string inner = text.substr(i + 1, j - i - 2);
                const bool plain = inner.find_first_not_of("0123456789 ") == std::string::npos || inner.rfind("int(", 0) == 0 || inner.rfind("uint(", 0) == 0;
                out += plain ? text.substr(i, j - i) : "[int(" + inner + ")]";
                touched = touched || !plain;
                i = j - 1;
            }
            if (touched) {
                glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, out);
                ++changed;
            }
            continue;
        }
        if (target == "return") {
            // the value of the line's `return` goes through we_to_T, T the return type of the function the line is in: the nearest
            // definition above it
            const size_t ret = text.find("return");
            const size_t semi = ret == std::string::npos ? ret : text.find(';', ret);
            if (semi == std::string::npos || !isWordAt(text, ret, 6) || text.find("we_to_", ret) < semi) {
                continue;
            }
            static const std::regex header(R"(^\s*(float|int|uint|vec2|vec3|vec4)\s+[A-Za-z_]\w*\s*\([^;)]*(\)|$))");
            std::string type;
            for (size_t lineEnd = begin; lineEnd > 0 && type.empty();) {
                const size_t lineStart = glsl.rfind('\n', lineEnd - 1);
                const size_t from = lineStart == std::string::npos ? 0 : lineStart + 1;
                std::smatch m;
                const std::string candidate = glsl.substr(from, lineEnd - from);
                if (std::regex_search(candidate, m, header)) {
                    type = m[1].str();
                }
                if (lineStart == std::string::npos) {
                    break;
                }
                lineEnd = lineStart;
            }
            if (type.empty()) {
                continue;
            }
            text = text.substr(0, ret + 6) + " we_to_" + type + "(" + text.substr(ret + 6, semi - ret - 6) + ")" + text.substr(semi);
            glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
            ++changed;
            continue;
        }
        if (target == "scalarindex") {
            // HLSL reads a subscript of a scalar as the scalar itself (`g_AudioSpectrum64Left[i / 4][i % 4]` of a float array is
            // element i / 4): a chain of subscripts deeper than its name's declaration allows loses the extra ones. Only names
            // declared here as a scalar or an array of scalars are touched.
            std::string out;
            bool touched = false;
            for (size_t i = 0; i < text.size();) {
                const bool starts = (std::isalpha(static_cast<unsigned char>(text[i])) || text[i] == '_') && (i == 0 || (!isIdentChar(text[i - 1]) && text[i - 1] != '.'));
                if (!starts) {
                    out += text[i++];
                    continue;
                }
                size_t e = i;
                while (e < text.size() && isIdentChar(text[e])) {
                    ++e;
                }
                const std::string name = text.substr(i, e - i);
                out += name;
                std::vector<std::pair<size_t, size_t>> subscripts; // [begin, end) of each `[...]`
                size_t k = e;
                while (k < text.size() && text[k] == '[') {
                    int depth = 0;
                    size_t j = k;
                    for (; j < text.size(); ++j) {
                        depth += text[j] == '[' ? 1 : text[j] == ']' ? -1 : 0;
                        if (depth == 0) {
                            break;
                        }
                    }
                    if (j >= text.size()) {
                        break;
                    }
                    subscripts.emplace_back(k, j + 1);
                    k = j + 1;
                }
                int allowed = -1;
                if (!subscripts.empty()) {
                    std::smatch m;
                    const std::regex declaration("\\b(float|int|uint|bool)\\s+" + name + "\\b\\s*(\\[)?");
                    if (std::regex_search(glsl, m, declaration)) {
                        allowed = m[2].matched ? 1 : 0;
                    }
                }
                if (allowed >= 0 && subscripts.size() > static_cast<size_t>(allowed)) {
                    for (size_t n = 0; n < static_cast<size_t>(allowed); ++n) {
                        out += text.substr(subscripts[n].first, subscripts[n].second - subscripts[n].first);
                    }
                    touched = true;
                    i = subscripts.back().second;
                } else {
                    i = e;
                }
            }
            if (touched) {
                glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, out);
                ++changed;
            }
            continue;
        }
        if (target.rfind("cutv", 0) == 0) {
            // the interpolators wider than the other operand (a vec4 v_TexCoord beside vec2s) keep their first components wherever
            // the line names them without a member
            const size_t width = static_cast<size_t>(target[4] - '0');
            std::vector<std::string> names;
            for (size_t i = glsl.find(" in vec"); i != std::string::npos; i = glsl.find(" in vec", i + 1)) {
                const size_t w = i + 7;
                const size_t semicolon = glsl.find(';', w);
                if (w < glsl.size() && std::isdigit(static_cast<unsigned char>(glsl[w])) && static_cast<size_t>(glsl[w] - '0') > width && semicolon != std::string::npos) {
                    const std::string name = glsl.substr(w + 2, semicolon - w - 2);
                    if (name.find_first_not_of(" ") != std::string::npos) {
                        names.push_back(name.substr(name.find_first_not_of(" ")));
                    }
                }
            }
            bool touched = false;
            for (const std::string &name : names) {
                for (size_t pos = text.find(name); pos != std::string::npos; pos = text.find(name, pos + 1)) {
                    const size_t after = pos + name.size();
                    if (!isWordAt(text, pos, name.size()) || (after < text.size() && (text[after] == '.' || text[after] == '['))) {
                        continue;
                    }
                    text.insert(after, width == 2 ? ".xy" : ".xyz");
                    touched = true;
                    pos = after;
                }
            }
            if (touched) {
                glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
                ++changed;
            }
            continue;
        }
        if (target.rfind("cur", 0) == 0) {
            // the right operand of the last such operator on the line, when it is a plain identifier, keeps its first components
            const size_t width = static_cast<size_t>(target[3] - '0');
            const std::string pattern = std::string(" ") + target[4] + " ";
            size_t opAt = text.rfind(pattern);
            if (opAt == std::string::npos || width < 2 || width > 3) {
                continue;
            }
            size_t b = opAt + pattern.size();
            size_t e = b;
            while (e < text.size() && (std::isalnum(static_cast<unsigned char>(text[e])) || text[e] == '_')) {
                ++e;
            }
            if (e < text.size() && text[e] == '(') {
                // a parenthesised expression or a call: the swizzle goes after its closing parenthesis
                int depth = 0;
                for (; e < text.size(); ++e) {
                    depth += text[e] == '(' ? 1 : text[e] == ')' ? -1 : 0;
                    if (depth == 0) {
                        break;
                    }
                }
                if (e >= text.size()) {
                    continue;
                }
                ++e;
            } else if (e == b) {
                continue; // not an operand this rule knows
            }
            if (e < text.size() && text[e] == '.') {
                continue; // already a member or a swizzle
            }
            text.insert(e, width == 2 ? ".xy" : ".xyz");
            glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
            ++changed;
            continue;
        }
        if (target.rfind("cut", 0) == 0) {
            // the left operand of the first such operator on the line keeps its first components only
            const size_t width = static_cast<size_t>(target[3] - '0');
            const char op = target[4];
            const size_t start = text.find('=');
            size_t opAt = text.find(std::string(" ") + op + " ", start == std::string::npos ? 0 : start);
            if (opAt == std::string::npos || width < 2 || width > 3) {
                continue;
            }
            size_t b = opAt;
            while (b > 0 && (std::isalnum(static_cast<unsigned char>(text[b - 1])) || text[b - 1] == '_' || text[b - 1] == '.')) {
                --b;
            }
            const std::string operand = text.substr(b, opAt - b);
            // a parenthesised expression or a call takes the swizzle after its closing parenthesis; a name must be a plain one
            if (!(opAt > 0 && text[opAt - 1] == ')') && (operand.empty() || operand.find('.') != std::string::npos)) {
                continue; // already a member or a swizzle: not this one
            }
            text.insert(opAt, width == 2 ? ".xy" : ".xyz");
            glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
            ++changed;
            continue;
        }
        if (target == "%") {
            if (rewriteModulo(text)) {
                glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
                ++changed;
            }
            continue;
        }
        // the first assignment of the line that has not been converted yet (a line may hold several statements: `a *= x; b *= y;`)
        size_t eq = std::string::npos;
        size_t semi = std::string::npos;
        for (size_t i = 0; i < text.size(); ++i) {
            if (text[i] == '=' && (i == 0 || std::string("=<>!").find(text[i - 1]) == std::string::npos) && (i + 1 >= text.size() || text[i + 1] != '=')) {
                const size_t end = text.find(';', i);
                if (end == std::string::npos) {
                    break;
                }
                if (text.substr(i, end - i).find("we_to_") == std::string::npos) {
                    eq = i;
                    semi = end;
                    break;
                }
                i = end;
            }
        }
        if (eq == std::string::npos || semi == std::string::npos) {
            continue;
        }
        // `vec2 a = 0.0, b = 0.0, c = x;`: a declaration of several names with one type; each initializer is converted, the commas
        // inside parentheses and brackets are the expressions' own
        {
            std::vector<size_t> commas;
            int depth = 0;
            for (size_t i = eq + 1; i < semi; ++i) {
                depth += (text[i] == '(' || text[i] == '[') ? 1 : (text[i] == ')' || text[i] == ']') ? -1 : 0;
                if (text[i] == ',' && depth == 0) {
                    commas.push_back(i);
                }
            }
            if (!commas.empty() && text.find_first_not_of(" \t") != std::string::npos && std::isalpha(static_cast<unsigned char>(text[text.find_first_not_of(" \t")]))) {
                std::string rebuilt = text.substr(0, eq + 1);
                size_t from = eq + 1;
                for (size_t k = 0; k <= commas.size(); ++k) {
                    const size_t to = k < commas.size() ? commas[k] : semi;
                    std::string piece = text.substr(from, to - from);
                    if (k > 0) {
                        // `, name = expr`: the name stays outside the conversion
                        const size_t innerEq = piece.find('=');
                        if (innerEq == std::string::npos) {
                            rebuilt += piece;
                        } else {
                            rebuilt += piece.substr(0, innerEq + 1) + " we_to_" + target + "(" + piece.substr(innerEq + 1) + ")";
                        }
                    } else {
                        rebuilt += " we_to_" + target + "(" + piece + ")";
                    }
                    if (k < commas.size()) {
                        rebuilt += ",";
                    }
                    from = to + 1;
                }
                text = rebuilt + text.substr(semi);
                glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
                ++changed;
                continue;
            }
        }
        text = text.substr(0, eq + 1) + " we_to_" + target + "(" + text.substr(eq + 1, semi - eq - 1) + ")" + text.substr(semi);
        glsl.replace(begin, end == std::string::npos ? std::string::npos : end - begin, text);
        ++changed;
    }
    for (const auto &[name, line] : unfinished) {
        changed += addFinalReturn(glsl, name, line) ? 1 : 0;
    }
    for (const std::string &name : helpers) {
        changed += addHelperOverloads(glsl, name) ? 1 : 0;
    }
    return changed;
}

std::optional<TranslatedText> translateToGlsl(const ShaderSource &vertex, const ShaderSource &fragment, const TranslateOptions &options,
                                              std::string *error)
{
    const auto fail = [&](const std::string &what) -> std::optional<TranslatedText> {
        if (error) {
            *error = what;
        }
        return std::nullopt;
    };
    TranslatedText result;
    result.layout.combos = resolveCombos(vertex, fragment, options);

    // HLSL as well as GLSL: the textures and targets here start at the top like Direct3D's, and the shipped shaders flip the
    // screen coordinates they read the framebuffer with under `#if HLSL` (refraction, copied backgrounds, blend modes)
    std::string defines = "#define GLSL 1\n#define HLSL 1\n";
    for (const auto &[name, value] : result.layout.combos) {
        defines += "#define " + name + " " + std::to_string(value) + "\n";
    }

    StageText stages[2];
    const ShaderSource *sources[2] = {&vertex, &fragment};
    for (int i = 0; i < 2; ++i) {
        const ShaderStage stage = i == 0 ? ShaderStage::Vertex : ShaderStage::Fragment;
        std::string preError;
        std::string prelude = kPrelude;
        if (options.lenient) {
            // texture coordinates of any width: HLSL drops the extra components, GLSL has no such overload
            const auto swap = [&](const std::string &from, const std::string &to) {
                const size_t at = prelude.find(from);
                if (at != std::string::npos) {
                    prelude.replace(at, from.size(), to);
                }
            };
            swap("#define texSample2D(s, uv) texture(s, uv)", "#define texSample2D(s, uv) texture(s, we_uv(uv))");
            swap("#define texSample2DLod(s, uv, l) textureLod(s, uv, l)", "#define texSample2DLod(s, uv, l) textureLod(s, we_uv(uv), l)");
            prelude += "vec2 we_uv(vec2 v) { return v; }\nvec2 we_uv(vec3 v) { return v.xy; }\nvec2 we_uv(vec4 v) { return v.xy; }\n";
        }
        const std::string pre = preprocessGlsl(stage, defines + prelude + (options.lenient ? dropHlslAliases(dropStrayConditionals(withRequiredFeatures(*sources[i]))) : withRequiredFeatures(*sources[i])), preError);
        if (pre.empty()) {
            return fail(std::string(i == 0 ? "vertex" : "fragment") + " preprocess of " + sources[i]->path + ": " + preError);
        }
        std::string splitError;
        // Words that are ordinary identifiers in the dialect but reserved in GLSL 450.
        std::string cleaned = stripDirectives(pre);
        for (const char *word : {"sample", "filter", "input", "output", "common", "partition", "active", "subroutine", "resource"}) {
            cleaned = replaceWord(cleaned, word, std::string("we_") + word);
        }
        // Under `#if HLSL` the shipped common_perspective.h defines its own inverse(mat3), which GLSL refuses next to the
        // built-in one: its name moves aside, with forwarders to the built-ins for the other sizes
        if (cleaned.find("mat3 inverse(mat3") != std::string::npos) {
            cleaned = "mat2 we_inverse(mat2 m) { return inverse(m); }\nmat4 we_inverse(mat4 m) { return inverse(m); }\n"
                + replaceWord(cleaned, "inverse", "we_inverse");
        }
        if (!splitStage(cleaned, stages[i], splitError)) {
            return fail(std::string(i == 0 ? "vertex" : "fragment") + " " + sources[i]->path + ": " + splitError);
        }
        if (options.lenient) {
            stages[i].body = dropHlslStatementSyntax(stages[i].body);
            hoistGlobalInitializers(stages[i].body, stages[i].uniforms);
            // a shader that defines a function the language already has (its own `float mod(float x, float y)`) gets a clash over
            // parameter precision: its definition and every use go by another name
            for (const char *builtin : {"mod", "round", "trunc", "sign", "fract", "step", "smoothstep"}) {
                const std::regex definition(std::string(R"((^|\n)\s*(float|vec2|vec3|vec4|int)\s+)") + builtin + R"(\s*\()");
                if (std::regex_search(stages[i].body, definition)) {
                    stages[i].body = replaceWord(stages[i].body, builtin, std::string("we_user_") + builtin);
                }
            }
            // precision qualifiers mean nothing on desktop GLSL, and two declarations of one function that differ in them (a shader
            // that redefines a helper of common.h) are an error
            for (const char *qualifier : {"highp", "mediump", "lowp", "static"}) { // (`static` is HLSL's file-scope const: effects/cloud)
                stages[i].body = replaceWord(stages[i].body, qualifier, "");
            }
            // `a %= b;` is `a = we_mod(a, b);` (HLSL's % on floats; GLSL has none, and its error names the assignment)
            stages[i].body = std::regex_replace(stages[i].body, std::regex(R"(([A-Za-z_][A-Za-z0-9_\.\[\]]*)\s*%=\s*([^;]+);)"), "$1 = we_mod($1, $2);");
            // a float uniform the shader marks as an integer is read as one (`for (int i = u_Min; ...)`)
            std::string macros;
            for (const Decl &u : stages[i].uniforms) {
                const auto note = sources[i]->notes.find(u.name);
                if (u.type == "float" && u.arrayCount == 0 && note != sources[i]->notes.end() && note->second.boolOr("int", false)) {
                    macros += "#define " + u.name + " int(" + u.name + ")\n";
                }
            }
            stages[i].body = macros + stages[i].body;
        }
    }

    // Annotations by name, merged over both stages.
    std::map<std::string, Json> notes = vertex.notes;
    for (const auto &[name, note] : fragment.notes) {
        notes.emplace(name, note);
    }

    // --- interface locations ---
    std::map<std::string, uint32_t> varyingLocation;
    uint32_t nextVarying = 0;
    for (const Decl &d : stages[0].varyings) {
        varyingLocation[d.name] = nextVarying;
        nextVarying += locationSlots(d.type, d.arrayCount);
    }
    uint32_t nextAttribute = 8;
    std::string vertexDecls;
    std::string copiedAttributes; // statements at the top of the vertex main
    for (const Decl &d : stages[0].attributes) {
        // an attribute the body never names (the `flat` shader's colour without VERTEXCOLOR) is not an input the draw must feed
        if (!mentions(stages[0].body, d.name)) {
            continue;
        }
        const auto fixed = kAttributeLocations.find(d.name);
        const uint32_t loc = fixed != kAttributeLocations.end() ? fixed->second : nextAttribute++;
        result.layout.attributes.push_back({d.name, d.type, loc});
        if (assignsTo(stages[0].body, d.name)) {
            // HLSL lets a vertex shader write to its inputs: the input keeps a name of its own and the shader's name is a global copy
            vertexDecls += "layout(location = " + std::to_string(loc) + ") in " + d.type + " we_in_" + d.name + ";\n" + d.type + " " + d.name + ";\n";
            copiedAttributes += "    " + d.name + " = we_in_" + d.name + ";\n";
            continue;
        }
        vertexDecls += "layout(location = " + std::to_string(loc) + ") in " + d.type + " " + d.name + ";\n";
    }
    std::string fragmentDecls;
    std::string copiedVaryings; // lenient: statements at the top of the fragment main
    for (const Decl &d : stages[0].varyings) {
        vertexDecls += "layout(location = " + std::to_string(varyingLocation[d.name]) + ") out " + (integerBased(d.type) ? "flat " : "")
            + d.type + " " + d.name + (d.arrayCount ? "[" + std::to_string(d.arrayCount) + "]" : "") + ";\n";
    }
    if (options.lenient) {
        // an interpolator the fragment shader declares narrower than the vertex shader writes it (`varying vec2 v_TexCoord` over a vec4,
        // then reads `.zw`): the semantic is the same, so the fragment side takes the vertex side's width
        const auto width = [](const std::string &type) { return type == "vec2" ? 2 : type == "vec3" ? 3 : type == "vec4" ? 4 : 0; };
        for (Decl &d : stages[1].varyings) {
            for (const Decl &v : stages[0].varyings) {
                if (v.name == d.name && v.arrayCount == 0 && d.arrayCount == 0 && width(d.type) > 0 && width(v.type) > width(d.type)) {
                    d.type = v.type;
                }
            }
        }
    }
    for (const Decl &d : stages[1].varyings) {
        const auto found = varyingLocation.find(d.name);
        const uint32_t loc = found != varyingLocation.end() ? found->second : (varyingLocation[d.name] = nextVarying, nextVarying += locationSlots(d.type, d.arrayCount), varyingLocation[d.name]);
        if (options.lenient && d.arrayCount == 0 && assignsTo(stages[1].body, d.name)) {
            // the shader writes to a varying: the input keeps a name of its own and the shader's name is a global copy of it
            fragmentDecls += "layout(location = " + std::to_string(loc) + ") in " + (integerBased(d.type) ? "flat " : "") + d.type + " we_in_" + d.name + ";\n"
                + d.type + " " + d.name + ";\n";
            copiedVaryings += "    " + d.name + " = we_in_" + d.name + ";\n";
            continue;
        }
        fragmentDecls += "layout(location = " + std::to_string(loc) + ") in " + (integerBased(d.type) ? "flat " : "") + d.type + " " + d.name
            + (d.arrayCount ? "[" + std::to_string(d.arrayCount) + "]" : "") + ";\n";
    }

    // --- uniform blocks and samplers ---
    std::map<uint32_t, SamplerInfo> samplers;
    uint32_t nextOddSampler = 16;
    for (int i = 0; i < 2; ++i) {
        StageProgram &program = i == 0 ? result.layout.vertex : result.layout.fragment;
        std::string &decls = i == 0 ? vertexDecls : fragmentDecls;
        uint32_t offset = 0;
        std::string block;
        for (const Decl &d : stages[i].uniforms) {
            const Std140 layout = std140Of(d.type);
            if (layout.size == 0) {
                return fail("unsupported uniform type '" + d.type + "' of " + d.name);
            }
            // Arrays take a vec4 slot per element (std140), whatever the element type.
            const uint32_t align = d.arrayCount ? 16 : layout.alignment;
            const uint32_t stride = d.arrayCount ? roundUp(layout.size, 16) : layout.size;
            offset = roundUp(offset, align);
            UniformInfo info;
            info.name = d.name;
            info.type = d.type;
            info.arrayCount = d.arrayCount;
            info.offset = offset;
            info.size = d.arrayCount ? stride * d.arrayCount : layout.size;
            info.input = classifyUniform(d.name, &info.aux);
            info.used = contains(stages[i].body, d.name);
            if (const auto note = notes.find(d.name); note != notes.end()) {
                info.materialKey = note->second.stringOr("material");
                info.lookupKey = lowerCase(info.materialKey);
                info.isColor = note->second.stringOr("type") == "color";
                if (const Json *def = note->second.find("default")) {
                    if (def->isString()) {
                        info.defaultText = def->asString();
                    } else if (def->isNumber()) {
                        std::ostringstream n;
                        n << def->asNumber();
                        info.defaultText = n.str();
                    } else if (def->isBool()) {
                        info.defaultText = def->asBool() ? "1" : "0";
                    }
                }
            }
            offset += info.size;
            block += "    " + d.type + " " + d.name + (d.arrayCount ? "[" + std::to_string(d.arrayCount) + "]" : "") + ";\n";
            program.uniforms.push_back(std::move(info));
        }
        program.blockSize = program.uniforms.empty() ? 0 : roundUp(offset, 16);
        if (program.blockSize > kMaxUniformBlock) {
            return fail(std::string(i == 0 ? "vertex" : "fragment") + " stage of " + sources[i]->path + ": the uniform block is " + std::to_string(program.blockSize)
                        + " bytes, over the " + std::to_string(kMaxUniformBlock) + " byte limit");
        }
        if (!block.empty()) {
            decls += "layout(std140, set = 0, binding = " + std::to_string(i) + ") uniform " + (i == 0 ? "WeVertexUniforms" : "WeFragmentUniforms")
                + " {\n" + block + "};\n";
        }
        for (const Decl &d : stages[i].samplers) {
            uint32_t binding = 0;
            if (d.name.rfind("g_Texture", 0) == 0 && d.name.size() > 9 && std::isdigit(static_cast<unsigned char>(d.name[9]))) {
                binding = static_cast<uint32_t>(std::atoi(d.name.c_str() + 9));
            } else {
                binding = nextOddSampler++;
            }
            std::string type = d.type;
            if (type == "sampler2DComparison") {
                type = "sampler2DShadow";
            } else if (type == "sampler2DBackBuffer") {
                type = "sampler2D";
            }
            decls += "layout(set = 1, binding = " + std::to_string(binding) + ") uniform " + type + " " + d.name + ";\n";
            SamplerInfo info;
            info.name = d.name;
            info.binding = binding;
            info.type = type;
            if (const auto note = notes.find(d.name); note != notes.end()) {
                info.comboName = note->second.stringOr("combo");
                info.defaultText = note->second.stringOr("default");
                info.hidden = note->second.boolOr("hidden", false);
            }
            samplers.emplace(binding, std::move(info));
        }
    }
    for (auto &[binding, info] : samplers) {
        result.layout.samplers.push_back(std::move(info));
    }

    // --- assemble ---
    std::string vertexBody = stages[0].body;
    vertexBody = replaceWord(vertexBody, "gl_VertexID", "gl_VertexIndex");
    vertexBody = replaceWord(vertexBody, "gl_InstanceID", "gl_InstanceIndex");
    // the viewport a vertex goes to (WE's shadow casters draw each instance into its own viewport): Vulkan needs a device feature for
    // that output, so the engine draws one viewport at a time instead (the instance index picks the matrix) and the value is dropped
    if (mentions(vertexBody, "gl_ViewportIndex")) {
        vertexBody = "int we_viewportIndex;\n" + replaceWord(vertexBody, "gl_ViewportIndex", "we_viewportIndex");
    }
    // wrap main so clip depth maps from [-1, 1] to [0, 1]
    const size_t mainAt = vertexBody.find("void main");
    if (mainAt == std::string::npos) {
        return fail("vertex shader " + vertex.path + " has no main()");
    }
    vertexBody.replace(mainAt, 9, "void we_main");
    if (!copiedAttributes.empty()) {
        const size_t brace = vertexBody.find('{', mainAt);
        if (brace != std::string::npos) {
            vertexBody.insert(brace + 1, "\n" + copiedAttributes);
        }
    }
    const std::string shim = options.lenient ? kShimConversions : "";
    result.vertex = "#version 450\n" + shim + vertexDecls + vertexBody
        + "\nvoid main() {\n    we_main();\n    gl_Position.y = -gl_Position.y;\n    gl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;\n}\n";

    std::string fragmentBody = stages[1].body;
    if (!copiedVaryings.empty()) {
        const size_t mainAt = fragmentBody.find("void main");
        const size_t brace = mainAt == std::string::npos ? mainAt : fragmentBody.find('{', mainAt);
        if (brace != std::string::npos) {
            fragmentBody.insert(brace + 1, "\n" + copiedVaryings);
        }
    }
    // HLSL's position input of a pixel shader (SV_Position) is the pixel's window position, which a fragment shader names
    // gl_FragCoord: a fragment shader that reads gl_Position (effects/light_pillar) means that
    fragmentBody = replaceWord(fragmentBody, "gl_Position", "gl_FragCoord");
    std::string fragmentOut;
    if (contains(fragmentBody, "gl_FragColor")) {
        fragmentBody = replaceWord(fragmentBody, "gl_FragColor", "we_FragColor");
        fragmentOut = "layout(location = 0) out vec4 we_FragColor;\n";
    }
    result.fragment = "#version 450\n" + shim + fragmentDecls + fragmentOut + fragmentBody;
    return result;
}

std::optional<TranslatedShader> translateShader(const ShaderSource &vertex, const ShaderSource &fragment, const TranslateOptions &options,
                                                std::string *error)
{
    const std::string key = shaderCacheKey(vertex, fragment, options.combos, options.boundTextures);
    if (auto cached = loadCachedShader(key, error); cached.hit) {
        if (cached.shader) {
            return std::move(cached.shader);
        }
        return std::nullopt; // a remembered failure; `error` has the reason
    }
    std::string detail;
    std::optional<TranslatedShader> out;
    // the strict translation first; a shader that does not compile gets one more try with the compatibility shim
    // The shim has a wall clock of its own over all its rounds: 20 s, or ten times what the strict attempt took when that was
    // slow (a machine under load deschedules the compiler, and a budget in plain seconds would count the waiting). The
    // compiler child is still bounded by its CPU limit, so a macro bomb ends quickly whatever this says.
    const auto strictStart = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point shimStart;
    std::chrono::steady_clock::duration shimBudget = std::chrono::seconds(20);
    const auto shimExpired = [&] { return std::chrono::steady_clock::now() - shimStart > shimBudget; };
    for (const bool lenient : {false, true}) {
        TranslateOptions attempt = options;
        attempt.lenient = lenient;
        detail.clear();
        auto text = translateToGlsl(vertex, fragment, attempt, &detail);
        if (text) {
            // debugging aid: MURASAKI_SCENE_VK_DUMP_GLSL=DIR writes the GLSL of every shader that is translated (a cache hit is not)
            if (const char *dir = std::getenv("MURASAKI_SCENE_VK_DUMP_GLSL"); dir && *dir) {
                const std::string base = std::string(dir) + "/" + key.substr(0, 16) + "-" + fragment.path.substr(fragment.path.find_last_of('/') + 1);
                std::string combos = "// combos:";
                for (const auto &[name, value] : text->layout.combos) {
                    combos += " " + name + "=" + std::to_string(value);
                }
                std::ofstream(base + ".vert.glsl") << combos << "\n" << text->vertex;
                std::ofstream(base + ".frag.glsl") << combos << "\n" << text->fragment;
            }
            TranslatedShader shader = text->layout;
            std::string compileError;
            // (the shim may need a few rounds: each fixes the conversions the compiler named, one error at a time)
            const int rounds = lenient ? 12 : 1;
            for (int round = 0; round < rounds && !(lenient && shimExpired()); ++round) {
                shader.vertex.spirv = compileGlsl(ShaderStage::Vertex, text->vertex, compileError);
                if (shader.vertex.spirv.empty()) {
                    detail = "vertex " + vertex.path + ": " + compileError;
                    if (lenient && fixConversions(text->vertex, compileError) > 0) {
                        continue;
                    }
                    break;
                }
                shader.fragment.spirv = compileGlsl(ShaderStage::Fragment, text->fragment, compileError);
                if (shader.fragment.spirv.empty()) {
                    detail = "fragment " + fragment.path + ": " + compileError;
                    if (const char *dir = std::getenv("MURASAKI_SCENE_VK_DUMP_GLSL"); dir && *dir && lenient) {
                        // what the shim made of it when it still would not compile
                        std::ofstream(std::string(dir) + "/" + key.substr(0, 16) + "-shimmed-" + fragment.path.substr(fragment.path.find_last_of('/') + 1) + ".frag.glsl") << text->fragment;
                    }
                    if (lenient && fixConversions(text->fragment, compileError) > 0) {
                        continue;
                    }
                    break;
                }
                out = std::move(shader);
                break;
            }
        }
        if (out) {
            if (lenient) {
                logInfo("shader {} compiled with the compatibility shim", fragment.path);
            }
            break;
        }
        // a stray `#endif` or `#else`, or a condition that is not an expression, fails the preprocessor: a diagnostic about the source the
        // lenient attempt can fix
        const bool strayDirective = !text && detail.find("preprocess of") != std::string::npos
            && (detail.find("mismatched statements") != std::string::npos || detail.find("unexpected tokens following directive") != std::string::npos);
        if (!text && !strayDirective) {
            break; // not a compile failure (a missing main, an unsupported uniform): the shim does not help
        }
        if (!lenient) {
            shimStart = std::chrono::steady_clock::now();
            shimBudget = std::max<std::chrono::steady_clock::duration>(shimBudget, (shimStart - strictStart) * 10);
        }
        if (!lenient && (detail.find("exceeded its limits") != std::string::npos || detail.find("ERROR:") == std::string::npos)) {
            break; // a resource limit or a crash of the compiler, not a diagnostic about the source
        }
    }
    // a crash of the compiler, or a child that gave no answer, is about the process it ran in (its memory, a signal), not about the
    // source: it is not remembered, so the next start tries again
    const bool aboutTheProcess = !out && (detail.find("the shader compiler crashed") != std::string::npos
                                          || detail.find("stopped without an answer") != std::string::npos);
    if (!aboutTheProcess) {
        storeCachedShader(key, out ? &*out : nullptr, detail);
    }
    if (!out && error) {
        *error = detail;
    }
    return out;
}

} // namespace murasaki::scene
