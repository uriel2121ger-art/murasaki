#include "render/targets.h"

namespace murasaki::scene {

bool depthSampleable(const VkContext &ctx, VkFormat format)
{
    if (format != VK_FORMAT_D32_SFLOAT && format != VK_FORMAT_X8_D24_UNORM_PACK32 && format != VK_FORMAT_D16_UNORM) {
        return false;
    }
    VkFormatProperties fp{};
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), format, &fp);
    const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT;
    return (fp.optimalTilingFeatures & need) == need;
}

RenderTarget &TargetPool::get(const std::string &key, VkExtent2D extent, VkFormat format, bool keep, uint32_t mipLevels)
{
    auto it = m_targets.find(key);
    if (it != m_targets.end()) {
        RenderTarget &have = *it->second.target;
        if (have.extent.width == extent.width && have.extent.height == extent.height && have.format == format && have.mipLevels == mipLevels) {
            it->second.lastUsed = m_frame;
            it->second.keep = it->second.keep || keep;
            return have;
        }
        m_retired.emplace_back(m_frame, std::move(it->second.target)); // a recorded command may still name it
    }
    auto target = std::make_unique<RenderTarget>();
    target->extent = extent;
    target->format = format;
    target->mipLevels = mipLevels;
    // a depth target is an attachment (the scene's depth buffer, cleared by the first mesh drawn in a frame), and is sampled where the
    // format has no stencil and the device samples it (the volumetric lights read the depth of the scene and of their volumes)
    VkImageUsageFlags usage = isDepthFormat(format) ? VkImageUsageFlags(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
                                                    : VkImageUsageFlags(VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT
                                                                        | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (depthSampleable(m_ctx, format)) {
        usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    }
    target->image = m_ctx.createImage(format, extent.width, extent.height, mipLevels, usage);
    RenderTarget &ref = *target;
    m_targets[key] = Entry{std::move(target), m_frame, keep};
    return ref;
}

void TargetPool::beginFrame()
{
    ++m_frame;
    std::erase_if(m_retired, [&](const auto &r) { return m_frame - r.first >= kRetireFrames; });
    std::erase_if(m_targets, [&](const auto &e) { return !e.second.keep && m_frame - e.second.lastUsed > kIdleFrames; });
}

void transition(VkCommandBuffer cb, RenderTarget &target, VkImageLayout layout, bool discard)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : target.layout;
    b.newLayout = layout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = target.handle();
    b.subresourceRange = {aspectOf(target.format), 0, target.mipLevels, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &dep);
    target.layout = layout;
}

} // namespace murasaki::scene
