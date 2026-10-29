#pragma once

// Effects: `effects/<name>/effect.json`, a chain of material passes applied to an image layer, and the instances a
// scene makes of them.
//
//   {"passes": [{"material": "materials/effects/blur_downsample4.json", "target": "_rt_QuarterCompoBuffer1",
//                "bind": [{"name": "previous", "index": 0}]}, ...],
//    "fbos": [{"name": "_rt_QuarterCompoBuffer1", "scale": 4, "format": "rgba_backbuffer"}]}
//
// A pass draws its material into `target` (an effect `fbo`, or when absent the layer's current ping-pong buffer).
// `bind` fills texture slots: `previous` is what the layer had before this effect (or the last pass that wrote
// the layer's buffer), any other name is a render target. A pass with `command: copy` copies `source` to `target`
// instead of drawing. A scene instance (an entry of an object's `effects`) overrides per pass: constants,
// combos and textures.

#include "scene/material.h"
#include "scene/properties.h"

#include <string>
#include <vector>

namespace murasaki::scene {

struct EffectBind {
    std::string name; // "previous" or a render target name
    int index = 0;    // texture slot
};

struct EffectFbo {
    std::string name;       // "_rt_QuarterCompoBuffer1"
    int scale = 1;          // the target is 1/scale of the layer's size; 0 when the file gave none
    std::string format;     // "rgba8888", "rgba_backbuffer", "r8"
    bool unique = false;
    int width = 0;          // explicit size (rare)
    int height = 0;
    int fit = 0;            // `fit`: the long side in pixels, the layer's aspect kept (an assumption, formats.md "Effects"); 0: none
    bool fitBare = false;   // the file gives `fit` as a flag with no size (not followed)
    bool repeat = false;    // `uvs` "repeat": the target is sampled with wrapping coordinates (clamped otherwise)
    bool uvsOther = false;  // the file gives `uvs` other than clamp and repeat (not followed)
};

struct FboSize {
    uint32_t width = 1;
    uint32_t height = 1;
};

// The size in pixels of an fbo for a layer buffer of `layerWidth` x `layerHeight`. An explicit width and height win; then `fit`, a
// fixed long side (not scaled by `reduction`, the chain's own quality step) with the layer's aspect; else the layer's size divided
// by `scale` (default 1) and `reduction`. No side is over kMaxFboDimension.
inline constexpr uint32_t kMaxFboDimension = 8192; // what a render target may measure (the renderer's own limit)
FboSize fboSize(const EffectFbo &fbo, uint32_t layerWidth, uint32_t layerHeight, int reduction = 1);

struct EffectPass {
    std::string materialPath;
    MaterialPass material;   // loaded
    std::string target;      // empty: the layer's ping-pong buffer
    std::vector<EffectBind> bind;
    std::string command;     // "copy" or empty
    std::string source;      // copy source
    bool compose = false;
};

struct EffectDef {
    std::string file;        // "effects/blur/effect.json"
    std::vector<EffectPass> passes;
    std::vector<EffectFbo> fbos;
};

// What a scene changes in one pass of an effect.
struct PassOverride {
    std::map<std::string, Constant> constants;
    std::map<std::string, int> combos;
    std::vector<std::string> textures; // slot -> texture name; empty entries leave the material's
    std::vector<MediaBinding> media;   // slots the media cover fills (the instance's own `usertextures`)
};

struct EffectInstance {
    EffectDef def;
    std::string name;
    Bound<bool> visible{true, {}};
    std::vector<PassOverride> overrides; // one per pass of `def` (missing ones are empty)
    std::vector<std::string> userTextureProperties; // properties naming a texture slot of the effect (its materials and the instance)
    std::vector<std::string> degraded;   // what the effect uses and is drawn without ("fbo fit", "usertextures", ...), see Scene::degraded
};

std::optional<EffectDef> loadEffect(const Vfs &vfs, const std::string &path, std::string *error = nullptr);

// Reads an entry of an object's `effects` (file, passes, visible, name) and loads its definition.
// `reject` receives reasons for what cannot be honoured (scripts); the result is null when the effect cannot load.
std::optional<EffectInstance> loadEffectInstance(const Vfs &vfs, const Json &entry, std::string *error,
                                                 std::vector<std::string> *reject = nullptr);

// The pass as it is drawn: the material's textures, combos and constants with the instance's overrides applied.
MaterialPass effectivePass(const EffectPass &pass, const PassOverride *override_);

} // namespace murasaki::scene
