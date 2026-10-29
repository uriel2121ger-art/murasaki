#include "scene/effect.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

std::optional<EffectDef> loadEffect(const Vfs &vfs, const std::string &path, std::string *error)
{
    const auto fail = [&](const std::string &what) -> std::optional<EffectDef> {
        if (error) {
            *error = what;
        }
        return std::nullopt;
    };
    const auto text = vfs.readText(path);
    if (!text) {
        return fail("effect '" + path + "' not found");
    }
    std::string parseError;
    const auto json = parseJson(*text, &parseError);
    if (!json || !json->isObject()) {
        return fail("effect '" + path + "': " + (json ? "not a JSON object" : parseError));
    }
    EffectDef def;
    def.file = path;
    const Json *passes = json->find("passes");
    if (!passes || !passes->isArray() || passes->items().empty()) {
        return fail("effect '" + path + "' has no passes");
    }
    for (const Json &p : passes->items()) {
        EffectPass pass;
        pass.command = p.stringOr("command");
        pass.target = p.stringOr("target");
        pass.source = p.stringOr("source");
        pass.compose = p.boolOr("compose", false);
        pass.materialPath = p.stringOr("material");
        if (pass.command.empty()) {
            std::string materialError;
            const auto material = loadMaterial(vfs, pass.materialPath, &materialError);
            if (!material || material->passes.empty()) {
                return fail("effect '" + path + "': " + (material ? "material without passes" : materialError));
            }
            pass.material = material->passes.front();
            if (material->passes.size() > 1) {
                return fail("effect '" + path + "': a material with several passes");
            }
        }
        if (const Json *bind = p.find("bind"); bind && bind->isArray()) {
            for (const Json &b : bind->items()) {
                pass.bind.push_back({b.stringOr("name"), static_cast<int>(b.numberOr("index", 0))});
            }
        }
        def.passes.push_back(std::move(pass));
    }
    if (const Json *fbos = json->find("fbos"); fbos && fbos->isArray()) {
        for (const Json &f : fbos->items()) {
            EffectFbo fbo;
            fbo.name = f.stringOr("name");
            fbo.scale = static_cast<int>(f.numberOr("scale", 1));
            fbo.format = f.stringOr("format", "rgba8888");
            fbo.unique = f.boolOr("unique", false);
            fbo.width = static_cast<int>(f.numberOr("width", 0));
            fbo.height = static_cast<int>(f.numberOr("height", 0));
            if (const Json *fit = f.find("fit")) {
                if (fit->isNumber() && fit->asNumber() >= 1.0) {
                    fbo.fit = static_cast<int>(std::min(fit->asNumber(), static_cast<double>(kMaxFboDimension)));
                } else {
                    fbo.fitBare = (fit->isNumber() && fit->asNumber() != 0.0) || (fit->isBool() && fit->asBool());
                }
            }
            const std::string uvs = f.stringOr("uvs");
            fbo.repeat = uvs == "repeat";
            fbo.uvsOther = !uvs.empty() && uvs != "clamp" && uvs != "repeat";
            def.fbos.push_back(std::move(fbo));
        }
    }
    // The buffers an effect declares are named by the effect ("blur_start_2" in a Workshop bloom); the engine knows its buffers
    // by the `_rt_` prefix, so the effect's own names get it, in the declarations, the targets and the binds alike.
    {
        const auto canonical = [&](std::string &name) {
            if (!name.empty() && name.rfind("_rt_", 0) != 0
                && std::any_of(def.fbos.begin(), def.fbos.end(), [&](const EffectFbo &f) { return f.name == name; })) {
                name = "_rt_" + name;
            }
        };
        for (EffectPass &pass : def.passes) {
            canonical(pass.target);
            canonical(pass.source);
            for (EffectBind &b : pass.bind) {
                canonical(b.name);
            }
        }
        for (EffectFbo &fbo : def.fbos) {
            canonical(fbo.name);
        }
    }
    return def;
}

std::optional<EffectInstance> loadEffectInstance(const Vfs &vfs, const Json &entry, std::string *error, std::vector<std::string> *reject)
{
    EffectInstance instance;
    const auto def = loadEffect(vfs, entry.stringOr("file"), error);
    if (!def) {
        return std::nullopt;
    }
    instance.def = *def;
    instance.name = entry.stringOr("name");
    const auto degrade = [&](const std::string &reason) {
        if (std::find(instance.degraded.begin(), instance.degraded.end(), reason) == instance.degraded.end()) {
            instance.degraded.push_back(reason);
        }
    };
    for (const EffectFbo &fbo : instance.def.fbos) {
        if (fbo.fitBare) {
            degrade("fbo fit");
        }
        if (fbo.uvsOther) {
            degrade("fbo uvs");
        }
    }
    const auto property = [&](const std::string &name) {
        if (std::find(instance.userTextureProperties.begin(), instance.userTextureProperties.end(), name) == instance.userTextureProperties.end()) {
            instance.userTextureProperties.push_back(name);
        }
    };
    for (const EffectPass &pass : instance.def.passes) {
        for (const std::string &reason : pass.material.degraded) {
            degrade(reason);
        }
        for (const std::string &name : pass.material.userTextureProperties) {
            property(name);
        }
    }
    if (const Json *visible = entry.find("visible")) {
        if (visible->isBool()) {
            instance.visible.value = visible->asBool();
        } else if (visible->isObject()) {
            // a script on the visibility is bound by the scene loader ("fxv:<effect>"): nothing to refuse
            if (const Json *user = visible->find("user"); user && user->isString()) {
                instance.visible.user = user->asString();
            } else if (user && user->isObject()) {
                const Json *name = user->find("name");
                const Json *condition = user->find("condition");
                if (name && name->isString() && condition) {
                    instance.visible.user = name->asString();
                    instance.visible.condition = condition->isString() ? condition->asString() : std::to_string(static_cast<long long>(condition->asNumber()));
                } else if (reject) {
                    reject->push_back("property condition (effect visibility): M4");
                }
            }
            if (const Json *value = visible->find("value"); value && value->isBool()) {
                instance.visible.value = value->asBool();
            }
        }
    }
    instance.overrides.resize(instance.def.passes.size());
    if (const Json *passes = entry.find("passes"); passes && passes->isArray()) {
        for (size_t i = 0; i < passes->items().size() && i < instance.overrides.size(); ++i) {
            const Json &p = passes->items()[i];
            PassOverride &o = instance.overrides[i];
            parseConstants(p.find("constantshadervalues"), o.constants);
            parseUserShaderValues(p.find("usershadervalues"), o.constants);
            parseCombos(p.find("combos"), o.combos);
            o.textures = parseTextureList(p.find("textures"));
            o.media = mediaBindings(p);
            for (const std::string &reason : userBindings(p)) { // the instance's own user bindings
                degrade(reason);
            }
            for (const std::string &name : userTextureProperties(p)) {
                property(name);
            }
            // a script on a constant is the script's (the scene loader binds it, see Loader::imageLayer): nothing to refuse
        }
    }
    return instance;
}

FboSize fboSize(const EffectFbo &fbo, uint32_t layerWidth, uint32_t layerHeight, int reduction)
{
    const auto capped = [](uint32_t v) { return std::clamp(v, 1u, kMaxFboDimension); };
    if (fbo.width > 0 && fbo.height > 0) {
        return {capped(static_cast<uint32_t>(fbo.width)), capped(static_cast<uint32_t>(fbo.height))};
    }
    const uint32_t w = std::max(1u, layerWidth), h = std::max(1u, layerHeight);
    if (fbo.fit > 0) {
        const double longSide = std::max(w, h), shortSide = std::min(w, h);
        const uint32_t fitted = capped(static_cast<uint32_t>(fbo.fit));
        const uint32_t other = capped(static_cast<uint32_t>(std::lround(fitted * shortSide / longSide)));
        return w >= h ? FboSize{fitted, other} : FboSize{other, fitted};
    }
    const auto divisor = static_cast<uint32_t>(std::max(1, (fbo.scale > 0 ? fbo.scale : 1) * reduction));
    return {capped(w / divisor), capped(h / divisor)};
}

MaterialPass effectivePass(const EffectPass &pass, const PassOverride *override_)
{
    MaterialPass out = pass.material;
    if (!override_) {
        return out;
    }
    for (const auto &[key, value] : override_->constants) {
        if (value.count == 0 && !value.user.empty()) {
            out.constants[key].user = value.user; // a bare `usershadervalues` binding: the pass keeps its literal for the unset property
        } else {
            out.constants[key] = value;
        }
    }
    for (const auto &[key, value] : override_->combos) {
        out.combos[key] = value;
    }
    for (size_t i = 0; i < override_->textures.size(); ++i) {
        if (override_->textures[i].empty()) {
            continue;
        }
        if (out.textures.size() <= i) {
            out.textures.resize(i + 1);
        }
        out.textures[i] = override_->textures[i];
    }
    return out;
}

} // namespace murasaki::scene
