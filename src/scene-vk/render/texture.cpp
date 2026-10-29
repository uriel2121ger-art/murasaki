#include "render/texture.h"

#include <algorithm>
#include <bit>
#include <cstring>

namespace murasaki::scene {

VkFormat vulkanFormat(PixelFormat format)
{
    switch (format) {
    case PixelFormat::Rgba8: return VK_FORMAT_R8G8B8A8_UNORM;
    case PixelFormat::R8: return VK_FORMAT_R8_UNORM;
    case PixelFormat::Rg8: return VK_FORMAT_R8G8_UNORM;
    case PixelFormat::R16f: return VK_FORMAT_R16_SFLOAT;
    case PixelFormat::Rg16f: return VK_FORMAT_R16G16_SFLOAT;
    case PixelFormat::Rgba16f: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case PixelFormat::Bc1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case PixelFormat::Bc2: return VK_FORMAT_BC2_UNORM_BLOCK;
    case PixelFormat::Bc3: return VK_FORMAT_BC3_UNORM_BLOCK;
    case PixelFormat::Bc7: return VK_FORMAT_BC7_UNORM_BLOCK;
    case PixelFormat::Rgb10A2: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    }
    return VK_FORMAT_UNDEFINED;
}

namespace {

bool compressed(PixelFormat f)
{
    return f == PixelFormat::Bc1 || f == PixelFormat::Bc2 || f == PixelFormat::Bc3 || f == PixelFormat::Bc7;
}

// One and two channel textures keep the channels they have: a sample of R8 is (r, 0, 0, 1) and of RG88 (r, g, 0, 1), as
// in Wallpaper Engine, whose shaders rebuild what they mean from the TEX<n>FORMAT combos (ConvertTexture0Format).
VkComponentMapping swizzleFor(PixelFormat)
{
    return {};
}

void barrier(VkCommandBuffer cb, VkImage image, uint32_t baseMip, uint32_t mips, VkImageLayout from, VkImageLayout to,
             VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

} // namespace

namespace {

// One image of a texture with the mips it carries (or a generated chain when it carries one), left sampled-ready.
GpuImage uploadImage(const VkContext &ctx, const std::vector<TexMip> &mips, PixelFormat pixelFormat, std::string &error)
{
    if (mips.empty() || mips[0].data.empty()) {
        error = "texture has no pixels";
        return {};
    }
    const VkFormat format = vulkanFormat(pixelFormat);
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), format, &props);
    constexpr VkFormatFeatureFlags kSample = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((props.optimalTilingFeatures & kSample) != kSample) {
        error = "the device cannot sample this texture format";
        return {};
    }

    // The mip chain the file carries, as far as it halves cleanly; Vulkan has no notion of any other.
    const TexMip &base = mips[0];
    uint32_t levels = 1;
    while (levels < mips.size()) {
        const TexMip &m = mips[levels];
        if (m.width != std::max(1u, base.width >> levels) || m.height != std::max(1u, base.height >> levels) || m.data.empty()) {
            break;
        }
        ++levels;
    }
    const bool generate = levels == 1 && !compressed(pixelFormat) && base.width > 1 && base.height > 1
        && (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (props.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    const uint32_t total = generate ? static_cast<uint32_t>(std::bit_width(std::max(base.width, base.height))) : levels;

    VkDeviceSize staged = 0;
    std::vector<VkDeviceSize> offsets;
    for (uint32_t i = 0; i < levels; ++i) {
        offsets.push_back(staged);
        staged += mips[i].data.size();
        staged = (staged + 15) & ~VkDeviceSize{15};
    }
    GpuBuffer staging = ctx.createBuffer(staged, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    for (uint32_t i = 0; i < levels; ++i) {
        std::memcpy(static_cast<uint8_t *>(staging.mapped()) + offsets[i], mips[i].data.data(), mips[i].data.size());
    }
    staging.flush();

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (generate) {
        usage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    }
    GpuImage result = ctx.createImage(format, base.width, base.height, total, usage, swizzleFor(pixelFormat));
    const VkImage image = result.handle();
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        barrier(cb, image, 0, total, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        for (uint32_t i = 0; i < levels; ++i) {
            VkBufferImageCopy region{};
            region.bufferOffset = offsets[i];
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
            region.imageExtent = {mips[i].width, mips[i].height, 1};
            vkCmdCopyBufferToImage(cb, staging.handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
        if (generate) {
            int32_t w = static_cast<int32_t>(base.width);
            int32_t h = static_cast<int32_t>(base.height);
            for (uint32_t i = 1; i < total; ++i) {
                barrier(cb, image, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                        VK_ACCESS_2_TRANSFER_READ_BIT);
                const int32_t nw = std::max(1, w / 2);
                const int32_t nh = std::max(1, h / 2);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
                blit.srcOffsets[1] = {w, h, 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
                blit.dstOffsets[1] = {nw, nh, 1};
                vkCmdBlitImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit,
                               VK_FILTER_LINEAR);
                barrier(cb, image, i - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                        VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                w = nw;
                h = nh;
            }
            barrier(cb, image, total - 1, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        } else {
            barrier(cb, image, 0, total, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
    });

    return result;
}

} // namespace

std::unique_ptr<GpuTexture> GpuTexture::create(const VkContext &ctx, const Tex &tex, std::string &error)
{
    GpuImage first = uploadImage(ctx, tex.mips, tex.format, error);
    if (!first) {
        return nullptr;
    }
    const TexMip &base = tex.mips[0];
    auto result = std::unique_ptr<GpuTexture>(new GpuTexture());
    result->m_image = std::move(first);
    result->m_rawFormat = tex.rawFormat;
    // the other images of a sprite sheet, each with its own size
    for (const TexPage &page : tex.pages) {
        GpuImage image = uploadImage(ctx, page.mips, tex.format, error);
        if (!image) {
            return nullptr;
        }
        result->m_pages.push_back(std::move(image));
    }
    result->m_droppedPages = tex.droppedPages;
    result->m_frames = tex.frames;
    result->m_imageWidth = tex.imageWidth > 0 ? tex.imageWidth : base.width;
    result->m_imageHeight = tex.imageHeight > 0 ? tex.imageHeight : base.height;
    result->m_uMax = tex.imageWidth > 0 ? std::min(1.0f, static_cast<float>(tex.imageWidth) / static_cast<float>(base.width)) : 1.0f;
    result->m_vMax = tex.imageHeight > 0 ? std::min(1.0f, static_cast<float>(tex.imageHeight) / static_cast<float>(base.height)) : 1.0f;
    result->m_nearest = tex.nearest();
    result->m_clamp = tex.clampUVs();
    return result;
}

std::unique_ptr<GpuTexture> GpuTexture::createDynamic(const VkContext &ctx, uint32_t width, uint32_t height, bool nearest, bool clamp, std::string &error)
{
    if (width == 0 || height == 0) {
        error = "texture has no size";
        return nullptr;
    }
    const VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), format, &props);
    constexpr VkFormatFeatureFlags kSample = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if ((props.optimalTilingFeatures & kSample) != kSample) {
        error = "the device cannot sample this texture format";
        return nullptr;
    }
    auto result = std::unique_ptr<GpuTexture>(new GpuTexture());
    result->m_image = ctx.createImage(format, width, height, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, {});
    result->m_imageWidth = width;
    result->m_imageHeight = height;
    result->m_nearest = nearest;
    result->m_clamp = clamp;
    // black until the first picture: a texture nothing was written to has no layout to be sampled in (the staging buffer, the size of
    // the picture, is made when the first picture comes from the CPU: a texture the GPU draws into never needs it)
    const VkImage image = result->m_image.handle();
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        barrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        const VkClearColorValue black{};
        const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    result->m_written = true;
    return result;
}

void GpuTexture::update(const VkContext &ctx, const uint8_t *rgba)
{
    const size_t bytes = static_cast<size_t>(m_image.width()) * m_image.height() * 4;
    if (!m_staging) {
        m_staging = ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    }
    if (rgba != m_staging.mapped()) {
        std::memcpy(m_staging.mapped(), rgba, bytes);
    }
    m_staging.flush();
    const VkImage image = m_image.handle();
    const VkImageLayout from = m_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    const uint32_t width = m_image.width(), height = m_image.height();
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        barrier(cb, image, 0, 1, from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        vkCmdCopyBufferToImage(cb, m_staging.handle(), image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        barrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    m_written = true;
}

void GpuTexture::drawInto(const VkContext &ctx, const std::function<void(VkCommandBuffer)> &draw, const std::function<void(VkCommandBuffer)> &before)
{
    const VkImage image = m_image.handle();
    const VkImageLayout from = m_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
    const uint32_t width = m_image.width(), height = m_image.height();
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        if (before) {
            before(cb);
        }
        barrier(cb, image, 0, 1, from, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_image.view();
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // the draw covers every texel
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachments = &color;
        vkCmdBeginRendering(cb, &rendering);
        draw(cb);
        vkCmdEndRendering(cb);
        barrier(cb, image, 0, 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    m_written = true;
}

} // namespace murasaki::scene
