#include "render/analysis.h"
#include "render/lights.h"

#include "assets/texfile.h"

#include "audio/soundstream.h"
#include "render/puppetdraw.h"
#include "render/scenescripts.h"
#include "util/log.h"

#include "assets/tex.h"
#include "assets/videodecoder.h"
#include "text/textraster.h"
#include "render/particlerenderer.h"
#include "render/passes.h"
#include "render/shaderlib.h"
#include "render/uniforms.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace murasaki::scene {

namespace {

// What a scene's scripts may cost, measured by --check on a copy of the scene after the inits and a few frames: the runtime's memory,
// and a frame's updates (the cheapest of those frames: a busy machine makes one slow, not all). The scripts of a scene share a realm
// (docs/scene-vk/scripts.md), so a script costs about 7 KiB besides what it allocates: the library's biggest scenes, 942 and 1382
// scripts, take 15 and 13 MiB. Time is what limits: 20 fps is the lowest rate of the engine's ladder (engine/fpscontrol.h), and the
// solar system of 3662790108 spends about 28 ms a frame in its updates on a Ryzen 7 9800X3D (one script of 237 KB, an ephemeris, half
// of it).
// A scene whose scripts need more than a frame at that rate is drawn, slowly: named; one whose scripts need more than 200 ms a frame
// (5 fps) is refused.
constexpr size_t kMaxScriptMemory = 128u << 20;
constexpr double kSlowScriptFrameSeconds = 1.0 / 20.0;
constexpr double kSlowScriptStartSeconds = 0.25;    // the scripts' start (modules and inits) over this delays the first picture: named
constexpr double kMaxScriptFrameSeconds = 0.2;
constexpr int kCheckedScriptFrames = 3;


constexpr uint32_t kBlockWindow = kPassBlockWindow;

bool isTarget(const std::string &name)
{
    return name.rfind("_rt_", 0) == 0;
}

std::string firstLine(const std::string &text)
{
    const size_t n = text.find('\n');
    std::string line = n == std::string::npos ? text : text.substr(0, n);
    if (line.size() > 200) {
        line.resize(200);
    }
    return line;
}

class Analysis
{
public:
    Analysis(const Scene &scene, const Vfs &vfs) : m_scene(scene), m_vfs(vfs), m_shaders(vfs)
    {
        m_shaders.setLightCombos(sceneLightCombos(scene)); // the lit shaders are checked with the counts they will be drawn with (constant for a scene)
        // the variants the renderer can ask for (SceneRenderer::updateFog): HDR, and each fog band that is or may be on (a property may
        // switch it), alone or together, or none; every pass is compiled in each (a shader the fog cannot change compiles once)
        const auto mayBeOn = [](const FogBand &band) { return band.enabled.value || !band.enabled.user.empty(); };
        for (int distance = 0; distance <= (mayBeOn(scene.fog.distance) ? 1 : 0); ++distance) {
            for (int height = 0; height <= (mayBeOn(scene.fog.height) ? 1 : 0); ++height) {
                std::map<std::string, int> combos;
                if (scene.hdr) {
                    combos["HDR"] = 1;
                }
                const bool anyFog = distance || height;
                if (distance) {
                    combos["FOG_DIST"] = 1;
                }
                if (height) {
                    combos["FOG_HEIGHT"] = 1;
                }
                if (anyFog) {
                    combos["FOG_COMPUTED"] = 1;
                }
                m_variants.push_back(std::move(combos));
            }
        }
        m_shaders.setTextureFormats([this](const std::string &name) -> std::optional<int> {
            if (const auto it = m_formats.find(name); it != m_formats.end()) {
                return it->second;
            }
            std::optional<int> format;
            if (const auto tex = readTexFile(m_vfs, "materials/" + name + ".tex", TexReadOptions{false})) {
                format = tex->rawFormat;
            }
            m_formats[name] = format;
            return format;
        });
    }

    std::vector<std::string> run()
    {
        for (const ImageLayer &layer : m_scene.layers) {
            const MaterialPass base = layer.puppet ? puppetPass(layer) : layer.base;
            if (layer.text && !textSupported()) {
                reject("text: this build has no FreeType");
            }
            checkTexture(base);
            checkBlending(base);
            if (const TranslatedShader *shader = shaderFor(base, nullptr, layer.puppet != nullptr)) {
                checkAnimation(layer, *shader);
                checkOwnOptions(base, *shader);
                budget(*shader);
            }
            budget(m_compositeBytes); // the composite through the layer's buffers
            if (layer.colorBlendMode > 0) {
                // the blend is WE's own genericimage3 reading the scene underneath: it must be there and compile
                MaterialPass blend;
                blend.shader = "genericimage3";
                blend.combos["BLENDMODE"] = layer.colorBlendMode;
                std::string error;
                if (const TranslatedShader *shader = shaderVariants(blend, {0, 4}, &error)) {
                    budget(*shader);
                } else {
                    reject("colour blend mode: " + firstLine(error));
                }
            }
            std::string current = "a";
            const auto canon = [&](const std::string &name, const EffectInstance &fx) -> std::string {
                const std::string own = "_rt_imageLayerComposite_" + std::to_string(m_scene.nodes[layer.node].id);
                if (name == "previous") {
                    return current;
                }
                if (name == own + "_a") {
                    return "a";
                }
                if (name == own + "_b") {
                    return "b";
                }
                if (name == "_rt_FullFrameBuffer" || name == "_rt_MipMappedFrameBuffer") {
                    return "full";
                }
                for (const EffectFbo &fbo : fx.def.fbos) {
                    if (fbo.name == name) {
                        return "fbo:" + name;
                    }
                }
                return isTarget(name) ? "fbo:" + name : std::string();
            };
            for (const EffectInstance &fx : layer.effects) {
                for (size_t p = 0; p < fx.def.passes.size(); ++p) {
                    const EffectPass &pass = fx.def.passes[p];
                    if (!pass.command.empty()) {
                        if (pass.command != "copy" && pass.command != "swap") {
                            reject("effect command '" + pass.command + "': M2");
                        }
                        continue;
                    }
                    const MaterialPass eff = effectivePass(pass, p < fx.overrides.size() ? &fx.overrides[p] : nullptr);
                    checkTexture(eff);
                    checkBlending(eff);
                    const TranslatedShader *shader = shaderFor(eff, &pass.bind);
                    for (const EffectBind &b : pass.bind) {
                        checkBindName(b.name, layer);
                    }
                    if (shader) {
                        budget(*shader);
                        // a pass may not read the target it writes: work out the same inputs the renderer binds
                        const std::string output = pass.target.empty() ? (current == "a" ? "b" : "a") : canon(pass.target, fx);
                        for (uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
                            const std::string file = slot < eff.textures.size() ? eff.textures[slot] : std::string();
                            std::string input;
                            bool bound = false;
                            for (const EffectBind &b : pass.bind) {
                                if (static_cast<uint32_t>(b.index) == slot) {
                                    input = canon(b.name, fx);
                                    bound = true;
                                }
                            }
                            if (!bound && file.empty() && slot == 0) {
                                input = current;
                            } else if (!bound && !file.empty() && isTarget(file)) {
                                input = canon(file, fx);
                            } else if (!bound && file.empty()) {
                                for (const SamplerInfo &s : shader->samplers) {
                                    if (s.binding == slot && isTarget(s.defaultText)) {
                                        input = canon(s.defaultText, fx);
                                    }
                                }
                            }
                            if (!input.empty() && input == output && pass.target.empty()) {
                                input = current; // the layer's own buffer named for the one being written means the picture so far
                            }
                            if (!input.empty() && input == output) {
                                reject("effect " + fx.def.file + " pass " + std::to_string(p) + " reads the target it writes");
                            }
                        }
                        if (pass.target.empty()) {
                            current = output;
                        }
                    }
                }
            }
        }
        for (const ParticleLayer &p : m_scene.particles) {
            if (p.definition) {
                checkParticles(*p.definition, 0);
            }
        }
        // 3D meshes: every part's material with the mesh vertex (a skinned mesh has the puppet's); a part that samples another
        // layer's composite needs that object to be an image layer of the scene
        for (const MeshLayer &mesh : m_scene.meshes) {
            const bool skinned = mesh.model && mesh.model->skinned;
            for (const MaterialPass &pass : mesh.materials) {
                checkTexture(pass);
                checkBlending(pass);
                for (const std::string &name : pass.textures) {
                    if (const auto id = layerCompositeId(name); id && !imageLayerOfObject(m_scene, *id)) {
                        reject("a 3D mesh samples the composite of object " + std::to_string(*id) + ", which is not an image layer of the scene");
                    }
                }
                if (const TranslatedShader *shader = shaderFor(pass, nullptr, skinned, !skinned)) {
                    budget(*shader);
                }
            }
        }
        // shadows: every casting part is drawn once per shadowed light (its pictures share the draw) with its material's caster;
        // a caster that does not compile leaves the part without a shadow
        if (sceneHasShadows(m_scene)) {
            const LightCounts counts = lightCounts(m_scene);
            const int shadowed = counts.pointShadow + counts.spotShadow + counts.directionalShadow;
            for (const MeshLayer &mesh : m_scene.meshes) {
                if (!mesh.castShadow) {
                    continue;
                }
                for (const MaterialPass &pass : mesh.materials) {
                    if (pass.blending == Blending::Additive) {
                        continue; // (additive parts cast nothing)
                    }
                    const MaterialPass caster = m_shaders.shadowCasterPass(pass);
                    std::string error;
                    const TranslatedShader *shader = shaderVariants(caster, boundSlotsOf(caster, {}), &error);
                    if (!shader) {
                        degrade("shadow caster " + caster.shader);
                        continue;
                    }
                    for (int i = 0; i < shadowed; ++i) {
                        budget(*shader);
                    }
                }
            }
            // below a render scale of 1 the lit shaders are built with LIGHTS_SHADOW_MAPPING_QUALITY 1 (the quality ladder): they
            // must compile that way too
            m_shaders.setLightCombos(sceneLightCombos(m_scene, 1));
            const auto lowQuality = [&](const MaterialPass &pass, const std::vector<uint32_t> &slots) {
                std::string error;
                if (!shaderVariants(pass, slots, &error)) {
                    reject("shader compile (shadow quality 1): " + pass.shader + ": " + firstLine(error));
                }
            };
            for (const MeshLayer &mesh : m_scene.meshes) {
                for (const MaterialPass &pass : mesh.materials) {
                    lowQuality(pass, boundSlotsOf(pass, {}));
                }
            }
            for (const ImageLayer &layer : m_scene.layers) {
                const MaterialPass base = layer.puppet ? puppetPass(layer) : layer.base;
                lowQuality(base, boundSlotsOf(base, {}));
            }
            m_shaders.setLightCombos(sceneLightCombos(m_scene));
        }
        if (m_scene.bloom.enabled.value || !m_scene.bloom.enabled.user.empty()) {
            // the bloom chain is WE's own shaders: they must be there (the Wallpaper Engine assets) and compile
            for (const char *name : {"downsample_quarter_bloom", "downsample_eighth_blur_v", "blur_h_bloom", "combine"}) {
                MaterialPass pass;
                pass.shader = name;
                std::string error;
                const bool two = std::string(name) == "combine";
                if (const TranslatedShader *shader = shaderVariants(pass, two ? std::vector<uint32_t>{0, 1} : std::vector<uint32_t>{0}, &error)) {
                    budget(*shader);
                } else {
                    reject("bloom: " + firstLine(error));
                }
            }
        }
        checkVolumetrics();
        checkSounds();
        checkScripts();
        // (the ring check below, at 768 bytes budgeted a draw, usually refuses a scene of thousands of draws first)
        if (m_draws > kPassMaxDraws) {
            reject("the scene needs " + std::to_string(m_draws) + " draws per frame, over the limit of " + std::to_string(kPassMaxDraws));
        }
        if (m_ringBytes + kPassBlockWindow > kPassRingBytes) {
            reject("the scene's uniform data (" + std::to_string(m_ringBytes >> 10) + " KiB per frame) does not fit the " + std::to_string(kPassRingBytes >> 20)
                   + " MiB ring");
        }
        return m_reasons;
    }

    const std::vector<std::string> &degraded() const { return m_degraded; }

private:
    static uint64_t alignedUp(uint64_t v) { return (v + kPassMaxAlignment - 1) / kPassMaxAlignment * kPassMaxAlignment; }

    // One draw: its quad, its two uniform blocks, each at the worst alignment.
    void budget(const TranslatedShader &shader) { budget(alignedUp(shader.vertex.blockSize) + alignedUp(shader.fragment.blockSize)); }
    void budget(uint64_t blockBytes)
    {
        ++m_draws;
        m_ringBytes += kPassMaxAlignment + blockBytes + 2 * kPassMaxAlignment;
    }

    // The beams of the lights that cast volumetrics (point and spot): WE's volumetricsback / volumetricsfront with the combos the
    // renderer sets, the blur and the combine. Shaders that do not build leave the beams undrawn, named. Each light is two draws, and
    // the blur and combine passes three more.
    void checkVolumetrics()
    {
        int point = 0, spot = 0, shadowedPoint = 0, shadowedSpot = 0;
        for (const SceneLight &l : m_scene.lights) {
            if (!l.castVolumetrics || (l.type != LightType::Point && l.type != LightType::Spot) || point + spot >= static_cast<int>(kMaxVolumetricLights)) {
                continue;
            }
            ++(l.type == LightType::Point ? point : spot);
            if (l.castShadow && sceneHasShadows(m_scene)) {
                ++(l.type == LightType::Point ? shadowedPoint : shadowedSpot);
            }
        }
        if (point + spot == 0) {
            return;
        }
        bool ok = true;
        const auto build = [&](const char *name, std::map<std::string, int> combos, Blending blending, std::vector<uint32_t> slots, int draws) {
            MaterialPass pass;
            pass.shader = name;
            pass.blending = blending;
            pass.combos = std::move(combos);
            std::string error;
            const TranslatedShader *shader = shaderVariants(pass, slots, &error);
            if (!shader) {
                degrade(std::string("volumetrics shader ") + name);
                ok = false;
                return;
            }
            for (int i = 0; i < draws; ++i) {
                budget(*shader);
            }
        };
        build("volumetricsback", {}, Blending::Translucent, {}, point + spot);
        for (const auto &[isPoint, count, shadowed] : {std::tuple{1, point, shadowedPoint}, std::tuple{0, spot, shadowedSpot}}) {
            if (count - shadowed > 0) {
                build("volumetricsfront", {{"QUALITY", 3}, {"POINTLIGHT", isPoint}, {"SHADOW", 0}}, Blending::Additive, {1, 3}, count - shadowed);
            }
            if (shadowed > 0) {
                build("volumetricsfront", {{"QUALITY", 3}, {"POINTLIGHT", isPoint}, {"SHADOW", 1}}, Blending::Additive, {0, 1, 3}, shadowed);
            }
        }
        if (ok) {
            build("blur_k3", {{"VERTICAL", 0}}, Blending::Normal, {0}, 1);
            build("blur_k3", {{"VERTICAL", 1}}, Blending::Normal, {0}, 1);
            build("passthrough", {}, Blending::Additive, {0}, 1);
        }
    }

    // A particle system: its material's shader with the combos the renderer forces, and the vertex layout it needs.
    void checkParticles(const ParticleDefinition &def, int depth)
    {
        bool sheet = false;
        if (!def.materialPass.textures.empty() && !def.materialPass.textures[0].empty()) {
            checkTexture(def.materialPass);
            const auto tex = readTexFile(m_vfs, "materials/" + def.materialPass.textures[0] + ".tex", TexReadOptions{false});
            sheet = tex && tex->frames.size() > 1 && tex->animated();
        }
        checkBlending(def.materialPass);
        const MaterialPass pass = particleMaterialPass(def, sheet);
        std::string error;
        const TranslatedShader *shader = shaderVariants(pass, boundSlotsOf(pass, particleSlots(pass)), &error);
        if (!shader) {
            reject("shader compile: " + pass.shader + ": " + firstLine(error));
        } else {
            for (const AttributeInfo &a : shader->attributes) {
                if (!particleAttributeKnown(a.name)) {
                    reject("particle shader " + pass.shader + " needs the vertex attribute " + a.name);
                }
            }
            if (shader->vertex.blockSize > kBlockWindow || shader->fragment.blockSize > kBlockWindow) {
                reject("shader " + pass.shader + " has a uniform block over 16 KiB");
            }
            budget(*shader);
            // lit particles read the shadow atlas in a slot of their own, which the particle draw does not bind: they are lit unshadowed
            if (const auto lit = shader->combos.find("LIGHTING"); lit != shader->combos.end() && lit->second != 0 && sceneHasShadows(m_scene)) {
                degrade("shadows on particles");
            }
        }
        for (const ParticleChild &c : def.children) {
            if (c.definition && depth < 4) {
                checkParticles(*c.definition, depth + 1);
            }
        }
    }

    // A sound must be a file the decoders open and produce samples from: some are probed here (the first and the last of a
    // playlist and two between: the files are played one at a time and never held together), so a scene with a broken file
    // is refused instead of playing silence. A file over the size limit is a sound the scene plays without
    // (the player skips it): the picture is this engine's either way, so that degrades instead.
    void checkSounds()
    {
        for (const SoundObject &sound : m_scene.sounds) {
            const size_t n = sound.files.size();
            std::vector<size_t> probed;
            for (size_t k = 0; k < std::min<size_t>(n, 4); ++k) {
                probed.push_back(n <= 4 ? k : k * (n - 1) / 3);
            }
            for (const size_t index : probed) {
                const std::string &file = sound.files[index];
                const auto bytes = m_vfs.read(file);
                if (!bytes) {
                    reject("audio: sound file '" + file + "' cannot be read");
                    continue;
                }
                if (bytes->size() > kMaxSoundFileBytes) {
                    degrade("sound file over " + std::to_string(kMaxSoundFileBytes >> 20) + " MiB (not played)");
                    continue;
                }
                std::string error;
                auto stream = SoundStream::open(*bytes, 48000, &error);
                std::vector<float> probe(1024 * 2);
                if (!stream) {
                    reject("audio: '" + file + "': " + error);
                } else if (stream->read(probe.data(), 1024) == 0) {
                    reject("audio: '" + file + "' has no decodable audio");
                }
            }
        }
    }

    // Scripts are compiled and their init run (with short budgets, on a copy of the scene and no user properties set), so a
    // script that cannot load within the engine's limits, or that uses a browser API, is named here and not found while it runs. A script
    // that throws a TypeError or ReferenceError is named `script error` and the scene goes on without it, as in WE. The hooks
    // and functions below are the browser ones a SceneScript cannot have (nothing here implements them).
    void checkScripts()
    {
        if (m_scene.scripts.empty()) {
            return;
        }
        // (the cursor events, timers and user property hooks are implemented: docs/scene-vk/scripts-api.tsv)
        static const char *const kUnsupported[] = {"requestAnimationFrame", "XMLHttpRequest", "fetch(", "WebSocket"};
        for (const ScriptBinding &b : m_scene.scripts) {
            // custom geometry (thisScene.createModelData) is kept by the script host and drawn by no layer: the scene is drawn without it
            if (b.source.find("createModelData") != std::string::npos) {
                degrade("custom model geometry (not drawn)");
            }
            for (const char *token : kUnsupported) {
                if (b.source.find(token) != std::string::npos) {
                    std::string name = token;
                    while (!name.empty() && name.back() == '(') {
                        name.pop_back();
                    }
                    reject("SceneScript: " + b.name + " uses " + name + ", which is not supported");
                }
            }
        }
        Scene copy = m_scene;
        PropertyStore properties;
        ScriptFlags flags;
        SceneScripts scripts(copy, m_vfs, properties, flags);
        // each init gets the second the renderer gives it (a quarter of one refused 3798688689, whose init takes ~0.23 s and runs when
        // drawn); a start slower than kSlowScriptStartSeconds is named below
        ScriptHost::Limits limits;
        limits.memoryBytes = 64u << 20;
        scripts.setLimits(limits);
        scripts.start();
        for (const std::string &failure : scripts.failures()) {
            // A script that dies on a TypeError or a ReferenceError (in its init or at the top level of its module) dies the same way in
            // WE, which logs it and goes on without the script: the scene is drawn without it, named `script error`, and not refused.
            // That covers a layer the script looks up that is not in the scene (`cannot read property .. of null`), a global spelt in the
            // wrong case (2955378002's `localstorage`) and an API WE does not have either (`thisLayer.getParticleSystem`,
            // `thisScene.getSprite`, a bare `scene`: 3448877775, 3212731906, 3326873240). The API this engine gives is the whole of WE's
            // declared one (docs/scene-vk/scripts-api.tsv, whose `na` rows are named in kUnsupported above), so a name missing here
            // is missing there. What stays a refusal is what is this engine's own limit and not the script's: an InternalError
            // (out of time or memory), a RangeError (stack), a prelude that does not load, too many or too large scripts.
            if (failure.find("InternalError") == std::string::npos
                && (failure.find(": TypeError: ") != std::string::npos || failure.find(": ReferenceError: ") != std::string::npos)) {
                logInfo("{}", firstLine(failure));
                degrade("script error");
                continue;
            }
            // A property script that does not compile (3815542599's hold a pasted JSON fragment) never runs: the property keeps its
            // static value, the renderer logs the failure once when it starts the scripts, and the scene is drawn without it
            if (failure.find(": init: ") == std::string::npos && failure.find("SyntaxError") != std::string::npos) {
                degrade("script error");
                continue;
            }
            reject("SceneScript: " + firstLine(failure));
        }
        if (scripts.startSeconds() > kSlowScriptStartSeconds) {
            logInfo("scripts: {} scripts start in {:.0f} ms", m_scene.scripts.size(), scripts.startSeconds() * 1000.0);
            degrade("slow script start");
        }
        // what they cost
        double best = 1e9;
        ScriptFrame frame;
        for (int i = 0; i < kCheckedScriptFrames; ++i) {
            frame.time = i * frame.frametime;
            scripts.update(frame);
            best = std::min(best, scripts.lastUpdateSeconds());
        }
        const size_t bytes = scripts.memoryUsed();
        const auto mib = [](double b) { return std::to_string(static_cast<int>(std::lround(b / 1048576.0))); };
        if (bytes > kMaxScriptMemory) {
            reject("scripts: " + std::to_string(m_scene.scripts.size()) + " scripts use " + mib(static_cast<double>(bytes)) + " MiB, over the " +
                   mib(static_cast<double>(kMaxScriptMemory)) + " this engine gives them");
        }
        const std::string ms = std::to_string(static_cast<int>(std::lround(best * 1000.0)));
        if (best > kMaxScriptFrameSeconds) {
            reject("scripts: a frame of " + std::to_string(m_scene.scripts.size()) + " scripts takes " + ms + " ms, over the " +
                   std::to_string(static_cast<int>(std::lround(kMaxScriptFrameSeconds * 1000.0))) + " this engine allows");
        } else if (best > kSlowScriptFrameSeconds) {
            logInfo("scripts: a frame of {} scripts takes {} ms: drawn at under 20 fps", m_scene.scripts.size(), ms);
            degrade("slow scripts");
        }
        // a script that throws in its updates is stopped and the scene carries on without it, as with one that does not compile
        if (scripts.disabledCount() > 0) {
            for (const std::string &m : scripts.messages()) {
                if (m.find("' stopped: ") != std::string::npos) {
                    logInfo("{}", firstLine(m));
                }
            }
            degrade("script error");
        }
    }

    void checkBlending(const MaterialPass &pass)
    {
        if (!pass.blendingKnown) {
            reject("blending '" + pass.blendingName + "' of " + pass.shader + " is not one this engine knows");
        }
    }

    // Same rule as reject, for what the scene is drawn without.
    void degrade(const std::string &reason)
    {
        if (std::find(m_degraded.begin(), m_degraded.end(), reason) == m_degraded.end()) {
            m_degraded.push_back(reason);
        }
    }

    void reject(const std::string &reason)
    {
        if (std::find(m_reasons.begin(), m_reasons.end(), reason) == m_reasons.end()) {
            m_reasons.push_back(reason);
        }
    }

    void checkBindName(const std::string &name, const ImageLayer &layer)
    {
        const std::string prefix = "_rt_imageLayerComposite_";
        if (name.rfind(prefix, 0) == 0) {
            const std::string own = prefix + std::to_string(m_scene.nodes[layer.node].id);
            if (name.rfind(own, 0) != 0) {
                reject("an effect reads another layer's composite (" + name + "): M2");
            }
        } else if (name != "previous" && !isTarget(name)) {
            reject("effect binds '" + name + "', which is neither previous nor a render target");
        }
    }

    void checkTexture(const MaterialPass &pass)
    {
        for (const std::string &name : pass.textures) {
            if (name.empty() || isTarget(name) || name.rfind("__text:", 0) == 0 || !m_checkedTextures.insert(name).second) {
                continue;
            }
            const bool found = m_vfs.exists("materials/" + name + ".tex") || m_vfs.exists("materials/" + name + ".png")
                || m_vfs.exists("materials/" + name + ".jpg");
            if (!found) {
                reject("texture 'materials/" + name + ".tex' not found");
                continue;
            }
            if (m_vfs.exists("materials/" + name + ".tex")) {
                std::string error;
                const auto tex = readTexFile(m_vfs, "materials/" + name + ".tex", TexReadOptions{false}, &error);
                if (!tex) {
                    reject("texture 'materials/" + name + ".tex': " + error);
                } else if (tex->hasVideo()) {
                    if (!videoSupported()) {
                        reject("video texture: the video plugin (libmurasaki-scene-vk-video.so) was not found next to the engine");
                    }
                } else if (tex->freeImageFormat == -1 && !texFormatSupported(tex->rawFormat)) {
                    reject(std::string("texture format ") + texFormatName(tex->rawFormat) + " (" + std::to_string(tex->rawFormat) + "): M2");
                }
            }
        }
    }

    // A base pass may carry BLENDMODE and SHADING (the loader lets them through: 2244339517's shader declares both and reads neither). A
    // shader that does read one with a value other than 0 draws something this engine has not checked, so it is named: the pass is
    // translated again with the option at 0 and a different program means the shader reads it.
    void checkOwnOptions(const MaterialPass &pass, const TranslatedShader &shader)
    {
        for (const char *option : {"BLENDMODE", "SHADING"}) {
            const auto set = pass.combos.find(option);
            if (set == pass.combos.end() || set->second == 0) {
                continue;
            }
            MaterialPass off = pass;
            off.combos[option] = 0;
            std::string error;
            const TranslatedShader *plain = shaderVariants(off, boundSlotsOf(off, {}), &error);
            if (plain && (plain->vertex.spirv != shader.vertex.spirv || plain->fragment.spirv != shader.fragment.spirv)) {
                degrade(std::string("combo ") + (option[0] == 'B' ? "blendmode" : "shading"));
            }
        }
    }

    const TranslatedShader *shaderFor(const MaterialPass &pass, const std::vector<EffectBind> *binds, bool puppet = false, bool mesh = false)
    {
        std::vector<int> extra;
        bool slotZero = !pass.textures.empty() && !pass.textures[0].empty();
        if (binds) {
            for (const EffectBind &b : *binds) {
                extra.push_back(b.index);
                slotZero = slotZero || b.index == 0;
            }
            if (!slotZero) {
                extra.push_back(0);
            }
        }
        std::string error;
        const TranslatedShader *shader = shaderVariants(pass, boundSlotsOf(pass, extra), &error);
        if (!shader) {
            reject("shader compile: " + pass.shader + ": " + firstLine(error));
            return nullptr;
        }
        for (const AttributeInfo &a : shader->attributes) {
            if (mesh ? !meshAttributeKnown(a.name) : puppet ? !puppetAttributeKnown(a.name) : (a.name != "a_Position" && a.name != "a_TexCoord")) {
                reject("shader " + pass.shader + " needs the vertex attribute " + a.name + ": M5");
            }
        }
        for (const SamplerInfo &s : shader->samplers) {
            if (s.binding >= kMaxTextureSlots) {
                reject("shader " + pass.shader + " uses texture slot " + std::to_string(s.binding));
            }
        }
        if (shader->vertex.blockSize > kBlockWindow || shader->fragment.blockSize > kBlockWindow) {
            reject("shader " + pass.shader + " has a uniform block over 16 KiB");
        }
        return shader;
    }

    // A texture with several frames needs the sprite-sheet inputs in the base shader.
    void checkAnimation(const ImageLayer &layer, const TranslatedShader &shader)
    {
        if (layer.base.textures.empty() || layer.base.textures[0].empty()) {
            return;
        }
        const auto tex = readTexFile(m_vfs, "materials/" + layer.base.textures[0] + ".tex", TexReadOptions{false});
        if (!tex || !tex->animated() || tex->frames.size() <= 1) {
            return;
        }
        const bool hasRotation = std::any_of(shader.vertex.uniforms.begin(), shader.vertex.uniforms.end(),
                                             [](const UniformInfo &u) { return u.name == "g_Texture0Rotation"; });
        if (!hasRotation) {
            reject("animated texture drawn with a shader that has no SPRITESHEET support");
        }
    }

    // The pass compiled in every scene combo variant; null with the first failure's reason when one does not compile.
    const TranslatedShader *shaderVariants(const MaterialPass &pass, const std::vector<uint32_t> &slots, std::string *error)
    {
        const TranslatedShader *shader = nullptr;
        for (const std::map<std::string, int> &combos : m_variants) {
            m_shaders.setSceneCombos(combos);
            shader = m_shaders.get(pass, slots, error);
            if (!shader) {
                return nullptr;
            }
        }
        return shader;
    }

    const Scene &m_scene;
    const Vfs &m_vfs;
    ShaderLibrary m_shaders;
    std::vector<std::map<std::string, int>> m_variants;
    std::vector<std::string> m_reasons;
    std::vector<std::string> m_degraded;
    std::set<std::string> m_checkedTextures;
    std::map<std::string, std::optional<int>> m_formats; // texture name -> its format code, read once
    uint64_t m_draws = 0;
    uint64_t m_ringBytes = 0;
    static constexpr uint64_t m_compositeBytes = 64; // the builtin composite shader's blocks are small
};

} // namespace

std::vector<std::string> renderabilityReasons(const Scene &scene, const Vfs &vfs, std::vector<std::string> *degraded)
{
    Analysis analysis(scene, vfs);
    std::vector<std::string> reasons = analysis.run();
    if (degraded) {
        *degraded = analysis.degraded();
    }
    return reasons;
}

} // namespace murasaki::scene
