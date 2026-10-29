#include "puppet/render.h"

#include "assets/imagedecode.h"
#include "core/vk.h"
#include "puppetlimits.h"
#include "shaders/glslcompiler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

namespace murasaki::puppet {

using namespace murasaki::scene;

namespace {

constexpr VkFormat kColorFormat = VK_FORMAT_R8G8B8A8_UNORM;

const char *kVertexShader = R"(#version 450
layout(location = 0) in vec2 pos;
layout(location = 1) in vec2 uv;
layout(push_constant) uniform P { vec2 scale; vec2 offset; vec4 tint; vec4 screen; } p;
layout(location = 0) out vec2 vUv;
void main() {
    gl_Position = vec4(pos * p.scale + p.offset, 0.0, 1.0);
    vUv = uv;
}
)";

// colour = S(c, a) * tint * opacity with S(x, a) = 1 - (1 - x)(1 - screen * a); alpha = a * opacity (the textures are premultiplied)
const char *kFragmentShader = R"(#version 450
layout(location = 0) in vec2 vUv;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P { vec2 scale; vec2 offset; vec4 tint; vec4 screen; } p;
layout(location = 0) out vec4 outColor;
void main() {
    vec4 c = texture(tex, vUv);
    vec3 s = vec3(1.0) - (vec3(1.0) - c.rgb) * (vec3(1.0) - p.screen.rgb * c.a);
    outColor = vec4(s * p.tint.rgb * p.tint.a, c.a * p.tint.a);
}
)";

// a mask source: only where its alpha is above the threshold (screen.a); no colour
const char *kMaskFragmentShader = R"(#version 450
layout(location = 0) in vec2 vUv;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform P { vec2 scale; vec2 offset; vec4 tint; vec4 screen; } p;
layout(location = 0) out vec4 outColor;
void main() {
    if (texture(tex, vUv).a <= p.screen.a)
        discard;
    outColor = vec4(0.0);
}
)";

struct Push {
    float scale[2];
    float offset[2];
    float tint[4];   // rgb, opacity
    float screen[4]; // rgb, mask threshold
};
static_assert(sizeof(Push) == 48);

struct BlendFactors {
    VkBlendFactor srcColor, dstColor;
    VkBlendOp colorOp;
    VkBlendFactor srcAlpha, dstAlpha;
    VkBlendOp alphaOp;
};

// The fixed-function equations of the reference (docs/pets/puppets.md, 12.3) for the colour; the modes it has no such equation for use Normal's. The reference draws on a screen whose alpha it never reads;
// ours is the alpha of a picture the compositor puts over a desktop, so every mode that is about colour (Multiply, ColorDodge, Subtract, Exclusion, Inverse) takes Normal's "over" for alpha, and
// only the three modes whose point is to cut coverage (DestinationIn, ClipToLower, SliceFromLower) keep the reference's alpha.
BlendFactors factorsOf(BlendMode mode)
{
    constexpr auto add = VK_BLEND_OP_ADD;
    switch (mode) {
    case BlendMode::Multiply: return {VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::Screen: return {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::Lighten: return {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_MAX};
    case BlendMode::ColorDodge: return {VK_BLEND_FACTOR_DST_COLOR, VK_BLEND_FACTOR_ONE, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::LinearDodge: return {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::AddGlow: return {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::Subtract: return {VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::Exclusion: return {VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::Inverse: return {VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::DestinationIn: return {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_ALPHA, add, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_SRC_ALPHA, add};
    case BlendMode::ClipToLower: return {VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add, VK_BLEND_FACTOR_DST_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::SliceFromLower: return {VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
    case BlendMode::Normal:
    case BlendMode::Overlay:
    case BlendMode::Darken:
    case BlendMode::ColorBurn:
    case BlendMode::HardLight:
    case BlendMode::SoftLight:
    case BlendMode::Difference:
        break;
    }
    return {VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, add};
}

// modes that share an equation share a pipeline
int pipelineClass(BlendMode mode)
{
    switch (mode) {
    case BlendMode::Overlay:
    case BlendMode::Darken:
    case BlendMode::ColorBurn:
    case BlendMode::HardLight:
    case BlendMode::SoftLight:
    case BlendMode::Difference:
        return static_cast<int>(BlendMode::Normal);
    default:
        return static_cast<int>(mode);
    }
}

void barrier(VkCommandBuffer cb, VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
             VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cb, &dep);
}

} // namespace

void premultiplyInPlace(std::vector<uint8_t> &rgba)
{
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        const unsigned a = rgba[i + 3];
        if (a == 255)
            continue;
        for (size_t c = 0; c < 3; ++c)
            rgba[i + c] = static_cast<uint8_t>((rgba[i + c] * a + 127) / 255);
    }
}

bool decodeTexture(const uint8_t *bytes, size_t size, TextureImage &out, std::string &error)
{
    DecodedImage image;
    if (!decodeImage({bytes, size}, static_cast<uint32_t>(kMaxTextureSide), static_cast<uint64_t>(kMaxTexturePixels) * 4, image, error))
        return false;
    if (static_cast<std::int64_t>(image.width) * image.height > kMaxTexturePixels) {
        error = "the texture is over kMaxTexturePixels";
        return false;
    }
    premultiplyInPlace(image.rgba);
    out.width = image.width;
    out.height = image.height;
    out.rgba = std::move(image.rgba);
    return true;
}

struct PuppetRenderer::Impl {
    VkContext &ctx;
    VkDevice device;
    VkFormat stencilFormat = VK_FORMAT_UNDEFINED;
    bool combinedDepth = true;
    size_t drawables = 0;
    bool modelFault = false; // init failed because of the model, not the device

    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE, maskFrag = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    std::map<int, VkPipeline> contentPipelines; // key: class * 2 + stencil
    VkPipeline maskPipeline = VK_NULL_HANDLE;

    std::vector<GpuImage> textures;
    GpuImage blank; // 1 x 1 transparent: what a drawable with no texture samples
    std::vector<VkDescriptorSet> sets; // textures, then blank, then the group
    GpuBuffer uvs, indices, positions, quad;
    std::vector<VkDeviceSize> uvOffset, indexOffset, indexCount, vertexCount;
    size_t positionCapacity = 0; // vertices
    GpuImage stencil, group;
    VkExtent2D sized{0, 0};

    explicit Impl(VkContext &c) : ctx(c), device(c.device()) {}
    ~Impl();
    bool init(const ModelBackend &model, const std::vector<TextureImage> &images, bool pointSampling, std::string &error);
    void ensureTargets(VkExtent2D extent);
    VkPipeline contentPipeline(BlendMode mode, bool stencilTest);
    VkPipeline createPipeline(const BlendFactors *blend, bool stencilTest, bool maskWrite);
    GpuImage upload(const uint8_t *rgba, uint32_t w, uint32_t h);
    void writeSet(VkDescriptorSet set, VkImageView view);
};

PuppetRenderer::Impl::~Impl()
{
    if (!device)
        return;
    vkDeviceWaitIdle(device);
    for (auto &[key, pipeline] : contentPipelines)
        vkDestroyPipeline(device, pipeline, nullptr);
    if (maskPipeline)
        vkDestroyPipeline(device, maskPipeline, nullptr);
    if (pool)
        vkDestroyDescriptorPool(device, pool, nullptr);
    if (sampler)
        vkDestroySampler(device, sampler, nullptr);
    if (layout)
        vkDestroyPipelineLayout(device, layout, nullptr);
    if (setLayout)
        vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    for (const VkShaderModule m : {vert, frag, maskFrag})
        if (m)
            vkDestroyShaderModule(device, m, nullptr);
}

GpuImage PuppetRenderer::Impl::upload(const uint8_t *rgba, uint32_t w, uint32_t h)
{
    GpuImage image = ctx.createImage(kColorFormat, w, h, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    const VkDeviceSize bytes = static_cast<VkDeviceSize>(w) * h * 4;
    GpuBuffer staging = ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    std::memcpy(staging.mapped(), rgba, bytes);
    staging.flush();
    ctx.submitAndWait([&](VkCommandBuffer cb) {
        barrier(cb, image.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage(cb, staging.handle(), image.handle(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        barrier(cb, image.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    });
    return image;
}

void PuppetRenderer::Impl::writeSet(VkDescriptorSet set, VkImageView view)
{
    VkDescriptorImageInfo info{sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &info;
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}

bool PuppetRenderer::Impl::init(const ModelBackend &model, const std::vector<TextureImage> &images, bool pointSampling, std::string &error)
{
    // a depth-stencil format the device renders to (one of the two is guaranteed by the specification)
    for (const VkFormat f : {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT_S8_UINT}) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(ctx.physicalDevice(), f, &props);
        if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
            stencilFormat = f;
            break;
        }
    }
    if (stencilFormat == VK_FORMAT_UNDEFINED) {
        error = "no depth-stencil format to draw masks with";
        return false;
    }
    std::string compileError;
    const auto module = [&](ShaderStage stage, const char *source) {
        const std::vector<uint32_t> spirv = compileGlsl(stage, source, compileError);
        if (spirv.empty())
            return VkShaderModule(VK_NULL_HANDLE);
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = spirv.size() * sizeof(uint32_t);
        ci.pCode = spirv.data();
        VkShaderModule m = VK_NULL_HANDLE;
        vkCheck(vkCreateShaderModule(device, &ci, nullptr, &m), "vkCreateShaderModule");
        return m;
    };
    vert = module(ShaderStage::Vertex, kVertexShader);
    frag = module(ShaderStage::Fragment, kFragmentShader);
    maskFrag = module(ShaderStage::Fragment, kMaskFragmentShader);
    if (!vert || !frag || !maskFrag) {
        error = "the shaders do not compile: " + compileError;
        return false;
    }
    VkDescriptorSetLayoutBinding binding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sli.bindingCount = 1;
    sli.pBindings = &binding;
    vkCheck(vkCreateDescriptorSetLayout(device, &sli, nullptr, &setLayout), "vkCreateDescriptorSetLayout");
    VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &setLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &range;
    vkCheck(vkCreatePipelineLayout(device, &pli, nullptr, &layout), "vkCreatePipelineLayout");
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter = sci.minFilter = pointSampling ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    vkCheck(vkCreateSampler(device, &sci, nullptr, &sampler), "vkCreateSampler");

    const uint32_t setCount = static_cast<uint32_t>(images.size()) + 2;
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, setCount};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = setCount;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    vkCheck(vkCreateDescriptorPool(device, &dpi, nullptr, &pool), "vkCreateDescriptorPool");
    std::vector<VkDescriptorSetLayout> layouts(setCount, setLayout);
    sets.resize(setCount);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = pool;
    dai.descriptorSetCount = setCount;
    dai.pSetLayouts = layouts.data();
    vkCheck(vkAllocateDescriptorSets(device, &dai, sets.data()), "vkAllocateDescriptorSets");
    for (size_t i = 0; i < images.size(); ++i) {
        const TextureImage &t = images[i];
        if (t.width == 0 || t.height == 0 || t.rgba.size() != static_cast<size_t>(t.width) * t.height * 4) {
            error = "a texture has no pixels";
            modelFault = true;
            return false;
        }
        textures.push_back(upload(t.rgba.data(), t.width, t.height));
        writeSet(sets[i], textures.back().view());
    }
    const uint8_t clear[4] = {0, 0, 0, 0};
    blank = upload(clear, 1, 1);
    writeSet(sets[images.size()], blank.view());

    // static geometry: uvs and indices of every drawable
    drawables = model.drawableCount();
    VkDeviceSize uvBytes = 0, indexBytes = 0, vertices = 0;
    uvOffset.resize(drawables);
    indexOffset.resize(drawables);
    indexCount.resize(drawables);
    vertexCount.resize(drawables);
    for (size_t i = 0; i < drawables; ++i) {
        const Shape &s = model.shape(static_cast<int>(i));
        uvOffset[i] = uvBytes;
        indexOffset[i] = indexBytes;
        indexCount[i] = s.indices.size();
        vertexCount[i] = s.uvs.size();
        uvBytes += s.uvs.size() * sizeof(float) * 2;
        indexBytes += (s.indices.size() * sizeof(uint16_t) + 3) / 4 * 4;
        vertices += s.uvs.size();
    }
    uvs = ctx.createBuffer(std::max<VkDeviceSize>(uvBytes, 16), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    indices = ctx.createBuffer(std::max<VkDeviceSize>(indexBytes, 16), VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    for (size_t i = 0; i < drawables; ++i) {
        const Shape &s = model.shape(static_cast<int>(i));
        float *u = reinterpret_cast<float *>(static_cast<uint8_t *>(uvs.mapped()) + uvOffset[i]);
        for (size_t k = 0; k < s.uvs.size(); ++k) {
            u[k * 2] = static_cast<float>(s.uvs[k].x);
            u[k * 2 + 1] = static_cast<float>(s.uvs[k].y);
        }
        if (!s.indices.empty())
            std::memcpy(static_cast<uint8_t *>(indices.mapped()) + indexOffset[i], s.indices.data(), s.indices.size() * sizeof(uint16_t));
    }
    uvs.flush();
    indices.flush();
    // a mesh can appear as a drawn item and as a mask source in one frame; the full-picture quad of a group comes first
    positionCapacity = static_cast<size_t>(vertices) * 2 + 8;
    positions = ctx.createBuffer(positionCapacity * sizeof(float) * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
    // the quad of a group: positions are normalised device coordinates, uvs the picture
    quad = ctx.createBuffer(2 * 4 * 2 * sizeof(float) + 6 * sizeof(uint16_t) + 16, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT, true);
    float *q = static_cast<float *>(quad.mapped());
    const float corners[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
    const float cornerUv[8] = {0, 0, 1, 0, 1, 1, 0, 1};
    std::memcpy(q, corners, sizeof corners);
    std::memcpy(q + 8, cornerUv, sizeof cornerUv);
    const uint16_t quadIndices[6] = {0, 1, 2, 0, 2, 3};
    std::memcpy(static_cast<uint8_t *>(quad.mapped()) + 64, quadIndices, sizeof quadIndices);
    quad.flush();
    return true;
}

VkPipeline PuppetRenderer::Impl::createPipeline(const BlendFactors *blend, bool stencilTest, bool maskWrite)
{
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, maskWrite ? maskFrag : frag, "main", nullptr};
    VkVertexInputBindingDescription bindings[2] = {{0, 8, VK_VERTEX_INPUT_RATE_VERTEX}, {1, 8, VK_VERTEX_INPUT_RATE_VERTEX}};
    VkVertexInputAttributeDescription attributes[2] = {{0, 0, VK_FORMAT_R32G32_SFLOAT, 0}, {1, 1, VK_FORMAT_R32G32_SFLOAT, 0}};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 2;
    vertexInput.pVertexBindingDescriptions = bindings;
    vertexInput.vertexAttributeDescriptionCount = 2;
    vertexInput.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // the reference does not cull
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    if (maskWrite || stencilTest) {
        ds.stencilTestEnable = VK_TRUE;
        VkStencilOpState op{};
        op.failOp = VK_STENCIL_OP_KEEP;
        op.depthFailOp = VK_STENCIL_OP_KEEP;
        op.passOp = maskWrite ? VK_STENCIL_OP_REPLACE : VK_STENCIL_OP_KEEP;
        op.compareOp = maskWrite ? VK_COMPARE_OP_ALWAYS : VK_COMPARE_OP_EQUAL;
        op.compareMask = 0xFF;
        op.writeMask = maskWrite ? 0xFF : 0;
        ds.front = ds.back = op;
    }
    VkPipelineColorBlendAttachmentState attachment{};
    if (maskWrite) {
        attachment.colorWriteMask = 0;
    } else {
        attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        attachment.blendEnable = VK_TRUE;
        attachment.srcColorBlendFactor = blend->srcColor;
        attachment.dstColorBlendFactor = blend->dstColor;
        attachment.colorBlendOp = blend->colorOp;
        attachment.srcAlphaBlendFactor = blend->srcAlpha;
        attachment.dstAlphaBlendFactor = blend->dstAlpha;
        attachment.alphaBlendOp = blend->alphaOp;
    }
    VkPipelineColorBlendStateCreateInfo colorBlend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &attachment;
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 3;
    dynamic.pDynamicStates = dynamicStates;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachmentFormats = &kColorFormat;
    rendering.depthAttachmentFormat = stencilFormat;
    rendering.stencilAttachmentFormat = stencilFormat;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rendering;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vertexInput;
    ci.pInputAssemblyState = &assembly;
    ci.pViewportState = &viewport;
    ci.pRasterizationState = &raster;
    ci.pMultisampleState = &multisample;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &colorBlend;
    ci.pDynamicState = &dynamic;
    ci.layout = layout;
    VkPipeline pipeline = VK_NULL_HANDLE;
    vkCheck(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &ci, nullptr, &pipeline), "vkCreateGraphicsPipelines");
    return pipeline;
}

VkPipeline PuppetRenderer::Impl::contentPipeline(BlendMode mode, bool stencilTest)
{
    const int key = pipelineClass(mode) * 2 + (stencilTest ? 1 : 0);
    if (const auto it = contentPipelines.find(key); it != contentPipelines.end())
        return it->second;
    const BlendFactors f = factorsOf(mode);
    return contentPipelines[key] = createPipeline(&f, stencilTest, false);
}

void PuppetRenderer::Impl::ensureTargets(VkExtent2D extent)
{
    if (sized.width == extent.width && sized.height == extent.height && stencil)
        return;
    vkDeviceWaitIdle(device);
    stencil = ctx.createImage(stencilFormat, extent.width, extent.height, 1, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
    group = ctx.createImage(kColorFormat, extent.width, extent.height, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
    writeSet(sets[sets.size() - 1], group.view());
    sized = extent;
}

PuppetRenderer::PuppetRenderer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
PuppetRenderer::~PuppetRenderer() = default;

std::unique_ptr<PuppetRenderer> PuppetRenderer::create(VkContext &ctx, const ModelBackend &model, const std::vector<TextureImage> &textures, bool pointSampling, std::string &error,
                                                       CreateFailure *failure)
{
    if (failure)
        *failure = CreateFailure::None;
    auto impl = std::make_unique<Impl>(ctx);
    try {
        if (!impl->init(model, textures, pointSampling, error)) {
            if (failure)
                *failure = impl->modelFault ? CreateFailure::Model : CreateFailure::Device;
            return nullptr;
        }
    } catch (const VkException &e) {
        error = e.what();
        if (failure) // a texture that does not fit the device's memory is this model's doing; any other Vulkan failure is the device's
            *failure = (e.result() == VK_ERROR_OUT_OF_DEVICE_MEMORY || e.result() == VK_ERROR_OUT_OF_HOST_MEMORY) ? CreateFailure::Model : CreateFailure::Device;
        return nullptr;
    }
    return std::unique_ptr<PuppetRenderer>(new PuppetRenderer(std::move(impl)));
}

uint32_t PuppetRenderer::cmdDraw(VkCommandBuffer cb, VkImageView target, VkExtent2D extent, const Frame &frame, const ViewTransform &view)
{
    Impl &m = *m_impl;
    m_stats = {};
    m.ensureTargets(extent);
    const VkRect2D full{{0, 0}, extent};
    const VkViewport viewport{0, 0, static_cast<float>(extent.width), static_cast<float>(extent.height), 0, 1};

    barrier(cb, m.stencil.handle(), aspectOf(m.stencilFormat), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
            VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT);

    const auto begin = [&](VkImageView color, bool clear) {
        VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        c.imageView = color;
        c.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        c.loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        c.clearValue.color = {{0, 0, 0, 0}};
        VkRenderingAttachmentInfo d{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        d.imageView = m.stencil.view();
        d.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        d.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        d.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        VkRenderingAttachmentInfo s = d;
        s.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        s.clearValue.depthStencil = {1.0f, 0};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = full;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &c;
        ri.pDepthAttachment = &d;
        ri.pStencilAttachment = &s;
        vkCmdBeginRendering(cb, &ri);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        vkCmdSetScissor(cb, 0, 1, &full);
    };

    // The positions this frame writes: every drawn mesh, and a mask source again for each item that uses it. The buffer grows to hold them (nothing is in flight: the caller waited for the last frame).
    {
        std::vector<int> sourceVertices(m.drawables, -1);
        for (const DrawItem &source : frame.maskSources) {
            if (source.drawable >= 0 && static_cast<size_t>(source.drawable) < m.drawables)
                sourceVertices[static_cast<size_t>(source.drawable)] = static_cast<int>(source.positions.size());
        }
        size_t need = 0;
        for (const DrawItem &item : frame.items) {
            if (item.kind == DrawKind::Mesh && item.opacity > 0)
                need += item.positions.size();
            for (const MaskRef &mask : item.masks) {
                if (mask.source >= 0 && static_cast<size_t>(mask.source) < m.drawables && sourceVertices[static_cast<size_t>(mask.source)] > 0)
                    need += static_cast<size_t>(sourceVertices[static_cast<size_t>(mask.source)]);
            }
        }
        if (need > static_cast<size_t>(kMaxPositionVertices)) {
            begin(target, true);
            vkCmdEndRendering(cb);
            m_stats.skipped = static_cast<uint32_t>(std::min<size_t>(frame.items.size(), UINT32_MAX));
            return 0;
        }
        if (need > m.positionCapacity) {
            vkDeviceWaitIdle(m.device);
            m.positionCapacity = std::min<size_t>(std::max(need, m.positionCapacity * 2), static_cast<size_t>(kMaxPositionVertices));
            m.positions = m.ctx.createBuffer(m.positionCapacity * sizeof(float) * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        }
    }

    // The geometry of one mesh: positions are written to the dynamic buffer, uvs and indices are the static ones.
    size_t cursor = 0; // vertices
    auto* const positionData = static_cast<float *>(m.positions.mapped());
    const auto drawMesh = [&](const DrawItem &item, const VkPipeline pipeline, const Push &push, int textureSet, bool stencilReference, uint32_t reference) -> bool {
        if (item.drawable < 0 || static_cast<size_t>(item.drawable) >= m.drawables) {
            return false;
        }
        const size_t d = static_cast<size_t>(item.drawable);
        if (item.positions.size() != m.vertexCount[d] || m.indexCount[d] == 0 || cursor + item.positions.size() > m.positionCapacity) {
            return false;
        }
        float *out = positionData + cursor * 2;
        // (a position is clamped before it becomes a float: a double beyond a float's range converts to nothing defined, and the renderer takes frames from any ModelBackend)
        constexpr double kBound = static_cast<double>(kMaxOutputCoordinate);
        for (size_t k = 0; k < item.positions.size(); ++k) {
            const double x = item.positions[k].x, y = item.positions[k].y;
            out[k * 2] = static_cast<float>(std::isfinite(x) ? std::clamp(x, -kBound, kBound) : 0.0);
            out[k * 2 + 1] = static_cast<float>(std::isfinite(y) ? std::clamp(y, -kBound, kBound) : 0.0);
        }
        const VkDeviceSize posOffset = cursor * sizeof(float) * 2;
        cursor += item.positions.size();
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        if (stencilReference)
            vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, reference);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m.layout, 0, 1, &m.sets[static_cast<size_t>(textureSet)], 0, nullptr);
        vkCmdPushConstants(cb, m.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
        const VkBuffer buffers[2] = {m.positions.handle(), m.uvs.handle()};
        const VkDeviceSize offsets[2] = {posOffset, m.uvOffset[d]};
        vkCmdBindVertexBuffers(cb, 0, 2, buffers, offsets);
        vkCmdBindIndexBuffer(cb, m.indices.handle(), m.indexOffset[d], VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(cb, static_cast<uint32_t>(m.indexCount[d]), 1, 0, 0, 0);
        return true;
    };

    const size_t blankSet = m.sets.size() - 2;
    const auto textureSetOf = [&](int texture) { return texture >= 0 && static_cast<size_t>(texture) < m.textures.size() ? texture : static_cast<int>(blankSet); };
    std::vector<int> sourceOf(m.drawables, -1);
    for (size_t i = 0; i < frame.maskSources.size(); ++i) {
        const int d = frame.maskSources[i].drawable;
        if (d >= 0 && static_cast<size_t>(d) < m.drawables)
            sourceOf[static_cast<size_t>(d)] = static_cast<int>(i);
    }

    // The stencil for an item with masks: cleared to 0 when any mask is a Mask (only their footprints show), else to 1 (everything shows but the dodged); each mask in order writes 1 or 0 where its source
    // is above its threshold. Returns whether the item may be drawn with the stencil test.
    const auto applyMasks = [&](const DrawItem &item) {
        bool anyMask = false;
        for (const MaskRef &mask : item.masks)
            anyMask = anyMask || !mask.dodge;
        VkClearAttachment clear{};
        clear.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
        clear.clearValue.depthStencil = {1.0f, anyMask ? 0u : 1u};
        const VkClearRect rect{full, 0, 1};
        vkCmdClearAttachments(cb, 1, &clear, 1, &rect);
        if (!m.maskPipeline)
            m.maskPipeline = m.createPipeline(nullptr, false, true);
        for (const MaskRef &mask : item.masks) {
            if (mask.source < 0 || static_cast<size_t>(mask.source) >= m.drawables || sourceOf[static_cast<size_t>(mask.source)] < 0)
                continue;
            const DrawItem &source = frame.maskSources[static_cast<size_t>(sourceOf[static_cast<size_t>(mask.source)])];
            Push push{{static_cast<float>(view.scaleX), static_cast<float>(view.scaleY)}, {static_cast<float>(view.offsetX), static_cast<float>(view.offsetY)}, {1, 1, 1, 1},
                      {0, 0, 0, static_cast<float>(source.maskThreshold)}};
            if (drawMesh(source, m.maskPipeline, push, textureSetOf(source.texture), true, mask.dodge ? 0u : 1u))
                ++m_stats.maskDraws;
            else
                ++m_stats.skipped;
        }
    };

    const auto pushOf = [&](const DrawItem &item, bool picture) {
        Push push{};
        if (picture) {
            push.scale[0] = push.scale[1] = 1;
        } else {
            push.scale[0] = static_cast<float>(view.scaleX);
            push.scale[1] = static_cast<float>(view.scaleY);
            push.offset[0] = static_cast<float>(view.offsetX);
            push.offset[1] = static_cast<float>(view.offsetY);
        }
        for (int c = 0; c < 3; ++c) {
            push.tint[c] = static_cast<float>(item.tint[c]);
            push.screen[c] = static_cast<float>(item.screenTint[c]);
        }
        push.tint[3] = static_cast<float>(std::clamp(item.opacity, 0.0, 1.0));
        return push;
    };

    begin(target, true);
    bool inGroup = false;
    for (const DrawItem &item : frame.items) {
        switch (item.kind) {
        case DrawKind::Mesh: {
            if (!(item.opacity > 0)) {
                break; // invisible
            }
            const bool masked = !item.masks.empty();
            if (masked)
                applyMasks(item);
            if (drawMesh(item, m.contentPipeline(item.blend, masked), pushOf(item, false), textureSetOf(item.texture), masked, 1))
                ++m_stats.meshes;
            else
                ++m_stats.skipped;
            break;
        }
        case DrawKind::BeginGroup:
            if (inGroup)
                break;
            vkCmdEndRendering(cb);
            barrier(cb, m.group.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_SAMPLED_READ_BIT, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            begin(m.group.view(), true);
            inGroup = true;
            break;
        case DrawKind::EndGroup: {
            if (!inGroup)
                break;
            vkCmdEndRendering(cb);
            barrier(cb, m.group.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            begin(target, false);
            inGroup = false;
            const bool masked = !item.masks.empty();
            if (masked)
                applyMasks(item);
            // the group's picture over the whole target
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m.contentPipeline(item.blend, masked));
            if (masked)
                vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_AND_BACK, 1);
            const Push push = pushOf(item, true);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m.layout, 0, 1, &m.sets[m.sets.size() - 1], 0, nullptr);
            vkCmdPushConstants(cb, m.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
            const VkBuffer buffers[2] = {m.quad.handle(), m.quad.handle()};
            const VkDeviceSize offsets[2] = {0, 32};
            vkCmdBindVertexBuffers(cb, 0, 2, buffers, offsets);
            vkCmdBindIndexBuffer(cb, m.quad.handle(), 64, VK_INDEX_TYPE_UINT16);
            vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);
            ++m_stats.groups;
            break;
        }
        }
    }
    if (inGroup) { // a group that was never closed is dropped
        vkCmdEndRendering(cb);
        barrier(cb, m.group.handle(), VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        begin(target, false);
        ++m_stats.skipped;
    }
    vkCmdEndRendering(cb);
    m.positions.flush();
    return m_stats.meshes;
}

} // namespace murasaki::puppet
