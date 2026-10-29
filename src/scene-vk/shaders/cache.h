#pragma once

// On-disk cache of translated shaders: $XDG_CACHE_HOME/murasaki/shader-cache/<key>.bin (the directory of
// glslcompiler.h's SPIR-V cache). One file holds both stages' SPIR-V and the reflection (uniform layouts, samplers,
// attributes, the combos that were defined), so a hit starts neither the preprocessor nor the compiler. A shader that
// failed to translate or compile is recorded with its message, so a broken shader costs one attempt, not one per
// start. The key hashes both sources (includes resolved), the combo overrides, the bound texture slots and
// kTranslatorVersion. MURASAKI_SHADER_CACHE=0 disables it.

#include "shaders/translate.h"

namespace murasaki::scene {

std::string shaderCacheKey(const ShaderSource &vertex, const ShaderSource &fragment, const std::map<std::string, int> &comboOverrides,
                           const std::vector<uint32_t> &boundTextures);

struct CachedShader {
    bool hit = false;                       // a file for the key exists and is readable
    std::optional<TranslatedShader> shader; // empty on a hit means a remembered failure
};

// On a remembered failure `*error` receives the stored message.
CachedShader loadCachedShader(const std::string &key, std::string *error);

// `shader` null records a failure with `failure` as its message. Best effort: errors writing are ignored.
void storeCachedShader(const std::string &key, const TranslatedShader *shader, const std::string &failure);

} // namespace murasaki::scene
