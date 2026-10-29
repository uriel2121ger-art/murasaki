#include "render/passes.h"

#include "core/vk.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace murasaki::scene {

namespace {

constexpr VkDeviceSize kRingBytes = kPassRingBytes;
constexpr uint32_t kBlockRange = kPassBlockWindow; // uniform block window of the dynamic descriptors; a block may not be larger
constexpr uint32_t kVertexStride = 5 * sizeof(float);
// The shadow casters' depth bias (render/shadows.h): a few steps of the depth format, and twice the depth's slope across a pixel.
constexpr float kShadowDepthBias = 2.0f;
constexpr float kShadowSlopeBias = 2.0f;

VkDeviceSize alignUp(VkDeviceSize v, VkDeviceSize a)
{
    return (v + a - 1) / a * a;
}

int blendIndex(const DrawRequest &r)
{
    if (!r.blend || r.blending == Blending::Normal) {
        return 0;
    }
    if (r.premultiplied) {
        return r.blending == Blending::Translucent ? 3 : 4;
    }
    return r.blending == Blending::Translucent ? 1 : 2;
}

} // namespace

PassRunner::PassRunner(const VkContext &ctx) : m_ctx(ctx)
{
    const VkDevice device = ctx.device();
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(ctx.physicalDevice(), &props);
    m_uniformAlignment = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
    if (props.limits.maxUniformBufferRange < kBlockRange) {
        throw VkException("the device's uniform buffer range is smaller than 16 KiB");
    }

    // set 0: vertex and fragment blocks
    VkDescriptorSetLayoutBinding ubo[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        ubo[i].binding = i;
        ubo[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        ubo[i].descriptorCount = 1;
        ubo[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo uci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    uci.bindingCount = 2;
    uci.pBindings = ubo;
    vkCheck(vkCreateDescriptorSetLayout(device, &uci, nullptr, &m_uniformLayout), "vkCreateDescriptorSetLayout");

    // set 1: g_Texture0 .. g_Texture8
    VkDescriptorSetLayoutBinding tex[kMaxTextureSlots]{};
    for (uint32_t i = 0; i < kMaxTextureSlots; ++i) {
        tex[i].binding = i;
        tex[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        tex[i].descriptorCount = 1;
        tex[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo tci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    tci.bindingCount = kMaxTextureSlots;
    tci.pBindings = tex;
    vkCheck(vkCreateDescriptorSetLayout(device, &tci, nullptr, &m_textureLayout), "vkCreateDescriptorSetLayout");

    const VkDescriptorSetLayout layouts[2] = {m_uniformLayout, m_textureLayout};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 2;
    plci.pSetLayouts = layouts;
    vkCheck(vkCreatePipelineLayout(device, &plci, nullptr, &m_layout), "vkCreatePipelineLayout");

    m_ring = ctx.createBuffer(kRingBytes, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);

    VkDescriptorPoolSize uboSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 2};
    VkDescriptorPoolCreateInfo upci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    upci.maxSets = 1;
    upci.poolSizeCount = 1;
    upci.pPoolSizes = &uboSize;
    vkCheck(vkCreateDescriptorPool(device, &upci, nullptr, &m_uniformPool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = m_uniformPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_uniformLayout;
    vkCheck(vkAllocateDescriptorSets(device, &ai, &m_uniformSet), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo bi{m_ring.handle(), 0, kBlockRange};
    VkWriteDescriptorSet writes[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = m_uniformSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[i].pBufferInfo = &bi;
    }
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    addTexturePool();

    m_linearClamp = sampler(false, true);

    // the fallback for slots nothing is bound to: one opaque white texel
    m_white = ctx.createImage(VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    GpuBuffer staging = ctx.createBuffer(4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memset(staging.mapped(), 0xFF, 4);
    staging.flush();
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_white.handle();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyBufferToImage(cb, staging.handle(), m_white.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
        b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cb, &dep);
    });

    // the comparison fallback: one depth texel at the far plane (D16_UNORM: every device samples it and renders into it)
    m_depthWhite = ctx.createImage(VK_FORMAT_D16_UNORM, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.dstStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.dstAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = m_depthWhite.handle();
        b.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &b;
        vkCmdPipelineBarrier2(cb, &dep);
        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        depth.imageView = m_depthWhite.view();
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
        info.renderArea = {{0, 0}, {1, 1}};
        info.layerCount = 1;
        info.pDepthAttachment = &depth;
        vkCmdBeginRendering(cb, &info);
        vkCmdEndRendering(cb);
        b.srcStageMask = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        b.srcAccessMask = VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier2(cb, &dep);
    });
}

PassRunner::~PassRunner()
{
    const VkDevice device = m_ctx.device();
    vkDeviceWaitIdle(device);
    for (auto &[key, pipeline] : m_pipelines) {
        vkDestroyPipeline(device, pipeline, nullptr);
    }
    for (auto &[shader, stages] : m_modules) {
        for (auto &[stage, module] : stages) {
            vkDestroyShaderModule(device, module, nullptr);
        }
    }
    for (auto &[key, s] : m_samplers) {
        vkDestroySampler(device, s, nullptr);
    }
    for (auto &[key, s] : m_compareSamplers) {
        vkDestroySampler(device, s, nullptr);
    }
    m_white.reset();
    m_depthWhite.reset();
    m_ring.reset();
    for (VkDescriptorPool pool : m_texturePools) {
        vkDestroyDescriptorPool(device, pool, nullptr);
    }
    if (m_uniformPool) {
        vkDestroyDescriptorPool(device, m_uniformPool, nullptr);
    }
    if (m_layout) {
        vkDestroyPipelineLayout(device, m_layout, nullptr);
    }
    if (m_textureLayout) {
        vkDestroyDescriptorSetLayout(device, m_textureLayout, nullptr);
    }
    if (m_uniformLayout) {
        vkDestroyDescriptorSetLayout(device, m_uniformLayout, nullptr);
    }
}

VkSampler PassRunner::sampler(bool nearest, bool clamp)
{
    const int key = (nearest ? 1 : 0) | (clamp ? 2 : 0);
    if (const auto it = m_samplers.find(key); it != m_samplers.end()) {
        return it->second;
    }
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = clamp ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    ci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler s = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(m_ctx.device(), &ci, nullptr, &s), "vkCreateSampler");
    m_samplers[key] = s;
    return s;
}

VkSampler PassRunner::compareSampler(bool linear)
{
    if (const auto it = m_compareSamplers.find(linear); it != m_compareSamplers.end()) {
        return it->second;
    }
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ci.magFilter = ci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    ci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    ci.addressModeU = ci.addressModeV = ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    ci.compareEnable = VK_TRUE;
    ci.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    ci.maxLod = 0.0f;
    VkSampler s = VK_NULL_HANDLE;
    vkCheck(vkCreateSampler(m_ctx.device(), &ci, nullptr, &s), "vkCreateSampler");
    m_compareSamplers[linear] = s;
    return s;
}

void PassRunner::beginFrame()
{
    m_ringOffset = 0;
    for (VkDescriptorPool pool : m_texturePools) {
        vkCheck(vkResetDescriptorPool(m_ctx.device(), pool, 0), "vkResetDescriptorPool");
    }
    m_activePool = 0;
}

namespace {

// The depth and cull state of a draw as one number (part of the pipeline key).
int depthState(const DrawRequest &r)
{
    const int cull = (r.cull == CullMode::Back ? 8 : 0) | (r.cull == CullMode::Front ? 64 : 0) | (r.mirrored ? 16 : 0);
    if (!r.depthTarget) {
        return cull; // (a draw without depth may still cull: the colour pass over a light's volume)
    }
    return 1 | (r.depthTest ? 2 : 0) | (r.depthWrite ? 4 : 0) | cull | (r.depthBias ? 32 : 0);
}

} // namespace

VkPipeline PassRunner::pipelineFor(const DrawRequest &r)
{
    const auto key = std::make_tuple(r.shader, blendIndex(r), r.blend, static_cast<int>(r.targetFormat), r.geometry ? r.geometry->attributes : nullptr,
                                     depthState(r), r.depthTarget ? static_cast<int>(r.depthFormat) : 0);
    if (const auto it = m_pipelines.find(key); it != m_pipelines.end()) {
        return it->second;
    }
    const VkDevice device = m_ctx.device();
    auto &modules = m_modules[r.shader];
    const auto module = [&](VkShaderStageFlagBits stage, const std::vector<uint32_t> &spirv) {
        if (const auto it = modules.find(stage); it != modules.end()) {
            return it->second;
        }
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * sizeof(uint32_t);
        ci.pCode = spirv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCheck(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
        modules[stage] = m;
        return m;
    };
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT,
                 module(VK_SHADER_STAGE_VERTEX_BIT, r.shader->vertex.spirv), "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT,
                 module(VK_SHADER_STAGE_FRAGMENT_BIT, r.shader->fragment.spirv), "main", nullptr};

    VkVertexInputBindingDescription binding{0, r.geometry ? r.geometry->stride : kVertexStride, VK_VERTEX_INPUT_RATE_VERTEX};
    std::vector<VkVertexInputAttributeDescription> attributes;
    if (r.geometry) {
        for (const AttributeInfo &a : r.shader->attributes) {
            for (uint32_t i = 0; i < r.geometry->attributeCount; ++i) {
                if (a.name == r.geometry->attributes[i].name) {
                    attributes.push_back({a.location, 0, r.geometry->attributes[i].format, r.geometry->attributes[i].offset});
                }
            }
        }
    } else {
        attributes = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0}, {1, 0, VK_FORMAT_R32G32_SFLOAT, 12}};
    }
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = r.geometry ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // (the translated vertex stage flips y, which keeps the picture as WE's: a face wound counter-clockwise on screen stays the front)
    raster.cullMode = r.cull == CullMode::Back ? VK_CULL_MODE_BACK_BIT : r.cull == CullMode::Front ? VK_CULL_MODE_FRONT_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = r.mirrored ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = r.depthTarget && r.depthTest ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = r.depthTarget && r.depthWrite ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    raster.lineWidth = 1.0f;
    if (r.depthTarget && r.depthBias) {
        // a shadow caster's depth, pushed away from the light so a lit surface does not shadow itself (the receivers compare their
        // own depth with what the casters wrote, filtered over a few texels)
        raster.depthBiasEnable = VK_TRUE;
        raster.depthBiasConstantFactor = kShadowDepthBias;
        raster.depthBiasSlopeFactor = kShadowSlopeBias;
    }
    const bool colour = r.targetFormat != VK_FORMAT_UNDEFINED;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    const int blend = blendIndex(r);
    if (blend != 0) {
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = blend >= 3 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_SRC_ALPHA;
        attachment.dstColorBlendFactor = blend == 2 || blend == 4 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.colorBlendOp = VK_BLEND_OP_ADD;
        attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        attachment.dstAlphaBlendFactor = blend == 2 || blend == 4 ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = colour ? 1 : 0;
    colorBlend.pAttachments = &attachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = colour ? 1 : 0;
    rendering.pColorAttachmentFormats = colour ? &r.targetFormat : nullptr;
    rendering.depthAttachmentFormat = r.depthTarget ? r.depthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pColorBlendState = &colorBlend;
    ci.pDepthStencilState = r.depthTarget ? &depth : nullptr;
    ci.pDynamicState = &dynamic;
    ci.layout = m_layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    m_pipelines[key] = pipeline;
    return pipeline;
}

void PassRunner::addTexturePool()
{
    VkDescriptorPoolSize texSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kPassTextureSets * kMaxTextureSlots};
    VkDescriptorPoolCreateInfo tpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    tpci.maxSets = kPassTextureSets;
    tpci.poolSizeCount = 1;
    tpci.pPoolSizes = &texSize;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    vkCheck(vkCreateDescriptorPool(m_ctx.device(), &tpci, nullptr, &pool), "vkCreateDescriptorPool");
    m_texturePools.push_back(pool);
}

VkDescriptorSet PassRunner::allocateTextureSet(const DrawRequest &r)
{
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_textureLayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    // the frame's pools fill in order; the next one is created the first time a frame needs it and kept (they are reset, not freed)
    for (;;) {
        ai.descriptorPool = m_texturePools[m_activePool];
        const VkResult result = vkAllocateDescriptorSets(m_ctx.device(), &ai, &set);
        if (result == VK_SUCCESS) {
            break;
        }
        if ((result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) || m_activePool + 1 >= kPassMaxTexturePools) {
            throw VkException("too many passes in one frame (descriptor pools exhausted)", result);
        }
        if (++m_activePool == m_texturePools.size()) {
            addTexturePool();
        }
    }
    VkDescriptorImageInfo images[kMaxTextureSlots];
    VkWriteDescriptorSet writes[kMaxTextureSlots]{};
    for (uint32_t i = 0; i < kMaxTextureSlots; ++i) {
        const PassTexture &t = r.textures[i];
        images[i] = {t.view ? t.sampler : m_linearClamp, t.view ? t.view : m_white.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        if (!t.view) {
            // a comparison sampler with nothing bound (a shadow atlas the draw does not give): the far plane, so it reads as lit
            for (const SamplerInfo &s : r.shader->samplers) {
                if (s.binding == i && s.type == "sampler2DShadow") {
                    images[i] = {compareSampler(false), m_depthWhite.view(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                }
            }
        }
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &images[i];
    }
    vkUpdateDescriptorSets(m_ctx.device(), kMaxTextureSlots, writes, 0, nullptr);
    return set;
}

void PassRunner::draw(VkCommandBuffer cb, const DrawRequest &r)
{
    drawMany(cb, std::span<const DrawRequest>(&r, 1));
}

void PassRunner::drawMany(VkCommandBuffer cb, std::span<const DrawRequest> requests)
{
    if (requests.empty()) {
        return;
    }
    // what each draw binds: its ring offsets and texture set, prepared before the rendering instance starts
    struct Prepared {
        VkDeviceSize vertexOffset = 0, vblockOffset = 0, fblockOffset = 0;
        VkDescriptorSet textureSet = VK_NULL_HANDLE;
    };
    std::vector<Prepared> prepared(requests.size());
    for (size_t k = 0; k < requests.size(); ++k) {
        const DrawRequest &r = requests[k];
        ++m_drawCount;
        for (const AttributeInfo &a : r.shader->attributes) {
            bool known = !r.geometry && (a.name == "a_Position" || a.name == "a_TexCoord");
            for (uint32_t i = 0; r.geometry && i < r.geometry->attributeCount; ++i) {
                known = known || a.name == r.geometry->attributes[i].name;
            }
            if (!known) {
                throw VkException("shader needs the vertex attribute " + a.name + ", which the geometry does not have");
            }
        }
        for (const SamplerInfo &s : r.shader->samplers) {
            if (s.binding >= kMaxTextureSlots) {
                throw VkException("shader uses texture slot " + std::to_string(s.binding) + ", the engine has " + std::to_string(kMaxTextureSlots));
            }
        }
        const uint32_t blocks[2] = {r.shader->vertex.blockSize, r.shader->fragment.blockSize};
        for (uint32_t b : blocks) {
            if (b > kBlockRange) {
                throw VkException("a uniform block of " + std::to_string(b) + " bytes is larger than the engine's 16 KiB window");
            }
        }

        // --- per-draw data in the ring: vertices, then the vertex block, then the fragment block ---
        Prepared &p = prepared[k];
        p.vertexOffset = alignUp(m_ringOffset, 16);
        const VkDeviceSize vertexBytes = r.geometry ? 0 : 4 * kVertexStride;
        p.vblockOffset = alignUp(p.vertexOffset + vertexBytes, m_uniformAlignment);
        p.fblockOffset = alignUp(p.vblockOffset + std::max<VkDeviceSize>(blocks[0], 16), m_uniformAlignment);
        const VkDeviceSize end = p.fblockOffset + std::max<VkDeviceSize>(blocks[1], 16);
        // dynamic descriptors expose kBlockRange bytes from their offset, so the window must fit in the buffer
        if (end + kBlockRange > kRingBytes) {
            throw VkException("the per-frame uniform ring is full (too many passes)");
        }
        auto *base = static_cast<uint8_t *>(m_ring.mapped());
        const Quad &q = r.quad;
        const float vertices[4][5] = {
            {q.minX, q.maxY, 0.0f, 0.0f, 0.0f},
            {q.maxX, q.maxY, 0.0f, q.uMax, 0.0f},
            {q.minX, q.minY, 0.0f, 0.0f, q.vMax},
            {q.maxX, q.minY, 0.0f, q.uMax, q.vMax},
        };
        if (!r.geometry) {
            std::memcpy(base + p.vertexOffset, vertices, sizeof(vertices));
        }
        fillUniforms(r.shader->vertex, r.uniforms, base + p.vblockOffset, &m_unknown);
        fillUniforms(r.shader->fragment, r.uniforms, base + p.fblockOffset, &m_unknown);
        m_ring.flush(p.vertexOffset, end - p.vertexOffset);
        m_ringOffset = end;
        p.textureSet = allocateTextureSet(r);
    }

    // one rendering instance for all of them, with the first request's attachments, area and load operations
    const DrawRequest &first = requests[0];
    VkRenderingAttachmentInfo attachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    attachment.imageView = first.target;
    // (no colour target: a depth-only pass, the shadow casters)
    attachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    attachment.loadOp = first.coversTarget && first.area.extent.width == 0 ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
        : first.clear                                                       ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                                            : VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    const VkRect2D area = first.area.extent.width > 0 ? first.area : VkRect2D{{0, 0}, first.extent};
    info.renderArea = area;
    info.layerCount = 1;
    info.colorAttachmentCount = first.target ? 1 : 0;
    info.pColorAttachments = first.target ? &attachment : nullptr;
    VkRenderingAttachmentInfo depthAttachment{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (first.depthTarget) {
        depthAttachment.imageView = first.depthTarget;
        depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthAttachment.loadOp = first.depthClear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depthAttachment.clearValue.depthStencil = {1.0f, 0};
        info.pDepthAttachment = &depthAttachment;
    }
    vkCmdBeginRendering(cb, &info);
    const VkViewport viewport{0.0f, 0.0f, static_cast<float>(first.extent.width), static_cast<float>(first.extent.height), 0.0f, 1.0f};
    vkCmdSetViewport(cb, 0, 1, &viewport);
    vkCmdSetScissor(cb, 0, 1, &area);
    for (size_t k = 0; k < requests.size(); ++k) {
        const DrawRequest &r = requests[k];
        const Prepared &p = prepared[k];
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineFor(r));
        const uint32_t dynamicOffsets[2] = {static_cast<uint32_t>(p.vblockOffset), static_cast<uint32_t>(p.fblockOffset)};
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 0, 1, &m_uniformSet, 2, dynamicOffsets);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m_layout, 1, 1, &p.textureSet, 0, nullptr);
        if (r.geometry && r.viewportCount > 0) {
            // one draw per picture: its viewport and scissor, and the instance index naming it to the shader
            vkCmdBindVertexBuffers(cb, 0, 1, &r.geometry->vertexBuffer, &r.geometry->vertexOffset);
            vkCmdBindIndexBuffer(cb, r.geometry->indexBuffer, r.geometry->indexOffset, r.geometry->indexType);
            for (uint32_t v = 0; v < r.viewportCount; ++v) {
                if (v < 32 && ((r.viewportMask >> v) & 1u) == 0) {
                    continue;
                }
                const VkViewport &port = r.viewports[v];
                const int32_t x0 = std::max(0, static_cast<int32_t>(std::floor(port.x)));
                const int32_t y0 = std::max(0, static_cast<int32_t>(std::floor(port.y)));
                const int32_t x1 = std::min(static_cast<int32_t>(first.extent.width), static_cast<int32_t>(std::ceil(port.x + port.width)));
                const int32_t y1 = std::min(static_cast<int32_t>(first.extent.height), static_cast<int32_t>(std::ceil(port.y + port.height)));
                if (x1 <= x0 || y1 <= y0) {
                    continue;
                }
                const VkRect2D scissor{{x0, y0}, {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0)}};
                vkCmdSetViewport(cb, 0, 1, &port);
                vkCmdSetScissor(cb, 0, 1, &scissor);
                if (!r.ranges) {
                    vkCmdDrawIndexed(cb, r.geometry->indexCount, 1, 0, 0, v);
                    continue;
                }
                for (uint32_t i = 0; i < r.rangeCount;) {
                    if (((r.rangeMasks[i] >> v) & 1u) == 0) {
                        ++i;
                        continue;
                    }
                    // the run of ranges into this viewport that follow each other in the index buffer
                    const uint32_t first = r.ranges[i].first;
                    uint32_t end = first + r.ranges[i].count;
                    for (++i; i < r.rangeCount && ((r.rangeMasks[i] >> v) & 1u) != 0 && r.ranges[i].first == end; ++i) {
                        end += r.ranges[i].count;
                    }
                    vkCmdDrawIndexed(cb, end - first, 1, first, 0, v);
                }
            }
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &area);
        } else if (r.geometry) {
            vkCmdBindVertexBuffers(cb, 0, 1, &r.geometry->vertexBuffer, &r.geometry->vertexOffset);
            vkCmdBindIndexBuffer(cb, r.geometry->indexBuffer, r.geometry->indexOffset, r.geometry->indexType);
            vkCmdDrawIndexed(cb, r.geometry->indexCount, 1, 0, 0, 0);
        } else {
            const VkBuffer buffer = m_ring.handle();
            vkCmdBindVertexBuffers(cb, 0, 1, &buffer, &p.vertexOffset);
            vkCmdDraw(cb, 4, 1, 0, 0);
        }
    }
    vkCmdEndRendering(cb);
}

} // namespace murasaki::scene
