#include "shaders/source.h"

#include "assets/pkg.h"

#include <algorithm>
#include <functional>
#include <set>

namespace murasaki::scene {

namespace {

constexpr int kMaxIncludeDepth = 16;
constexpr size_t kMaxFileBytes = 1u << 20;
constexpr size_t kMaxTotalBytes = 4u << 20;

struct Context {
    const IncludeResolver &resolve;
    std::vector<std::string> stack; // files being expanded, for cycle detection
    size_t total = 0;
    ShaderSource &out;
    std::string error;
};

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) {
        s.remove_suffix(1);
    }
    return s;
}

// "// [COMBO] {...}" and friends. Returns the tag and the text after it.
bool commentTag(std::string_view line, std::string &tag, std::string_view &rest)
{
    line = trim(line);
    if (line.substr(0, 2) != "//") {
        return false;
    }
    line = trim(line.substr(2));
    if (line.empty() || line.front() != '[') {
        return false;
    }
    const size_t close = line.find(']');
    if (close == std::string_view::npos) {
        return false;
    }
    tag = std::string(line.substr(1, close - 1));
    rest = trim(line.substr(close + 1));
    return true;
}

void addCombo(ShaderSource &out, const Json &json, bool exposed)
{
    ComboDecl combo;
    combo.name = json.stringOr("combo");
    if (combo.name.empty()) {
        return;
    }
    combo.defaultValue = static_cast<int>(json.numberOr("default", 0));
    combo.material = json.stringOr("material");
    combo.exposed = exposed;
    const auto same = std::find_if(out.combos.begin(), out.combos.end(), [&](const ComboDecl &c) { return c.name == combo.name; });
    if (same == out.combos.end()) {
        out.combos.push_back(std::move(combo));
    }
}

// The name a `uniform ...;` line declares, or empty when the line is something else.
std::string declaredUniform(std::string_view line)
{
    line = trim(line);
    if (line.substr(0, 8) != "uniform " && line.substr(0, 8) != "uniform\t") {
        return {};
    }
    const size_t semicolon = line.find(';');
    if (semicolon == std::string_view::npos) {
        return {};
    }
    std::string_view decl = trim(line.substr(8, semicolon - 8));
    // drop an array suffix and take the last identifier
    if (const size_t bracket = decl.find('['); bracket != std::string_view::npos) {
        decl = trim(decl.substr(0, bracket));
    }
    const size_t space = decl.find_last_of(" \t");
    return std::string(space == std::string_view::npos ? decl : decl.substr(space + 1));
}

void annotate(Context &ctx, std::string_view line)
{
    std::string tag;
    std::string_view rest;
    if (commentTag(line, tag, rest)) {
        if (tag == "COMBO" || tag == "COMBO_OFF" || tag == "COMBO_DISABLED" || tag == "OFF_COMBO") {
            if (const auto json = parseJson(rest); json && json->isObject()) {
                addCombo(ctx.out, *json, tag == "COMBO");
            }
        } else if (tag == "PASS") {
            ctx.out.extraPasses.emplace_back(rest);
        }
        return;
    }
    const std::string name = declaredUniform(line);
    if (name.empty()) {
        return;
    }
    const size_t comment = line.find("//", line.find(';'));
    if (comment == std::string_view::npos) {
        return;
    }
    if (const auto json = parseJson(trim(line.substr(comment + 2))); json && json->isObject()) {
        ctx.out.notes[name] = *json; // the first annotation of a name wins, like the first declaration
        if (const Json *combo = json->find("combo"); combo && combo->isString()) {
            // a sampler's `combo` is a combo too (turned on by binding a texture): declare it with default 0
            ComboDecl decl;
            decl.name = combo->asString();
            decl.defaultValue = 0;
            decl.material = json->stringOr("label");
            if (std::none_of(ctx.out.combos.begin(), ctx.out.combos.end(), [&](const ComboDecl &c) { return c.name == decl.name; })) {
                ctx.out.combos.push_back(std::move(decl));
            }
        }
    }
}

bool expand(Context &ctx, const std::string &file, const std::string &text, int depth)
{
    if (depth > kMaxIncludeDepth) {
        ctx.error = "includes nested deeper than " + std::to_string(kMaxIncludeDepth) + " in " + file;
        return false;
    }
    if (text.size() > kMaxFileBytes) {
        ctx.error = file + " is larger than 1 MiB";
        return false;
    }
    ctx.stack.push_back(file);
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) {
            end = text.size();
        }
        const std::string_view line(text.data() + pos, end - pos);
        const std::string_view t = trim(line);
        if (t.substr(0, 8) == "#include") {
            const size_t open = t.find_first_of("\"<");
            const size_t close = open == std::string_view::npos ? open : t.find_first_of("\">", open + 1);
            if (open == std::string_view::npos || close == std::string_view::npos) {
                ctx.error = "malformed #include in " + file;
                return false;
            }
            const std::string name(t.substr(open + 1, close - open - 1));
            // WE headers have no include guards and several headers include the same ones: each file is
            // expanded once, at its first #include, which also makes a cycle harmless.
            if (std::find(ctx.out.includes.begin(), ctx.out.includes.end(), name) != ctx.out.includes.end()) {
                ctx.out.text += "// already included " + name + "\n";
            } else {
                const auto included = ctx.resolve(file, name);
                if (!included) {
                    ctx.error = "cannot find #include \"" + name + "\" (from " + file + ")";
                    return false;
                }
                ctx.out.includes.push_back(name);
                ctx.out.text += "// begin include " + name + "\n";
                if (!expand(ctx, name, *included, depth + 1)) {
                    return false;
                }
                ctx.out.text += "// end include " + name + "\n";
            }
            if (end == text.size()) {
                break;
            }
            pos = end + 1;
            continue;
        } else if (t.substr(0, 8) == "#require") {
            // `#require LightingV1` asks the engine for a built-in feature set; it is not GLSL. Recorded, not emitted.
            ctx.out.requires_.emplace_back(trim(t.substr(8)));
            ctx.out.text += "// " + std::string(t) + "\n";
            ctx.total += line.size() + 1;
        } else {
            annotate(ctx, line);
            ctx.out.text.append(line);
            ctx.out.text += '\n';
            ctx.total += line.size() + 1;
            if (ctx.total > kMaxTotalBytes) {
                ctx.error = "shader source over 4 MiB after includes";
                return false;
            }
        }
        if (end == text.size()) {
            break;
        }
        pos = end + 1;
    }
    ctx.stack.pop_back();
    return true;
}

} // namespace

std::optional<ShaderSource> parseShaderSource(const std::string &path, const std::string &text, const IncludeResolver &include,
                                              std::string *error)
{
    ShaderSource out;
    out.path = path;
    Context ctx{include, {}, 0, out, {}};
    if (!expand(ctx, path, text, 0)) {
        if (error) {
            *error = ctx.error;
        }
        return std::nullopt;
    }
    return out;
}

std::optional<ShaderSource> loadShaderSource(const Vfs &vfs, const std::string &path, std::string *error)
{
    const auto text = vfs.readText(path);
    if (!text) {
        if (error) {
            *error = "shader '" + path + "' not found";
        }
        return std::nullopt;
    }
    const IncludeResolver resolver = [&vfs](const std::string &from, const std::string &name) -> std::optional<std::string> {
        // Includes name files relative to `shaders/`; a header may also include a sibling of its own folder.
        for (const std::string &candidate : {"shaders/" + name, name}) {
            if (const auto t = vfs.readText(candidate)) {
                return t;
            }
        }
        const size_t slash = from.find_last_of('/');
        if (slash != std::string::npos) {
            return vfs.readText(from.substr(0, slash + 1) + name);
        }
        return std::nullopt;
    };
    return parseShaderSource(path, *text, resolver, error);
}

} // namespace murasaki::scene
