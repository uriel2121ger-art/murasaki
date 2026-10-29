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

void SceneRenderer::drawMesh(VkCommandBuffer &cb, size_t index)
{
    if (index >= m_scene.meshes.size() || index >= m_meshes.size() || !m_meshes[index]) {
        return;
    }
    const MeshLayer &mesh = m_scene.meshes[index];
    if (!nodeVisible(m_scene, mesh.node, m_props)) {
        return;
    }
    const MeshGpu &gpu = *m_meshes[index];
    const Mat4 model = nodeWorldMatrix(m_scene, mesh.node);
    const Mat4 viewProjection = sceneProjection();
    // a mirroring transform turns the winding over: the front faces are then the clockwise ones
    const float det = model.at(0, 0) * (model.at(1, 1) * model.at(2, 2) - model.at(1, 2) * model.at(2, 1))
        - model.at(0, 1) * (model.at(1, 0) * model.at(2, 2) - model.at(1, 2) * model.at(2, 0))
        + model.at(0, 2) * (model.at(1, 0) * model.at(2, 1) - model.at(1, 1) * model.at(2, 0));
    RenderTarget &depth = m_targets->get("scene:depth", m_scene_target->extent, m_ctx.depthFormat(), true);
    // a skinned mesh has the puppet's vertex; while it plays its animation layers the shader skins it with this frame's bones
    const bool skinned = mesh.model && mesh.model->skinned;
    uint32_t attributeCount = 0;
    const GeometryAttribute *attributes = skinned ? puppetAttributes(attributeCount) : meshAttributes(attributeCount);
    if (mesh.skinning) {
        updateMeshBones(index);
    }
    const auto isScene = isSceneCopy;
    // another layer's composite (_rt_imageLayerComposite_<id>_a): that layer is rendered into its buffers now, before the mesh's
    // rendering instance, whatever its visibility and wherever it stands in the file (the library's sources are hidden layers that
    // come after the mesh); every part that names it reads the same buffer
    std::map<std::string, RenderTarget *> composites;
    const auto compositeOf = [&](const std::string &name) -> RenderTarget * {
        if (const auto have = composites.find(name); have != composites.end()) {
            return have->second;
        }
        RenderTarget *target = nullptr;
        if (const auto id = layerCompositeId(name)) {
            if (const auto layer = imageLayerOfObject(m_scene, *id)) {
                ++m_noSplit; // (a mesh's material is being set up: its state must stay in this command buffer)
                target = drawLayer(cb, m_scene.layers[*layer], *layer, true);
                --m_noSplit;
            }
        }
        composites.emplace(name, target);
        return target;
    };

    // what every part samples, decided before the rendering instance: a part that reads the scene drawn so far (REFLECTION's
    // _rt_MipMappedFrameBuffer) needs the copy made first, once for the whole mesh
    const size_t count = std::min(gpu.parts.size(), mesh.materials.size());
    std::vector<const TranslatedShader *> shaders(count, nullptr);
    std::vector<std::array<std::string, kMaxTextureSlots>> slots(count);
    RenderTarget *snapshot = nullptr;
    RenderTarget *noReflection = nullptr; // what `_rt_Reflection` holds: nothing is `reflected` in the library, so it is transparent black
    bool sceneRead = false, mipsRead = false; // the copy is taken once all the parts are looked at: with a chain when one reflects
    for (size_t p = 0; p < count; ++p) {
        const MaterialPass &pass = mesh.materials[p];
        std::string error;
        shaders[p] = m_shaders->get(pass, slotsOf(pass, nullptr), &error);
        if (!shaders[p]) {
            continue;
        }
        for (uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
            std::string name = slot < pass.textures.size() ? pass.textures[slot] : std::string();
            if (name.empty()) {
                // a slot the material leaves empty: the picture the shader names for it (a gradient, the scene so far)
                for (const SamplerInfo &sampler : shaders[p]->samplers) {
                    if (sampler.binding == slot) {
                        name = sampler.defaultText;
                    }
                }
            }
            if (isScene(name)) {
                sceneRead = true;
                mipsRead = mipsRead || readsMipInfo(*shaders[p], slot);
            }
            if (name == "_rt_Reflection" && !noReflection) {
                noReflection = &m_targets->get("reflection:none", {1, 1}, m_colorFormat);
                clearTarget(cb, *noReflection);
                transition(cb, *noReflection, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            }
            if (layerCompositeId(name)) {
                compositeOf(name);
            }
            slots[p][slot] = std::move(name);
        }
    }
    if (sceneRead) {
        snapshot = &snapshotScene(cb, mipsRead);
    }

    std::vector<Geometry> geometries(count);
    std::vector<DrawRequest> requests;
    requests.reserve(count);
    for (size_t p = 0; p < count; ++p) {
        if (!shaders[p]) {
            continue;
        }
        const MaterialPass &pass = mesh.materials[p];
        DrawRequest req;
        req.shader = shaders[p];
        req.uniforms.material = &pass;
        req.uniforms.properties = &m_props;
        req.uniforms.inputs = m_inputs;
        req.uniforms.mvp = viewProjection * model;
        req.uniforms.model = model;
        req.uniforms.viewProjection = viewProjection;
        if (mesh.skinning) {
            req.uniforms.bones = &m_meshBones[index];
        }
        for (uint32_t slot = 0; slot < kMaxTextureSlots; ++slot) {
            const std::string &name = slots[p][slot];
            if (name.empty()) {
                continue;
            }
            if (isScene(name) && snapshot) {
                req.textures[slot] = targetTexture(*snapshot, *m_runner);
            } else if (name == "_rt_Reflection" && noReflection) {
                req.textures[slot] = targetTexture(*noReflection, *m_runner);
            } else if (RenderTarget *composite = layerCompositeId(name) ? compositeOf(name) : nullptr) {
                req.textures[slot] = targetTexture(*composite, *m_runner);
            } else if (const GpuTexture *t = isRenderTarget(name) ? nullptr : textureNamed(name)) {
                req.textures[slot] = fileTexture(*t, *m_runner);
            } else {
                continue;
            }
            req.uniforms.textures[slot] = req.textures[slot].info;
        }
        bindShadowAtlas(req);
        const MeshGpu::Part &part = gpu.parts[p];
        Geometry &geometry = geometries[p];
        geometry.vertexBuffer = part.vertices.handle();
        geometry.stride = static_cast<uint32_t>(p < mesh.model->parts.size() ? mesh.model->parts[p].stride : kMeshVertexStride);
        geometry.indexBuffer = part.indices.handle();
        geometry.indexCount = part.indexCount;
        geometry.indexType = part.indexType;
        geometry.attributes = attributes;
        geometry.attributeCount = attributeCount;
        req.geometry = &geometry;
        req.blending = pass.blending;
        req.blend = true;
        ++m_sceneWrites;
        req.target = m_scene_target->view();
        req.targetFormat = m_colorFormat;
        req.extent = m_scene_target->extent;
        req.clear = false;
        req.depthTarget = depth.view();
        req.depthFormat = depth.format;
        req.depthClear = !m_depthCleared; // (only the first request's load operations count: the whole mesh is one rendering instance)
        req.depthTest = pass.depthTest;
        req.depthWrite = pass.depthWrite;
        req.cull = pass.cull;
        req.mirrored = det < 0.0f;
        requests.push_back(req);
    }
    if (requests.empty()) {
        return;
    }
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // (also orders this mesh's depth after the last one's)
    transition(cb, depth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, !m_depthCleared);
    m_runner->drawMany(cb, requests);
    m_depthCleared = true;
    if (m_profiler->detailed()) {
        profileMark(cb, "mesh " + std::to_string(index) + " (" + mesh.path + ")");
    }
}

namespace {

constexpr GeometryAttribute kVolumeAttributes[] = {{"a_Position", VK_FORMAT_R32G32B32_SFLOAT, 0}};
constexpr uint32_t kVolumeSegments = 32;
constexpr int kVolumeRings = 12;

// A closed triangle mesh wound counter-clockwise seen from outside: each triangle is turned if its normal points at `inside`.
struct VolumeData {
    std::vector<float> vertices;
    std::vector<uint16_t> indices;
    void triangle(uint16_t a, uint16_t b, uint16_t c, Vec3 inside)
    {
        const auto at = [&](uint16_t i) { return Vec3{vertices[i * 3], vertices[i * 3 + 1], vertices[i * 3 + 2]}; };
        const Vec3 p = at(a), q = at(b), r = at(c);
        const Vec3 u{q.x - p.x, q.y - p.y, q.z - p.z}, v{r.x - p.x, r.y - p.y, r.z - p.z};
        const Vec3 n{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
        const Vec3 away{(p.x + q.x + r.x) / 3 - inside.x, (p.y + q.y + r.y) / 3 - inside.y, (p.z + q.z + r.z) / 3 - inside.z};
        const bool outward = n.x * away.x + n.y * away.y + n.z * away.z >= 0.0f;
        indices.insert(indices.end(), outward ? std::initializer_list<uint16_t>{a, b, c} : std::initializer_list<uint16_t>{a, c, b});
    }
    uint16_t vertex(float x, float y, float z)
    {
        vertices.insert(vertices.end(), {x, y, z});
        return static_cast<uint16_t>(vertices.size() / 3 - 1);
    }
};

VolumeData unitSphere()
{
    constexpr float kPiF = 3.14159265358979f;
    VolumeData d;
    d.vertex(0, 0, 1);
    for (int ring = 1; ring < kVolumeRings; ++ring) {
        const float phi = kPiF * static_cast<float>(ring) / kVolumeRings;
        for (uint32_t i = 0; i < kVolumeSegments; ++i) {
            const float t = 2.0f * kPiF * static_cast<float>(i) / kVolumeSegments;
            d.vertex(std::sin(phi) * std::cos(t), std::sin(phi) * std::sin(t), std::cos(phi));
        }
    }
    d.vertex(0, 0, -1);
    const uint16_t south = static_cast<uint16_t>(d.vertices.size() / 3 - 1);
    const auto ringVertex = [&](int ring, uint32_t i) { return static_cast<uint16_t>(1 + (ring - 1) * kVolumeSegments + i % kVolumeSegments); };
    for (uint32_t i = 0; i < kVolumeSegments; ++i) {
        d.triangle(0, ringVertex(1, i), ringVertex(1, i + 1), {0, 0, 0});
        d.triangle(south, ringVertex(kVolumeRings - 1, i + 1), ringVertex(kVolumeRings - 1, i), {0, 0, 0});
        for (int ring = 1; ring + 1 < kVolumeRings; ++ring) {
            d.triangle(ringVertex(ring, i), ringVertex(ring + 1, i), ringVertex(ring + 1, i + 1), {0, 0, 0});
            d.triangle(ringVertex(ring, i), ringVertex(ring + 1, i + 1), ringVertex(ring, i + 1), {0, 0, 0});
        }
    }
    return d;
}

// The apex at the origin, the base circle (radius 1) at z = 1.
VolumeData unitCone()
{
    constexpr float kPiF = 3.14159265358979f;
    VolumeData d;
    const uint16_t apex = d.vertex(0, 0, 0);
    const uint16_t centre = d.vertex(0, 0, 1);
    for (uint32_t i = 0; i < kVolumeSegments; ++i) {
        const float t = 2.0f * kPiF * static_cast<float>(i) / kVolumeSegments;
        d.vertex(std::cos(t), std::sin(t), 1.0f);
    }
    for (uint32_t i = 0; i < kVolumeSegments; ++i) {
        const uint16_t a = static_cast<uint16_t>(2 + i), b = static_cast<uint16_t>(2 + (i + 1) % kVolumeSegments);
        d.triangle(apex, a, b, {0, 0, 0.75f});
        d.triangle(centre, a, b, {0, 0, 0.75f});
    }
    return d;
}

} // namespace

// The beams, after every layer, particle and mesh (an assumption: formats.md, "Volumetric lights"). Per light, as WE's materials
// (materials/util/volumetrics_*.json) run them: the back faces of the light's volume into a depth image (volumetricsback), then its
// front faces, additive, into a half size light buffer, where volumetricsfront marches from the front face to the back face or the
// scene's depth, whichever is nearer. Then the buffer is blurred (blur_k3, horizontal and vertical) and added to the scene. Returns the
// target that holds the scene with its beams: the scene target, or, when the scene is not cleared each frame (what it keeps would
// collect the beams of every frame), a copy of it.
// What a frame builds is kept: the lights are worked out once a frame (recordScenePass), the shaders and requests of each light until
// something that changes them does (the properties: fog, the render scale).
RenderTarget *SceneRenderer::drawVolumetrics(VkCommandBuffer &cb)
{
    const std::vector<VolumetricLight> &lights = m_volumetricLights;
    if (m_volumetricsFailed || lights.empty()) {
        return m_scene_target;
    }
    const auto fail = [&](const std::string &why) {
        logError("volumetric lights: {} (no beam is drawn)", why);
        m_volumetricsFailed = true;
        return m_scene_target;
    };
    std::string error;
    const auto prepareMesh = [&](VolumeMesh &mesh, const VolumeData &data) {
        const VkDeviceSize vertexBytes = data.vertices.size() * sizeof(float);
        mesh.buffer = m_ctx.createBuffer(vertexBytes + data.indices.size() * sizeof(uint16_t), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
        std::memcpy(mesh.buffer.mapped(), data.vertices.data(), vertexBytes);
        std::memcpy(static_cast<uint8_t *>(mesh.buffer.mapped()) + vertexBytes, data.indices.data(), data.indices.size() * sizeof(uint16_t));
        mesh.buffer.flush();
        mesh.indexOffset = vertexBytes;
        mesh.indexCount = static_cast<uint32_t>(data.indices.size());
    };
    if (m_volumeSphere.indexCount == 0) {
        prepareMesh(m_volumeSphere, unitSphere());
        prepareMesh(m_volumeCone, unitCone());
        for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D16_UNORM}) {
            if (depthSampleable(m_ctx, f)) {
                m_volumeDepthFormat = f;
                break;
            }
        }
    }
    if (m_volumeDepthFormat == VK_FORMAT_UNDEFINED) {
        return fail("the device samples no depth format");
    }

    // the shaders: for the properties (fog combos) and the render scale as they are
    const uint64_t shadersFor = mix(m_props.revision(), static_cast<uint64_t>(m_renderScale * 1000.0f));
    if (m_volumeShadersFor != shadersFor || !m_volumeBack.shader) {
        m_volumeShadersFor = shadersFor;
        m_volumeBack.pass = MaterialPass{};
        m_volumeBack.pass.shader = "volumetricsback";
        m_volumeBack.shader = m_shaders->get(m_volumeBack.pass, {}, &error);
        const struct {
            PostPass &post;
            const char *shader;
            int vertical;
            Blending blending;
        } posts[] = {{m_volumeBlurH, "blur_k3", 0, Blending::Normal}, {m_volumeBlurV, "blur_k3", 1, Blending::Normal}, {m_volumeCombine, "passthrough", -1, Blending::Additive}};
        for (const auto &post : posts) {
            post.post.pass = MaterialPass{};
            post.post.pass.shader = post.shader;
            post.post.pass.blending = post.blending;
            if (post.vertical >= 0) {
                post.post.pass.combos["VERTICAL"] = post.vertical;
            }
            post.post.shader = m_volumeBack.shader ? m_shaders->get(post.post.pass, {0}, &error) : nullptr;
        }
        if (!m_volumeBack.shader || !m_volumeBlurH.shader || !m_volumeBlurV.shader || !m_volumeCombine.shader) {
            return fail(error);
        }
        m_volumeDraws.clear();
    }
    m_volumeDraws.resize(lights.size());

    const bool shadowsReady = m_shadowAtlas && m_shadowDrawn;
    const Mat4 viewProjection = sceneProjection();
    const Mat4 effectModel = inverse(viewProjection);
    const VkExtent2D full = m_scene_target->extent;
    const VkExtent2D half = scaledExtent(full, 2);
    RenderTarget &backDepth = m_targets->get("volumetrics:back", half, m_volumeDepthFormat);
    RenderTarget &lightBuffer = m_targets->get("volumetrics:light", half, m_colorFormat);
    RenderTarget &lightBufferB = m_targets->get("volumetrics:lightB", half, m_colorFormat);

    // the depth the beams stop at: the scene's own where meshes drew it, else the far plane
    PassTexture limit;
    limit.sampler = m_runner->sampler(true, true);
    limit.view = m_runner->farDepthView();
    limit.info = {1, 1, 1, 1};
    if (m_depthCleared) {
        RenderTarget &sceneDepth = m_targets->get("scene:depth", full, m_ctx.depthFormat(), true);
        if (depthSampleable(m_ctx, sceneDepth.format)) {
            transition(cb, sceneDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            limit.view = sceneDepth.view();
            limit.info = {full.width, full.height, full.width, full.height};
        } else if (!m_warnedVolumeDepth) {
            m_warnedVolumeDepth = true;
            logInfo("volumetric lights: the scene's depth format cannot be sampled, so meshes do not stop the beams");
        }
    }
    PassTexture back;
    back.sampler = m_runner->sampler(true, true);
    back.view = backDepth.view();
    back.info = {half.width, half.height, half.width, half.height};

    // where the beams are added
    RenderTarget *target = m_scene_target;
    if (!resolve(m_scene.clearEnabled, m_props)) {
        RenderTarget &copy = m_targets->get("volumetrics:scene", full, m_colorFormat);
        transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cb, copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
        VkImageCopy region{};
        region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {full.width, full.height, 1};
        vkCmdCopyImage(cb, m_scene_target->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, copy.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        target = &copy;
    }
    transition(cb, *target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    static const std::string kAltViewProjection = "g_AltViewProjectionMatrix", kEffectModel = "g_EffectModelMatrix", kAltModel = "g_AltModelMatrix",
                             kVar0 = "g_RenderVar0", kVar1 = "g_RenderVar1", kVar2 = "g_RenderVar2", kVar3 = "g_RenderVar3", kVar4 = "g_RenderVar4";
    bool first = true;
    for (size_t index = 0; index < lights.size(); ++index) {
        const VolumetricLight &light = lights[index];
        VolumeDraw &draw = m_volumeDraws[index];
        const bool shadowed = light.shadowSlot >= 0 && shadowsReady;
        if (!draw.front.shader || draw.shadowed != shadowed || draw.point != (light.type == LightType::Point)) {
            draw.shadowed = shadowed;
            draw.point = light.type == LightType::Point;
            draw.front.pass = MaterialPass{};
            draw.front.pass.shader = "volumetricsfront";
            draw.front.pass.blending = Blending::Additive;
            draw.front.pass.combos["QUALITY"] = 3;
            draw.front.pass.combos["POINTLIGHT"] = draw.point ? 1 : 0;
            draw.front.pass.combos["SHADOW"] = shadowed ? 1 : 0;
            draw.front.shader = m_shaders->get(draw.front.pass, shadowed ? std::vector<uint32_t>{0, 1, 3} : std::vector<uint32_t>{1, 3}, &error);
            if (!draw.front.shader) {
                return fail(error);
            }
        }
        const VolumeMesh &mesh = light.sphere ? m_volumeSphere : m_volumeCone;
        Geometry &geometry = draw.geometry;
        geometry.vertexBuffer = mesh.buffer.handle();
        geometry.stride = 3 * sizeof(float);
        geometry.indexBuffer = mesh.buffer.handle();
        geometry.indexOffset = mesh.indexOffset;
        geometry.indexCount = mesh.indexCount;
        geometry.indexType = VK_INDEX_TYPE_UINT16;
        geometry.attributes = kVolumeAttributes;
        geometry.attributeCount = 1;

        const auto refresh = [&](DrawRequest &req, const PassTexture &, const TranslatedShader *shader, const MaterialPass &pass) {
            req.shader = shader;
            req.uniforms.material = &pass;
            req.uniforms.properties = &m_props;
            req.uniforms.inputs = m_inputs;
            req.uniforms.viewProjection = viewProjection;
            req.uniforms.matrixOverrides[kAltViewProjection] = light.volume;
            req.geometry = &geometry;
            req.extent = half;
        };
        // the back faces' depth (nothing is written to colour)
        DrawRequest &backReq = draw.back;
        refresh(backReq, back, m_volumeBack.shader, m_volumeBack.pass);
        backReq.blend = false;
        backReq.target = VK_NULL_HANDLE;
        backReq.targetFormat = VK_FORMAT_UNDEFINED;
        backReq.depthTarget = backDepth.view();
        backReq.depthFormat = backDepth.format;
        backReq.depthClear = true;
        backReq.depthTest = true;
        backReq.depthWrite = true;
        backReq.cull = CullMode::Front;
        transition(cb, backDepth, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, true);
        m_runner->draw(cb, backReq);
        transition(cb, backDepth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        // the front faces: the march, added to the light buffer
        DrawRequest &req = draw.frontRequest;
        refresh(req, back, draw.front.shader, draw.front.pass);
        req.blending = Blending::Additive;
        req.blend = true;
        req.target = lightBuffer.view();
        req.targetFormat = lightBuffer.format;
        req.clear = first;
        req.cull = CullMode::Back;
        req.textures[0] = {};
        req.textures[1] = back;
        req.textures[3] = limit;
        req.uniforms.textures[0] = {};
        req.uniforms.textures[1] = back.info;
        req.uniforms.textures[3] = limit.info;
        req.uniforms.matrixOverrides[kEffectModel] = effectModel;
        if (shadowed) {
            req.uniforms.matrixOverrides[kAltModel] = light.type == LightType::Spot ? m_shadowUniforms.spotProjection[light.shadowSlot] : Mat4::identity();
            bindShadowAtlas(req);
        }
        const Vec4 transform = !shadowed ? Vec4{0, 0, 1, 1}
            : light.type == LightType::Spot ? m_shadowUniforms.spotTransform[light.shadowSlot]
                                            : m_shadowUniforms.pointTransform[light.shadowSlot];
        const Vec4 pointProjection = shadowed && light.type == LightType::Point ? m_shadowUniforms.pointProjection[light.shadowSlot] : Vec4{0, 0, 0, 0};
        req.uniforms.overrides[kVar0] = {transform.x, transform.y, transform.z, transform.w};
        req.uniforms.overrides[kVar1] = {light.radius, light.cosInner, light.cosOuter, light.intensity};
        req.uniforms.overrides[kVar2] = {light.origin.x, light.origin.y, light.origin.z, light.density};
        req.uniforms.overrides[kVar3] = light.type == LightType::Point ? std::array<float, 4>{pointProjection.x, pointProjection.y, pointProjection.z, pointProjection.w}
                                                                        : std::array<float, 4>{light.forward.x, light.forward.y, light.forward.z, 0.0f};
        req.uniforms.overrides[kVar4] = {light.color.x, light.color.y, light.color.z, light.exponent};
        transition(cb, lightBuffer, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, first);
        m_runner->draw(cb, req);
        first = false;
        if (m_profiler->detailed()) {
            profileMark(cb, "volumetric light");
        }
    }

    // the buffer blurred, horizontal then vertical (volumetrics_blur_h / _v), and added to the scene (volumetrics_combine)
    const auto screenPass = [&](const PostPass &post, RenderTarget &input, RenderTarget &output, bool onScene) {
        DrawRequest req;
        req.shader = post.shader;
        req.uniforms.material = &post.pass;
        req.uniforms.properties = &m_props;
        req.uniforms.inputs = m_inputs;
        req.blending = post.pass.blending;
        req.blend = onScene;
        req.clear = !onScene;
        req.target = output.view();
        req.targetFormat = output.format;
        req.extent = output.extent;
        req.textures[0] = targetTexture(input, *m_runner);
        req.uniforms.textures[0] = req.textures[0].info;
        transition(cb, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (!onScene) {
            transition(cb, output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        }
        m_runner->draw(cb, req);
        if (!onScene) {
            transition(cb, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
    };
    screenPass(m_volumeBlurH, lightBuffer, lightBufferB, false);
    screenPass(m_volumeBlurV, lightBufferB, lightBuffer, false);
    screenPass(m_volumeCombine, lightBuffer, *target, true);
    profileMark(cb, "volumetrics");
    transition(cb, *target, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return target;
}

void SceneRenderer::drawParticles(VkCommandBuffer &cb, size_t index)
{
    if (!m_particles) {
        return;
    }
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    ParticleRenderer::Frame frame;
    frame.cb = cb;
    frame.viewProjection = sceneProjection();
    frame.parallax = m_scene.perspective ? Vec2{0.0f, 0.0f} : m_parallax; // perspective: the eye moves instead (updateView)
    frame.perspectiveScene = m_scene.perspective;
    frame.orthoSize = m_scene.orthoSize;
    frame.fov = m_cameraFov;
    frame.nearZ = m_scene.camera.nearZ;
    frame.farZ = m_scene.camera.farZ;
    ++m_sceneWrites;
    frame.target = m_scene_target->view();
    frame.targetFormat = m_colorFormat;
    frame.extent = m_scene_target->extent;
    frame.inputs = m_inputs;
    frame.snapshot = [this, &cb]() -> RenderTarget & { return snapshotScene(cb); };
    if (m_halfSizeParticles) {
        frame.scratch = [this](VkExtent2D extent) -> RenderTarget & { return m_targets->get("particles:half", extent, m_colorFormat); };
    }
    const size_t quads = m_particles->draw(frame, index);
    if (m_profiler->detailed()) {
        profileMark(cb, "particles " + std::to_string(index) + " (" + std::to_string(quads) + " quads)");
    }
}

} // namespace murasaki::scene
