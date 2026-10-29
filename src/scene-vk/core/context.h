#pragma once

// Headless Vulkan setup: instance (with the validation layer when MURASAKI_VK_VALIDATE=1 and the layer exists),
// physical device choice, one graphics queue, the VMA allocator and a command pool. There is no surface and no
// swapchain: frames leave the process as DMA-BUFs or shared memory (see framebuffers.h).
//
// Needs Vulkan 1.3 (dynamic rendering and synchronization2 are core there), which every driver Murasaki targets
// ships. The DMA-BUF export extensions are optional: without them the engine falls back to shared memory.

#include "core/vk.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace murasaki::scene {

struct ContextOptions {
    bool validate = false;  // request VK_LAYER_KHRONOS_validation; skipped with a log line when it is absent
    std::string device;     // empty: automatic. Otherwise an index, a render node ("renderD128") or a name fragment
    // The queue asks the GPU's scheduler for low priority (VK_KHR/EXT_global_priority, no privilege needed): a wallpaper's
    // frames wait behind the compositor's and every application's work instead of competing with it. A driver without it, or
    // that refuses it, gives the ordinary priority.
    bool lowPriority = true;
};

// Reads MURASAKI_VK_VALIDATE, MURASAKI_VK_DEVICE and MURASAKI_VK_QUEUE_PRIORITY (low, the default, or medium).
ContextOptions contextOptionsFromEnvironment();

// Number of validation warnings and errors the layer reported so far (always 0 without the layer).
uint32_t validationMessageCount();

class GpuBuffer;
class GpuImage;

class VkContext
{
public:
    // Never throws: returns null and fills `error` when no usable device exists.
    static std::unique_ptr<VkContext> create(const ContextOptions &options, std::string &error);
    ~VkContext();
    VkContext(const VkContext &) = delete;
    VkContext &operator=(const VkContext &) = delete;

    VkInstance instance() const { return m_instance; }
    VkPhysicalDevice physicalDevice() const { return m_physical; }
    VkDevice device() const { return m_device; }
    VkQueue queue() const { return m_queue; }
    uint32_t queueFamily() const { return m_queueFamily; }
    VmaAllocator allocator() const { return m_allocator; }
    VkCommandPool commandPool() const { return m_pool; }

    const std::string &deviceName() const { return m_deviceName; }
    // "/dev/dri/renderD128" and friends; empty when the driver does not say (VK_EXT_physical_device_drm).
    const std::string &renderNode() const { return m_renderNode; }
    // VK_EXT_image_drm_format_modifier + VK_EXT_external_memory_dma_buf + VK_KHR_external_memory_fd.
    bool dmabufExport() const { return m_dmabufExport; }
    bool queueFamilyForeign() const { return m_queueFamilyForeign; }
    // The device converts YCbCr samples itself (a video's NV12 pictures are sampled as they come from the decoder).
    bool samplerYcbcr() const { return m_samplerYcbcr; }
    bool lowPriority() const { return m_lowPriority; } // the queue was made with the low global priority
    // The depth format of the scene target's depth buffer, chosen when the device is made: D32_SFLOAT where the device renders to
    // it, else X8_D24_UNORM_PACK32 (Vulkan guarantees one of the two as a depth attachment; D24_UNORM_S8_UINT or D32_SFLOAT_S8_UINT,
    // one of which is guaranteed too, are the last resort).
    VkFormat depthFormat() const { return m_depthFormat; }

    // DRM format modifiers the device can render to (colour attachment + transfer destination) and export as a
    // single-memory-plane DMA-BUF for `format`/size, best first (tiled before linear). Empty without dmabufExport().
    std::vector<uint64_t> exportModifiers(VkFormat format, uint32_t width, uint32_t height) const;

    uint32_t findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const;

    // Records `record` into a fresh primary command buffer, submits it and waits for completion.
    void submitAndWait(const std::function<void(VkCommandBuffer)> &record) const;

    GpuBuffer createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostAccess) const;
    GpuImage createImage(VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels, VkImageUsageFlags usage,
                         VkComponentMapping swizzle = {}) const;

private:
    VkContext() = default;
    void init(const ContextOptions &options);

    VkInstance m_instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
    VkPhysicalDevice m_physical = VK_NULL_HANDLE;
    VkDevice m_device = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;
    VmaAllocator m_allocator = nullptr;
    VkCommandPool m_pool = VK_NULL_HANDLE;
    std::string m_deviceName;
    std::string m_renderNode;
    bool m_dmabufExport = false;
    bool m_queueFamilyForeign = false;
    bool m_samplerYcbcr = false;
    bool m_lowPriority = false;
    VkFormat m_depthFormat = VK_FORMAT_D32_SFLOAT;
};

// Formats with a depth (and maybe a stencil) aspect, and the aspect an image view or barrier of one covers.
inline bool isDepthFormat(VkFormat f)
{
    return f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM
        || f == VK_FORMAT_X8_D24_UNORM_PACK32 || f == VK_FORMAT_D16_UNORM_S8_UINT;
}
inline VkImageAspectFlags aspectOf(VkFormat f)
{
    if (!isDepthFormat(f)) {
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
    const bool stencil = f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D16_UNORM_S8_UINT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
}

// A buffer owned through VMA. `hostAccess` buffers stay mapped (random access, for read-back and uploads).
class GpuBuffer
{
public:
    GpuBuffer() = default;
    GpuBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation, void *mapped, VkDeviceSize size)
        : m_allocator(allocator), m_buffer(buffer), m_allocation(allocation), m_mapped(mapped), m_size(size)
    {
    }
    GpuBuffer(GpuBuffer &&other) noexcept { swap(other); }
    GpuBuffer &operator=(GpuBuffer &&other) noexcept
    {
        GpuBuffer tmp(std::move(other));
        swap(tmp);
        return *this;
    }
    GpuBuffer(const GpuBuffer &) = delete;
    GpuBuffer &operator=(const GpuBuffer &) = delete;
    ~GpuBuffer() { reset(); }

    VkBuffer handle() const { return m_buffer; }
    void *mapped() const { return m_mapped; }
    VkDeviceSize size() const { return m_size; }
    explicit operator bool() const { return m_buffer != VK_NULL_HANDLE; }
    // Makes device writes visible to the host on memory that is not coherent (a no-op on coherent memory).
    void invalidate() const { vmaInvalidateAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE); }
    // Makes host writes visible to the device.
    void flush() const { vmaFlushAllocation(m_allocator, m_allocation, 0, VK_WHOLE_SIZE); }
    void flush(VkDeviceSize offset, VkDeviceSize size) const { vmaFlushAllocation(m_allocator, m_allocation, offset, size); }
    void reset();

private:
    void swap(GpuBuffer &other) noexcept
    {
        std::swap(m_allocator, other.m_allocator);
        std::swap(m_buffer, other.m_buffer);
        std::swap(m_allocation, other.m_allocation);
        std::swap(m_mapped, other.m_mapped);
        std::swap(m_size, other.m_size);
    }
    VmaAllocator m_allocator = nullptr;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VmaAllocation m_allocation = nullptr;
    void *m_mapped = nullptr;
    VkDeviceSize m_size = 0;
};

// A 2D image with a full-range colour view, owned through VMA.
class GpuImage
{
public:
    GpuImage() = default;
    GpuImage(VkDevice device, VmaAllocator allocator, VkImage image, VkImageView view, VmaAllocation allocation,
             VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels)
        : m_device(device), m_allocator(allocator), m_image(image), m_view(view), m_allocation(allocation),
          m_format(format), m_width(width), m_height(height), m_mipLevels(mipLevels)
    {
    }
    GpuImage(GpuImage &&other) noexcept { swap(other); }
    GpuImage &operator=(GpuImage &&other) noexcept
    {
        GpuImage tmp(std::move(other));
        swap(tmp);
        return *this;
    }
    GpuImage(const GpuImage &) = delete;
    GpuImage &operator=(const GpuImage &) = delete;
    ~GpuImage() { reset(); }

    VkImage handle() const { return m_image; }
    VkImageView view() const { return m_view; }
    VkFormat format() const { return m_format; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }
    uint32_t mipLevels() const { return m_mipLevels; }
    explicit operator bool() const { return m_image != VK_NULL_HANDLE; }
    void reset();

private:
    void swap(GpuImage &other) noexcept
    {
        std::swap(m_device, other.m_device);
        std::swap(m_allocator, other.m_allocator);
        std::swap(m_image, other.m_image);
        std::swap(m_view, other.m_view);
        std::swap(m_allocation, other.m_allocation);
        std::swap(m_format, other.m_format);
        std::swap(m_width, other.m_width);
        std::swap(m_height, other.m_height);
        std::swap(m_mipLevels, other.m_mipLevels);
    }
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    VkImage m_image = VK_NULL_HANDLE;
    VkImageView m_view = VK_NULL_HANDLE;
    VmaAllocation m_allocation = nullptr;
    VkFormat m_format = VK_FORMAT_UNDEFINED;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    uint32_t m_mipLevels = 0;
};

} // namespace murasaki::scene
