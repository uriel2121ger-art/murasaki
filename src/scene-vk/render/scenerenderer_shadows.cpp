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

// Whether a box (its 8 corners) can be seen through a view-projection: not all corners beyond the same plane of the clip volume.
// A caster may move this many texels in a shadow picture before the picture is drawn again: half a texel in a point light's face or
// a spot's picture, a whole one in a cascade (the nine-tap filter hides a shift that small; a cascade's texels are large).
constexpr float kShadowCreep = 0.5f;
constexpr float kShadowCascadeCreep = 1.0f;
// The least time between two drawings of one picture whose casters keep moving (scene seconds): a point light's faces and the near
// cascades 1/30 s, the far cascade and spots 1/10 s. The receivers keep reading the picture as drawn meanwhile.
constexpr double kShadowNearInterval = 1.0 / 30.0;
constexpr double kShadowFarInterval = 0.1;

const GeometryAttribute kPositionAttributes[] = {{"a_Position", VK_FORMAT_R32G32B32_SFLOAT, 0}};

Vec4 times(const Mat4 &m, Vec3 p)
{
    return {m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3), m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3),
            m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3), m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3)};
}

// Whether a box given by its corners in clip space can be seen: not all corners beyond the same plane of the clip volume.
bool clipBoxVisible(const std::array<Vec4, 8> &c)
{
    std::array<int, 6> outside{};
    for (const Vec4 &p : c) {
        outside[0] += p.x < -p.w;
        outside[1] += p.x > p.w;
        outside[2] += p.y < -p.w;
        outside[3] += p.y > p.w;
        outside[4] += p.z < -p.w;
        outside[5] += p.z > p.w;
    }
    return std::none_of(outside.begin(), outside.end(), [](int n) { return n == 8; });
}

// Whether a box (its corners in world space) can be seen through a view-projection.
bool boxInFrustum(const std::array<Vec3, 8> &corners, const Mat4 &m)
{
    std::array<Vec4, 8> clip;
    for (size_t q = 0; q < 8; ++q) {
        clip[q] = times(m, corners[q]);
    }
    return clipBoxVisible(clip);
}

// The corners of the box from `low` to `high` through `m` (corner c takes high's x when bit 0 is set, y bit 1, z bit 2).
std::array<Vec3, 8> worldCorners(const Mat4 &m, Vec3 low, Vec3 high)
{
    std::array<Vec3, 8> out;
    for (size_t c = 0; c < 8; ++c) {
        const Vec4 p = times(m, {(c & 1) ? high.x : low.x, (c & 2) ? high.y : low.y, (c & 4) ? high.z : low.z});
        out[c] = {p.x, p.y, p.z};
    }
    return out;
}

// A skinned mesh's part bounds are its bind pose's: widened by half on every side, for the animation.
void widenForAnimation(Vec3 &low, Vec3 &high)
{
    const Vec3 pad{(high.x - low.x) * 0.5f, (high.y - low.y) * 0.5f, (high.z - low.z) * 0.5f};
    low = {low.x - pad.x, low.y - pad.y, low.z - pad.z};
    high = {high.x + pad.x, high.y + pad.y, high.z + pad.z};
}

// Whether a box (its corners in world space) is smaller than a texel of a shadow picture in both directions (wholly in front of the
// light): what it casts there is lost in the filter.
bool underTexel(const std::array<Vec3, 8> &corners, const ShadowView &view)
{
    float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    for (const Vec3 &corner : corners) {
        const Vec4 p = times(view.viewProjection, corner);
        if (p.w <= 1e-6f) {
            return false;
        }
        x0 = std::min(x0, p.x / p.w);
        x1 = std::max(x1, p.x / p.w);
        y0 = std::min(y0, p.y / p.w);
        y1 = std::max(y1, p.y / p.w);
    }
    return (x1 - x0) * view.width * 0.5f < 1.0f && (y1 - y0) * view.height * 0.5f < 1.0f;
}

} // namespace

// The casters' passes (each casting part's material's caster, render/shaderlib.h) and the atlas's format.
void SceneRenderer::prepareShadows()
{
    m_shadows = sceneHasShadows(m_scene);
    m_shadowSettings = m_renderScale < 1.0f ? ShadowSettings{kShadowCell / 2, 1} : ShadowSettings{};
    m_casterPasses.assign(m_scene.meshes.size(), {});
    if (!m_shadows) {
        return;
    }
    // a depth format the GPU renders into and samples, D32 first (the point and spot pictures are perspective ones); a linear filter
    // where the format has one (the comparisons are then filtered 2 x 2, as WE's comparison sampler is assumed to be)
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(m_ctx.physicalDevice(), f, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((fp.optimalTilingFeatures & need) == need) {
            m_shadowFormat = f;
            m_shadowLinear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
            break;
        }
    }
    if (m_shadowFormat == VK_FORMAT_UNDEFINED) {
        logError("shadows: the device has no depth format it can sample; the scene is drawn without shadows");
        m_shadows = false;
        return;
    }
    const auto receives = [&](const MaterialPass &pass) {
        std::string error;
        const TranslatedShader *shader = m_shaders->get(pass, slotsOf(pass, nullptr), &error);
        if (!shader || std::none_of(shader->samplers.begin(), shader->samplers.end(), [](const SamplerInfo &s) { return s.defaultText == "_rt_shadowAtlas"; })) {
            return false;
        }
        // generic4 declares the atlas whatever its LIGHTING says, and reads it only when lit (an unlit sky box reads nothing)
        const auto lighting = shader->combos.find("LIGHTING");
        return lighting == shader->combos.end() || lighting->second != 0;
    };
    m_meshReceives.assign(m_scene.meshes.size(), false);
    for (size_t i = 0; i < m_scene.meshes.size(); ++i) {
        m_meshReceives[i] = std::any_of(m_scene.meshes[i].materials.begin(), m_scene.meshes[i].materials.end(), receives);
    }
    m_layerReceives.assign(m_scene.layers.size(), 0);
    for (size_t i = 0; i < m_scene.layers.size(); ++i) {
        const ImageLayer &layer = m_scene.layers[i];
        if (receives(layer.puppet ? puppetPass(layer) : layer.base)) {
            m_layerReceives[i] = layer.fullscreen || layer.puppet ? 2 : 1;
        }
    }
    for (size_t i = 0; i < m_scene.meshes.size(); ++i) {
        const MeshLayer &mesh = m_scene.meshes[i];
        if (!mesh.castShadow || i >= m_meshes.size() || !m_meshes[i]) {
            continue;
        }
        for (const MaterialPass &material : mesh.materials) {
            CasterPass caster;
            if (material.blending == Blending::Additive) {
                // light added to the scene (a glow, a halo) casts nothing: an assumption, WE's rule is not known (formats.md, "Shadows")
                m_casterPasses[i].push_back(std::move(caster));
                continue;
            }
            caster.pass = m_shaders->shadowCasterPass(material);
            std::string error;
            caster.shader = m_shaders->get(caster.pass, slotsOf(caster.pass, nullptr), &error);
            if (!caster.shader) {
                logError("shadow caster of {}: {} (the part casts no shadow)", mesh.path, error);
            }
            m_casterPasses[i].push_back(std::move(caster));
        }
        // nothing to draw (all additive, or no caster compiled): the mesh is no caster at all
        if (std::none_of(m_casterPasses[i].begin(), m_casterPasses[i].end(), [](const CasterPass &c) { return c.shader != nullptr; })) {
            m_casterPasses[i].clear();
        }
    }
}

void SceneRenderer::updateMeshBones(size_t index)
{
    if (index >= m_meshBonesFrame.size() || m_meshBonesFrame[index] == m_frameIndex) {
        return;
    }
    const MeshLayer &mesh = m_scene.meshes[index];
    puppetBoneMatrices(mesh.model->rig, puppetLayerStates(mesh.animationLayers, m_props), m_inputs.time, m_meshBones[index]);
    m_meshBonesFrame[index] = m_frameIndex;
}

// The shadow pass: the casters of every shadowed light into the atlas (render/shadows.h), at the start of a scene pass. The atlas is
// kept from frame to frame and drawn again picture by picture (a point light's face, a spot, a cascade):
//   * the whole atlas when its structure changes: a caster shown or hidden, a light's pictures appearing or going, the cell size;
//   * a picture never drawn, or one whose being seen by a receiver changed (it is cleared, and drawn if seen);
//   * a seen picture where a caster moved (moved(): kShadowCreep texels, kShadowCascadeCreep in a cascade, or 2 x 10^-5 of the depth
//     range) or a skinned caster in it moved its bones, once kShadowNearInterval / kShadowFarInterval has passed since it was drawn
//     (until then the scene pass keeps running: m_shadowPending);
//   * a point light's six faces together when its depth range changes (they share it).
// A drawn picture's receiver values are taken from this frame's plan; the others keep the values they were drawn with, so the atlas
// and what the receivers read always agree. Lit 2D layers kept in their buffers are drawn again after a shadow pass (m_staticEpoch).
void SceneRenderer::drawShadows(VkCommandBuffer &cb)
{
    m_shadowPending = false;
    if (!m_shadows) {
        return;
    }
    m_shadowCasters.clear();
    ShadowBounds bounds;
    uint64_t signature = mix(mix(0x5348414430ull, m_shadowSettings.cell), static_cast<uint64_t>(m_shadowSettings.quality));
    for (size_t i = 0; i < m_scene.meshes.size(); ++i) {
        const MeshLayer &mesh = m_scene.meshes[i];
        if (m_casterPasses[i].empty() || !mesh.model || !nodeVisible(m_scene, mesh.node, m_props)) {
            continue;
        }
        ShadowCaster &caster = m_shadowCasters.emplace_back();
        caster.mesh = i;
        caster.model = nodeWorldMatrix(m_scene, mesh.node);
        caster.parts.clear();
        caster.local.clear();
        signature = mix(signature, i);
        if (mesh.skinning) {
            updateMeshBones(i);
            caster.bones = 0x426f6e6573ull;
            for (float f : m_meshBones[i]) {
                caster.bones = mix(caster.bones, std::bit_cast<uint32_t>(f));
            }
        }
        for (const MeshPart &part : mesh.model->parts) {
            Vec3 low = part.boundsMin, high = part.boundsMax;
            if (mesh.skinning) {
                widenForAnimation(low, high);
            }
            caster.local.push_back(worldCorners(Mat4::identity(), low, high));
            caster.parts.push_back(worldCorners(caster.model, low, high));
            for (const Vec3 &corner : caster.parts.back()) {
                bounds.add(corner);
            }
        }
    }
    ShadowCamera camera;
    camera.viewProjection = sceneProjection();
    if (m_scene.perspective) {
        camera.nearDistance = m_scene.camera.nearZ;
        camera.farDistance = m_scene.camera.farZ;
    } else {
        // the orthographic view's depth runs 0 to 20000 (screenProjection: 10000 units either side of the scene's plane); the cascades'
        // distances are measured from the plane (an assumption: WE's are not known), not from the near end of the depth
        camera.clipNear = 0.0f;
        camera.clipFar = 20000.0f;
        camera.nearDistance = 10000.0f;
        camera.farDistance = 20000.0f;
    }
    const ShadowPlan plan = planShadows(m_scene, m_props, m_lights, camera, bounds, m_shadowSettings);
    if (plan.atlasWidth == 0) {
        m_shadowUniforms = plan.uniforms;
        return;
    }
    // the receivers (the draws that read the atlas): a picture no receiver is in is left empty (nothing reads it), e.g. a cascade
    // whose slice of the view holds no lit mesh
    m_shadowReceivers.clear();
    bool everywhere = false;
    for (size_t i = 0; i < m_scene.meshes.size() && !everywhere; ++i) {
        const MeshLayer &mesh = m_scene.meshes[i];
        if (!m_meshReceives[i] || !mesh.model || !nodeVisible(m_scene, mesh.node, m_props)) {
            continue;
        }
        const Mat4 model = nodeWorldMatrix(m_scene, mesh.node);
        for (const MeshPart &part : mesh.model->parts) {
            Vec3 low = part.boundsMin, high = part.boundsMax;
            if (mesh.skinning) {
                widenForAnimation(low, high);
            }
            m_shadowReceivers.push_back(worldCorners(model, low, high));
        }
    }
    for (size_t i = 0; i < m_scene.layers.size() && !everywhere; ++i) {
        const ImageLayer &layer = m_scene.layers[i];
        if (m_layerReceives[i] == 0 || !nodeVisible(m_scene, layer.node, m_props)) {
            continue;
        }
        Vec2 size = layer.size;
        if (const GpuTexture *t = layer.base.textures.empty() ? nullptr : textureNamed(layer.base.textures[0]); t && (size.x <= 0.0f || size.y <= 0.0f)) {
            size = {static_cast<float>(t->imageWidth()), static_cast<float>(t->imageHeight())};
        }
        if (m_layerReceives[i] == 2 || size.x <= 0.0f || size.y <= 0.0f) {
            everywhere = true; // (a fullscreen or puppet layer: taken as anywhere)
            break;
        }
        // the layer's quad as drawLayer places it, grown to twice its size (the parallax shift of an orthographic scene)
        const Vec2 anchor = anchorFor(layer.alignment);
        const Mat4 model = nodeWorldMatrix(m_scene, layer.node) * translation({-anchor.x * size.x, -anchor.y * size.y, 0.0f}) * scaling({size.x, size.y, 1.0f});
        m_shadowReceivers.push_back(worldCorners(model, {-0.5f, -0.5f, 0.0f}, {1.5f, 1.5f, 0.0f}));
    }
    // a shadowed volumetric light reads the atlas through its whole volume
    for (const VolumetricLight &light : m_volumetricLights) {
        if (light.shadowSlot >= 0) {
            m_shadowReceivers.push_back(worldCorners(light.volume, {-1.0f, -1.0f, light.sphere ? -1.0f : 0.0f}, {1.0f, 1.0f, 1.0f}));
        }
    }
    for (const ShadowLightViews &light : plan.lights) {
        signature = mix(signature, light.views.size());
    }
    const bool atlasReady = m_shadowDrawn && m_shadowAtlas && m_shadowAtlas->extent.width == plan.atlasWidth && m_shadowAtlas->extent.height == plan.atlasHeight;
    const bool redrawAll = !atlasReady || signature != m_shadowSignature || m_shadowPictures.size() != plan.lights.size();
    if (redrawAll) {
        m_shadowPictures.assign(plan.lights.size(), {});
        for (size_t l = 0; l < plan.lights.size(); ++l) {
            m_shadowPictures[l].resize(plan.lights[l].views.size());
        }
    }
    const LightCounts &counts = m_lights.counts;
    const size_t points = static_cast<size_t>(counts.pointShadow);
    const size_t spots = static_cast<size_t>(counts.spotShadow);
    const auto isCascade = [&](size_t l) { return l >= points + spots; };
    // Whether a caster lands elsewhere in a picture than when the picture was drawn: the corners of its parts' boxes through the old
    // and the new matrices (the light's, the camera's for a cascade, the caster's); a part outside the picture then and now does not count
    const auto moved = [&](size_t l, size_t v) {
        const ShadowView &view = plan.lights[l].views[v];
        const ShadowPicture &picture = m_shadowPictures[l][v];
        const float creep = isCascade(l) ? kShadowCascadeCreep : kShadowCreep;
        if (picture.casters.size() != m_shadowCasters.size()) {
            return true;
        }
        for (size_t k = 0; k < m_shadowCasters.size(); ++k) {
            const ShadowCaster &c = m_shadowCasters[k];
            if (picture.casters[k].mesh != c.mesh) {
                return true;
            }
            const Mat4 now = view.viewProjection * c.model;
            const Mat4 then = picture.viewProjection * picture.casters[k].model;
            for (const std::array<Vec3, 8> &box : c.local) {
                std::array<Vec4, 8> a, b;
                for (size_t q = 0; q < 8; ++q) {
                    a[q] = times(now, box[q]);
                    b[q] = times(then, box[q]);
                }
                if (!clipBoxVisible(a) && !clipBoxVisible(b)) {
                    continue;
                }
                if (picture.casters[k].bones != c.bones) {
                    return true; // a skinned caster that moved its bones
                }
                for (size_t q = 0; q < 8; ++q) {
                    if ((a[q].w > 1e-6f) != (b[q].w > 1e-6f)) {
                        return true;
                    }
                    if (a[q].w > 1e-6f
                        && (std::fabs(a[q].x / a[q].w - b[q].x / b[q].w) * view.width * 0.5f > creep
                            || std::fabs(a[q].y / a[q].w - b[q].y / b[q].w) * view.height * 0.5f > creep
                            || std::fabs(a[q].z / a[q].w - b[q].z / b[q].w) > 2e-5f)) {
                        return true;
                    }
                }
            }
        }
        return false;
    };
    const double now = m_inputs.time;
    const auto interval = [&](size_t l, size_t v) {
        if (l < points) {
            return kShadowNearInterval;
        }
        if (l < points + spots) {
            return kShadowFarInterval;
        }
        return v + 1 < static_cast<size_t>(kShadowCascades) ? kShadowNearInterval : kShadowFarInterval;
    };
    std::vector<std::vector<bool>> &dirty = m_shadowDirty;
    std::vector<std::vector<bool>> &seen = m_shadowSeen;
    dirty.resize(plan.lights.size());
    seen.resize(plan.lights.size());
    bool anyDirty = false;
    for (size_t l = 0; l < plan.lights.size(); ++l) {
        const std::vector<ShadowView> &views = plan.lights[l].views;
        dirty[l].assign(views.size(), false);
        seen[l].assign(views.size(), false);
        bool urgent = false, waiting = false;
        for (size_t v = 0; v < views.size(); ++v) {
            ShadowPicture &picture = m_shadowPictures[l][v];
            seen[l][v] = everywhere
                || std::any_of(m_shadowReceivers.begin(), m_shadowReceivers.end(), [&](const auto &box) { return boxInFrustum(box, views[v].viewProjection); });
            if (!picture.drawn || seen[l][v] != picture.seen) {
                dirty[l][v] = urgent = true; // (not held back: the picture is wrong, or newly read)
            } else if (seen[l][v] && moved(l, v)) {
                if (now - picture.drawnAt >= interval(l, v) || now < picture.drawnAt) {
                    dirty[l][v] = true;
                } else {
                    waiting = true;
                }
            }
        }
        // a point light's faces share its depth range: when that changes, the six are drawn together (once they all may be)
        if (l < points
            && (std::memcmp(&m_shadowUniforms.pointProjection[l], &plan.uniforms.pointProjection[l], sizeof(Vec4)) != 0
                || std::memcmp(&m_shadowUniforms.pointTransform[l], &plan.uniforms.pointTransform[l], sizeof(Vec4)) != 0)
            && std::find(dirty[l].begin(), dirty[l].end(), true) != dirty[l].end()) {
            const bool all = urgent || std::all_of(m_shadowPictures[l].begin(), m_shadowPictures[l].end(), [&](const ShadowPicture &p) {
                                 return now - p.drawnAt >= kShadowNearInterval || now < p.drawnAt;
                             });
            dirty[l].assign(views.size(), all);
            waiting = waiting || !all;
        }
        m_shadowPending = m_shadowPending || (waiting && now != m_shadowLastTime); // (a frozen clock never gets to draw it)
        anyDirty = anyDirty || std::find(dirty[l].begin(), dirty[l].end(), true) != dirty[l].end();
    }
    m_shadowLastTime = now;
    if (!anyDirty && !redrawAll) {
        return; // the atlas holds this already (and m_shadowUniforms what it was drawn with)
    }
    if (redrawAll) {
        m_shadowUniforms = plan.uniforms;
    }
    // the receivers' values of what is drawn again (the rest keep the values their pictures were drawn with)
    for (size_t l = 0; l < plan.lights.size(); ++l) {
        for (size_t v = 0; v < plan.lights[l].views.size(); ++v) {
            if (!dirty[l][v]) {
                continue;
            }
            ShadowPicture &picture = m_shadowPictures[l][v];
            picture.viewProjection = plan.lights[l].views[v].viewProjection;
            picture.casters.clear();
            for (const ShadowCaster &c : m_shadowCasters) {
                picture.casters.push_back({c.mesh, c.model, c.bones});
            }
            picture.seen = seen[l][v];
            picture.drawn = true;
            picture.drawnAt = now;
            ++m_shadowPicturesDrawn;
            if (l < points) {
                m_shadowUniforms.pointProjection[l] = plan.uniforms.pointProjection[l];
                m_shadowUniforms.pointTransform[l] = plan.uniforms.pointTransform[l];
            } else if (l < points + spots) {
                m_shadowUniforms.spotProjection[l - points] = plan.uniforms.spotProjection[l - points];
                m_shadowUniforms.spotTransform[l - points] = plan.uniforms.spotTransform[l - points];
            } else {
                const size_t k = (l - points - spots) * kShadowCascades + v;
                m_shadowUniforms.cascadeProjection[k] = plan.uniforms.cascadeProjection[k];
                m_shadowUniforms.cascadeTransform[k] = plan.uniforms.cascadeTransform[k];
                m_shadowUniforms.cascadeTexel[k] = plan.uniforms.cascadeTexel[k];
            }
        }
    }

    // (the atlas's size follows from the light counts and the cell: constant for a scene until the render scale crosses 1)
    if (!m_shadowAtlas || m_shadowAtlas->extent.width != plan.atlasWidth || m_shadowAtlas->extent.height != plan.atlasHeight) {
        if (m_shadowAtlas) {
            vkDeviceWaitIdle(m_ctx.device()); // (a recorded frame may still read the old one)
        }
        m_shadowAtlas = std::make_unique<RenderTarget>();
        m_shadowAtlas->extent = {plan.atlasWidth, plan.atlasHeight};
        m_shadowAtlas->format = m_shadowFormat;
        m_shadowAtlas->image = m_ctx.createImage(m_shadowFormat, plan.atlasWidth, plan.atlasHeight, 1,
                                                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    }
    RenderTarget &atlas = *m_shadowAtlas;
    transition(cb, atlas, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, redrawAll);
    // the far plane where pictures are drawn again (everywhere when the whole atlas is): nothing casts where nothing is drawn
    const auto clearRect = [&](VkRect2D rect) {
        if (rect.extent.width == 0 || rect.extent.height == 0) {
            return;
        }
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = atlas.view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = rect;
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cb, &info);
        vkCmdEndRendering(cb);
    };
    // whole pixels of a rectangle, kept inside `inside` (a point light's faces are a third of its block, measured as 0.3333)
    const auto pixels = [&](float x0, float y0, float x1, float y1, const ShadowBlock &inside) {
        const int32_t left = static_cast<int32_t>(inside.x), top = static_cast<int32_t>(inside.y);
        const int32_t right = static_cast<int32_t>(inside.x + inside.width), bottom = static_cast<int32_t>(inside.y + inside.height);
        const int32_t a = std::clamp(static_cast<int32_t>(std::floor(x0)), left, right);
        const int32_t b = std::clamp(static_cast<int32_t>(std::floor(y0)), top, bottom);
        const int32_t c = std::clamp(static_cast<int32_t>(std::ceil(x1)), a, right);
        const int32_t d = std::clamp(static_cast<int32_t>(std::ceil(y1)), b, bottom);
        return VkRect2D{{a, b}, {static_cast<uint32_t>(c - a), static_cast<uint32_t>(d - b)}};
    };
    if (redrawAll) {
        clearRect({{0, 0}, atlas.extent});
    } else {
        for (size_t l = 0; l < plan.lights.size(); ++l) {
            const ShadowLightViews &light = plan.lights[l];
            const std::vector<ShadowView> &views = light.views;
            if (l < points && !views.empty() && std::find(dirty[l].begin(), dirty[l].end(), false) == dirty[l].end()) {
                clearRect(pixels(light.block.x, light.block.y, light.block.x + light.block.width, light.block.y + light.block.height, light.block));
                continue;
            }
            for (size_t v = 0; v < views.size(); ++v) {
                if (dirty[l][v]) {
                    const ShadowBlock inside = l < points ? light.block : ShadowBlock{views[v].x, views[v].y, views[v].width, views[v].height};
                    clearRect(pixels(views[v].x, views[v].y, views[v].x + views[v].width, views[v].y + views[v].height, inside));
                }
            }
        }
    }
    // the clears are written before the casters are tested against them
    transition(cb, atlas, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    m_shadowMatrices.resize(plan.lights.size());
    m_shadowViewports.resize(plan.lights.size());
    m_shadowGeometry.clear();
    m_shadowRequests.clear();
    m_shadowRanges.clear();
    m_shadowRangeMasks.clear();
    size_t parts = 0;
    for (const ShadowCaster &c : m_shadowCasters) {
        parts += m_casterPasses[c.mesh].size();
    }
    m_shadowGeometry.reserve(parts); // (the requests point into it)
    for (const ShadowCaster &c : m_shadowCasters) {
        const MeshLayer &mesh = m_scene.meshes[c.mesh];
        const MeshGpu &gpu = *m_meshes[c.mesh];
        uint32_t attributeCount = 0;
        const GeometryAttribute *attributes = mesh.model->skinned ? puppetAttributes(attributeCount) : meshAttributes(attributeCount);
        const std::vector<CasterPass> &passes = m_casterPasses[c.mesh];
        for (size_t p = 0; p < std::min(passes.size(), gpu.parts.size()); ++p) {
            Geometry &g = m_shadowGeometry.emplace_back();
            g.vertexBuffer = gpu.parts[p].vertices.handle();
            g.stride = static_cast<uint32_t>(p < mesh.model->parts.size() ? mesh.model->parts[p].stride : kMeshVertexStride);
            g.indexBuffer = gpu.parts[p].indices.handle();
            g.indexCount = gpu.parts[p].indexCount;
            g.indexType = gpu.parts[p].indexType;
            g.attributes = attributes;
            g.attributeCount = attributeCount;
            const TranslatedShader *shader = passes[p].shader;
            if (gpu.parts[p].positions.handle() != VK_NULL_HANDLE && shader
                && std::all_of(shader->attributes.begin(), shader->attributes.end(), [](const AttributeInfo &a) { return a.name == "a_Position"; })) {
                g.vertexBuffer = gpu.parts[p].positions.handle(); // (a caster that reads the position only)
                g.stride = 12;
                g.attributes = kPositionAttributes;
                g.attributeCount = 1;
            }
        }
    }
    // per request, where its runs start in m_shadowRanges (-1: none), filled in once the vectors stop growing
    std::vector<std::pair<size_t, size_t>> &runs = m_shadowRuns;
    runs.clear();
    for (size_t l = 0; l < plan.lights.size(); ++l) {
        const std::vector<ShadowView> &views = plan.lights[l].views;
        m_shadowViewports[l].clear();
        if (views.empty()) {
            continue;
        }
        m_shadowMatrices[l].fill(views.front().viewProjection);
        for (size_t v = 0; v < views.size() && v < 6; ++v) {
            m_shadowMatrices[l][v] = views[v].viewProjection;
            m_shadowViewports[l].push_back({views[v].x, views[v].y, views[v].width, views[v].height, 0.0f, 1.0f});
        }
        size_t geometry = 0;
        for (const ShadowCaster &c : m_shadowCasters) {
            const MeshLayer &mesh = m_scene.meshes[c.mesh];
            const MeshGpu &gpu = *m_meshes[c.mesh];
            const std::vector<CasterPass> &passes = m_casterPasses[c.mesh];
            for (size_t p = 0; p < std::min(passes.size(), gpu.parts.size()); ++p, ++geometry) {
                const CasterPass &caster = passes[p];
                if (!caster.shader) {
                    continue;
                }
                // the pictures drawn again that a receiver sees, the part reaches and where it is larger than a texel
                uint32_t mask = 0;
                for (size_t v = 0; v < views.size() && v < 6; ++v) {
                    if (dirty[l][v] && seen[l][v]
                        && (p >= c.parts.size() || (boxInFrustum(c.parts[p], views[v].viewProjection) && !underTexel(c.parts[p], views[v])))) {
                        mask |= 1u << v;
                    }
                }
                if (mask == 0) {
                    continue;
                }
                // a big part: only its runs that reach a picture, into the pictures they reach
                size_t runStart = 0, runCount = 0;
                if (!gpu.parts[p].chunks.empty()) {
                    runStart = m_shadowRanges.size();
                    uint32_t any = 0;
                    for (const MeshGpu::Part::Chunk &chunk : gpu.parts[p].chunks) {
                        const std::array<Vec3, 8> box = worldCorners(c.model, chunk.low, chunk.high);
                        uint8_t bits = 0;
                        for (size_t v = 0; v < views.size() && v < 6; ++v) {
                            if (((mask >> v) & 1u) != 0 && boxInFrustum(box, views[v].viewProjection)) {
                                bits = static_cast<uint8_t>(bits | (1u << v));
                            }
                        }
                        if (bits != 0) {
                            m_shadowRanges.push_back(chunk.range);
                            m_shadowRangeMasks.push_back(bits);
                            any |= bits;
                        }
                    }
                    runCount = m_shadowRanges.size() - runStart;
                    if (any == 0) {
                        continue;
                    }
                    mask = any;
                }
                DrawRequest &req = m_shadowRequests.emplace_back();
                runs.emplace_back(runStart, runCount);
                req.shader = caster.shader;
                req.uniforms.material = &caster.pass;
                req.uniforms.properties = &m_props;
                req.uniforms.inputs = m_inputs;
                req.uniforms.model = c.model;
                req.uniforms.viewportViewProjections = &m_shadowMatrices[l];
                if (mesh.skinning) {
                    req.uniforms.bones = &m_meshBones[c.mesh];
                }
                for (const SamplerInfo &sampler : caster.shader->samplers) {
                    // (an alpha-tested caster reads its material's picture)
                    const std::string name = sampler.binding < caster.pass.textures.size() ? caster.pass.textures[sampler.binding] : sampler.defaultText;
                    if (sampler.binding >= kMaxTextureSlots || name.empty() || isRenderTarget(name)) {
                        continue;
                    }
                    if (const GpuTexture *t = textureNamed(name)) {
                        req.textures[sampler.binding] = fileTexture(*t, *m_runner);
                        req.uniforms.textures[sampler.binding] = req.textures[sampler.binding].info;
                    }
                }
                req.geometry = &m_shadowGeometry[geometry];
                req.blend = false;
                req.target = VK_NULL_HANDLE;
                req.targetFormat = VK_FORMAT_UNDEFINED;
                req.extent = atlas.extent;
                req.clear = false;
                req.depthTarget = atlas.view();
                req.depthFormat = atlas.format;
                req.depthClear = false;
                req.depthTest = true;
                req.depthWrite = true;
                req.cull = CullMode::None;
                req.depthBias = true;
                req.viewports = m_shadowViewports[l].data();
                req.viewportCount = static_cast<uint32_t>(m_shadowViewports[l].size());
                req.viewportMask = mask;
            }
        }
    }
    for (size_t r = 0; r < m_shadowRequests.size(); ++r) {
        if (runs[r].second > 0) {
            m_shadowRequests[r].ranges = m_shadowRanges.data() + runs[r].first;
            m_shadowRequests[r].rangeMasks = m_shadowRangeMasks.data() + runs[r].first;
            m_shadowRequests[r].rangeCount = static_cast<uint32_t>(runs[r].second);
        }
    }
    if (!m_shadowRequests.empty()) {
        m_runner->drawMany(cb, m_shadowRequests);
    }
    transition(cb, atlas, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_shadowSignature = signature;
    m_shadowDrawn = true;
    ++m_shadowPasses;
    if (std::any_of(m_layerReceives.begin(), m_layerReceives.end(), [](uint8_t r) { return r != 0; })) {
        ++m_staticEpoch; // a lit 2D layer kept in its buffers would show the old shadows
    }
    profileMark(cb, "shadows");
}

void SceneRenderer::bindShadowAtlas(DrawRequest &request) const
{
    if (!m_shadowAtlas || !m_shadowDrawn || !request.shader) {
        return;
    }
    for (const SamplerInfo &sampler : request.shader->samplers) {
        if (sampler.defaultText != "_rt_shadowAtlas" || sampler.type != "sampler2DShadow" || sampler.binding >= kMaxTextureSlots
            || request.textures[sampler.binding].view) {
            continue;
        }
        PassTexture t;
        t.view = m_shadowAtlas->view();
        t.sampler = m_runner->compareSampler(m_shadowLinear);
        t.info = {m_shadowAtlas->extent.width, m_shadowAtlas->extent.height, m_shadowAtlas->extent.width, m_shadowAtlas->extent.height};
        request.textures[sampler.binding] = t;
        request.uniforms.textures[sampler.binding] = t.info;
    }
}

} // namespace murasaki::scene
