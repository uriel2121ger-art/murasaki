#include "core/context.h"

#include "util/log.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

namespace murasaki::scene {

namespace {

constexpr uint64_t kDrmModLinear = 0;
constexpr uint64_t kFenceTimeoutNs = 10ull * 1000 * 1000 * 1000;

std::atomic<uint32_t> g_validationMessages{0};

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT *data,
                                             void *)
{
    if (severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) {
        ++g_validationMessages;
        logError("vulkan validation: {}", data->pMessage);
    }
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties> &list, const char *name)
{
    return std::any_of(list.begin(), list.end(), [&](const VkExtensionProperties &p) { return std::strcmp(p.extensionName, name) == 0; });
}

std::vector<VkExtensionProperties> deviceExtensions(VkPhysicalDevice device)
{
    uint32_t count = 0;
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
    std::vector<VkExtensionProperties> list(count);
    vkEnumerateDeviceExtensionProperties(device, nullptr, &count, list.data());
    return list;
}

struct Candidate {
    VkPhysicalDevice device = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    std::string name;
    std::string renderNode;
    VkPhysicalDeviceType type = VK_PHYSICAL_DEVICE_TYPE_OTHER;
    std::vector<VkExtensionProperties> extensions;
};

// Software rasterizers last, then integrated before discrete: a wallpaper should not wake the big GPU, and the
// compositor of a hybrid machine usually runs on the integrated one.
int rank(VkPhysicalDeviceType type)
{
    switch (type) {
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 0;
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 1;
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
    case VK_PHYSICAL_DEVICE_TYPE_CPU: return 4;
    default: return 3;
    }
}

bool matchesSelector(const Candidate &c, size_t index, const std::string &selector)
{
    if (selector.empty()) {
        return true;
    }
    if (selector == std::to_string(index)) {
        return true;
    }
    if (!c.renderNode.empty() && c.renderNode.find(selector) != std::string::npos) {
        return true;
    }
    return c.name.find(selector) != std::string::npos;
}

} // namespace

uint32_t validationMessageCount()
{
    return g_validationMessages.load();
}

ContextOptions contextOptionsFromEnvironment()
{
    ContextOptions options;
    const char *validate = std::getenv("MURASAKI_VK_VALIDATE");
    options.validate = validate && validate[0] != '\0' && std::strcmp(validate, "0") != 0;
    if (const char *device = std::getenv("MURASAKI_VK_DEVICE")) {
        options.device = device;
    }
    if (const char *priority = std::getenv("MURASAKI_VK_QUEUE_PRIORITY"); priority && priority[0] != '\0') {
        options.lowPriority = std::strcmp(priority, "low") == 0;
    }
    return options;
}

std::unique_ptr<VkContext> VkContext::create(const ContextOptions &options, std::string &error)
{
    std::unique_ptr<VkContext> ctx(new VkContext());
    try {
        ctx->init(options);
    } catch (const std::exception &e) {
        error = e.what();
        return nullptr;
    }
    return ctx;
}

void VkContext::init(const ContextOptions &options)
{
    if (volkInitialize() != VK_SUCCESS) {
        throw VkException("cannot load the Vulkan loader (libvulkan.so.1)");
    }
    uint32_t instanceVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) {
        vkEnumerateInstanceVersion(&instanceVersion);
    }
    if (instanceVersion < VK_API_VERSION_1_3) {
        throw VkException("the Vulkan loader is older than 1.3");
    }

    std::vector<const char *> layers;
    std::vector<const char *> instanceExtensions;
    if (options.validate) {
        uint32_t count = 0;
        vkEnumerateInstanceLayerProperties(&count, nullptr);
        std::vector<VkLayerProperties> available(count);
        vkEnumerateInstanceLayerProperties(&count, available.data());
        const bool haveLayer = std::any_of(available.begin(), available.end(), [](const VkLayerProperties &p) {
            return std::strcmp(p.layerName, "VK_LAYER_KHRONOS_validation") == 0;
        });
        uint32_t extCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extCount, exts.data());
        if (haveLayer && hasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) {
            layers.push_back("VK_LAYER_KHRONOS_validation");
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        } else {
            logInfo("MURASAKI_VK_VALIDATE is set but the validation layer is not installed, continuing without it");
        }
    }

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "murasaki-scene-vk";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    ici.enabledExtensionCount = static_cast<uint32_t>(instanceExtensions.size());
    ici.ppEnabledExtensionNames = instanceExtensions.data();
    vkCheck(vkCreateInstance(&ici, nullptr, &m_instance), "vkCreateInstance");
    volkLoadInstance(m_instance);

    if (!layers.empty()) {
        VkDebugUtilsMessengerCreateInfoEXT mci{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
            | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = debugCallback;
        vkCreateDebugUtilsMessengerEXT(m_instance, &mci, nullptr, &m_messenger);
    }

    // ---- physical device ----
    uint32_t deviceCount = 0;
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, nullptr);
    std::vector<VkPhysicalDevice> devices(deviceCount);
    vkEnumeratePhysicalDevices(m_instance, &deviceCount, devices.data());
    std::vector<Candidate> candidates;
    for (VkPhysicalDevice device : devices) {
        Candidate c;
        c.device = device;
        c.extensions = deviceExtensions(device);
        VkPhysicalDeviceDrmPropertiesEXT drm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT};
        VkPhysicalDeviceProperties2 props{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        if (hasExtension(c.extensions, VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME)) {
            props.pNext = &drm;
        }
        vkGetPhysicalDeviceProperties2(device, &props);
        if (props.properties.apiVersion < VK_API_VERSION_1_3) {
            continue;
        }
        c.name = props.properties.deviceName;
        c.type = props.properties.deviceType;
        if (props.pNext == &drm && drm.hasRender) {
            c.renderNode = "/dev/dri/renderD" + std::to_string(drm.renderMinor);
        }
        uint32_t familyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(familyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &familyCount, families.data());
        bool found = false;
        for (uint32_t i = 0; i < familyCount; ++i) {
            if (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                c.queueFamily = i;
                found = true;
                break;
            }
        }
        if (found) {
            candidates.push_back(std::move(c));
        }
    }
    if (candidates.empty()) {
        throw VkException("no Vulkan 1.3 device with a graphics queue");
    }
    std::vector<size_t> order(candidates.size());
    for (size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return rank(candidates[a].type) < rank(candidates[b].type); });
    const Candidate *chosen = nullptr;
    for (size_t index : order) {
        if (matchesSelector(candidates[index], index, options.device)) {
            chosen = &candidates[index];
            break;
        }
    }
    if (!chosen) {
        throw VkException("MURASAKI_VK_DEVICE '" + options.device + "' matches no device");
    }
    m_physical = chosen->device;
    m_queueFamily = chosen->queueFamily;
    m_deviceName = chosen->name;
    m_renderNode = chosen->renderNode;

    // ---- logical device ----
    std::vector<const char *> deviceExts;
    const auto optional = [&](const char *name) {
        if (hasExtension(chosen->extensions, name)) {
            deviceExts.push_back(name);
            return true;
        }
        return false;
    };
    const bool fd = optional(VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
    const bool dmabuf = optional(VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME);
    const bool modifiers = optional(VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME);
    m_dmabufExport = fd && dmabuf && modifiers;
    m_queueFamilyForeign = optional(VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME);

    // Sampling a video's YCbCr pictures straight from the decoder's dma-buf (assets/videoapi.h) needs the sampler conversion feature
    VkPhysicalDeviceVulkan11Features have11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    VkPhysicalDeviceFeatures2 query{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    query.pNext = &have11;
    vkGetPhysicalDeviceFeatures2(m_physical, &query);
    m_samplerYcbcr = have11.samplerYcbcrConversion == VK_TRUE;
    VkPhysicalDeviceVulkan11Features features11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
    features11.samplerYcbcrConversion = m_samplerYcbcr ? VK_TRUE : VK_FALSE;
    VkPhysicalDeviceVulkan13Features features13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    features13.pNext = &features11;
    features13.dynamicRendering = VK_TRUE;
    features13.synchronization2 = VK_TRUE;
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = m_queueFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    // low priority against the other processes on the GPU (see ContextOptions::lowPriority); the extension goes last so that
    // a refusal can drop it
    const char *priorityExt = !options.lowPriority                                                    ? nullptr
                              : hasExtension(chosen->extensions, VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME) ? VK_KHR_GLOBAL_PRIORITY_EXTENSION_NAME
                              : hasExtension(chosen->extensions, VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME) ? VK_EXT_GLOBAL_PRIORITY_EXTENSION_NAME
                                                                                                        : nullptr;
    VkDeviceQueueGlobalPriorityCreateInfoKHR globalPriority{VK_STRUCTURE_TYPE_DEVICE_QUEUE_GLOBAL_PRIORITY_CREATE_INFO_KHR};
    globalPriority.globalPriority = VK_QUEUE_GLOBAL_PRIORITY_LOW_KHR;
    if (priorityExt) {
        deviceExts.push_back(priorityExt);
        qci.pNext = &globalPriority;
    }
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features13;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
    dci.ppEnabledExtensionNames = deviceExts.data();
    VkResult created = vkCreateDevice(m_physical, &dci, nullptr, &m_device);
    if (created != VK_SUCCESS && priorityExt) {
        // a driver that will not give it (VK_ERROR_NOT_PERMITTED is for higher ones, but be safe): the ordinary priority
        logInfo("vkCreateDevice with the low queue priority failed ({}): the ordinary priority", static_cast<int>(created));
        deviceExts.pop_back();
        qci.pNext = nullptr;
        dci.enabledExtensionCount = static_cast<uint32_t>(deviceExts.size());
        dci.ppEnabledExtensionNames = deviceExts.data();
        priorityExt = nullptr;
        created = vkCreateDevice(m_physical, &dci, nullptr, &m_device);
    }
    vkCheck(created, "vkCreateDevice");
    m_lowPriority = priorityExt != nullptr;
    volkLoadDevice(m_device);
    vkGetDeviceQueue(m_device, m_queueFamily, 0, &m_queue);

    VmaVulkanFunctions functions{};
    functions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
    functions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
    VmaAllocatorCreateInfo aci{};
    aci.physicalDevice = m_physical;
    aci.device = m_device;
    aci.instance = m_instance;
    aci.vulkanApiVersion = VK_API_VERSION_1_3;
    aci.pVulkanFunctions = &functions;
    vkCheck(vmaCreateAllocator(&aci, &m_allocator), "vmaCreateAllocator");

    // the scene's depth buffer (3D meshes): the spec promises D32_SFLOAT or X8_D24_UNORM_PACK32, and D24_UNORM_S8_UINT or
    // D32_SFLOAT_S8_UINT, as depth attachments
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(m_physical, f, &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            m_depthFormat = f;
            break;
        }
    }

    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = m_queueFamily;
    vkCheck(vkCreateCommandPool(m_device, &pci, nullptr, &m_pool), "vkCreateCommandPool");

    logInfo("Vulkan device: {} ({}), dma-buf export {}, queue priority {}", m_deviceName,
            m_renderNode.empty() ? "no render node reported" : m_renderNode, m_dmabufExport ? "available" : "not available",
            m_lowPriority ? "low" : "ordinary");
}

VkContext::~VkContext()
{
    if (m_device) {
        vkDeviceWaitIdle(m_device);
    }
    if (m_pool) {
        vkDestroyCommandPool(m_device, m_pool, nullptr);
    }
    if (m_allocator) {
        vmaDestroyAllocator(m_allocator);
    }
    if (m_device) {
        vkDestroyDevice(m_device, nullptr);
    }
    if (m_messenger) {
        vkDestroyDebugUtilsMessengerEXT(m_instance, m_messenger, nullptr);
    }
    if (m_instance) {
        vkDestroyInstance(m_instance, nullptr);
    }
}

std::vector<uint64_t> VkContext::exportModifiers(VkFormat format, uint32_t width, uint32_t height) const
{
    std::vector<uint64_t> result;
    if (!m_dmabufExport) {
        return result;
    }
    VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
    VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
    props.pNext = &list;
    vkGetPhysicalDeviceFormatProperties2(m_physical, format, &props);
    std::vector<VkDrmFormatModifierPropertiesEXT> entries(list.drmFormatModifierCount);
    list.pDrmFormatModifierProperties = entries.data();
    vkGetPhysicalDeviceFormatProperties2(m_physical, format, &props);

    constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    for (const auto &entry : entries) {
        // Only one memory plane: extra planes carry compression metadata the consumer would have to understand.
        if (entry.drmFormatModifierPlaneCount != 1 || (entry.drmFormatModifierTilingFeatures & kNeeded) != kNeeded) {
            continue;
        }
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT modifierInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT};
        modifierInfo.drmFormatModifier = entry.drmFormatModifier;
        modifierInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkPhysicalDeviceExternalImageFormatInfo externalInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
        externalInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        externalInfo.pNext = &modifierInfo;
        VkPhysicalDeviceImageFormatInfo2 info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
        info.pNext = &externalInfo;
        info.format = format;
        info.type = VK_IMAGE_TYPE_2D;
        info.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VkExternalImageFormatProperties externalProps{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
        VkImageFormatProperties2 out{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
        out.pNext = &externalProps;
        if (vkGetPhysicalDeviceImageFormatProperties2(m_physical, &info, &out) != VK_SUCCESS) {
            continue;
        }
        if (!(externalProps.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
            continue;
        }
        if (out.imageFormatProperties.maxExtent.width < width || out.imageFormatProperties.maxExtent.height < height) {
            continue;
        }
        result.push_back(entry.drmFormatModifier);
    }
    // Tiled layouts first: they are what the GPU renders fastest, linear is the compatibility fallback.
    std::stable_partition(result.begin(), result.end(), [](uint64_t m) { return m != kDrmModLinear; });
    return result;
}

uint32_t VkContext::findMemoryType(uint32_t typeBits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred) const
{
    VkPhysicalDeviceMemoryProperties mem;
    vkGetPhysicalDeviceMemoryProperties(m_physical, &mem);
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i) {
        if (!(typeBits & (1u << i)) || (mem.memoryTypes[i].propertyFlags & required) != required) {
            continue;
        }
        if ((mem.memoryTypes[i].propertyFlags & preferred) == preferred) {
            return i;
        }
        if (fallback == UINT32_MAX) {
            fallback = i;
        }
    }
    if (fallback == UINT32_MAX) {
        throw VkException("no suitable memory type");
    }
    return fallback;
}

void VkContext::submitAndWait(const std::function<void(VkCommandBuffer)> &record) const
{
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = m_pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkCheck(vkAllocateCommandBuffers(m_device, &ai, &cb), "vkAllocateCommandBuffers");
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    try {
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkCheck(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
        record(cb);
        vkCheck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
        vkCheck(vkCreateFence(m_device, &fci, nullptr, &fence), "vkCreateFence");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        vkCheck(vkQueueSubmit(m_queue, 1, &si, fence), "vkQueueSubmit");
        vkCheck(vkWaitForFences(m_device, 1, &fence, VK_TRUE, kFenceTimeoutNs), "vkWaitForFences");
    } catch (...) {
        if (fence) {
            vkDestroyFence(m_device, fence, nullptr);
        }
        vkFreeCommandBuffers(m_device, m_pool, 1, &cb);
        throw;
    }
    vkDestroyFence(m_device, fence, nullptr);
    vkFreeCommandBuffers(m_device, m_pool, 1, &cb);
}

GpuBuffer VkContext::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostAccess) const
{
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = size;
    bci.usage = usage;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostAccess) {
        aci.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VmaAllocationInfo info{};
    vkCheck(vmaCreateBuffer(m_allocator, &bci, &aci, &buffer, &allocation, &info), "vmaCreateBuffer");
    return GpuBuffer(m_allocator, buffer, allocation, info.pMappedData, size);
}

GpuImage VkContext::createImage(VkFormat format, uint32_t width, uint32_t height, uint32_t mipLevels, VkImageUsageFlags usage,
                                VkComponentMapping swizzle) const
{
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {width, height, 1};
    ici.mipLevels = mipLevels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VmaAllocationCreateInfo aci{};
    aci.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    vkCheck(vmaCreateImage(m_allocator, &ici, &aci, &image, &allocation, nullptr), "vmaCreateImage");
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.components = swizzle;
    vci.subresourceRange = {aspectOf(format), 0, mipLevels, 0, 1};
    VkImageView view = VK_NULL_HANDLE;
    const VkResult r = vkCreateImageView(m_device, &vci, nullptr, &view);
    if (r != VK_SUCCESS) {
        vmaDestroyImage(m_allocator, image, allocation);
        throw VkException("vkCreateImageView", r);
    }
    return GpuImage(m_device, m_allocator, image, view, allocation, format, width, height, mipLevels);
}

void GpuBuffer::reset()
{
    if (m_buffer) {
        vmaDestroyBuffer(m_allocator, m_buffer, m_allocation);
    }
    m_buffer = VK_NULL_HANDLE;
    m_allocation = nullptr;
    m_mapped = nullptr;
    m_size = 0;
}

void GpuImage::reset()
{
    if (m_view) {
        vkDestroyImageView(m_device, m_view, nullptr);
    }
    if (m_image) {
        vmaDestroyImage(m_allocator, m_image, m_allocation);
    }
    m_view = VK_NULL_HANDLE;
    m_image = VK_NULL_HANDLE;
    m_allocation = nullptr;
}

} // namespace murasaki::scene
