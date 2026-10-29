#include "render/videoconvert.h"

#include "shaders/glslcompiler.h"
#include "util/log.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <deque>
#include <map>
#include <vector>

namespace murasaki::scene {

namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(a) | (static_cast<uint32_t>(b) << 8) | (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
}
constexpr uint32_t kNv12 = fourcc('N', 'V', '1', '2');
constexpr uint32_t kP010 = fourcc('P', '0', '1', '0');
constexpr size_t kMaxImports = 48; // surfaces of a decoder in flight: its pool plus the queue, with room (a ring smaller than the pool re-imports every picture)
constexpr uint32_t kDescriptorsPerSet = 3; // a YCbCr sampler takes one descriptor per plane of the format (two for NV12 and P010), with room

constexpr const char *kVertex = R"(#version 450
layout(location = 0) out vec2 uv;
void main() {
    // one triangle over the whole target
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

constexpr const char *kFragment = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Push { vec2 scale; } push; // the picture's share of the buffer (the buffer is padded up)
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    color = vec4(texture(picture, uv * push.scale).rgb, 1.0);
}
)";

VkFormat vulkanFormatOf(uint32_t drmFourcc)
{
    switch (drmFourcc) {
    case kNv12: return VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
    case kP010: return VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16;
    default: return VK_FORMAT_UNDEFINED;
    }
}


} // namespace

struct VideoConverter::Impl {
    const VkContext &ctx;
    VkDevice device;

    struct Variant {
        VkFormat format = VK_FORMAT_UNDEFINED;
        uint32_t colorspace = 0;
        bool fullRange = false;
        VkSamplerYcbcrConversion conversion = VK_NULL_HANDLE;
        VkSampler sampler = VK_NULL_HANDLE;
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        bool failed = false;
    };
    struct Imported {
        uint64_t inode = 0;
        size_t variant = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
    };

    std::vector<uint32_t> vertexSpirv, fragmentSpirv;
    VkShaderModule vertexModule = VK_NULL_HANDLE, fragmentModule = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::vector<std::unique_ptr<Variant>> variants;
    std::deque<Imported> imports; // oldest first

    explicit Impl(const VkContext &c) : ctx(c), device(c.device()) {}
    ~Impl()
    {
        vkDeviceWaitIdle(device);
        for (Imported &i : imports) {
            destroy(i);
        }
        for (auto &v : variants) {
            vkDestroyPipeline(device, v->pipeline, nullptr);
            vkDestroyPipelineLayout(device, v->pipelineLayout, nullptr);
            vkDestroyDescriptorSetLayout(device, v->setLayout, nullptr);
            vkDestroySampler(device, v->sampler, nullptr);
            vkDestroySamplerYcbcrConversion(device, v->conversion, nullptr);
        }
        vkDestroyDescriptorPool(device, pool, nullptr);
        vkDestroyShaderModule(device, vertexModule, nullptr);
        vkDestroyShaderModule(device, fragmentModule, nullptr);
    }

    void destroy(Imported &i)
    {
        // the descriptor set goes back with the pool's FREE_DESCRIPTOR_SET flag
        if (i.set) {
            vkFreeDescriptorSets(device, pool, 1, &i.set);
        }
        vkDestroyImageView(device, i.view, nullptr);
        vkDestroyImage(device, i.image, nullptr);
        vkFreeMemory(device, i.memory, nullptr);
        i = {};
    }

    bool init(std::string &why)
    {
        if (!ctx.dmabufExport()) {
            why = "the device has no dma-buf import (VK_EXT_external_memory_dma_buf, VK_EXT_image_drm_format_modifier)";
            return false;
        }
        if (!ctx.samplerYcbcr()) {
            why = "the device has no sampler YCbCr conversion";
            return false;
        }
        std::string error;
        vertexSpirv = compileGlsl(ShaderStage::Vertex, kVertex, error);
        fragmentSpirv = vertexSpirv.empty() ? std::vector<uint32_t>() : compileGlsl(ShaderStage::Fragment, kFragment, error);
        if (vertexSpirv.empty() || fragmentSpirv.empty()) {
            why = "the conversion shaders do not compile: " + error;
            return false;
        }
        const auto module = [&](const std::vector<uint32_t> &code, VkShaderModule &out) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = code.size() * sizeof(uint32_t);
            info.pCode = code.data();
            return vkCreateShaderModule(device, &info, nullptr, &out) == VK_SUCCESS;
        };
        if (!module(vertexSpirv, vertexModule) || !module(fragmentSpirv, fragmentModule)) {
            why = "cannot make the conversion shaders";
            return false;
        }
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(kMaxImports + 2) * kDescriptorsPerSet};
        VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        info.maxSets = static_cast<uint32_t>(kMaxImports + 2);
        info.poolSizeCount = 1;
        info.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &info, nullptr, &pool) != VK_SUCCESS) {
            why = "cannot make the descriptor pool";
            return false;
        }
        return true;
    }

    // What the driver can do with `format` stored with `modifier`: sampled, with the YCbCr conversion filtering the chroma linearly.
    // Returns the format features of the modifier; 0 when the combination is not usable at all.
    VkFormatFeatureFlags featuresOf(VkFormat format, uint64_t modifier) const
    {
        VkDrmFormatModifierPropertiesListEXT list{VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT};
        VkFormatProperties2 props{VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2};
        props.pNext = &list;
        vkGetPhysicalDeviceFormatProperties2(ctx.physicalDevice(), format, &props);
        std::vector<VkDrmFormatModifierPropertiesEXT> all(list.drmFormatModifierCount);
        list.pDrmFormatModifierProperties = all.data();
        vkGetPhysicalDeviceFormatProperties2(ctx.physicalDevice(), format, &props);
        for (const auto &m : all) {
            if (m.drmFormatModifier == modifier) {
                return m.drmFormatModifierTilingFeatures;
            }
        }
        return 0;
    }

    Variant *variantFor(VkFormat format, uint32_t colorspace, bool fullRange, uint64_t modifier, std::string &why)
    {
        for (auto &v : variants) {
            if (v->format == format && v->colorspace == colorspace && v->fullRange == fullRange) {
                if (v->failed) {
                    why = "this conversion was refused by the driver before";
                    return nullptr;
                }
                return v.get();
            }
        }
        auto v = std::make_unique<Variant>();
        v->format = format;
        v->colorspace = colorspace;
        v->fullRange = fullRange;
        Variant *raw = v.get();
        variants.push_back(std::move(v));
        raw->failed = true; // until it is whole
        const VkFormatFeatureFlags features = featuresOf(format, modifier);
        constexpr VkFormatFeatureFlags kNeeded = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        if ((features & kNeeded) != kNeeded) {
            why = "the driver cannot sample this format with this modifier";
            return nullptr;
        }
        const bool linear = (features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT) != 0;
        const bool cosited = (features & VK_FORMAT_FEATURE_COSITED_CHROMA_SAMPLES_BIT) != 0;
        VkSamplerYcbcrConversionCreateInfo conv{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO};
        conv.format = format;
        conv.ycbcrModel = colorspace == 2 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_2020
                          : colorspace == 1 ? VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709
                                            : VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_601;
        conv.ycbcrRange = fullRange ? VK_SAMPLER_YCBCR_RANGE_ITU_FULL : VK_SAMPLER_YCBCR_RANGE_ITU_NARROW;
        conv.components = {VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY};
        // MPEG-2 style siting, which H.264 and HEVC streams use unless they say otherwise: chroma sits on the left of the luma pair
        conv.xChromaOffset = cosited ? VK_CHROMA_LOCATION_COSITED_EVEN : VK_CHROMA_LOCATION_MIDPOINT;
        conv.yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT;
        conv.chromaFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        conv.forceExplicitReconstruction = VK_FALSE;
        if (vkCreateSamplerYcbcrConversion(device, &conv, nullptr, &raw->conversion) != VK_SUCCESS) {
            why = "cannot make the sampler YCbCr conversion";
            return nullptr;
        }
        VkSamplerYcbcrConversionInfo convInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
        convInfo.conversion = raw->conversion;
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.pNext = &convInfo;
        sci.magFilter = sci.minFilter = conv.chromaFilter; // without separate reconstruction these must match the chroma filter
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        if (vkCreateSampler(device, &sci, nullptr, &raw->sampler) != VK_SUCCESS) {
            why = "cannot make the sampler";
            return nullptr;
        }
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        binding.pImmutableSamplers = &raw->sampler; // a YCbCr sampler must be immutable
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 1;
        lci.pBindings = &binding;
        if (vkCreateDescriptorSetLayout(device, &lci, nullptr, &raw->setLayout) != VK_SUCCESS) {
            why = "cannot make the descriptor layout";
            return nullptr;
        }
        const VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 2 * sizeof(float)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &raw->setLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device, &plci, nullptr, &raw->pipelineLayout) != VK_SUCCESS) {
            why = "cannot make the pipeline layout";
            return nullptr;
        }
        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vertexModule, "main", nullptr};
        stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fragmentModule, "main", nullptr};
        VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount = viewport.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState attachment{};
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &attachment;
        const std::array<VkDynamicState, 2> dynamic{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamicState{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamic.size());
        dynamicState.pDynamicStates = dynamic.data();
        const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 1;
        rendering.pColorAttachmentFormats = &colorFormat;
        VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        pci.pNext = &rendering;
        pci.stageCount = static_cast<uint32_t>(stages.size());
        pci.pStages = stages.data();
        pci.pVertexInputState = &vertexInput;
        pci.pInputAssemblyState = &assembly;
        pci.pViewportState = &viewport;
        pci.pRasterizationState = &raster;
        pci.pMultisampleState = &multisample;
        pci.pColorBlendState = &blend;
        pci.pDynamicState = &dynamicState;
        pci.layout = raw->pipelineLayout;
        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pci, nullptr, &raw->pipeline) != VK_SUCCESS) {
            why = "cannot make the conversion pipeline";
            return nullptr;
        }
        raw->failed = false;
        return raw;
    }

    // The buffer as a Vulkan image (the descriptor is consumed on success; the caller closes it on failure).
    bool import(const MurasakiVideoExport &p, VkFormat format, size_t variantIndex, Variant &variant, Imported &out, std::string &why)
    {
        std::array<VkSubresourceLayout, 4> layouts{};
        for (uint32_t i = 0; i < p.planes; ++i) {
            layouts[i].offset = p.offsets[i];
            layouts[i].rowPitch = p.pitches[i];
        }
        VkImageDrmFormatModifierExplicitCreateInfoEXT modifier{VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT};
        modifier.drmFormatModifier = p.modifier;
        modifier.drmFormatModifierPlaneCount = p.planes;
        modifier.pPlaneLayouts = layouts.data();
        VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
        external.pNext = &modifier;
        external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ici.pNext = &external;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = format;
        ici.extent = {p.surfaceWidth, p.surfaceHeight, 1};
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
        ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device, &ici, nullptr, &out.image) != VK_SUCCESS) {
            why = "the driver refuses an image for this buffer (format, modifier or layout)";
            return false;
        }
        VkMemoryDedicatedRequirements dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
        VkMemoryRequirements2 requirements{VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2};
        requirements.pNext = &dedicated;
        VkImageMemoryRequirementsInfo2 reqInfo{VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2};
        reqInfo.image = out.image;
        vkGetImageMemoryRequirements2(device, &reqInfo, &requirements);
        VkMemoryFdPropertiesKHR fdProps{VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
        if (vkGetMemoryFdPropertiesKHR(device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, p.fd, &fdProps) != VK_SUCCESS) {
            why = "the driver does not know this buffer";
            return false;
        }
        const uint32_t typeBits = requirements.memoryRequirements.memoryTypeBits & fdProps.memoryTypeBits;
        if (typeBits == 0) {
            why = "no memory type fits the buffer";
            return false;
        }
        const uint32_t type = static_cast<uint32_t>(std::countr_zero(typeBits));
        VkMemoryDedicatedAllocateInfo dedicatedInfo{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicatedInfo.image = out.image;
        VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
        importInfo.pNext = &dedicatedInfo;
        importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
        importInfo.fd = p.fd; // consumed by a successful allocation
        VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.pNext = &importInfo;
        mai.allocationSize = requirements.memoryRequirements.size;
        mai.memoryTypeIndex = type;
        if (vkAllocateMemory(device, &mai, nullptr, &out.memory) != VK_SUCCESS) {
            why = "cannot import the buffer";
            return false;
        }
        if (vkBindImageMemory(device, out.image, out.memory, 0) != VK_SUCCESS) {
            why = "cannot bind the imported buffer";
            return false;
        }
        VkSamplerYcbcrConversionInfo convInfo{VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO};
        convInfo.conversion = variant.conversion;
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.pNext = &convInfo;
        vci.image = out.image;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vci, nullptr, &out.view) != VK_SUCCESS) {
            why = "cannot make a view of the imported buffer";
            return false;
        }
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = pool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &variant.setLayout;
        if (vkAllocateDescriptorSets(device, &dai, &out.set) != VK_SUCCESS) {
            why = "cannot allocate a descriptor set";
            return false;
        }
        VkDescriptorImageInfo imageInfo{VK_NULL_HANDLE, out.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet = out.set;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &imageInfo; // the sampler is immutable
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        out.variant = variantIndex;
        return true;
    }

    bool convert(const MurasakiVideoExport &p, GpuTexture &target, std::string &why)
    {
        struct Closer {
            int fd;
            bool owned = true;
            ~Closer()
            {
                if (owned && fd >= 0) {
                    ::close(fd);
                }
            }
        } closer{p.fd};
        const VkFormat format = vulkanFormatOf(p.fourcc);
        if (format == VK_FORMAT_UNDEFINED) {
            why = "the surface format is not one the conversion knows";
            return false;
        }
        if (p.fd < 0 || p.planes < 2 || p.surfaceWidth == 0 || p.surfaceHeight == 0) {
            why = "the exported picture is incomplete";
            return false;
        }
        struct stat st {};
        if (::fstat(p.fd, &st) != 0) {
            why = "the exported buffer has no identity";
            return false;
        }
        Variant *variant = variantFor(format, p.colorspace, p.fullRange != 0, p.modifier, why);
        if (!variant) {
            return false;
        }
        size_t variantIndex = 0;
        for (size_t i = 0; i < variants.size(); ++i) {
            if (variants[i].get() == variant) {
                variantIndex = i;
            }
        }
        Imported *image = nullptr;
        for (Imported &i : imports) {
            if (i.inode == static_cast<uint64_t>(st.st_ino) && i.variant == variantIndex) {
                image = &i;
                break;
            }
        }
        if (!image) {
            Imported fresh;
            fresh.inode = static_cast<uint64_t>(st.st_ino);
            if (!import(p, format, variantIndex, *variant, fresh, why)) {
                // the descriptor is ours to close unless the allocation took it
                if (fresh.memory) {
                    closer.owned = false;
                }
                destroy(fresh);
                return false;
            }
            closer.owned = false; // the import has it
            if (imports.size() >= kMaxImports) {
                destroy(imports.front());
                imports.pop_front();
            }
            imports.push_back(fresh);
            image = &imports.back();
        }
        const float scaleX = static_cast<float>(p.pictureWidth) / static_cast<float>(p.surfaceWidth);
        const float scaleY = static_cast<float>(p.pictureHeight) / static_cast<float>(p.surfaceHeight);
        const float pushData[2] = {scaleX, scaleY};
        const uint32_t width = target.width(), height = target.height();
        const bool foreign = ctx.queueFamilyForeign();
        const uint32_t family = ctx.queueFamily();
        target.drawInto(ctx, [&](VkCommandBuffer cb) {
            // (the barrier below is recorded inside the render pass of drawInto only for the draw; the acquire is made before it)
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, variant->pipeline);
            const VkViewport viewport{0, 0, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
            const VkRect2D scissor{{0, 0}, {width, height}};
            vkCmdSetViewport(cb, 0, 1, &viewport);
            vkCmdSetScissor(cb, 0, 1, &scissor);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, variant->pipelineLayout, 0, 1, &image->set, 0, nullptr);
            vkCmdPushConstants(cb, variant->pipelineLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pushData), pushData);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }, [&](VkCommandBuffer cb) {
            // before the render pass: the decoder wrote the buffer, this queue reads it
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
            b.srcAccessMask = 0;
            b.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcQueueFamilyIndex = foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_IGNORED;
            b.dstQueueFamilyIndex = foreign ? family : VK_QUEUE_FAMILY_IGNORED;
            b.image = image->image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &b;
            vkCmdPipelineBarrier2(cb, &dep);
        });
        return true;
    }
};

VideoConverter::VideoConverter(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
VideoConverter::~VideoConverter() = default;

std::unique_ptr<VideoConverter> VideoConverter::create(const VkContext &ctx, std::string &why)
{
    auto impl = std::make_unique<Impl>(ctx);
    if (!impl->init(why)) {
        return nullptr;
    }
    return std::unique_ptr<VideoConverter>(new VideoConverter(std::move(impl)));
}

bool VideoConverter::convert(const MurasakiVideoExport &picture, GpuTexture &target, std::string &why)
{
    return m_impl->convert(picture, target, why);
}

size_t VideoConverter::importedCount() const
{
    return m_impl->imports.size();
}

} // namespace murasaki::scene
