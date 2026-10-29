#pragma once

// Translated shaders for the passes of a scene, built once and kept: (shader name, combos, bound texture slots) ->
// TranslatedShader. Works on the CPU only (translation and glslang), so `--check` can use it to find out whether a
// scene's shaders compile without a GPU.
//
// Shader names resolve through the Vfs (`shaders/<name>.vert|.frag`: item, assets, shipped effect folders). Two
// built-in sources exist: `builtin/composite` (draws a texture with a tint, the last step of a layer with effects)
// and a fallback for `genericimage` / `genericimage2`, used only when the Vfs has no such shader (a machine without
// the Wallpaper Engine assets, tests), written to behave like the shipped one; and one for `shadowcaster` (render/shadows.h).

#include "assets/vfs.h"
#include "scene/material.h"
#include "shaders/translate.h"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace murasaki::scene {

class ShaderLibrary
{
public:
    explicit ShaderLibrary(const Vfs &vfs) : m_vfs(vfs) {}

    // The format code of a texture by the name a material gives it (nothing for a render target or an unknown name).
    // Shaders read it as the TEX<n>FORMAT combos (WE's own: R8 and RG88 samples are rearranged by
    // ConvertTexture0Format, compressed normal maps decoded by DecompressNormal); a format of 0 sets nothing.
    using TextureFormats = std::function<std::optional<int>(const std::string &)>;
    void setTextureFormats(TextureFormats formats) { m_formats = std::move(formats); }
    // The scene's combos reach a shader, and its cache key, only where they can change it (one rule, sceneComboUse), so a shader
    // that reads neither the lights nor a scene combo keeps one key whatever the scene sets:
    //
    // The combos of the scene's lights (render/lights.h: the counts per type and SCENE_ORTHO), set on every shader that reads the
    // lights: one that asks for `#require LightingV1` or declares light arrays itself (`g_LPoint_*`: generic3, genericimage3;
    // `g_LightsPosition`: generic2, genericimage2). They are constant for a scene, so it compiles each lit shader once for its counts.
    void setLightCombos(std::map<std::string, int> combos) { m_lightCombos = std::move(combos); }

    // Combos the scene sets on the passes it asks for: HDR in an HDR scene, FOG_DIST / FOG_HEIGHT / FOG_COMPUTED while a fog band
    // is on. A scene combo reaches a shader (and its cache key) only when the shader's text names it, and the fog ones only when
    // the shader's own FOG combo is on (declared, and not turned off by the material), so switching the fog does not rebuild the
    // shaders it cannot change. A material's own value of a combo wins.
    void setSceneCombos(std::map<std::string, int> combos) { m_sceneCombos = std::move(combos); }
    const std::map<std::string, int> &sceneCombos() const { return m_sceneCombos; }

    // Null with `*error` set when the shader cannot be found, translated or compiled (the failure is remembered).
    const TranslatedShader *get(const MaterialPass &pass, const std::vector<uint32_t> &boundSlots, std::string *error = nullptr);
    // The composite shader (no material needed).
    const TranslatedShader *composite(std::string *error = nullptr);
    // The scaling pass that puts the scene into the export image.
    const TranslatedShader *present(std::string *error = nullptr);

    // The shader that draws a material's shadow casters: the one its shader names in `// [PASS] shadow <shader>` (chroma4, foliage4,
    // fur4), else WE's `shadowcaster` (generic4 and every other; a built-in one of the same interface where the shaders are not
    // installed). Read once per shader name.
    std::string shadowCasterOf(const std::string &shaderName);
    // A material's shadow-caster pass: that shader with the material's textures and constants and, for `shadowcaster`, only the
    // combos that move the vertices or test the alpha (SKINNING, BONECOUNT, ALPHATOCOVERAGE); a material's own caster gets all its
    // combos. Depth tested and written, both faces.
    MaterialPass shadowCasterPass(const MaterialPass &material);

    // Every shader built so far, for the animation and pointer analysis.
    std::vector<const TranslatedShader *> all() const;

private:
    const TranslatedShader *build(const std::string &key, const std::string &shaderName, const TranslateOptions &options, std::string *error);

    // What a shader (by name) can take of the scene's combos: whether it reads the lights (then all the light combos), the scene
    // combos (HDR, FOG_DIST, FOG_HEIGHT, FOG_COMPUTED) its text names, and whether it declares FOG (and its default). Read once per
    // shader name.
    struct SceneComboUse {
        bool readsLights = false;
        std::set<std::string> named;
        bool declaresFog = false;
        int fogDefault = 0;
    };
    const SceneComboUse &sceneComboUse(const std::string &shaderName);

    const Vfs &m_vfs;
    TextureFormats m_formats;
    std::map<std::string, int> m_sceneCombos;
    std::map<std::string, SceneComboUse> m_sceneComboUse;
    std::map<std::string, int> m_lightCombos;
    std::map<std::string, std::string> m_shadowCasters;
    std::map<std::string, std::unique_ptr<TranslatedShader>> m_built;
    std::map<std::string, std::string> m_failed;
};

// The slots a pass binds: material textures, effect binds and (when nothing else gives slot 0) `previous`.
std::vector<uint32_t> boundSlotsOf(const MaterialPass &pass, const std::vector<int> &extraSlots);

} // namespace murasaki::scene
