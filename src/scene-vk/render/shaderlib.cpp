#include "render/shaderlib.h"

#include <algorithm>
#include <cctype>
#include <set>
#include <sstream>

namespace murasaki::scene {

namespace {

constexpr const char *kCompositeVert = R"(
uniform mat4 g_ModelViewProjectionMatrix;
attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec2 v_TexCoord;
void main() {
    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
    v_TexCoord = a_TexCoord;
}
)";

constexpr const char *kCompositeFrag = R"(
uniform sampler2D g_Texture0;
uniform vec4 g_Tint;
varying vec2 v_TexCoord;
void main() {
    vec4 c = texSample2D(g_Texture0, v_TexCoord);
    gl_FragColor = vec4(c.rgb * g_Tint.rgb, c.a * g_Tint.a);
}
)";

// The last step of a frame: the scene target shown in the export image with a fill, fit or stretch placement.
// g_Present is (width, height) of the placed scene in output pixels and (x, y) of its top left corner; g_Fade darkens it (camera fade).
constexpr const char *kPresentFrag = R"(
uniform sampler2D g_Texture0;
uniform vec4 g_Present;
uniform float g_Fade; // how far the picture has faded to black (a camera fade between paths): 0 shows it whole
varying vec2 v_TexCoord;
void main() {
    vec2 uv = (gl_FragCoord.xy - g_Present.zw) / g_Present.xy;
    if (uv.x < 0.0 || uv.y < 0.0 || uv.x > 1.0 || uv.y > 1.0) {
        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);
    } else {
        gl_FragColor = vec4(texSample2D(g_Texture0, uv).rgb * (1.0 - g_Fade), 1.0);
    }
}
)";

// What genericimage2 does for an ordinary picture (brightness and alpha, or the colour of VERSION 2).
constexpr const char *kGenericImageVert = R"(
uniform mat4 g_ModelViewProjectionMatrix;
#if SPRITESHEET
uniform vec4 g_Texture0Rotation;
uniform vec2 g_Texture0Translation;
#endif
attribute vec3 a_Position;
attribute vec2 a_TexCoord;
varying vec2 v_TexCoord;
void main() {
    gl_Position = mul(vec4(a_Position, 1.0), g_ModelViewProjectionMatrix);
#if SPRITESHEET
    v_TexCoord = g_Texture0Translation + a_TexCoord.x * g_Texture0Rotation.xy + a_TexCoord.y * g_Texture0Rotation.zw;
#else
    v_TexCoord = a_TexCoord;
#endif
}
)";

constexpr const char *kGenericImageFrag = R"(
uniform sampler2D g_Texture0; // {"label":"ui_editor_properties_albedo","nonremovable":true}
#ifndef VERSION
uniform float g_Brightness; // {"material":"Brightness","label":"ui_editor_properties_brightness","default":1,"range":[0,2]}
uniform float g_UserAlpha; // {"material":"Alpha","label":"ui_editor_properties_alpha","default":1,"range":[0,1]}
#else
uniform vec4 g_Color4;
#endif
varying vec2 v_TexCoord;
void main() {
    vec4 color = texSample2D(g_Texture0, v_TexCoord);
#ifndef VERSION
    color.rgb *= g_Brightness;
    color.a *= g_UserAlpha;
#else
    color *= g_Color4;
#endif
    gl_FragColor = color;
}
)";

// The shadow caster of a machine without Wallpaper Engine's shaders (tests): the depth of a mesh, skinned when SKINNING, seen
// through the picture's matrix of g_ViewportViewProjectionMatrices the instance index names (the shipped `shadowcaster`'s interface).
constexpr const char *kShadowCasterVert = R"(
uniform mat4 g_ModelMatrix;
uniform mat4 g_ViewportViewProjectionMatrices[6];
attribute vec3 a_Position;
#if SKINNING
uniform mat4x3 g_Bones[BONECOUNT];
attribute uvec4 a_BlendIndices;
attribute vec4 a_BlendWeights;
#endif
void main() {
    vec3 p = a_Position;
#if SKINNING
    p = mul(vec4(p, 1.0), g_Bones[a_BlendIndices.x] * a_BlendWeights.x + g_Bones[a_BlendIndices.y] * a_BlendWeights.y
                        + g_Bones[a_BlendIndices.z] * a_BlendWeights.z + g_Bones[a_BlendIndices.w] * a_BlendWeights.w);
#endif
    gl_Position = mul(mul(vec4(p, 1.0), g_ModelMatrix), g_ViewportViewProjectionMatrices[gl_InstanceID]);
}
)";

constexpr const char *kShadowCasterFrag = R"(
void main() {
}
)";

std::string upper(std::string s)
{
    for (char &c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return s;
}

std::optional<ShaderSource> loadStage(const Vfs &vfs, const std::string &shaderName, const char *extension, std::string *error)
{
    const std::string path = "shaders/" + shaderName + extension;
    const bool vertex = std::string(extension) == ".vert";
    if (shaderName == "builtin/composite" || shaderName == "builtin/present") {
        const char *frag = shaderName == "builtin/composite" ? kCompositeFrag : kPresentFrag;
        return parseShaderSource(path, vertex ? kCompositeVert : frag, [](const std::string &, const std::string &) { return std::nullopt; }, error);
    }
    if (vfs.exists(path)) {
        return loadShaderSource(vfs, path, error);
    }
    if (shaderName == "genericimage2" || shaderName == "genericimage") {
        return parseShaderSource(path, vertex ? kGenericImageVert : kGenericImageFrag, [](const std::string &, const std::string &) { return std::nullopt; }, error);
    }
    if (shaderName == "shadowcaster") {
        return parseShaderSource(path, vertex ? kShadowCasterVert : kShadowCasterFrag, [](const std::string &, const std::string &) { return std::nullopt; }, error);
    }
    return loadShaderSource(vfs, path, error); // sets the not-found message
}

} // namespace

std::vector<uint32_t> boundSlotsOf(const MaterialPass &pass, const std::vector<int> &extraSlots)
{
    std::set<uint32_t> slots;
    for (size_t i = 0; i < pass.textures.size(); ++i) {
        if (!pass.textures[i].empty()) {
            slots.insert(static_cast<uint32_t>(i));
        }
    }
    for (int s : extraSlots) {
        if (s >= 0) {
            slots.insert(static_cast<uint32_t>(s));
        }
    }
    return {slots.begin(), slots.end()};
}

const TranslatedShader *ShaderLibrary::build(const std::string &key, const std::string &shaderName, const TranslateOptions &options, std::string *error)
{
    if (const auto it = m_built.find(key); it != m_built.end()) {
        return it->second.get();
    }
    if (const auto it = m_failed.find(key); it != m_failed.end()) {
        if (error) {
            *error = it->second;
        }
        return nullptr;
    }
    std::string failure;
    const auto vert = loadStage(m_vfs, shaderName, ".vert", &failure);
    const auto frag = vert ? loadStage(m_vfs, shaderName, ".frag", &failure) : std::nullopt;
    if (vert && frag) {
        if (auto translated = translateShader(*vert, *frag, options, &failure)) {
            const TranslatedShader *out = m_built.emplace(key, std::make_unique<TranslatedShader>(std::move(*translated))).first->second.get();
            return out;
        }
    }
    m_failed[key] = "shader '" + shaderName + "': " + failure;
    if (error) {
        *error = m_failed[key];
    }
    return nullptr;
}

const TranslatedShader *ShaderLibrary::get(const MaterialPass &material, const std::vector<uint32_t> &boundSlots, std::string *error)
{
    MaterialPass pass = material;
    // the scene's combos, each only on a shader it can change (sceneComboUse): a shader that reads neither the lights nor any scene
    // combo keeps one key whatever the scene sets
    const SceneComboUse *use = m_lightCombos.empty() && m_sceneCombos.empty() ? nullptr : &sceneComboUse(pass.shader);
    if (use && use->readsLights) {
        for (const auto &[name, value] : m_lightCombos) {
            pass.combos[name] = value;
        }
    }
    if (m_formats) {
        for (size_t i = 0; i < pass.textures.size(); ++i) {
            if (pass.textures[i].empty() || pass.textures[i].rfind("_rt_", 0) == 0) {
                continue;
            }
            if (const auto format = m_formats(pass.textures[i]); format && *format > 0) {
                pass.combos["TEX" + std::to_string(i) + "FORMAT"] = *format;
            }
        }
    }
    if (use && !m_sceneCombos.empty()) {
        // the shader's FOG as this material resolves it: the material's value (any case), else the declared default
        bool fogOn = use->declaresFog && use->fogDefault != 0;
        for (const auto &[name, value] : pass.combos) {
            if (upper(name) == "FOG") {
                fogOn = use->declaresFog && value != 0;
            }
        }
        for (const auto &[name, value] : m_sceneCombos) {
            const bool fogCombo = name.rfind("FOG_", 0) == 0;
            if (use->named.count(name) && (!fogCombo || fogOn)) {
                pass.combos.emplace(name, value);
            }
        }
    }
    std::ostringstream key;
    key << pass.shader;
    for (const auto &[name, value] : pass.combos) {
        key << ' ' << upper(name) << '=' << value;
    }
    key << " |";
    for (uint32_t s : boundSlots) {
        key << s << ',';
    }
    TranslateOptions options;
    options.combos = pass.combos;
    options.boundTextures = boundSlots;
    return build(key.str(), pass.shader, options, error);
}

std::string ShaderLibrary::shadowCasterOf(const std::string &shaderName)
{
    if (const auto it = m_shadowCasters.find(shaderName); it != m_shadowCasters.end()) {
        return it->second;
    }
    std::string caster = "shadowcaster";
    for (const char *extension : {".frag", ".vert"}) {
        std::string ignored;
        const auto source = loadStage(m_vfs, shaderName, extension, &ignored);
        if (!source) {
            continue;
        }
        for (const std::string &pass : source->extraPasses) {
            // `// [PASS] shadow shadowcasterfoliage4`: the pass's name, then its shader
            std::istringstream words(pass);
            std::string name, shader, extra;
            if (words >> name >> shader && !(words >> extra) && name == "shadow") {
                caster = shader;
            }
        }
    }
    return m_shadowCasters.emplace(shaderName, caster).first->second;
}

MaterialPass ShaderLibrary::shadowCasterPass(const MaterialPass &material)
{
    MaterialPass pass;
    pass.shader = shadowCasterOf(material.shader);
    pass.blending = Blending::Normal;
    pass.textures = material.textures;
    pass.constants = material.constants;
    if (pass.shader == "shadowcaster") {
        // WE's own caster takes what moves the vertices (the skinning) and the alpha test of its texture
        for (const char *name : {"SKINNING", "BONECOUNT", "ALPHATOCOVERAGE"}) {
            if (const auto it = material.combos.find(name); it != material.combos.end()) {
                pass.combos.insert(*it);
            }
        }
    } else {
        pass.combos = material.combos; // a material's own caster (foliage, fur) moves its vertices with the material's values
    }
    pass.depthTest = true;
    pass.depthWrite = true;
    pass.cull = CullMode::None;
    return pass;
}

const ShaderLibrary::SceneComboUse &ShaderLibrary::sceneComboUse(const std::string &shaderName)
{
    if (const auto it = m_sceneComboUse.find(shaderName); it != m_sceneComboUse.end()) {
        return it->second;
    }
    SceneComboUse use;
    const auto isWordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    for (const char *extension : {".vert", ".frag"}) {
        std::string ignored;
        const auto source = loadStage(m_vfs, shaderName, extension, &ignored);
        if (!source) {
            continue; // the build reports it
        }
        // asks for the engine's lighting function, or declares light arrays of its own (generic3 and genericimage3 the per-type ones,
        // generic2 and genericimage2 the older four-light ones)
        use.readsLights = use.readsLights || std::find(source->requires_.begin(), source->requires_.end(), "LightingV1") != source->requires_.end()
            || source->text.find("g_LPoint_") != std::string::npos || source->text.find("g_LightsPosition") != std::string::npos;
        for (const ComboDecl &c : source->combos) {
            if (c.name == "FOG") {
                use.declaresFog = true;
                use.fogDefault = c.defaultValue;
            }
        }
        // every combo a scene can set, not only the current ones: a property may switch a fog band on later
        for (const std::string name : {"HDR", "FOG_DIST", "FOG_HEIGHT", "FOG_COMPUTED"}) {
            const std::string &text = source->text;
            for (size_t at = text.find(name); at != std::string::npos; at = text.find(name, at + 1)) {
                const size_t end = at + name.size();
                if ((at == 0 || !isWordChar(text[at - 1])) && (end >= text.size() || !isWordChar(text[end]))) {
                    use.named.insert(name);
                    break;
                }
            }
        }
    }
    return m_sceneComboUse.emplace(shaderName, std::move(use)).first->second;
}

const TranslatedShader *ShaderLibrary::present(std::string *error)
{
    TranslateOptions options;
    options.boundTextures = {0};
    return build("builtin/present", "builtin/present", options, error);
}

const TranslatedShader *ShaderLibrary::composite(std::string *error)
{
    TranslateOptions options;
    options.boundTextures = {0};
    return build("builtin/composite", "builtin/composite", options, error);
}

std::vector<const TranslatedShader *> ShaderLibrary::all() const
{
    std::vector<const TranslatedShader *> out;
    for (const auto &[key, shader] : m_built) {
        out.push_back(shader.get());
    }
    return out;
}

} // namespace murasaki::scene
