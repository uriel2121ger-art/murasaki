#include "scene/material.h"

#include "util/math.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace murasaki::scene {

namespace {

std::string lower(std::string s)
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

} // namespace

Constant parseConstant(const Json &json)
{
    Constant c;
    const Json *value = &json;
    if (json.isObject()) {
        if (json.find("script")) {
            c.scripted = true;
        }
        if (const Json *user = json.find("user"); user && user->isString()) {
            c.user = user->asString();
        }
        value = json.find("value");
    }
    if (!value) {
        return c;
    }
    if (value->isNumber()) {
        c.value[0] = static_cast<float>(value->asNumber());
        c.count = 1;
    } else if (value->isBool()) {
        c.value[0] = value->asBool() ? 1.0f : 0.0f;
        c.count = 1;
    } else if (value->isString()) {
        float f[4] = {0, 0, 0, 0};
        c.count = parseFloats(value->asString(), f, 4);
        for (int i = 0; i < 4; ++i) {
            // "-nan(ind)" and friends are Windows spellings of NaN: the component stays at its default of 0 here,
            // consumers fall back to the uniform's default when a constant has no components.
            c.value[static_cast<size_t>(i)] = std::isfinite(f[i]) ? f[i] : 0.0f;
        }
    }
    return c;
}

void parseConstants(const Json *object, std::map<std::string, Constant> &out)
{
    if (!object || !object->isObject()) {
        return;
    }
    for (const auto &[key, value] : object->members()) {
        out[lower(key)] = parseConstant(value); // looked up lower-cased (annotations and files differ in case)
    }
}

void parseUserShaderValues(const Json *object, std::map<std::string, Constant> &out)
{
    if (!object || !object->isObject()) {
        return;
    }
    for (const auto &[key, value] : object->members()) {
        if (value.isString() && !value.asString().empty()) {
            out[lower(key)].user = value.asString(); // the literal already there (if any) stays: what shows while the property is unset
        }
    }
}

bool parseCombo(const std::string &name, const Json &value, std::map<std::string, int> &out)
{
    if (value.isNumber()) {
        out[upper(name)] = static_cast<int>(value.asNumber());
    } else if (value.isBool()) {
        out[upper(name)] = value.asBool() ? 1 : 0;
    } else {
        return false;
    }
    return true;
}

void parseCombos(const Json *object, std::map<std::string, int> &out)
{
    if (!object || !object->isObject()) {
        return;
    }
    for (const auto &[key, value] : object->members()) {
        parseCombo(key, value, out);
    }
}

std::vector<std::string> parseTextureList(const Json *array)
{
    std::vector<std::string> out;
    if (array && array->isArray()) {
        for (const Json &item : array->items()) {
            out.push_back(item.isString() ? item.asString() : std::string());
        }
    }
    return out;
}

namespace {

// The name a user texture binding is degraded under, or empty when nothing is lost. No binding (null) and a user property
// (a bare property name: its default picture is what WE shows until the user sets it) are not named. The media thumbnails and
// the shortcuts of the user's desktop are filled by WE while a player plays / once the user picks an application, and nothing
// here fills them: the slot shows what the file lists there (docs/scene-vk/formats.md "User textures and instances").
std::string unfollowedUserTexture(const Json &binding)
{
    if (binding.isNull() || binding.isString()) {
        return {};
    }
    if (binding.isObject()) {
        const std::string type = binding.stringOr("type");
        if (type == "usershortcut") {
            return {}; // the host fills it with the application's icon (mediaBindings)
        }
        if (type == "system") {
            const std::string name = binding.stringOr("name");
            return name == "$mediaThumbnail" || name == "$mediaPreviousThumbnail" ? std::string() : "usertextures system " + name; // (the covers: mediaBindings)
        }
        return type.empty() ? "usertextures" : "usertextures " + type;
    }
    return "usertextures value";
}

} // namespace

std::vector<std::string> userBindings(const Json &pass)
{
    std::vector<std::string> out;
    const auto add = [&out](const std::string &reason) {
        if (std::find(out.begin(), out.end(), reason) == out.end()) {
            out.push_back(reason);
        }
    };
    // (a value that is not a property name binds nothing)
    if (const Json *values = pass.find("usershadervalues"); values && values->isObject()) {
        for (const auto &[key, value] : values->members()) {
            if (!value.isString() || value.asString().empty()) {
                add("usershadervalues");
            }
        }
    }
    if (const Json *textures = pass.find("usertextures"); textures && textures->isArray()) {
        for (const Json &t : textures->items()) {
            if (const std::string reason = unfollowedUserTexture(t); !reason.empty()) {
                add(reason);
            }
        }
    }
    return out;
}

std::vector<MediaBinding> mediaBindings(const Json &pass)
{
    std::vector<MediaBinding> out;
    const Json *textures = pass.find("usertextures");
    if (!textures || !textures->isArray()) {
        return out;
    }
    uint32_t slot = 0;
    for (const Json &t : textures->items()) {
        if (t.isObject() && t.stringOr("type") == "usershortcut" && slot < 32) {
            const std::string key = t.stringOr("name");
            if (!key.empty() && key.size() < 64) {
                MediaBinding b;
                b.slot = slot;
                b.shortcut = key;
                out.push_back(b);
            }
        } else if (t.isObject() && t.stringOr("type") == "system" && slot < 32) {
            const std::string name = t.stringOr("name");
            if (name == "$mediaThumbnail" || name == "$mediaPreviousThumbnail") {
                MediaBinding b;
                b.slot = slot;
                b.previous = name == "$mediaPreviousThumbnail";
                out.push_back(b);
            }
        }
        ++slot;
    }
    return out;
}

std::vector<std::string> userTextureProperties(const Json &pass)
{
    std::vector<std::string> out;
    if (const Json *textures = pass.find("usertextures"); textures && textures->isArray()) {
        for (const Json &t : textures->items()) {
            if (t.isString() && !t.asString().empty() && std::find(out.begin(), out.end(), t.asString()) == out.end()) {
                out.push_back(t.asString());
            }
        }
    }
    return out;
}

MaterialPass parseMaterialPass(const Json &pass)
{
    MaterialPass p;
    p.shader = pass.stringOr("shader");
    p.blendingName = pass.stringOr("blending", "translucent");
    if (p.blendingName == "translucent") {
        p.blending = Blending::Translucent;
    } else if (p.blendingName == "additive") {
        p.blending = Blending::Additive;
    } else if (p.blendingName == "normal" || p.blendingName == "disabled" || p.blendingName == "alphatocoverage") {
        p.blending = Blending::Normal;
    } else {
        p.blendingKnown = false;
    }
    p.textures = parseTextureList(pass.find("textures"));
    parseCombos(pass.find("combos"), p.combos);
    if (p.blendingName == "alphatocoverage") {
        // WE's shipped shaders do alpha to coverage themselves under ALPHATOCOVERAGE on GLSL: the alpha sharpened to an edge and what
        // is under one half discarded; one sample a pixel, the rest is drawn opaque
        p.blending = Blending::Normal;
        p.combos.emplace("ALPHATOCOVERAGE", 1);
    }
    parseConstants(pass.find("constantshadervalues"), p.constants);
    parseUserShaderValues(pass.find("usershadervalues"), p.constants);
    p.depthTest = pass.stringOr("depthtest", "enabled") != "disabled";
    p.depthWrite = pass.stringOr("depthwrite", "enabled") != "disabled";
    p.cull = pass.stringOr("cullmode", "normal") == "nocull" ? CullMode::None : CullMode::Back;
    p.degraded = userBindings(pass);
    p.userTextureProperties = userTextureProperties(pass);
    p.media = mediaBindings(pass);
    return p;
}

std::optional<Material> loadMaterial(const Vfs &vfs, const std::string &path, std::string *error)
{
    const auto fail = [&](const std::string &what) -> std::optional<Material> {
        if (error) {
            *error = what;
        }
        return std::nullopt;
    };
    const auto text = vfs.readText(path);
    if (!text) {
        return fail("material '" + path + "' not found");
    }
    std::string parseError;
    const auto json = parseJson(*text, &parseError);
    if (!json || !json->isObject()) {
        return fail("material '" + path + "': " + (json ? "not a JSON object" : parseError));
    }
    const Json *passes = json->find("passes");
    if (!passes || !passes->isArray() || passes->items().empty()) {
        return fail("material '" + path + "' has no passes");
    }
    Material material;
    for (const Json &pass : passes->items()) {
        if (!pass.isObject()) {
            return fail("material '" + path + "' has a pass that is not an object");
        }
        material.passes.push_back(parseMaterialPass(pass));
    }
    return material;
}

} // namespace murasaki::scene
