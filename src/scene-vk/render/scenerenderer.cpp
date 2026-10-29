#include "render/scenerenderer.h"
#include "render/scenerenderer_internal.h"

#include "assets/texfile.h"
#include "assets/imagedecode.h"
#include "util/parallel.h"
#include "render/puppetdraw.h"

#include "core/framebuffers.h"
#include "render/gpuprofiler.h"
#include "render/shake.h"
#include "render/sprite.h"
#include "render/viewmapping.h"
#include "util/log.h"


#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <set>
#include <stdexcept>

namespace murasaki::scene {

namespace {

constexpr float kMinRenderScale = 0.25f;
constexpr float kMaxRenderScale = 2.0f;

constexpr float kShakeMargin = 0.04f; // the camera shake moves the view by a few percent of the scene

} // namespace

SceneRenderer::SceneRenderer(VkContext &ctx, Scene scene, Vfs vfs, const PropertyStore &properties)
    : m_ctx(ctx), m_scene(std::move(scene)), m_vfs(std::move(vfs)), m_props(properties)
{
    if (const char *off = std::getenv("MURASAKI_SCENE_VK_HALF_PARTICLES"); off && std::strcmp(off, "0") == 0) {
        m_halfSizeParticles = false;
    }
    if (const char *off = std::getenv("MURASAKI_SCENE_VK_REDUCE_CHAINS"); off && std::strcmp(off, "0") == 0) {
        m_reduceChains = false;
    }
    {
        // `skip` by default: the partial draws still leave 15 library scenes under their own floor (int6 validation)
        const char *mode = std::getenv("MURASAKI_SCENE_VK_PASS_OPT");
        if (!mode || mode[0] == '\0') {
            mode = "skip";
        }
        m_skipIdentity = std::strcmp(mode, "0") != 0;
        m_probeOnly = std::strcmp(mode, "probe") == 0; // (a diagnostic: measure, apply nothing)
        m_partialDraw = m_skipIdentity && std::strcmp(mode, "skip") != 0;
        m_reducePasses = m_partialDraw && std::strcmp(mode, "lossless") != 0;
    }
    if (const char *off = std::getenv("MURASAKI_SCENE_VK_LAYER_CACHE"); off && std::strcmp(off, "0") == 0) {
        m_layerCaching = false;
    }
    if (const char *env = std::getenv("MURASAKI_SCENE_VK_RENDER_SCALE"); env && env[0] != '\0') {
        setRenderScale(std::strtof(env, nullptr));
    }
    if (const char *env = std::getenv("MURASAKI_SCENE_VK_SPLIT_MPIX"); env && env[0] != '\0') {
        m_splitPixels = static_cast<uint64_t>(std::max(0.0, std::strtod(env, nullptr)) * 1e6);
    }
    m_profiler = std::make_unique<GpuProfiler>(m_ctx, GpuProfiler::detailedFromEnvironment());
    m_shaders = std::make_unique<ShaderLibrary>(m_vfs);
    m_colorFormat = m_scene.hdr ? kHdrFormat : kSdrFormat;
    updateFog();
    m_inputs.ambient = m_scene.ambientColor;
    m_inputs.skylight = m_scene.skylightColor;
    // before any shader is built: the counts are part of the lit shaders (and the shadow filter, which the render scale's ladder sets)
    m_shaders->setLightCombos(sceneLightCombos(m_scene, m_renderScale < 1.0f ? 1 : kShadowQuality));
    m_inputs.lights = &m_lights;
    m_inputs.shadows = &m_shadowUniforms;
    m_shaders->setTextureFormats([this](const std::string &name) -> std::optional<int> {
        const GpuTexture *t = textureNamed(name);
        return t ? std::optional<int>(t->rawFormat()) : std::nullopt;
    });
    m_runner = std::make_unique<PassRunner>(m_ctx);
    m_targets = std::make_unique<TargetPool>(m_ctx);
    applyUserVectors(m_scene, m_props); // bound origins, scales, angles, sizes and parallax depths, before anything reads them
    m_lights = packLights(m_scene, m_props);
    if (m_scene.perspective) {
        // each camera object's paths, in its queue mode (a hidden path, or one without both an eye and a centre curve, is skipped)
        for (size_t c = 0; c < m_scene.cameraObjects.size(); ++c) {
            const SceneCameraObject &camera = m_scene.cameraObjects[c];
            std::vector<double> seconds;
            for (const CameraPath &path : camera.paths) {
                seconds.push_back(path.visible && path.drivesCamera() ? path.seconds() : 0.0);
            }
            m_cameraPaths.emplace_back();
            m_cameraPaths.back().reset(std::move(seconds), camera.queueMode == "random", 0x6d757261u + static_cast<uint32_t>(c));
        }
    }
    startScripts();
    updateTexts();
    loadTextures();
    applyMediaSlots(); // notes the pictures the media slots show until there is a cover
    prepare();
    if (!m_scene.particles.empty()) {
        m_particles = std::make_unique<ParticleRenderer>(m_ctx, *m_runner, *m_shaders, m_scene, m_props,
                                                         [this](const std::string &name) { return textureNamed(name); });
        m_usesAudio = m_usesAudio || m_particles->usesAudio(); // emitters that follow the spectrum
    }
    startAudio();
    m_lastRevision = m_props.revision();
    reportUserTextures();
}

// A property that names a texture slot holds the picture the user chose; the slot is drawn with its default whatever it says, which is
// said once per property, in the words of the load-time line.
void SceneRenderer::reportUserTextures()
{
    for (const std::string &name : m_scene.userTextureProperties) {
        const PropertyValue *value = m_props.find(name);
        if (value && std::holds_alternative<std::string>(*value) && !std::get<std::string>(*value).empty()
            && m_reportedUserTextures.insert(name).second) {
            logInfo("drawing without: user texture {}", name);
        }
    }
}

void SceneRenderer::startScripts()
{
    if (m_scene.scripts.empty()) {
        return;
    }
    m_scripts = std::make_unique<SceneScripts>(m_scene, m_vfs, m_props, m_scriptFlags);
    m_scripts->setShortcutLaunchHandler(m_shortcutLaunchHandler);
    m_scripts->start();
    for (const std::string &m : m_scripts->messages()) {
        logInfo("{}", m);
    }
}

SceneRenderer::~SceneRenderer()
{
    vkDeviceWaitIdle(m_ctx.device());
    m_audio.reset();
    m_scripts.reset();
    m_particles.reset();
    m_profiler.reset();
    m_targets.reset();
    m_runner.reset();
    m_textures.clear();
}

// Translates every shader the scene will draw with and learns what changes from frame to frame.
void SceneRenderer::prepare()
{
    // A pass whose output can change without a property, size or mode change: it reads the clock, the pointer, the audio,
    // the scene drawn so far or a matrix that follows the camera. A layer with none of those draws the same buffers
    // every frame (render/scenerenderer.h, layer caching).
    // a lit pass draws something else whenever a light or the lit object moves (keyframes, scripts): its buffers are not kept then
    const bool lightsMove = !m_scene.lights.empty() && (!m_scene.animations.empty() || !m_scene.scripts.empty());
    const auto build = [&](const MaterialPass &pass, const std::vector<EffectBind> *binds) -> bool {
        std::string error;
        const TranslatedShader *shader = m_shaders->get(pass, slotsOf(pass, binds), &error);
        if (!shader) {
            throw std::runtime_error(error);
        }
        bool varies = false;
        for (const StageProgram *stage : {&shader->vertex, &shader->fragment}) {
            for (const UniformInfo &u : stage->uniforms) {
                m_animated = m_animated || changesEveryFrame(u.name);
                m_usesPointer = m_usesPointer || dependsOnPointer(u.name);
                m_usesAudio = m_usesAudio || u.input == EngineInput::AudioSpectrum;
                varies = varies || changesEveryFrame(u.name) || dependsOnPointer(u.name) || u.input == EngineInput::AudioSpectrum
                    || u.input == EngineInput::ModelViewProjectionInverse || u.input == EngineInput::EffectTextureProjection
                    || u.input == EngineInput::EffectTextureProjectionInverse || u.input == EngineInput::EffectModelViewProjection
                    || (lightsMove && readsLight(u.input));
            }
        }
        for (const SamplerInfo &sampler : shader->samplers) {
            loadDefaultTexture(sampler.defaultText);
            varies = varies || sampler.defaultText.rfind("_rt_FullFrame", 0) == 0 || sampler.defaultText.rfind("_rt_MipMapped", 0) == 0;
        }
        const auto readsScene = [](const std::string &name) {
            return name.rfind("_rt_FullFrame", 0) == 0 || name.rfind("_rt_MipMapped", 0) == 0;
        };
        for (const std::string &t : pass.textures) {
            varies = varies || readsScene(t)
                || std::any_of(m_videos.begin(), m_videos.end(), [&](const VideoTexture &v) { return v.name == t; }); // a video changes with the clock
        }
        if (binds) {
            for (const EffectBind &b : *binds) {
                varies = varies || readsScene(b.name);
            }
        }
        return varies;
    };
    std::string error;
    if (!m_shaders->composite(&error) || !m_shaders->present(&error)) {
        throw std::runtime_error(error);
    }
    // (a script may switch the parallax on)
    m_parallaxPossible = m_scene.parallax.enabled.value || !m_scene.parallax.enabled.user.empty() || m_scripts != nullptr;
    m_usesPointer = m_parallaxPossible;
    preparePuppets();
    prepareMeshes();
    prepareShadows();
    for (const MeshLayer &mesh : m_scene.meshes) {
        for (const MaterialPass &pass : mesh.materials) {
            build(pass, nullptr); // (what the shader reads decides m_animated and friends; meshes are not cached)
        }
        if (mesh.skinning) {
            m_animated = true; // the bones move with the clock, whether or not a shader reads g_Time
        }
    }
    for (const PropertyAnimation &a : m_scene.animations) {
        m_animationsEnd = std::max(m_animationsEnd, a.endSeconds()); // origins, scales, angles, alphas and constants move with the clock until then
    }
    m_animated = m_animated || !m_videos.empty();           // so do the pictures of a video
    m_layerPlans.assign(m_scene.layers.size(), {});
    m_layerCache.assign(m_scene.layers.size(), {});
    for (size_t layerIndex = 0; layerIndex < m_scene.layers.size(); ++layerIndex) {
        const ImageLayer &layer = m_scene.layers[layerIndex];
        LayerPlan &plan = m_layerPlans[layerIndex];
        const MaterialPass basePass = layer.puppet ? puppetPass(layer) : layer.base;
        bool baseVaries = build(basePass, nullptr);
        if (!m_scene.perspective && m_parallaxPossible && (layer.parallaxDepth.x != 0.0f || layer.parallaxDepth.y != 0.0f || !layer.parallaxDepthUser.empty())) {
            // a lit base pass is lit where the parallax puts the layer in the world (drawLayer): its picture changes as the camera moves
            if (const TranslatedShader *shader = m_shaders->get(basePass, slotsOf(basePass, nullptr), &error)) {
                const auto lighting = shader->combos.find("LIGHTING");
                baseVaries = baseVaries || (lighting != shader->combos.end() && lighting->second != 0);
            }
        }
        bool effectsVary = std::any_of(m_scene.animations.begin(), m_scene.animations.end(), [&](const PropertyAnimation &a) {
            return a.target == PropertyAnimation::Target::EffectConstant && a.index == layerIndex; // keyframes on a constant of its effects
        });
        bool effectsUseCompositeNames = false;
        // the base picture is the scene so far (fullscreen), or the layer places its own quad from the scene (compose, project)
        const bool noTexture = layer.base.textures.empty() || layer.base.textures[0].empty();
        baseVaries = baseVaries || layer.fullscreen || layer.passthrough || (noTexture && !layer.flatColor) || (!noTexture && isRenderTarget(layer.base.textures[0]));
        if (std::any_of(m_scene.animations.begin(), m_scene.animations.end(), [&](const PropertyAnimation &a) {
                return a.target == PropertyAnimation::Target::Alpha && a.index == layerIndex;
            })) {
            baseVaries = true; // the alpha is a uniform of the base pass, so the layer's buffers are not reusable while it moves
        }
        if (puppetSkinned(layer)) {
            m_animated = true; // the bones move with the clock
            baseVaries = true;
        }
        if (const GpuTexture *t = textureNamed(layer.base.textures.empty() ? std::string() : layer.base.textures[0]); t && t->frames().size() > 1) {
            // a sprite sheet: the shader has to place the frame, and the frame changes with the clock
            const TranslatedShader *shader = m_shaders->get(layer.base, slotsOf(layer.base, nullptr), &error);
            const bool placed = shader
                && std::any_of(shader->vertex.uniforms.begin(), shader->vertex.uniforms.end(),
                               [](const UniformInfo &u) { return u.name == "g_Texture0Rotation"; });
            if (!placed) {
                throw std::runtime_error("texture '" + layer.base.textures[0] + "' is a sprite sheet but its shader has no SPRITESHEET support");
            }
            m_animated = true;
            baseVaries = true;
        }
        for (const EffectInstance &fx : layer.effects) {
            for (size_t i = 0; i < fx.def.passes.size(); ++i) {
                const EffectPass &ep = fx.def.passes[i];
                const auto composite = [](const std::string &n) { return n.find("imageLayerComposite") != std::string::npos; };
                effectsUseCompositeNames = effectsUseCompositeNames || composite(ep.target) || composite(ep.source)
                    || std::any_of(ep.bind.begin(), ep.bind.end(), [&](const EffectBind &b) { return composite(b.name); })
                    || std::any_of(ep.material.textures.begin(), ep.material.textures.end(), composite);
                if (ep.command.empty()) {
                    effectsVary = build(effectivePass(ep, i < fx.overrides.size() ? &fx.overrides[i] : nullptr), &ep.bind) || effectsVary;
                }
            }
        }
        plan.baseStatic = !baseVaries && !layer.text && !effectsUseCompositeNames;
        plan.allStatic = plan.baseStatic && !effectsVary;
    }
}

void SceneRenderer::setScaling(bridge::msg::ScalingMode mode)
{
    if (mode != m_scaling) {
        m_scaling = mode;
        m_dirty = true;
        m_outputExtent = {}; // the scene target's size depends on the mode
    }
}

void SceneRenderer::setSpanView(const bridge::msg::SpanView &view)
{
    if (std::memcmp(&view, &m_span, sizeof(view)) != 0) {
        m_span = view;
        m_dirty = true;
        m_outputExtent = {}; // the scene target's size depends on the canvas
    }
}

void SceneRenderer::setRenderScale(float scale)
{
    const float clamped = std::clamp(std::isfinite(scale) ? scale : 1.0f, kMinRenderScale, kMaxRenderScale);
    if (clamped != m_renderScale) {
        m_renderScale = clamped;
        m_dirty = true;
        m_outputExtent = {}; // the scene target's size depends on the scale
    }
    // the shadows follow the same ladder: below a scale of 1, cells of half the size and one comparison instead of nine (the lit shaders
    // are built again for the other LIGHTS_SHADOW_MAPPING_QUALITY, at this step only)
    const ShadowSettings settings = m_renderScale < 1.0f ? ShadowSettings{kShadowCell / 2, 1} : ShadowSettings{};
    if (m_shadows && (settings.cell != m_shadowSettings.cell || settings.quality != m_shadowSettings.quality)) {
        m_shadowSettings = settings;
        m_shaders->setLightCombos(sceneLightCombos(m_scene, settings.quality));
        m_dirty = true;
    }
}

void SceneRenderer::setPointer(int x, int y, unsigned buttons, VkExtent2D viewport)
{
    if (!m_pointerEnabled || viewport.width == 0 || viewport.height == 0) {
        return;
    }
    const ScaledView view = viewOf(m_scaling, m_scene.orthoSize, viewport.width, viewport.height, m_span);
    const auto clamp01 = [](float v) { return std::clamp(v, 0.0f, 1.0f); };
    const Vec2 p{clamp01((static_cast<float>(x) - view.offsetX) / view.sizeX), clamp01((static_cast<float>(y) - view.offsetY) / view.sizeY)};
    if (p.x != m_inputs.pointer.x || p.y != m_inputs.pointer.y || buttons != m_inputs.buttons) {
        m_inputs.pointer = p;
        m_inputs.buttons = buttons;
        if (m_usesPointer) {
            m_dirty = true;
        }
    }
}

void SceneRenderer::ensureTarget(VkExtent2D output)
{
    if (m_scene_target && output.width == m_outputExtent.width && output.height == m_outputExtent.height) {
        return;
    }
    // The target covers what the output shows of the scene (and a margin), at m_renderScale target pixels per output pixel,
    // never more than one per scene unit; the projection keeps the scene's coordinates (sceneProjection).
    TargetPlanInput input;
    input.mode = m_scaling;
    input.scene = m_scene.orthoSize;
    input.outputWidth = output.width;
    input.outputHeight = output.height;
    input.span = m_span;
    input.renderScale = m_renderScale;
    input.extraMargin = (m_scene.shake.enabled.value || !m_scene.shake.enabled.user.empty()) ? kShakeMargin : 0.0f;
    input.maxDimension = kMaxTargetDimension;
    m_plan = planSceneTarget(input);
    const VkExtent2D extent{m_plan.extent.width, m_plan.extent.height};
    m_scene_target = &m_targets->get("scene", extent, m_colorFormat, true);
    m_present_source = m_scene_target;
    m_sceneCleared = false;
    m_inputs.texel = {1.0f / static_cast<float>(extent.width), 1.0f / static_cast<float>(extent.height)};
    m_sceneToTarget = static_cast<float>(extent.width) / m_plan.windowSize.x;
    m_outputExtent = output;
    m_dirty = true;
}

RenderTarget &SceneRenderer::layerTarget(size_t layer, const std::string &key, VkExtent2D extent, VkFormat format)
{
    return m_targets->get("L" + std::to_string(layer) + ":" + key, extent, format);
}

void SceneRenderer::clearTarget(VkCommandBuffer &cb, RenderTarget &target)
{
    transition(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = target.view();
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, target.extent};
    info.layerCount = 1;
    info.colorAttachmentCount = 1;
    info.pColorAttachments = &attachment;
    vkCmdBeginRendering(cb, &info);
    vkCmdEndRendering(cb);
}

// Camera parallax: the layers shift against the pointer (the camera follows it), by an amount that grows with the
// layer's depth. The displacement eases towards the pointer; `delay` is a smoothing amount where 0 follows at once.
// (WE's own feel, matched against the reference engine: rate of 10 per second at delay 0 falling to nothing at 3.)
void SceneRenderer::updateParallax()
{
    m_parallaxSettling = false;
    if (!m_parallaxEnabled || !m_parallaxPossible || !resolve(m_scene.parallax.enabled, m_props)) {
        m_parallax = {0.0f, 0.0f};
        m_inputs.parallax = m_parallax;
        return;
    }
    const float amount = resolve(m_scene.parallax.amount, m_props);
    const float delay = resolve(m_scene.parallax.delay, m_props);
    const float influence = resolve(m_scene.parallax.mouseInfluence, m_props);
    // pointer coordinates start at the top left, scene coordinates go up: only x flips
    const Vec2 target{-(m_inputs.pointer.x - 0.5f) * amount * influence, (m_inputs.pointer.y - 0.5f) * amount * influence};
    constexpr float kDelayRange = 3.0f;
    constexpr float kResponseRate = 10.0f;
    const float rate = kResponseRate * std::max(1.0f - delay / kDelayRange, 0.0f);
    const float follow = delay <= 0.0f ? 1.0f : std::clamp(rate * static_cast<float>(m_inputs.frametime), 0.0f, 1.0f);
    m_parallax = {m_parallax.x + (target.x - m_parallax.x) * follow, m_parallax.y + (target.y - m_parallax.y) * follow};
    m_inputs.parallax = m_parallax;
    m_parallaxSettling = std::fabs(m_parallax.x - target.x) > 1e-4f || std::fabs(m_parallax.y - target.y) > 1e-4f;
}

void SceneRenderer::recordScenePass(VkCommandBuffer &cb, const FrameParams &)
{
    if (m_dirty) {
        ++m_staticEpoch; // something that is not the clock changed: the layers' cached buffers are stale
    }
    m_targets->beginFrame();
    m_mipCopy = nullptr; // (the pool may have freed it)
    m_volumetricLights = volumetricLights(m_scene, m_props); // (once a frame: the shadow receivers and the beams both read it)
    drawShadows(cb); // (before the scene's first draw: every lit draw reads the atlas)
    // the scene starts as its clear colour; with `clearenabled` off only the first frame does, the rest draw over the last one
    const bool clear = resolve(m_scene.clearEnabled, m_props) || !m_sceneCleared;
    m_sceneCleared = true;
    ++m_sceneWrites;
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, clear);
    {
        VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        attachment.imageView = m_scene_target->view();
        attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachment.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.clearValue.color = {{m_scene.clearColor.x, m_scene.clearColor.y, m_scene.clearColor.z, 1.0f}};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, m_scene_target->extent};
        info.layerCount = 1;
        info.colorAttachmentCount = 1;
        info.pColorAttachments = &attachment;
        vkCmdBeginRendering(cb, &info);
        vkCmdEndRendering(cb);
    }
    profileMark(cb, "scene clear");
    m_depthCleared = false;
    m_layerFinal.clear();
    // (a layer's own buffers are not known here: an eighth of the scene for its draw; its effect passes count their own)
    const uint64_t perEntry = static_cast<uint64_t>(m_scene_target->extent.width) * m_scene_target->extent.height / 8;
    for (const DrawEntry &entry : frameOrder()) {
        yieldPoint(cb, perEntry);
        if (entry.kind == DrawEntry::Kind::Mesh) {
            drawMesh(cb, entry.index);
        } else if (entry.kind == DrawEntry::Kind::Particle) {
            drawParticles(cb, entry.index);
        } else {
            drawLayer(cb, m_scene.layers[entry.index], entry.index);
        }
    }
    // the scene with its beams: the scene itself, or (a scene that is not cleared each frame keeps its picture, which beams added to
    // would pile up) a copy of it
    RenderTarget &shown = *drawVolumetrics(cb);
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // the picture to show: the scene, or the scene with its bloom; an HDR scene without bloom is clamped by the scaling pass
    if (resolve(m_scene.bloom.enabled, m_props)) {
        m_present_source = m_scene.hdr ? drawHdrBloom(cb, shown) : drawBloom(cb, shown);
    } else {
        m_present_source = &shown;
    }
    ++m_scenePasses;
}

const std::vector<DrawEntry> &SceneRenderer::frameOrder()
{
    if (!m_scene.transparentSorting) {
        return m_scene.order;
    }
    // opaque: a mesh every part of which writes depth with plain (normal) blending
    const auto opaque = [&](const DrawEntry &e) {
        if (e.kind != DrawEntry::Kind::Mesh || e.index >= m_scene.meshes.size()) {
            return false;
        }
        const MeshLayer &mesh = m_scene.meshes[e.index];
        return !mesh.materials.empty() && std::all_of(mesh.materials.begin(), mesh.materials.end(), [](const MaterialPass &p) {
            return p.depthWrite && p.blending == Blending::Normal;
        });
    };
    const auto nodeOf = [&](const DrawEntry &e) -> size_t {
        switch (e.kind) {
        case DrawEntry::Kind::Mesh:
            return m_scene.meshes[e.index].node;
        case DrawEntry::Kind::Particle:
            return m_scene.particles[e.index].node;
        case DrawEntry::Kind::Image:
            break;
        }
        return m_scene.layers[e.index].node;
    };
    m_sortedOrder.clear();
    m_sortKeys.clear();
    for (size_t i = 0; i < m_scene.order.size(); ++i) {
        const DrawEntry &e = m_scene.order[i];
        if (opaque(e)) {
            m_sortedOrder.push_back(e);
            continue;
        }
        // the view-space z of the object's world origin: the camera looks down -z, so the smallest is the farthest
        const Mat4 world = nodeWorldMatrix(m_scene, nodeOf(e));
        const Mat4 &v = m_view;
        const float z = v.m[2] * world.m[12] + v.m[6] * world.m[13] + v.m[10] * world.m[14] + v.m[14];
        m_sortKeys.emplace_back(std::isfinite(z) ? z : 0.0f, i);
    }
    std::stable_sort(m_sortKeys.begin(), m_sortKeys.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[z, i] : m_sortKeys) {
        m_sortedOrder.push_back(m_scene.order[i]);
    }
    return m_sortedOrder;
}

void SceneRenderer::drawPresent(const FrameParams &params)
{
    DrawRequest req;
    req.shader = m_shaders->present();
    req.uniforms.overrides["g_Present"] = {m_plan.presentSize.x, m_plan.presentSize.y, m_plan.presentOffset.x, m_plan.presentOffset.y};
    req.uniforms.overrides["g_Fade"] = {m_cameraFadeLevel, 0.0f, 0.0f, 0.0f};
    req.blend = false;
    req.textures[0] = targetTexture(*(m_present_source ? m_present_source : m_scene_target), *m_runner);
    req.target = params.view;
    req.targetFormat = kExportFormat;
    req.extent = params.extent;
    req.clear = true;
    // the picture's rectangle reaches every edge of the output (a fill, or a fit with no bars): nothing outside it to blacken, so the
    // output is neither cleared nor read first
    static const bool keepClear = std::getenv("MURASAKI_SCENE_VK_PRESENT_CLEAR") != nullptr; // the old way, to compare
    req.coversTarget = !keepClear && m_plan.presentOffset.x <= 0.0f && m_plan.presentOffset.y <= 0.0f &&
                       m_plan.presentOffset.x + m_plan.presentSize.x >= static_cast<float>(params.extent.width) &&
                       m_plan.presentOffset.y + m_plan.presentSize.y >= static_cast<float>(params.extent.height);
    m_runner->draw(params.cb, req);
    profileMark(params.cb, "present");
}

// The attachment points of the puppets that have any, at this frame's bones (animated and simulated), for the objects standing on
// them (SceneNode::attachPoses, read by nodeWorldMatrix). A puppet that stands on another's point comes after that puppet.
void SceneRenderer::updateAttachments(double time)
{
    std::vector<std::pair<int, const ImageLayer *>> puppets;
    for (const ImageLayer &layer : m_scene.layers) {
        if (!layer.attachmentPoses || layer.attachmentPoses->users == 0 || !puppetSkinned(layer)) {
            continue; // a puppet that does not move keeps the points of its bind pose
        }
        int depth = 0;
        for (size_t node = layer.node; depth < 32;) {
            const auto parent = m_scene.nodes[node].parentId < 0 ? m_scene.nodeIndexById.end() : m_scene.nodeIndexById.find(m_scene.nodes[node].parentId);
            if (parent == m_scene.nodeIndexById.end()) {
                break;
            }
            node = parent->second;
            ++depth;
        }
        puppets.emplace_back(depth, &layer);
    }
    std::stable_sort(puppets.begin(), puppets.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[depth, layer] : puppets) {
        const BoneWorld world = boneWorldInLayer(m_scene.boneWorld, nodeWorldMatrix(m_scene, layer->node));
        puppetLayerStates(layer->animationLayers, m_props, m_attachmentStates);
        puppetAttachmentPoses(*layer->puppet, m_attachmentStates, time, layer->attachmentPoses->matrices, layer->bonePhysics.get(), &world);
    }
}

// One frame of a video's player: what a script asked for (VideoControl) is carried out, the clock runs, and what a script reads is told
// back. The clock restarts at the seek target and then runs at `rate` times the scene's while the video plays; a video that loops reports
// every wrap as an end, one that does not stops at its last picture.
void SceneRenderer::stepVideo(VideoTexture &v)
{
    VideoControl &c = m_scene.videos[v.name];
    VideoDecoder &decoder = *v.decoder;
    if (c.loop != v.loopApplied) {
        decoder.setLoop(c.loop);
        v.loopApplied = c.loop;
        // a looping stream is never spent: left set, a later loop = false would send the playing video back to its start
        if (c.loop) {
            v.spent = false;
        }
    }
    if (c.seek) {
        // the decoder's own bound for a stream of unknown length
        const double target = std::clamp(*c.seek, 0.0, c.duration > 0.0 ? c.duration : 1e9);
        decoder.seek(target);
        v.clock = target;
        v.lastPosition = target;
        v.spent = false;
        c.seek.reset();
        m_dirty = true;
    } else if (c.playing && v.spent && !c.loop) {
        // play() on a video that has run out starts it again
        decoder.seek(0.0);
        v.clock = 0.0;
        v.lastPosition = 0.0;
        v.spent = false;
    }
    if (c.playing && c.rate > 0.0f) {
        v.clock += m_inputs.frametime * c.rate;
        if (!std::isfinite(v.clock)) {
            v.clock = 0.0;
        }
    }
    c.position = decoder.position();
    if (c.loop) {
        // the picture on show is from an earlier place in the stream than the one before: the video has gone round
        if (c.duration > 0.0 && v.lastPosition - c.position > 0.5 * c.duration) {
            ++c.ended;
        }
        v.lastPosition = c.position;
    } else if (!v.spent && decoder.ended()) {
        v.spent = true;
        c.playing = false;
        ++c.ended;
    }
}

std::string SceneRenderer::fboAliasKey(size_t layer, size_t effect, const std::string &name)
{
    return std::to_string(layer) + ":" + std::to_string(effect) + ":" + name;
}

const std::string &SceneRenderer::fboAlias(size_t layer, size_t effect, const std::string &name) const
{
    const auto it = m_fboAlias.find(fboAliasKey(layer, effect, name));
    return it == m_fboAlias.end() || it->second.empty() ? name : it->second;
}

void SceneRenderer::render(const FrameParams &params)
{
    ensureTarget(params.extent);
    ++m_frameIndex;
    std::erase_if(m_retiredTextures, [&](const auto &r) { return m_frameIndex - r.first >= kRetireTextureFrames; });
    const bool animatedBefore = animationsRunning(); // judged by the frame before: the one that passes the end still draws the final values
    m_inputs.frametime = std::max(1e-4, params.timeSeconds - m_lastTime);
    m_lastTime = params.timeSeconds;
    m_inputs.time = params.timeSeconds;
    for (VideoTexture &v : m_videos) {
        stepVideo(v);
        if (v.zeroCopy) {
            // the picture whose time has come, as the decoder's dma-buf, converted into the texture by the GPU
            MurasakiVideoExport shown = v.decoder->exportFrame(v.clock);
            if (shown.status == 0) {
                continue;
            }
            std::string why = "the decoder cannot export its surfaces";
            if (shown.status == 1 && m_videoConverter->convert(shown, *m_textures.at(v.name), why)) {
                m_dirty = true;
                continue;
            }
            v.zeroCopy = false; // from here on the pictures are copied (the decoder converts them on the spot)
            logInfo("video texture '{}': zero-copy stopped ({}), the pictures are copied through the CPU", v.name, why);
        }
        // the picture whose time has come (a command buffer of its own copies it in, ahead of this frame's)
        if (const uint8_t *picture = v.decoder->frameAt(v.clock)) {
            m_textures.at(v.name)->update(m_ctx, picture);
            m_dirty = true;
        }
    }
    {
        const std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_r(&now, &local);
        m_inputs.daytime = static_cast<float>(local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec) / 86400.0f;
    }
    m_inputs.screen = {static_cast<float>(m_scene_target->extent.width), static_cast<float>(m_scene_target->extent.height),
                       static_cast<float>(m_scene_target->extent.width) / static_cast<float>(m_scene_target->extent.height)};
    if (m_props.revision() != m_lastRevision) {
        m_lastRevision = m_props.revision();
        m_dirty = true;
        applyUserVectors(m_scene, m_props);
        updateFog();
        reportUserTextures();
        if (m_audio) {
            for (size_t i = 0; i < m_scene.sounds.size(); ++i) {
                m_audio->setTrackVolume(i, resolve(m_scene.sounds[i].volume, m_props) * (m_scene.sounds[i].playing ? 1.0f : 0.0f));
            }
        }
    }
    if (m_audio && m_usesAudio) {
        m_inputs.audio = m_audio->spectrum(m_inputs.frametime);
    }
    for (const PropertyAnimation &a : m_scene.animations) {
        // keyframes first: a script (which owns its property, so never one of these) and the properties come after
        if (a.target == PropertyAnimation::Target::Alpha) {
            if (a.index < m_scene.layers.size()) {
                m_scene.layers[a.index].alpha.value = sampleAnimation(a, 0, params.timeSeconds, m_scene.layers[a.index].alpha.value);
            }
        } else if (a.target == PropertyAnimation::Target::EffectConstant) {
            if (a.index < m_scene.layers.size() && a.effect < m_scene.layers[a.index].effects.size()) {
                EffectInstance &fx = m_scene.layers[a.index].effects[a.effect];
                if (fx.overrides.size() <= a.pass) {
                    fx.overrides.resize(a.pass + 1);
                }
                Constant &c = fx.overrides[a.pass].constants[a.key];
                c.count = std::max(c.count, a.components);
                for (int i = 0; i < a.components && i < 3; ++i) {
                    c.value[static_cast<size_t>(i)] = sampleAnimation(a, i, params.timeSeconds, c.value[static_cast<size_t>(i)]);
                }
                c.user.clear(); // the keyframes own it
            }
        } else if (a.target >= PropertyAnimation::Target::LightIntensity) {
            if (a.index < m_scene.lights.size()) {
                SceneLight &light = m_scene.lights[a.index];
                if (a.target == PropertyAnimation::Target::LightColor) {
                    Vec3 &c = light.color.value;
                    c = {sampleAnimation(a, 0, params.timeSeconds, c.x), sampleAnimation(a, 1, params.timeSeconds, c.y), sampleAnimation(a, 2, params.timeSeconds, c.z)};
                } else {
                    Bound<float> &value = a.target == PropertyAnimation::Target::LightIntensity ? light.intensity
                        : a.target == PropertyAnimation::Target::LightRadius                     ? light.radius
                        : a.target == PropertyAnimation::Target::LightExponent                   ? light.exponent
                        : a.target == PropertyAnimation::Target::LightInnerCone                  ? light.innerCone
                        : a.target == PropertyAnimation::Target::LightOuterCone                  ? light.outerCone
                        : a.target == PropertyAnimation::Target::LightDensity                    ? light.density
                                                                                                  : light.volumetricsExponent;
                    value.value = sampleAnimation(a, 0, params.timeSeconds, value.value);
                }
            }
        } else if (a.target == PropertyAnimation::Target::CameraZoom) {
            if (a.index < m_scene.cameraObjects.size()) {
                Bound<float> &zoom = m_scene.cameraObjects[a.index].zoom;
                zoom.value = sampleAnimation(a, 0, params.timeSeconds, zoom.value);
            }
        } else if (a.index < m_scene.nodes.size()) {
            SceneNode &node = m_scene.nodes[a.index];
            Vec3 &v = a.target == PropertyAnimation::Target::Origin ? node.origin : a.target == PropertyAnimation::Target::Scale ? node.scale : node.angles;
            v = {sampleAnimation(a, 0, params.timeSeconds, v.x), sampleAnimation(a, 1, params.timeSeconds, v.y), sampleAnimation(a, 2, params.timeSeconds, v.z)};
        }
    }
    if (m_scripts) {
        ScriptFrame f;
        f.time = params.timeSeconds;
        f.frametime = m_inputs.frametime;
        f.timeOfDay = m_inputs.daytime;
        f.canvasWidth = m_scene.orthoSize.x;
        f.canvasHeight = m_scene.orthoSize.y;
        f.screenWidth = static_cast<float>(params.extent.width);
        f.screenHeight = static_cast<float>(params.extent.height);
        // the pointer is in screen space (m_inputs.pointer stays so for shaders and the parallax); scripts get the scene position
        // under it: the window's centre moved by the camera object, and half the window over the zoom on each side
        f.cursorX = m_plan.windowMin.x + m_plan.windowSize.x * 0.5f + m_cameraOffset.x + (m_inputs.pointer.x - 0.5f) * m_plan.windowSize.x / m_cameraZoom;
        f.cursorY = m_plan.windowMin.y + m_plan.windowSize.y * 0.5f + m_cameraOffset.y + (0.5f - m_inputs.pointer.y) * m_plan.windowSize.y / m_cameraZoom;
        if (m_scene.perspective) {
            // a perspective scene has no size of its own: the canvas scripts see is the screen, and the cursor is its pixel there
            // (not unprojected into the world; assumption, formats.md "Camera")
            f.canvasWidth = f.screenWidth;
            f.canvasHeight = f.screenHeight;
            f.cursorX = m_inputs.pointer.x * f.screenWidth;
            f.cursorY = (1.0f - m_inputs.pointer.y) * f.screenHeight;
        }
        f.cursorDown = (m_inputs.buttons & 1) != 0;
        f.audio = &m_inputs.audio;
        m_scripts->update(f);
        if (m_audio) {
            // scripts set volumes and play and stop sounds (ISoundLayer)
            for (size_t i = 0; i < m_scene.sounds.size(); ++i) {
                m_audio->setTrackVolume(i, resolve(m_scene.sounds[i].volume, m_props) * (m_scene.sounds[i].playing ? 1.0f : 0.0f));
            }
        }
    }
    updateTexts();
    updateAttachments(params.timeSeconds);
    updateCameraObject(params.timeSeconds);
    if (m_scriptFlags.dirty) {
        m_dirty = true;
        m_scriptFlags.dirty = false;
    }
    if (m_scriptFlags.animationsChanged) {
        // a script played, paused or moved a keyframe animation: it runs on from now until its new end
        m_scriptFlags.animationsChanged = false;
        for (const PropertyAnimation &a : m_scene.animations) {
            m_animationsEnd = std::max(m_animationsEnd, a.endSeconds());
        }
    }
    updateParallax();
    {
        // camera shake (render/shake.h): on every frame while it is on
        const bool on = resolve(m_scene.shake.enabled, m_props) && resolve(m_scene.shake.amplitude, m_props) > 0.0f;
        const Vec2 now = on ? cameraShake(params.timeSeconds, resolve(m_scene.shake.amplitude, m_props), resolve(m_scene.shake.roughness, m_props),
                                          resolve(m_scene.shake.speed, m_props), m_scene.orthoSize)
                            : Vec2{0.0f, 0.0f};
        m_dirty = m_dirty || now.x != m_shake.x || now.y != m_shake.y;
        m_shake = now;
        m_shaking = on;
    }
    updateView();
    if (!m_scene.lights.empty()) {
        // the lights after this frame's keyframes, scripts and properties. Only lit passes read them, and those are not kept in a layer's
        // buffers while the lights can move (prepare(): lightsMove), so a change redraws the scene without making every kept buffer
        // stale (a property change does that already)
        const LightUniforms lights = packLights(m_scene, m_props);
        if (std::memcmp(&lights, &m_lights, sizeof lights) != 0) {
            m_lights = lights;
            m_lightsChanged = true;
        }
    }
    if (m_particles) {
        m_particles->update(m_inputs.frametime, m_inputs.pointer, true, {1.0f, 1.0f}, &m_inputs.audio); // hidden systems stand still
    }
    m_runner->beginFrame();
    m_probesThisFrame = 0;
    VkCommandBuffer cb = params.cb; // the piece being recorded (yieldPoint moves it on)
    m_split = m_splitPixels > 0 ? params.split : nullptr;
    m_pixelsSinceSplit = 0;
    m_pieces = 1;
    m_profiler->begin(cb);
    // particles move every frame, but only those of objects that are shown
    const bool particlesMoving = m_particles && m_particles->anyVisible();
    if (m_dirty || m_animated || animatedBefore || particlesMoving || m_parallaxSettling || m_shaking || m_cameraPathPlaying || m_lightsChanged
        || m_shadowPending) {
        recordScenePass(cb, params);
        m_dirty = false;
        m_lightsChanged = false;
    }
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    FrameParams last = params;
    last.cb = cb;
    drawPresent(last);
    m_profiler->end(cb);
    m_split = nullptr;
}

void SceneRenderer::yieldPoint(VkCommandBuffer &cb, uint64_t pixels)
{
    if (m_split && m_noSplit == 0 && m_pixelsSinceSplit >= m_splitPixels) {
        cb = m_split(cb);
        m_pixelsSinceSplit = 0;
        ++m_pieces;
    }
    m_pixelsSinceSplit += pixels;
}

// Everything render() looks at to decide whether the scene pass runs, without changing anything. Scripts run on every
// frame (timers, cursor and audio callbacks), so a scene with one is never still.
bool SceneRenderer::needsFrame() const
{
    if (!m_scene_target || m_props.revision() != m_lastRevision) {
        return true;
    }
    const bool particlesMoving = m_particles && m_particles->anyVisible();
    return m_dirty || m_animated || animationsRunning() || (m_scripts && m_scripts->active()) || particlesMoving || m_parallaxSettling || m_shaking || m_cameraPathPlaying || (m_audio && m_usesAudio)
        || m_shadowPending;
}

void SceneRenderer::frameCompleted()
{
    m_profiler->collect();
}

double SceneRenderer::gpuFrameMs() const
{
    return m_profiler->lastFrameMs();
}

void SceneRenderer::profileMark(VkCommandBuffer cb, const std::string &label)
{
    m_profiler->mark(cb, label);
}

// "L<node id> <name>": what a profile line is about.
std::string SceneRenderer::layerLabel(const ImageLayer &layer, size_t) const
{
    if (!m_profiler->detailed()) {
        return {};
    }
    const SceneNode &node = m_scene.nodes[layer.node];
    return "L" + std::to_string(node.id) + " " + node.name;
}

} // namespace murasaki::scene
