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

// WE's bloom, with its own shaders: the scene is averaged down to a quarter of its size while everything below the
// threshold is dropped (`downsample_quarter_bloom`), blurred along x into an eighth (`downsample_eighth_blur_v`, whose
// name says the opposite of what it does) and along y (`blur_h_bloom`), and added to the scene (`combine`).
RenderTarget *SceneRenderer::drawBloom(VkCommandBuffer &cb, RenderTarget &source)
{
    const VkExtent2D full = m_scene_target->extent;
    RenderTarget &quarter = m_targets->get("bloom:4", scaledExtent(full, 4), m_colorFormat);
    RenderTarget &eighthA = m_targets->get("bloom:8a", scaledExtent(full, 8), m_colorFormat);
    RenderTarget &eighthB = m_targets->get("bloom:8b", scaledExtent(full, 8), m_colorFormat);
    RenderTarget &combined = m_targets->get("bloom:full", full, m_colorFormat);
    const auto constant = [&](float value, const std::string &user) {
        Constant c;
        c.value[0] = value;
        c.count = 1;
        c.user = user;
        return c;
    };
    bool ok = true;
    const auto pass = [&](const char *shader, RenderTarget &input, RenderTarget *second, RenderTarget &output) {
        MaterialPass material;
        material.shader = shader;
        material.blending = Blending::Normal;
        material.constants["bloomstrength"] = constant(m_scene.bloom.strength.value, m_scene.bloom.strength.user);
        material.constants["bloomthreshold"] = constant(m_scene.bloom.threshold.value, m_scene.bloom.threshold.user);
        Constant tint;
        tint.value = {m_scene.bloom.tint.x, m_scene.bloom.tint.y, m_scene.bloom.tint.z, 0.0f};
        tint.count = 3;
        material.constants["bloomtint"] = tint;
        std::string error;
        const TranslatedShader *translated = m_shaders->get(material, second ? std::vector<uint32_t>{0, 1} : std::vector<uint32_t>{0}, &error);
        if (!translated) {
            ok = false;
            return;
        }
        DrawRequest req;
        req.shader = translated;
        req.uniforms.material = &material;
        req.uniforms.properties = &m_props;
        req.uniforms.inputs = m_inputs;
        req.blend = false;
        req.clear = true;
        req.target = output.view();
        req.targetFormat = output.format;
        req.extent = output.extent;
        req.textures[0] = targetTexture(input, *m_runner);
        req.uniforms.textures[0] = req.textures[0].info;
        if (second) {
            req.textures[1] = targetTexture(*second, *m_runner);
            req.uniforms.textures[1] = req.textures[1].info;
        }
        transition(cb, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (second) {
            transition(cb, *second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        transition(cb, output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        m_runner->draw(cb, req);
        transition(cb, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        profileMark(cb, std::string("bloom ") + shader);
    };
    pass("downsample_quarter_bloom", source, nullptr, quarter);
    pass("downsample_eighth_blur_v", quarter, nullptr, eighthA);
    pass("blur_h_bloom", eighthA, nullptr, eighthB);
    pass("combine", source, &eighthB, combined);
    return ok ? &combined : &source; // a shader that is missing shows the scene without the glow
}

// WE's HDR bloom, with its own shaders as its `materials/util` files run them (formats.md, "HDR scenes"); every pass of the chain is
// the one shader `hdr_downsample` (the `hdr_upsample` material is that shader with UPSAMPLE and additive blending):
//   hdr_downsample_bloom  the scene into level 1 (half size), four taps, the soft threshold, strength and tint (BLOOM);
//   hdr_downsample        level i into level i + 1, each half the size of the one before, `iterations` levels in all;
//   hdr_upsample          from the smallest level up, level i + 1 added onto level i, times the scatter (UPSAMPLE, additive);
//   combine_hdr           the scene plus level 1, clamped into the 8 bit target that is shown (LINEAR: the output is plain 8 bit).
// g_RenderVar0 holds the four taps' offsets: half a texel of the picture read, either way.
RenderTarget *SceneRenderer::drawHdrBloom(VkCommandBuffer &cb, RenderTarget &source)
{
    const VkExtent2D full = m_scene_target->extent;
    const float threshold = std::max(resolve(m_scene.bloom.threshold, m_props), 0.0f);
    const float feather = std::max(resolve(m_scene.bloom.feather, m_props), 0.0f);
    const int wanted = static_cast<int>(std::lround(resolve(m_scene.bloom.iterations, m_props)));
    constexpr int kMaxLevels = 16;
    std::vector<RenderTarget *> levels; // levels[0] is level 1, half the scene's size
    {
        VkExtent2D e = full;
        for (int i = 0; i < std::clamp(wanted, 1, kMaxLevels); ++i) {
            if (i > 0 && e.width == 1 && e.height == 1) {
                break; // nothing smaller to make
            }
            e = scaledExtent(e, 2);
            levels.push_back(&m_targets->get("hdrbloom:" + std::to_string(i + 1), e, m_colorFormat));
        }
    }
    RenderTarget &combined = m_targets->get("hdrbloom:combined", full, kSdrFormat);
    // each level reaches level 1 multiplied by the scatter once per step up: level k by scatter^(k - 1). The square root of the sum
    // of those weights divides the strength (assumption, fitted on the previews: with the editor's 1.619 and 8 levels the plain sum
    // is 74, which turns every large area near the threshold white; dividing by the whole sum leaves almost no glow; formats.md)
    const float scatter = std::max(resolve(m_scene.bloom.scatter, m_props), 0.0f);
    float weights = 0.0f;
    for (size_t k = 0; k < levels.size(); ++k) {
        weights += std::pow(scatter, static_cast<float>(k));
    }
    const float strength = resolve(m_scene.bloom.strength, m_props) / std::sqrt(std::max(weights, 1e-6f));

    // the soft threshold (g_BloomBlendParams, the material key `blend`): brightness above x passes, and a knee of `feather` times
    // the threshold below it fades in quadratically (y: where the knee starts, z: its width, w: the quadratic's scale)
    const float knee = threshold * feather;
    const Vec4 blend{threshold, threshold - knee, 2.0f * knee, knee > 0.0f ? 0.25f / knee : 0.0f};
    const auto constant = [](std::initializer_list<float> values, const std::string &user = {}) {
        Constant c;
        int i = 0;
        for (float v : values) {
            c.value[static_cast<size_t>(i++)] = v;
        }
        c.count = i;
        c.user = user;
        return c;
    };
    bool ok = true;
    // one pass of the chain: `input` (and `second`) read, `output` written (cleared, or added onto)
    const auto pass = [&](const char *label, const char *shader, std::map<std::string, int> combos, RenderTarget &input, RenderTarget *second,
                          RenderTarget &output, bool additive) {
        if (!ok) {
            return;
        }
        MaterialPass material;
        material.shader = shader;
        material.combos = std::move(combos);
        material.blending = additive ? Blending::Additive : Blending::Normal;
        material.constants["bloomstrength"] = constant({strength});
        material.constants["bloomtint"] = constant({m_scene.bloom.tint.x, m_scene.bloom.tint.y, m_scene.bloom.tint.z});
        material.constants["blend"] = constant({blend.x, blend.y, blend.z, blend.w});
        material.constants["scatter"] = constant({scatter});
        std::string error;
        const TranslatedShader *translated = m_shaders->get(material, second ? std::vector<uint32_t>{0, 1} : std::vector<uint32_t>{0}, &error);
        if (!translated) {
            ok = false;
            return;
        }
        DrawRequest req;
        req.shader = translated;
        req.uniforms.material = &material;
        req.uniforms.properties = &m_props;
        req.uniforms.inputs = m_inputs;
        const float hx = 0.5f / static_cast<float>(input.extent.width);
        const float hy = 0.5f / static_cast<float>(input.extent.height);
        req.uniforms.renderVar0 = {hx, hy, -hx, -hy};
        req.blending = material.blending;
        req.blend = additive;
        req.clear = !additive;
        req.target = output.view();
        req.targetFormat = output.format;
        req.extent = output.extent;
        req.textures[0] = targetTexture(input, *m_runner);
        req.uniforms.textures[0] = req.textures[0].info;
        if (second) {
            req.textures[1] = targetTexture(*second, *m_runner);
            req.uniforms.textures[1] = req.textures[1].info;
        }
        transition(cb, input, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (second) {
            transition(cb, *second, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        transition(cb, output, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, !additive);
        m_runner->draw(cb, req);
        transition(cb, output, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        profileMark(cb, label);
    };
    pass("hdr bloom threshold", "hdr_downsample", {{"BLOOM", 1}}, source, nullptr, *levels[0], false);
    for (size_t i = 1; i < levels.size(); ++i) {
        pass("hdr bloom down", "hdr_downsample", {}, *levels[i - 1], nullptr, *levels[i], false);
    }
    for (size_t i = levels.size() - 1; i > 0; --i) {
        pass("hdr bloom up", "hdr_downsample", {{"UPSAMPLE", 1}}, *levels[i], nullptr, *levels[i - 1], true);
    }
    pass("hdr combine", "combine_hdr", {{"LINEAR", 1}}, source, levels[0], combined, false);
    return ok ? &combined : &source; // a shader that is missing shows the scene without the glow (the scaling pass clamps it)
}

void SceneRenderer::updateFog()
{
    std::map<std::string, int> combos;
    if (m_scene.hdr) {
        combos["HDR"] = 1; // what WE's shaders read as "the scene is HDR" (generic4's HDR brightness, the emissive overbright)
    }
    // each band that is on: its combo, its colour and (start, end - start, start density, end density - start density)
    const auto band = [&](const FogBand &fog, const char *combo, Vec3 &color, Vec4 &params) {
        if (!resolve(fog.enabled, m_props)) {
            return;
        }
        combos[combo] = 1;
        combos["FOG_COMPUTED"] = 1; // image layers fog per pixel too (genericimage4), not only meshes
        color = resolve(fog.color, m_props);
        const float start = resolve(fog.start, m_props);
        float range = resolve(fog.end, m_props) - start;
        if (std::fabs(range) < 1e-6f) {
            range = std::copysign(1e-6f, range); // a band of no length is a step, not a division by zero
        }
        // densities are fractions of the fog colour: kept in 0..1 (what WE does outside that range is not known; mix() would extrapolate)
        const float startDensity = std::clamp(resolve(fog.startDensity, m_props), 0.0f, 1.0f);
        const float endDensity = std::clamp(resolve(fog.endDensity, m_props), 0.0f, 1.0f);
        params = {start, range, startDensity, endDensity - startDensity};
    };
    band(m_scene.fog.distance, "FOG_DIST", m_inputs.fogDistanceColor, m_inputs.fogDistanceParams);
    band(m_scene.fog.height, "FOG_HEIGHT", m_inputs.fogHeightColor, m_inputs.fogHeightParams);
    m_shaders->setSceneCombos(std::move(combos));
}

RenderTarget &SceneRenderer::snapshotScene(VkCommandBuffer &cb, bool mips)
{
    if (mips) {
        if (m_mipCopy && m_mipCopyWrites == m_sceneWrites) {
            return *m_mipCopy; // nothing was drawn on the scene since the last copy
        }
        const VkExtent2D extent = m_scene_target->extent;
        uint32_t levels = 1;
        for (uint32_t side = std::max(extent.width, extent.height); side > 1; side >>= 1) {
            ++levels;
        }
        RenderTarget &chain = m_targets->get("full:mips", extent, m_colorFormat, false, levels);
        // each level is a linear blit of the one above (a box filter); a level is read by the next blit, then left for sampling
        const auto levelBarrier = [&](uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage,
                                      VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = srcStage;
            b.srcAccessMask = srcAccess;
            b.dstStageMask = dstStage;
            b.dstAccessMask = dstAccess;
            b.oldLayout = from;
            b.newLayout = to;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = chain.handle();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, count, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cb, &dep);
        };
        transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cb, chain, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {static_cast<int32_t>(extent.width), static_cast<int32_t>(extent.height), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = blit.srcOffsets[1];
        vkCmdBlitImage(cb, m_scene_target->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                       VK_FILTER_NEAREST);
        int32_t w = static_cast<int32_t>(extent.width), h = static_cast<int32_t>(extent.height);
        for (uint32_t level = 1; level < levels; ++level) {
            levelBarrier(level - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            const int32_t nw = std::max(1, w / 2), nh = std::max(1, h / 2);
            VkImageBlit down{};
            down.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, 1};
            down.srcOffsets[1] = {w, h, 1};
            down.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, 1};
            down.dstOffsets[1] = {nw, nh, 1};
            vkCmdBlitImage(cb, chain.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, chain.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &down,
                           VK_FILTER_LINEAR);
            w = nw;
            h = nh;
        }
        // levels 0 .. n-2 were read by a blit, the last was only written
        if (levels > 1) {
            levelBarrier(0, levels - 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                         VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        }
        levelBarrier(levels - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                     VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
        chain.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        profileMark(cb, "scene snapshot (mips)");
        m_mipCopy = &chain;
        m_mipCopyWrites = m_sceneWrites;
        return chain;
    }
    RenderTarget &full = m_targets->get("full", m_scene_target->extent, m_colorFormat);
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, full, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, true);
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {static_cast<int32_t>(m_scene_target->extent.width), static_cast<int32_t>(m_scene_target->extent.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = blit.srcOffsets[1];
    vkCmdBlitImage(cb, m_scene_target->handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, full.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                   VK_FILTER_NEAREST);
    transition(cb, full, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    transition(cb, *m_scene_target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    profileMark(cb, "scene snapshot");
    return full;
}

} // namespace murasaki::scene
