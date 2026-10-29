#include "core/framebuffers.h"

#include <algorithm>
#include <cstring>
#include <span>

namespace murasaki::scene {

namespace {

constexpr uint64_t kDrmModLinear = 0;

VkImageMemoryBarrier2 imageBarrier(VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return b;
}

void pipelineBarrier(VkCommandBuffer cb, const VkImageMemoryBarrier2 &barrier)
{
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(cb, &dep);
}

} // namespace

FrameBufferSet::FrameBufferSet(VkContext &ctx, uint32_t width, uint32_t height, Transport transport)
    : m_ctx(ctx), m_width(width), m_height(height), m_transport(transport)
{
}

FrameBufferSet::~FrameBufferSet()
{
    destroy();
}

std::unique_ptr<FrameBufferSet> FrameBufferSet::create(VkContext &ctx, uint32_t width, uint32_t height, Transport transport,
                                                       std::string &error)
{
    if (width == 0 || height == 0 || width > bridge::kMaxDimension || height > bridge::kMaxDimension) {
        error = "invalid buffer size";
        return nullptr;
    }
    std::unique_ptr<FrameBufferSet> set(new FrameBufferSet(ctx, width, height, transport));
    try {
        if (transport == Transport::Shm) {
            set->createShm();
        } else {
            std::vector<uint64_t> modifiers = ctx.exportModifiers(kExportFormat, width, height);
            if (transport == Transport::DmabufLinear) {
                const bool linear = std::find(modifiers.begin(), modifiers.end(), kDrmModLinear) != modifiers.end();
                modifiers.clear();
                if (linear) {
                    modifiers.push_back(kDrmModLinear);
                }
            }
            if (modifiers.empty()) {
                throw VkException(transport == Transport::DmabufLinear ? "the device cannot export linear dma-bufs"
                                                                        : "the device cannot export dma-bufs");
            }
            set->createDmabuf(modifiers);
        }
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
    return set;
}

void FrameBufferSet::createDmabuf(const std::vector<uint64_t> &modifiers)
{
    const VkDevice device = m_ctx.device();
    bool first = true;
    for (Slot &slot : m_slots) {
        // The message announces one modifier for the whole set, so every image must have it: the first one is
        // free to pick, the others may only take what it took.
        const std::vector<uint64_t> onlyFirstChoice{m_modifier};
        const std::vector<uint64_t> &allowed = first ? modifiers : onlyFirstChoice;
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageDrmFormatModifierListCreateInfoEXT list{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT};
        list.drmFormatModifierCount = static_cast<uint32_t>(allowed.size());
        list.pDrmFormatModifiers = allowed.data();
        external.pNext = &list;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &external;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = kExportFormat;
        ici.extent = {m_width, m_height, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vkCheck(vkCreateImage(device, &ici, nullptr, &slot.image), "vkCreateImage (dma-buf)");

        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 info{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        info.image = slot.image;
        vkGetImageMemoryRequirements2(device, &info, &requirements);

        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = slot.image;
        VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        exportInfo.pNext = &dedicatedInfo;
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &exportInfo;
        mai.allocationSize = requirements.memoryRequirements.size;
        // Linear buffers are the compatibility path (other GPUs, CPU readers): keep them in system memory.
        mai.memoryTypeIndex = m_transport == Transport::DmabufLinear
            ? m_ctx.findMemoryType(requirements.memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
            : m_ctx.findMemoryType(requirements.memoryRequirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        vkCheck(vkAllocateMemory(device, &mai, nullptr, &slot.memory), "vkAllocateMemory (dma-buf)");
        vkCheck(vkBindImageMemory(device, slot.image, slot.memory, 0), "vkBindImageMemory");

        VkImageDrmFormatModifierPropertiesEXT chosen{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT};
        vkCheck(vkGetImageDrmFormatModifierPropertiesEXT(device, slot.image, &chosen), "vkGetImageDrmFormatModifierPropertiesEXT");
        if (!first && chosen.drmFormatModifier != m_modifier) {
            throw VkException("the driver gave the buffers of a set different modifiers");
        }
        m_modifier = chosen.drmFormatModifier;
        first = false;

        VkImageSubresource subresource{VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0};
        VkSubresourceLayout layout{};
        vkGetImageSubresourceLayout(device, slot.image, &subresource, &layout);
        if (layout.offset > UINT32_MAX || layout.rowPitch == 0 || layout.rowPitch > UINT32_MAX) {
            throw VkException("the driver reported a plane layout the bridge cannot describe");
        }
        slot.plane = {static_cast<uint32_t>(layout.offset), static_cast<uint32_t>(layout.rowPitch)};

        VkMemoryGetFdInfoKHR fdInfo{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
        fdInfo.memory = slot.memory;
        fdInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        int fd = -1;
        vkCheck(vkGetMemoryFdKHR(device, &fdInfo, &fd), "vkGetMemoryFdKHR");
        slot.fd.reset(fd);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = slot.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = kExportFormat;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCheck(vkCreateImageView(device, &vci, nullptr, &slot.view), "vkCreateImageView");
    }
}

void FrameBufferSet::createShm()
{
    bridge::ShmGeometry geometry;
    geometry.count = kBufferCount;
    geometry.width = m_width;
    geometry.height = m_height;
    geometry.stride = m_width * 4;
    bridge::ShmError shmError = bridge::ShmError::Ok;
    m_ring = bridge::ShmRing::create(geometry, &shmError);
    if (!m_ring) {
        throw VkException("cannot create the shared-memory ring");
    }
    for (Slot &slot : m_slots) {
        slot.owned = m_ctx.createImage(kExportFormat, m_width, m_height, 1,
                                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
        slot.image = slot.owned.handle();
        slot.view = slot.owned.view();
    }
    m_staging = m_ctx.createBuffer(static_cast<VkDeviceSize>(m_width) * m_height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
}

void FrameBufferSet::destroy()
{
    const VkDevice device = m_ctx.device();
    vkDeviceWaitIdle(device);
    for (Slot &slot : m_slots) {
        if (m_transport != Transport::Shm) {
            if (slot.view) {
                vkDestroyImageView(device, slot.view, nullptr);
            }
            if (slot.image) {
                vkDestroyImage(device, slot.image, nullptr);
            }
            if (slot.memory) {
                vkFreeMemory(device, slot.memory, nullptr);
            }
        }
        slot = Slot{};
    }
}

std::array<int, kBufferCount> FrameBufferSet::dmabufFds() const
{
    std::array<int, kBufferCount> fds{};
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        fds[i] = m_slots[i].fd.get();
    }
    return fds;
}

void FrameBufferSet::cmdAcquire(VkCommandBuffer cb, uint32_t index, VkImageLayout layout)
{
    VkImageMemoryBarrier2 b = imageBarrier(m_slots[index].image, VK_IMAGE_LAYOUT_UNDEFINED, layout);
    b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.srcAccessMask = 0;
    b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    b.dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
    pipelineBarrier(cb, b);
}

void FrameBufferSet::cmdRelease(VkCommandBuffer cb, uint32_t index, VkImageLayout layout)
{
    Slot &slot = m_slots[index];
    if (isDmabuf()) {
        VkImageMemoryBarrier2 b = imageBarrier(slot.image, layout, VK_IMAGE_LAYOUT_GENERAL);
        b.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        b.dstAccessMask = 0;
        if (m_ctx.queueFamilyForeign()) {
            b.srcQueueFamilyIndex = m_ctx.queueFamily();
            b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
        }
        pipelineBarrier(cb, b);
        return;
    }

    VkImageMemoryBarrier2 toSrc = imageBarrier(slot.image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    toSrc.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    toSrc.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    toSrc.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toSrc.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    pipelineBarrier(cb, toSrc);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {m_width, m_height, 1};
    vkCmdCopyImageToBuffer(cb, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_staging.handle(), 1, &region);

    VkBufferMemoryBarrier2 toHost{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    toHost.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    toHost.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    toHost.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHost.buffer = m_staging.handle();
    toHost.size = VK_WHOLE_SIZE;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.bufferMemoryBarrierCount = 1;
    dep.pBufferMemoryBarriers = &toHost;
    vkCmdPipelineBarrier2(cb, &dep);
}

void FrameBufferSet::finishFrame(uint32_t index)
{
    if (isDmabuf() || !m_ring) {
        return;
    }
    std::span<uint8_t> slot = m_ring->frame(index);
    if (slot.size() >= m_staging.size()) {
        m_staging.invalidate();
        std::memcpy(slot.data(), m_staging.mapped(), static_cast<size_t>(m_staging.size()));
    }
}

} // namespace murasaki::scene
