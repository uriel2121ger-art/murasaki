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

// Divisor of a standard target name (_rt_HalfCompoBuffer1 ...), for targets an effect uses without declaring them.
int divisorFromName(const std::string &name)
{
    if (name.find("Half") != std::string::npos) {
        return 2;
    }
    if (name.find("Quarter") != std::string::npos) {
        return 4;
    }
    if (name.find("Eight") != std::string::npos) {
        return 8;
    }
    return 1;
}

// Effects whose intermediate targets only carry blurred light (rays, shine): WE declares them at half size, they look the
// same at a quarter (the blur kernels are in texels, so the light spreads a little wider).
bool lowFrequencyChain(const std::string &effectFile)
{
    return effectFile.find("/godrays/") != std::string::npos || effectFile.find("/shine/") != std::string::npos;
}

constexpr uint32_t kProbesPerFrame = 2;
constexpr float kMaxChangedArea = 0.7f;          // a pass changing more of the picture than this is drawn whole
constexpr uint32_t kMaxSignatureChanges = 4;      // a pass whose signature moves more often than this is animated: not judged
constexpr uint64_t kMinReducedPixels = 640ull * 360; // below this a reduction saves nothing worth the blur

// Everything the look of an effect pass depends on besides the clock: its shader and combos, the textures named, the
// constants (after keyframes and overrides), the properties' revision (a constant may be bound to one), the sizes, and
// the quality tier the verdict was asked for.
struct PassSignature {
    uint64_t content = 0; // what the pass is made of: moving every frame means it is animated
    uint64_t shape = 0;   // the sizes and quality tier it is drawn at: moving now and then is the window or the controller
};

PassSignature passSignature(const MaterialPass &pass, const TranslatedShader *shader, uint64_t propertyRevision, VkExtent2D in, VkExtent2D out, uint32_t tier)
{
    uint64_t h = mixText(0, pass.shader);
    h = mix(h, reinterpret_cast<uintptr_t>(shader));
    for (const auto &[name, value] : pass.combos) {
        h = mix(mixText(h, name), static_cast<uint64_t>(value));
    }
    for (const std::string &t : pass.textures) {
        h = mixText(h, t);
    }
    for (const auto &[name, c] : pass.constants) {
        h = mixText(h, name);
        for (int i = 0; i < c.count; ++i) {
            uint32_t bits;
            std::memcpy(&bits, &c.value[static_cast<size_t>(i)], sizeof(bits));
            h = mix(h, bits);
        }
        h = mixText(h, c.user);
    }
    h = mix(h, propertyRevision);
    uint64_t shape = mix(0, (static_cast<uint64_t>(in.width) << 32) | in.height);
    shape = mix(shape, (static_cast<uint64_t>(out.width) << 32) | out.height);
    return {h, mix(shape, tier)};
}

} // namespace

static bool isWhite(const Vec3 &c)
{
    return c.x == 1.0f && c.y == 1.0f && c.z == 1.0f;
}

RenderTarget *SceneRenderer::drawLayer(VkCommandBuffer &cb, const ImageLayer &layer, size_t index, bool bufferOnly)
{
    if (!bufferOnly && !nodeVisible(m_scene, layer.node, m_props)) {
        return nullptr;
    }
    const float alpha = std::clamp(resolve(layer.alpha, m_props) * resolve(layer.material.alpha, m_props), 0.0f, 1.0f);
    if (alpha <= 0.0f && !bufferOnly) {
        return nullptr;
    }
    if (!bufferOnly && layer.passthrough && !layer.fullscreen && layer.colorBlendMode == 0 && alpha >= 1.0f &&
        resolve(layer.brightness, m_props) * resolve(layer.material.brightness, m_props) == 1.0f &&
        isWhite(resolve(layer.color, m_props)) && isWhite(resolve(layer.material.color, m_props)) &&
        std::none_of(layer.effects.begin(), layer.effects.end(), [&](const EffectInstance &fx) { return resolve(fx.visible, m_props); })) {
        // a compose or project layer with nothing on it hands the scene back as it found it: its shader writes a viewport
        // quad of its own, which drawn straight into the scene would paint the whole viewport from the layer's position
        return nullptr;
    }
    const std::string baseName = layer.base.textures.empty() ? std::string() : layer.base.textures[0];
    // the base picture: a file, or the scene as drawn so far (a `fullscreen` layer starts from it)
    const GpuTexture *baseTexture = isRenderTarget(baseName) ? nullptr : textureNamed(baseName);
    if (!baseTexture && !isRenderTarget(baseName) && !layer.flatColor) {
        return nullptr;
    }
    std::string error;
    PuppetState *puppet = layer.puppet ? &m_puppets.at(index) : nullptr;
    const MaterialPass &basePass = puppet ? puppet->pass : layer.base;
    const TranslatedShader *baseShader = m_shaders->get(basePass, slotsOf(basePass, nullptr), &error);
    if (!baseShader) {
        return nullptr;
    }
    PassTexture basePicture;
    TextureInfo baseInfo;
    if (baseTexture) {
        basePicture = fileTexture(*baseTexture, *m_runner);
        baseInfo = basePicture.info;
    } else if (layer.flatColor) {
        baseInfo = {1, 1, 1, 1}; // nothing to sample: the flat shader has no sampler
    } else {
        RenderTarget &snapshot = snapshotScene(cb);
        basePicture = targetTexture(snapshot, *m_runner);
        baseInfo = basePicture.info;
    }

    // --- geometry: the layer's quad in scene units, then the scene's orthogonal projection ---
    Vec2 size = layer.size;
    if (size.x <= 0.0f || size.y <= 0.0f) {
        size = {static_cast<float>(baseInfo.imageWidth), static_cast<float>(baseInfo.imageHeight)};
    }
    const Vec2 anchor = anchorFor(layer.alignment);
    Mat4 model = nodeWorldMatrix(m_scene, layer.node) * translation({-anchor.x * size.x, -anchor.y * size.y, 0.0f}) * scaling({size.x, size.y, 1.0f});
    if (layer.fullscreen) {
        // the whole view, whatever the object says: it starts from the picture drawn so far, which is already in screen space, so
        // a camera object's zoom and offset must not move it again
        Vec2 low, size2;
        viewBox(low, size2);
        model = translation({low.x, low.y, 0.0f}) * scaling({size2.x, size2.y, 1.0f});
    }
    // a fullscreen layer is already in screen space; any other is placed in the scene and seen through the camera
    const Mat4 viewProjection = layer.fullscreen ? screenProjection() : sceneProjection();
    // parallax moves the layer in the scene, in proportion to its depth (under a perspective camera the eye moves instead: updateView)
    const Mat4 shift = layer.fullscreen || m_scene.perspective ? Mat4::identity()
                                        : translation({layer.parallaxDepth.x * m_parallax.x * m_scene.orthoSize.x,
                                                       layer.parallaxDepth.y * m_parallax.y * m_scene.orthoSize.y, 0.0f});
    const Mat4 sceneMvp = viewProjection * shift * model;
    // a lit shader (LIGHTING) places its vertices through the world, g_ModelMatrix then g_ViewProjectionMatrix, and lights them where
    // they are there: its model matrix carries the parallax shift too
    const bool lit = [&] {
        const auto it = baseShader->combos.find("LIGHTING");
        return it != baseShader->combos.end() && it->second != 0;
    }();

    // --- tint: brightness and alpha reach the base shader as uniforms, the colour either as g_Color4 (VERSION 2) or at the composite ---
    const Vec3 colorA = resolve(layer.color, m_props);
    const Vec3 colorB = resolve(layer.material.color, m_props);
    const float brightness = resolve(layer.brightness, m_props) * resolve(layer.material.brightness, m_props);
    const Vec3 tint{colorA.x * colorB.x, colorA.y * colorB.y, colorA.z * colorB.z};
    bool shaderTakesColor = false;
    for (const UniformInfo &u : baseShader->fragment.uniforms) {
        shaderTakesColor = shaderTakesColor || u.name == "g_Color4" || (layer.flatColor && u.name == "g_Color");
    }
    const bool tinted = std::fabs(tint.x - 1.0f) > 1e-4f || std::fabs(tint.y - 1.0f) > 1e-4f || std::fabs(tint.z - 1.0f) > 1e-4f;

    std::vector<const EffectInstance *> effects;
    for (const EffectInstance &fx : layer.effects) {
        if (resolve(fx.visible, m_props)) {
            effects.push_back(&fx);
        }
    }
    const bool viaBuffers = bufferOnly || !effects.empty() || (tinted && !shaderTakesColor) || layer.colorBlendMode > 0;

    DrawRequest base;
    base.shader = baseShader;
    base.uniforms.material = &basePass;
    base.uniforms.properties = &m_props;
    base.uniforms.inputs = m_inputs;
    base.uniforms.textures[0] = baseInfo;
    base.uniforms.overrides["g_Brightness"] = {brightness, 0, 0, 0};
    base.uniforms.overrides["g_UserAlpha"] = {alpha, 0, 0, 0};
    base.uniforms.overrides["g_Color4"] = {tint.x * brightness, tint.y * brightness, tint.z * brightness, alpha};
    if (layer.flatColor) {
        base.uniforms.overrides["g_Color"] = {tint.x * brightness, tint.y * brightness, tint.z * brightness, 0};
        base.uniforms.overrides["g_Alpha"] = {alpha, 0, 0, 0};
    }
    base.textures[0] = basePicture;
    // the material's other slots (a normal map, a mask): files bound beside the first picture
    if (layer.base.textures.size() > kMaxTextureSlots && !m_warnedSlots) {
        m_warnedSlots = true;
        logError("a material names {} textures, the engine binds {}: the rest are ignored", layer.base.textures.size(), kMaxTextureSlots);
    }
    for (size_t slot = 1; slot < layer.base.textures.size() && slot < kMaxTextureSlots; ++slot) {
        const std::string &name = layer.base.textures[slot];
        if (name.empty() || isRenderTarget(name)) {
            continue;
        }
        if (const GpuTexture *extra = textureNamed(name)) {
            base.textures[slot] = fileTexture(*extra, *m_runner);
            base.uniforms.textures[slot] = base.textures[slot].info;
        }
    }
    // REFLECTION (genericimage2/4): the slots whose sampler names the scene drawn so far as its default (the mipped copy, and the plain
    // one beside it for the blending) are filled from a copy of the scene, with a mip chain for the lod = roughness * MipMapInfo
    // the shader reads. Only a shader that reads the chain's size asks for it (a layer without REFLECTION copies nothing). Called
    // just before the base is drawn, once the layer's caches have said it is: the copy is shared with the layer's effects and with
    // the layers after it while nothing has been drawn on the scene.
    const auto bindSceneCopies = [&]() {
        const bool reflecting = std::any_of(baseShader->samplers.begin(), baseShader->samplers.end(), [&](const SamplerInfo &s) {
            return s.binding > 0 && s.binding < kMaxTextureSlots && s.defaultText == "_rt_MipMappedFrameBuffer" && readsMipInfo(*baseShader, s.binding);
        });
        if (!reflecting) {
            return;
        }
        const RenderTarget &scene = snapshotScene(cb, true);
        for (const SamplerInfo &s : baseShader->samplers) {
            const bool filled = s.binding < layer.base.textures.size() && !layer.base.textures[s.binding].empty() && !isSceneCopy(layer.base.textures[s.binding]);
            if (s.binding > 0 && s.binding < kMaxTextureSlots && isSceneCopy(s.defaultText) && !filled) {
                base.textures[s.binding] = targetTexture(scene, *m_runner);
                base.uniforms.textures[s.binding] = base.textures[s.binding].info;
            }
        }
        // (a slot the material names as the scene copy itself binds it too)
        for (uint32_t slot = 1; slot < kMaxTextureSlots && slot < layer.base.textures.size(); ++slot) {
            if (isSceneCopy(layer.base.textures[slot])) {
                base.textures[slot] = targetTexture(scene, *m_runner);
                base.uniforms.textures[slot] = base.textures[slot].info;
            }
        }
    };
    bindShadowAtlas(base); // a lit layer under shadow-casting lights (genericimage4)
    base.quad.uMax = baseTexture ? baseTexture->uMax() : 1.0f;
    base.quad.vMax = baseTexture ? baseTexture->vMax() : 1.0f;
    if (baseTexture && baseTexture->frames().size() > 1) {
        // a sprite sheet: the quad's coordinates run over the frame, the shader places the frame in the sheet
        std::vector<std::array<uint32_t, 2>> pageSizes;
        for (size_t page = 0; page < baseTexture->pageCount(); ++page) {
            pageSizes.push_back({baseTexture->pageWidth(page), baseTexture->pageHeight(page)});
        }
        const SpriteFrameUniforms frame = spriteFrame(baseTexture->frames(), pageSizes, m_inputs.time, layer.textureFrame);
        if (frame.page > 0) { // the frame lies on a later image of the sheet: that one is sampled, at its own size
            base.textures[0].view = baseTexture->pageView(frame.page);
            base.uniforms.textures[0] = {pageSizes[frame.page][0], pageSizes[frame.page][1], pageSizes[frame.page][0], pageSizes[frame.page][1]};
        }
        base.uniforms.spriteRotation = frame.rotation;
        base.uniforms.spriteTranslation = frame.translation;
        base.quad.uMax = 1.0f;
        base.quad.vMax = 1.0f;
    }
    base.blending = layer.base.blending;
    Geometry puppetGeometry;
    Mat4 puppetModel = Mat4::identity();
    if (puppet) {
        // the mesh is in the object's own space (centred on its origin, y up, in scene units): the node's matrix places it
        puppetModel = nodeWorldMatrix(m_scene, layer.node);
        puppetGeometry.vertexBuffer = puppet->vertices.handle();
        puppetGeometry.stride = static_cast<uint32_t>(kPuppetVertexStride);
        puppetGeometry.indexBuffer = puppet->indices.handle();
        puppetGeometry.indexCount = puppet->indexCount;
        puppetGeometry.attributes = puppetAttributes(puppetGeometry.attributeCount);
        base.geometry = &puppetGeometry;
        if (puppet->skinned) {
            const BoneWorld layerWorld = boneWorldInLayer(m_scene.boneWorld, puppetModel); // the wind in the mesh's space
            puppetBoneMatrices(*layer.puppet, puppetLayerStates(layer, m_props), m_inputs.time, puppet->bones, layer.bonePhysics.get(), &layerWorld);
            base.uniforms.bones = &puppet->bones;
        }
    }

    if (!viaBuffers) {
        // straight into the scene: the layer's transform and blending
        base.uniforms.mvp = puppet ? viewProjection * shift * puppetModel : sceneMvp;
        base.uniforms.model = puppet ? puppetModel : model;
        if (lit) {
            base.uniforms.model = shift * base.uniforms.model;
        }
        base.uniforms.viewProjection = viewProjection;
        // a fullscreen layer is the whole viewport, whatever the object says: a clip space quad and no transform (WE's own
        // passthrough shader, without TRANSFORM, takes its vertices as clip positions and never reads the matrix)
        const float low = layer.fullscreen ? -1.0f : 0.0f;
        if (layer.fullscreen) {
            base.uniforms.mvp = Mat4::identity();
        }
        base.quad.minX = low;
        base.quad.maxX = 1.0f;
        base.quad.minY = low;
        base.quad.maxY = 1.0f;
        base.target = m_scene_target->view();
        base.targetFormat = m_colorFormat;
        base.extent = m_scene_target->extent;
        base.clear = false;
        base.blend = true;
        bindSceneCopies();
        transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        ++m_sceneWrites;
        m_runner->draw(cb, base);
        profileMark(cb, layerLabel(layer, index) + " direct");
        return nullptr;
    }

    // --- through the layer's buffers ---
    const VkExtent2D extent{
        static_cast<uint32_t>(std::clamp(std::lround(size.x * m_sceneToTarget), 1l, static_cast<long>(kMaxTargetDimension))),
        static_cast<uint32_t>(std::clamp(std::lround(size.y * m_sceneToTarget), 1l, static_cast<long>(kMaxTargetDimension)))};
    RenderTarget *a = &layerTarget(index, "a", extent, m_colorFormat);
    RenderTarget *b = &layerTarget(index, "b", extent, m_colorFormat);
    RenderTarget *current = a;

    // Layer caching: a layer whose passes read neither the clock, the pointer, the audio nor the scene draws the same
    // buffers until something is marked dirty (m_staticEpoch). One whose base pass is such keeps the base picture in a
    // target of its own, so the effect passes (which ping-pong in a and b) do not overwrite it.
    const LayerPlan &plan = m_layerPlans[index];
    LayerCache &cache = m_layerCache[index];
    const bool cachingOn = m_layerCaching && !(m_scripts && m_scripts->active());
    const bool cacheFinal = cachingOn && plan.allStatic;
    const bool keepBase = cachingOn && plan.baseStatic && !plan.allStatic && !effects.empty();
    RenderTarget *baseTarget = keepBase ? &layerTarget(index, "base", extent, m_colorFormat) : a;
    const auto filled = [](const RenderTarget *t) { return t->layout != VK_IMAGE_LAYOUT_UNDEFINED; };
    bool reuseFinal = false;
    bool reuseBase = false;
    if (const auto done = m_layerFinal.find(index); done != m_layerFinal.end()) {
        // already rendered this frame (for a mesh that samples its composite, or before it): its buffer holds the finished picture
        current = done->second;
        reuseFinal = true;
    } else if (cacheFinal && cache.finalEpoch == m_staticEpoch) {
        RenderTarget *kept = cache.finalKey == 'b' ? b : a;
        if (filled(kept)) {
            current = kept;
            reuseFinal = true;
            ++m_layerCacheHits;
        }
    } else if (keepBase && cache.baseEpoch == m_staticEpoch && filled(baseTarget)) {
        reuseBase = true;
        ++m_layerCacheHits;
    }

    base.uniforms.mvp = Mat4::identity();
    base.uniforms.model = Mat4::identity();
    base.uniforms.viewProjection = Mat4::identity();
    if (layer.passthrough && !layer.fullscreen) {
        // a compose or project layer (WE's composelayer shader) writes its quad from the texture coordinates and reads the
        // scene at the screen position it gets from the matrix: that is the layer's matrix on the screen, and a unit quad
        base.uniforms.mvp = sceneMvp;
        base.uniforms.model = model;
        base.uniforms.viewProjection = viewProjection;
        base.quad.minX = 0.0f;
        base.quad.maxX = 1.0f;
        base.quad.minY = 0.0f;
        base.quad.maxY = 1.0f;
    }
    if (puppet) {
        // into the layer's own buffer: the mesh's centre-relative units onto the buffer's clip square
        // (flat: the pieces are layered in z by the thousandths and by tens, and the clip volume would cut what lies outside 0 to 1; the
        // draw order is the mesh's)
        base.uniforms.mvp = scaling({2.0f / size.x, 2.0f / size.y, 0.0f});
    }
    if (lit && !(layer.passthrough && !layer.fullscreen)) {
        // the world matrix is the layer's (the unit quad, or the puppet's mesh), and the view-projection takes the world back onto the
        // buffer the way the plain matrix would
        const Mat4 world = puppet ? shift * puppetModel : shift * model * translation({0.5f, 0.5f, 0.0f}) * scaling({0.5f, 0.5f, 1.0f});
        base.uniforms.viewProjection = base.uniforms.mvp * inverse(world);
        base.uniforms.model = world;
    }
    if (!reuseFinal && !reuseBase) {
        base.target = baseTarget->view();
        base.targetFormat = baseTarget->format;
        base.extent = extent;
        // the buffer holds the shader's colour as it is (straight alpha), except for a puppet: its pieces overlap by their rectangles
        // and a piece must not wipe out, with its transparent texels, what the ones before it drew
        base.blend = puppet != nullptr;
        base.clear = true;
        bindSceneCopies();
        transition(cb, *baseTarget, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        m_runner->draw(cb, base);
        transition(cb, *baseTarget, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        profileMark(cb, layerLabel(layer, index) + " base");
        if (keepBase) {
            cache.baseEpoch = m_staticEpoch;
        }
    }
    if (keepBase && !reuseFinal) {
        current = baseTarget;
    }

    bool snapshotTaken = false;
    const auto snapshot = [&]() -> RenderTarget & {
        if (!snapshotTaken) {
            snapshotTaken = true;
            return snapshotScene(cb);
        }
        return m_targets->get("full", m_scene_target->extent, m_colorFormat);
    };
    const auto mipSnapshot = [&]() -> RenderTarget & { return snapshotScene(cb, true); }; // (shared: see snapshotScene)

    const std::string compositeA = "_rt_imageLayerComposite_" + std::to_string(m_scene.nodes[layer.node].id) + "_a";
    const std::string compositeB = "_rt_imageLayerComposite_" + std::to_string(m_scene.nodes[layer.node].id) + "_b";

    for (size_t fxIndex = 0; fxIndex < effects.size() && !reuseFinal; ++fxIndex) {
        const EffectInstance *fx = effects[fxIndex];
        // a target belongs to its effect instance and size: asking for the same name at another size never replaces
        // (and so never frees) a target the command buffer being recorded already uses
        const auto fboKey = [&](const std::string &name, VkExtent2D e) {
            return "fx" + std::to_string(fxIndex) + ":" + name + ":" + std::to_string(e.width) + "x" + std::to_string(e.height);
        };
        const int chainReduction = m_reduceChains && lowFrequencyChain(fx->def.file) ? 2 : 1;
        std::vector<const RenderTarget *> wrapping; // the effect's buffers declared `uvs` "repeat": sampled with wrapping coordinates
        const auto named = [&](const std::string &name, bool mips = false) -> RenderTarget * {
            if (name == "previous") {
                return current;
            }
            if (name == compositeA) {
                return a;
            }
            if (name == compositeB) {
                return b;
            }
            if (isSceneCopy(name)) {
                return mips ? &mipSnapshot() : &snapshot();
            }
            // a `swap` command exchanged what two names of this effect stand for: the name is read through that, then found as declared
            const std::string &standsFor = fboAlias(index, fxIndex, name);
            for (const EffectFbo &fbo : fx->def.fbos) {
                if (fbo.name == standsFor) {
                    const FboSize size = fboSize(fbo, extent.width, extent.height, chainReduction);
                    const VkExtent2D e{size.width, size.height};
                    RenderTarget *target = &layerTarget(index, fboKey(standsFor, e), e, fbo.format == "r8" ? VK_FORMAT_R8_UNORM : m_colorFormat);
                    if (fbo.repeat && std::find(wrapping.begin(), wrapping.end(), target) == wrapping.end()) {
                        wrapping.push_back(target);
                    }
                    return target;
                }
            }
            if (isRenderTarget(name)) {
                const VkExtent2D e = scaledExtent(extent, divisorFromName(name) * chainReduction);
                return &layerTarget(index, fboKey(name, e), e, m_colorFormat);
            }
            return nullptr;
        };
        const auto ready = [&](RenderTarget *t) {
            // a target nothing has written yet reads as transparent black
            if (t->layout == VK_IMAGE_LAYOUT_UNDEFINED) {
                clearTarget(cb, *t);
            }
        };

        for (size_t p = 0; p < fx->def.passes.size(); ++p) {
            yieldPoint(cb, current ? static_cast<uint64_t>(current->extent.width) * current->extent.height : 0);
            const EffectPass &pass = fx->def.passes[p];
            if (pass.command == "swap") {
                // The two buffers trade places for the rest of the effect and for the frames that follow (a ping-pong pair): afterwards
                // each name reads and writes what the other did. Only buffers the effect declares can be swapped.
                const auto declared = [&](const std::string &n) {
                    return std::any_of(fx->def.fbos.begin(), fx->def.fbos.end(), [&](const EffectFbo &f) { return f.name == n; });
                };
                if (pass.source != pass.target && declared(pass.source) && declared(pass.target)) {
                    std::string &first = m_fboAlias[fboAliasKey(index, fxIndex, pass.source)];
                    std::string &second = m_fboAlias[fboAliasKey(index, fxIndex, pass.target)];
                    if (first.empty()) first = pass.source;
                    if (second.empty()) second = pass.target;
                    std::swap(first, second);
                }
                continue;
            }
            if (!pass.command.empty()) {
                RenderTarget *src = named(pass.source);
                RenderTarget *dst = named(pass.target);
                if (pass.command != "copy" || !src || !dst || src == dst) {
                    continue;
                }
                ready(src);
                transition(cb, *src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition(cb, *dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(src->extent.width), static_cast<int32_t>(src->extent.height), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(dst->extent.width), static_cast<int32_t>(dst->extent.height), 1};
                vkCmdBlitImage(cb, src->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_LINEAR);
                transition(cb, *src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                transition(cb, *dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                profileMark(cb, layerLabel(layer, index) + " " + fx->def.file + " copy");
                continue;
            }

            const MaterialPass eff = effectivePass(pass, p < fx->overrides.size() ? &fx->overrides[p] : nullptr);
            const TranslatedShader *shader = m_shaders->get(eff, slotsOf(eff, &pass.bind), &error);
            if (!shader) {
                continue;
            }
            RenderTarget *output = pass.target.empty() ? (current == a ? b : a) : named(pass.target);
            if (!output) {
                continue;
            }

            DrawRequest req;
            req.shader = shader;
            req.uniforms.material = &eff;
            req.uniforms.properties = &m_props;
            req.uniforms.inputs = m_inputs;
            // A vertex shader that takes the model-view-projection matrix gets the layer in its own units, centred (WE's effect shaders
            // add pixel amounts to a_Position); the matrix takes it onto the clip square. One that does not (the downsample and
            // blur passes: gl_Position = vec4(a_Position, 1.0)) gets the clip square itself.
            const bool takesMvp = std::any_of(shader->vertex.uniforms.begin(), shader->vertex.uniforms.end(),
                                              [](const UniformInfo &u) { return u.input == EngineInput::ModelViewProjection && u.used; });
            req.uniforms.mvp = takesMvp ? scaling({2.0f / size.x, 2.0f / size.y, 1.0f}) : Mat4::identity();
            req.uniforms.model = Mat4::identity();
            req.uniforms.viewProjection = Mat4::identity();
            req.uniforms.screenMvp = sceneMvp;
            if (takesMvp) {
                req.uniforms.quadSize = size;
                req.quad = {-size.x / 2.0f, size.x / 2.0f, -size.y / 2.0f, size.y / 2.0f, 1.0f, 1.0f};
            }
            req.blending = eff.blending;
            req.blend = false; // the target was cleared: the pass writes its colour as it is (straight alpha)
            req.target = output->view();
            req.targetFormat = output->format;
            req.extent = output->extent;
            req.clear = true;

            std::array<RenderTarget *, kMaxTextureSlots> inputs{};
            for (uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
                const std::string fileName = slot < eff.textures.size() ? eff.textures[slot] : std::string();
                RenderTarget *rt = nullptr;
                for (const EffectBind &bind : pass.bind) {
                    if (static_cast<uint32_t>(bind.index) == slot) {
                        rt = named(bind.name, readsMipInfo(*shader, slot));
                    }
                }
                if (!rt && fileName.empty() && slot == 0) {
                    rt = current;
                }
                if (!rt && !fileName.empty() && isRenderTarget(fileName)) {
                    rt = named(fileName, readsMipInfo(*shader, slot));
                }
                if (!rt && fileName.empty()) {
                    for (const SamplerInfo &s : shader->samplers) {
                        if (s.binding == slot && isRenderTarget(s.defaultText)) {
                            rt = named(s.defaultText, readsMipInfo(*shader, slot));
                        }
                    }
                }
                if (rt && rt == output && pass.target.empty()) {
                    // an effect saved with the layer's own composite buffer named for the buffer this pass is about to write (CRT and sine
                    // wave effects of the library): what is meant is the picture so far, which is what the pass would read unbound
                    rt = current;
                }
                std::string defaultFile; // a picture the shader names for a slot nothing is bound to
                if (!rt && fileName.empty()) {
                    for (const SamplerInfo &s : shader->samplers) {
                        if (s.binding == slot && !s.defaultText.empty() && !isRenderTarget(s.defaultText) && textureNamed(s.defaultText)) {
                            defaultFile = s.defaultText;
                        }
                    }
                }
                if (rt) {
                    inputs[slot] = rt;
                    ready(rt);
                    req.textures[slot] = targetTexture(*rt, *m_runner);
                    if (std::find(wrapping.begin(), wrapping.end(), rt) != wrapping.end()) {
                        req.textures[slot].sampler = m_runner->sampler(false, false);
                    }
                    req.uniforms.textures[slot] = req.textures[slot].info;
                } else if (!fileName.empty() || !defaultFile.empty()) {
                    if (const GpuTexture *t = textureNamed(fileName.empty() ? defaultFile : fileName)) {
                        req.textures[slot] = fileTexture(*t, *m_runner);
                        req.uniforms.textures[slot] = req.textures[slot].info;
                    }
                }
            }
            for (RenderTarget *in : inputs) {
                if (in == output) {
                    throw VkException("effect '" + fx->def.file + "' reads the target it writes");
                }
            }
            // Pass optimiser: a pass that reads only the picture so far (and file textures) and writes the other ping-pong buffer
            // is measured once per signature; if it changes nothing it is not drawn, and if a reduced draw looks the same it is drawn small.
            uint32_t reduction = 1;
            VkRect2D changed{}; // the part of the picture the pass changes, when it is a small part (else width 0)
            // (not in an HDR scene: the probe draws and compares in 8 bits, so a pass that only clamps values above 1 would look like
            // the identity there; a probe in half floats is future work)
            const bool judgeable = (m_skipIdentity || m_reducePasses) && m_colorFormat == kSdrFormat && !cacheFinal && pass.target.empty() && pass.bind.empty()
                && inputs[0] == current && probeable(*shader)
                && std::none_of(inputs.begin() + 1, inputs.end(), [](const RenderTarget *t) { return t != nullptr; });
            if (judgeable) {
                const float scale = m_renderScale;
                const uint32_t tier = scale >= 0.999f ? 0u : scale >= 0.5f ? 1u : 2u;
                PassVerdict &verdict = m_passVerdicts[(static_cast<uint64_t>(index) << 40) | (static_cast<uint64_t>(fxIndex) << 20) | p];
                const PassSignature signature = passSignature(eff, shader, m_props.revision(), current->extent, output->extent, tier);
                const bool same = verdict.known && verdict.signature == signature.content && verdict.shape == signature.shape;
                if (!same) {
                    if (verdict.signature != signature.content) {
                        verdict.giveUp = verdict.giveUp || ++verdict.changes > kMaxSignatureChanges + 1; // (the first sighting is a change too)
                    }
                    if (verdict.signature != signature.content || verdict.shape != signature.shape) {
                        verdict.signature = signature.content;
                        verdict.shape = signature.shape;
                        verdict.known = false;
                    }
                    if (!verdict.known && !verdict.giveUp && m_probesThisFrame < kProbesPerFrame) {
                        if (!m_probe) {
                            m_probe = std::make_unique<PassProbe>(m_ctx, *m_runner);
                        }
                        ProbeOptions options;
                        options.testIdentity = m_skipIdentity;
                        const bool big = static_cast<uint64_t>(output->extent.width) * output->extent.height >= kMinReducedPixels;
                        if (m_reducePasses && big) {
                            // 100%: a half, kept only when it looks the same. Below 100% (a fixed lower quality, or the controller
                            // having stepped down) the buffers are already smaller, so the total stays bounded: at most a quarter
                            // while the scale is 0.5 or more, a half under it, and a looser gate.
                            options.factors = tier == 0 ? std::vector<uint32_t>{2} : tier == 1 ? std::vector<uint32_t>{4, 2} : std::vector<uint32_t>{2};
                            options.gate = tier == 0 ? 0.995 : 0.98;
                        }
                        ++m_probesThisFrame;
                        DrawRequest probeReq = req;
                        if (const auto outcome = m_probe->measure(probeReq, options)) {
                            verdict.outcome = *outcome;
                            verdict.known = true;
                            if (std::getenv("MURASAKI_SCENE_VK_PASS_OPT_LOG")) {
                                logInfo("pass-opt: {} {} p{} identity {} maxdiff {} factor {} ssim {:.4f} region {:.3f}", layerLabel(layer, index), fx->def.file, p,
                                        outcome->identity, outcome->maxDiff, outcome->factor, outcome->ssim, outcome->region.area());
                            }
                        } else {
                            verdict.giveUp = true;
                        }
                    }
                }
                if (!m_probeOnly && verdict.known && verdict.signature == signature.content && verdict.shape == signature.shape) {
                    if (verdict.outcome.identity) {
                        ++m_passOpt.skipped;
                        continue;
                    }
                    reduction = verdict.outcome.factor;
                    const ProbeRegion &r = verdict.outcome.region;
                    if (m_partialDraw && reduction == 1 && r.area() <= kMaxChangedArea && current->extent.width == output->extent.width
                        && current->extent.height == output->extent.height && current->format == output->format) {
                        const int32_t x0 = static_cast<int32_t>(std::floor(r.x0 * static_cast<float>(output->extent.width)));
                        const int32_t y0 = static_cast<int32_t>(std::floor(r.y0 * static_cast<float>(output->extent.height)));
                        const int32_t x1 = static_cast<int32_t>(std::ceil(r.x1 * static_cast<float>(output->extent.width)));
                        const int32_t y1 = static_cast<int32_t>(std::ceil(r.y1 * static_cast<float>(output->extent.height)));
                        changed = {{x0, y0}, {static_cast<uint32_t>(std::max(1, x1 - x0)), static_cast<uint32_t>(std::max(1, y1 - y0))}};
                    }
                }
            }
            for (RenderTarget *in : inputs) {
                if (in) {
                    transition(cb, *in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                }
            }
            if (reduction > 1) {
                const VkExtent2D small{std::max<uint32_t>(1, output->extent.width / reduction), std::max<uint32_t>(1, output->extent.height / reduction)};
                RenderTarget &low = layerTarget(index, fboKey("reduced" + std::to_string(p), small), small, output->format);
                req.target = low.view();
                req.extent = small;
                transition(cb, low, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
                m_runner->draw(cb, req);
                transition(cb, low, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition(cb, *output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.srcOffsets[1] = {static_cast<int32_t>(small.width), static_cast<int32_t>(small.height), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                blit.dstOffsets[1] = {static_cast<int32_t>(output->extent.width), static_cast<int32_t>(output->extent.height), 1};
                vkCmdBlitImage(cb, low.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_LINEAR);
                ++m_passOpt.reduced;
            } else if (changed.extent.width > 0) {
                // everything outside the rectangle is the picture so far: copy it, then draw the rectangle over it
                transition(cb, *current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
                transition(cb, *output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
                VkImageCopy copy{};
                copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.extent = {output->extent.width, output->extent.height, 1};
                vkCmdCopyImage(cb, current->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, output->handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                transition(cb, *current, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                transition(cb, *output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                req.clear = false;
                req.area = changed;
                m_runner->draw(cb, req);
                ++m_passOpt.partial;
            } else {
                transition(cb, *output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
                m_runner->draw(cb, req);
            }
            transition(cb, *output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            if (m_profiler->detailed()) {
                profileMark(cb, layerLabel(layer, index) + " " + fx->def.file + " p" + std::to_string(p) + " " + eff.shader + " " + std::to_string(output->extent.width) + "x" + std::to_string(output->extent.height)
                                    + (reduction > 1 ? " /" + std::to_string(reduction) : std::string()));
            }
            if (pass.target.empty()) {
                current = output;
            }
        }
    }

    if (cacheFinal && !reuseFinal) {
        cache.finalEpoch = m_staticEpoch;
        cache.finalKey = current == b ? 'b' : 'a';
    }
    m_layerFinal[index] = current;
    if (bufferOnly) {
        // the composite another object samples: the final buffer as it is (the tint a composite into the scene would add is not in it)
        transition(cb, *current, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return current;
    }
    // --- composite the layer's final buffer into the scene ---
    const TranslatedShader *composite = m_shaders->composite(&error);
    // a colour blend mode combines the buffer with the scene underneath in the shader (WE's own blend functions),
    // so that pass replaces the target's colour instead of being blended onto it
    MaterialPass blendPass;
    const TranslatedShader *blendShader = nullptr;
    if (layer.colorBlendMode > 0) {
        blendPass.shader = "genericimage3";
        blendPass.combos["BLENDMODE"] = layer.colorBlendMode;
        blendPass.textures = {"", "", "", "", "_rt_FullFrameBuffer"};
        blendShader = m_shaders->get(blendPass, {0, 4}, &error);
    }
    DrawRequest req;
    req.shader = composite;
    req.uniforms.inputs = m_inputs;
    req.uniforms.mvp = sceneMvp;
    req.uniforms.model = model;
    req.uniforms.viewProjection = viewProjection;
    req.uniforms.overrides["g_Tint"] = {shaderTakesColor ? 1.0f : tint.x, shaderTakesColor ? 1.0f : tint.y, shaderTakesColor ? 1.0f : tint.z, 1.0f};
    req.blending = layer.base.blending;
    req.blend = true;
    req.textures[0] = targetTexture(*current, *m_runner);
    req.uniforms.textures[0] = req.textures[0].info;
    req.quad = {0.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f};
    ++m_sceneWrites;
    req.target = m_scene_target->view();
    req.targetFormat = m_colorFormat;
    req.extent = m_scene_target->extent;
    req.clear = false;
    transition(cb, *current, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (blendShader) {
        RenderTarget &below = snapshot();
        req.shader = blendShader;
        req.uniforms.material = &blendPass;
        req.uniforms.properties = &m_props;
        req.uniforms.overrides["g_Color4"] = {tint.x, tint.y, tint.z, 1.0f};
        req.blend = false;
        req.textures[4] = targetTexture(below, *m_runner);
        req.uniforms.textures[4] = req.textures[4].info;
    }
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    m_runner->draw(cb, req);
    profileMark(cb, layerLabel(layer, index) + " composite");
    return nullptr;
}

} // namespace murasaki::scene
