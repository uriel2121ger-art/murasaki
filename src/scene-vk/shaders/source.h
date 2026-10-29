#pragma once

// Wallpaper Engine shader sources: loading with `#include` resolved, and the annotations that ride in comments.
//
// A WE shader is GLSL-flavoured text (`shaders/<name>.vert` and `.frag`) with three habits of its own:
//   * `#include "common.h"` pulls headers from `shaders/` (the item first, then the WE assets);
//   * `// [COMBO] {"combo":"NAME","default":N,...}` declares a compile-time option, which the material or the scene
//     sets and the source tests with `#if NAME`;
//   * a trailing `// {...}` on a `uniform` line is JSON metadata: `material` is the key a material or an effect
//     instance uses to set it, `default` its default, and `combo` (on samplers) the option that turns on when a
//     texture is bound to it.
// docs/scene-vk/shaders.md describes every annotation found in the shipped corpus.

#include "assets/vfs.h"
#include "util/json.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace murasaki::scene {

struct ComboDecl {
    std::string name;     // as written in the source (upper case)
    int defaultValue = 0;
    std::string material; // the editor label key; only informational
    bool exposed = true;  // false for [COMBO_OFF], [COMBO_DISABLED], [OFF_COMBO]: declared but hidden in the editor
};

struct ShaderSource {
    std::string path;                         // "shaders/effects/tint.frag"
    std::string text;                         // includes resolved
    std::vector<ComboDecl> combos;            // in order of appearance, no duplicates
    // Uniform and sampler annotations by uniform name (the JSON object after the `//`).
    std::map<std::string, Json> notes;
    std::vector<std::string> extraPasses;     // `// [PASS] name shader` lines: other passes of the shader (shadows)
    std::vector<std::string> requires_;      // `#require Feature` lines ("LightingV1"), removed from the text
    std::vector<std::string> includes;        // every file pulled in, for cache keys and diagnostics
};

// Reads `path` from the Vfs (effects ship their shaders under their own folder, which the Vfs searches), resolves
// includes (depth <= 16, cycles and files over 1 MiB refused, total over 4 MiB refused) and parses the annotations.
std::optional<ShaderSource> loadShaderSource(const Vfs &vfs, const std::string &path, std::string *error = nullptr);

// The same from text already in memory (tests). `include` resolves an include name to its text.
using IncludeResolver = std::function<std::optional<std::string>(const std::string &from, const std::string &name)>;
std::optional<ShaderSource> parseShaderSource(const std::string &path, const std::string &text, const IncludeResolver &include,
                                              std::string *error = nullptr);

} // namespace murasaki::scene
