#pragma once

// Materials: `materials/*.json`, the recipe of one draw (shader, blending, textures, combos and constants). A scene
// object's model points to one, an effect pass points to one, and a scene's effect instance overrides parts of it.
//
//   {"passes": [{"shader": "effects/tint", "blending": "normal", "depthtest": "disabled", "depthwrite": "disabled",
//                "cullmode": "nocull", "textures": [null, "effects/mask"], "combos": {"VERSION": 2},
//                "constantshadervalues": {"alpha": 1.0, "color": "1 0.5 0", "scale": {"user": "prop", "value": 2}}}]}
//
// `constantshadervalues` are keyed by the uniform's `material` annotation (see shaders/translate.h), values are
// numbers, "x y z" strings, or `{"user": property, "value": default}` objects bound to a user property.

#include "assets/vfs.h"
#include "util/json.h"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace murasaki::scene {

enum class Blending { Normal, Translucent, Additive };
// `cullmode`: "normal" culls the faces seen from behind (counter-clockwise is the front, as the models are wound), "nocull" none.
// `Front` is for the engine's own draws (the back faces of a volumetric light's volume), no material asks for it.
enum class CullMode { None, Back, Front };

// One constant: up to four floats and, when it is bound to a user property, the property's name.
struct Constant {
    std::array<float, 4> value{};
    int count = 0;       // components given (1 for a number, up to 4 for a vector or colour)
    std::string user;    // bound property; empty for a literal
    bool scripted = false; // {"script": ...}: SceneScript, not supported before M4
};

// A texture slot the user's media or a user shortcut fills: `usertextures` entry {"type":"system","name":"$mediaThumbnail"} (the cover of what plays) or
// "$mediaPreviousThumbnail" (the one before the last change). The renderer swaps the slot's texture name for the cover while there is
// one and puts the file the scene lists there (`placeholder`, noted the first time) back when there is none.
// A `usertextures` entry {"type":"usershortcut","name":K} is the same with `shortcut` = K: the slot shows the icon of the application the
// user chose for the property K (the host sends it, bridge/shortcutview.h), the file's texture until there is one.
struct MediaBinding {
    uint32_t slot = 0;
    bool previous = false;
    std::string shortcut; // the property key of a shortcut slot ("" for a media cover)
    std::string placeholder;
    bool captured = false;
};

struct MaterialPass {
    std::string shader;                     // "genericimage2", "effects/tint"
    Blending blending = Blending::Translucent;
    bool blendingKnown = true;              // false: a blending name this engine does not know
    std::string blendingName;
    std::vector<std::string> textures;      // slot -> texture name ("" for null); the file is materials/<name>.tex
    std::map<std::string, int> combos;      // names upper-cased
    std::map<std::string, Constant> constants;
    std::vector<MediaBinding> media;        // slots the media cover fills
    std::vector<std::string> userTextureProperties; // properties that name a texture slot (bare `usertextures` strings)
    std::vector<std::string> degraded;      // keys the pass uses and the engine does not follow ("usershadervalues" without a property, "usertextures <type>")
    // The depth keys, followed by 3D meshes only (2D layers are drawn in file order without depth): "enabled" unless "disabled",
    // and the cull mode "normal" unless "nocull" (what the editor writes into every mesh material of the library).
    bool depthTest = true;
    bool depthWrite = true;
    CullMode cull = CullMode::Back;
};

struct Material {
    std::vector<MaterialPass> passes;
};

// Reads one value of `constantshadervalues` (or of a scene's per-instance override).
Constant parseConstant(const Json &value);
void parseConstants(const Json *object, std::map<std::string, Constant> &out);
// `usershadervalues`: constants keyed like `constantshadervalues` whose value is the name of a user property ("flagcolor1": "color2").
// Each key gets the property as its `user` binding (keeping a literal `constantshadervalues` gave it, the value shown while the
// property is not set); the binding wins over the literal.
void parseUserShaderValues(const Json *object, std::map<std::string, Constant> &out);
// One combo (name upper-cased) of a number or a bool; false (and nothing set) for any other value, such as a user binding.
bool parseCombo(const std::string &name, const Json &value, std::map<std::string, int> &out);
void parseCombos(const Json *object, std::map<std::string, int> &out);
std::vector<std::string> parseTextureList(const Json *array);

std::optional<Material> loadMaterial(const Vfs &vfs, const std::string &path, std::string *error = nullptr);
// A pass from an already parsed JSON object (used for materials and for tests).
MaterialPass parseMaterialPass(const Json &pass);

// The user bindings a pass (or an instance override of one) carries that the engine does not follow: a `usershadervalues` entry that
// names no property ("usershadervalues") and, for a user texture it cannot fill: "media thumbnail" ($mediaThumbnail, $mediaPreviousThumbnail), "user shortcuts" (usershortcut), and
// "usertextures <type>" for a kind it does not know ("usertextures system <name>", "usertextures value").
std::vector<std::string> userBindings(const Json &pass);
// The media covers a pass's `usertextures` asks for (see MediaBinding).
std::vector<MediaBinding> mediaBindings(const Json &pass);
// The user properties a pass's `usertextures` names (a texture the user picks): not degraded, the slot shows its default until set.
std::vector<std::string> userTextureProperties(const Json &pass);

} // namespace murasaki::scene
