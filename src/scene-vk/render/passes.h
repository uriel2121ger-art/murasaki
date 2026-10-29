#pragma once

// Draws one pass of the scene: a quad with a translated shader into a render target. Owns what every pass shares:
// the pipeline layout (set 0: the two uniform blocks with dynamic offsets, set 1: texture slots g_Texture0..8), a
// ring buffer the per-draw uniform blocks and quad vertices are written into, a descriptor pool reset every frame,
// a pipeline cache and a white fallback texture.
//
// Orientation: the shaders get WE's clip space (y up) and the translated vertex stage flips it, so a target holds
// its picture top row first. A quad is described by the clip-space rectangle it covers and the texture range it
// shows (u from 0 to uMax, v from 0 at the top to vMax at the bottom).

#include "core/context.h"
#include "render/uniforms.h"
#include "scene/material.h"
#include "shaders/translate.h"

#include <array>
#include <map>
#include <span>
#include <tuple>
#include <vector>

namespace murasaki::scene {

// Per-frame budgets of the pass machinery; the scene analysis checks a scene against them before it is accepted.
inline constexpr uint64_t kPassRingBytes = 8u << 20;   // uniform and vertex ring
inline constexpr uint32_t kPassTextureSets = 2048;     // texture descriptor sets (one per draw) in one pool
inline constexpr uint32_t kPassMaxTexturePools = 8;    // pools chained when a frame outgrows the first, which are created on demand
// Draws per frame the pass machinery accepts. A memory bound, not a Vulkan one: every pool holds kPassTextureSets x kMaxTextureSlots
// combined image samplers. In practice the uniform ring gives out first: the scene analysis budgets at least 768 bytes of it per draw
// (the alignment bound three times over), so it refuses a scene near 10900 draws, and the real consumption of a plain quad (about
// 520 bytes) would fill the 8 MiB at about 16000. The ceiling is a backstop above both: the ring, not the pools, is what limits a scene.
inline constexpr uint32_t kPassMaxDraws = kPassTextureSets * kPassMaxTexturePools;
inline constexpr uint32_t kPassBlockWindow = 16384;    // largest uniform block
inline constexpr uint64_t kPassMaxAlignment = 256;     // upper bound of minUniformBufferOffsetAlignment we plan for

struct PassTexture {
    VkImageView view = VK_NULL_HANDLE; // null: the white fallback
    VkSampler sampler = VK_NULL_HANDLE;
    TextureInfo info;
};

struct Quad {
    float minX = -1.0f, maxX = 1.0f, minY = -1.0f, maxY = 1.0f; // clip-space rectangle (y up)
    float uMax = 1.0f, vMax = 1.0f;
};

// Vertex data other than the built-in quad (particles): one interleaved buffer drawn as indexed triangles. The shader's
// attributes are matched by name to `attributes`; a shader that wants another one is refused.
struct GeometryAttribute {
    const char *name;
    VkFormat format;
    uint32_t offset;
};

struct Geometry {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceSize vertexOffset = 0;
    uint32_t stride = 0;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    uint32_t indexCount = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32; // a 3D mesh keeps its file's u16 indices
    const GeometryAttribute *attributes = nullptr; // must outlive the pass runner: its address keys the pipeline
    uint32_t attributeCount = 0;
};

// A run of indices of a geometry: `count` of them from `first`.
struct IndexRange {
    uint32_t first = 0;
    uint32_t count = 0;
};

struct DrawRequest {
    const TranslatedShader *shader = nullptr;
    UniformContext uniforms;
    Blending blending = Blending::Normal;
    bool blend = true;                  // false: the shader's colour replaces the target's, whatever `blending` says
    bool premultiplied = false;         // the source colour already holds its alpha (a buffer of translucent or additive draws)
    std::array<PassTexture, kMaxTextureSlots> textures{};
    VkImageView target = VK_NULL_HANDLE;
    VkFormat targetFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D extent{};
    bool clear = true;                  // clear to transparent black first (else keep the contents)
    bool coversTarget = false;          // the draw writes every pixel of the target: its old contents are neither cleared nor read
    VkRect2D area{};                    // draw only inside this rectangle of the target (width 0: all of it); the viewport stays whole
    Quad quad;
    const Geometry *geometry = nullptr; // null: the quad
    // Depth (3D meshes): a depth attachment beside the colour one, cleared to the far plane first when `depthClear`, tested (less or
    // equal) and written as the material says. Null: no depth at all, as every 2D draw.
    VkImageView depthTarget = VK_NULL_HANDLE;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    bool depthClear = false;
    bool depthTest = false;
    bool depthWrite = false;
    CullMode cull = CullMode::None;
    bool mirrored = false; // the model matrix mirrors (negative determinant): the front faces are the clockwise ones on screen
    // Shadow casters (render/shadows.h): no colour target at all (`target` null, `targetFormat` VK_FORMAT_UNDEFINED), the depth pushed
    // away from the light by a slope-scaled bias, and the geometry drawn once per viewport, the instance index (firstInstance) telling
    // the shader which picture it is drawing. No viewports: one draw over the whole extent.
    bool depthBias = false;
    const VkViewport *viewports = nullptr;
    uint32_t viewportCount = 0;
    uint32_t viewportMask = ~0u; // the viewports the geometry is drawn into (bit i: viewport i); the instance index stays i
    // Only these ranges of the indices, each into the viewports its mask names (bit i: viewport i; consecutive ranges with the same
    // bit are one draw): a big caster cut into pieces that each reach few pictures. Null: all the indices.
    const IndexRange *ranges = nullptr;
    const uint8_t *rangeMasks = nullptr;
    uint32_t rangeCount = 0;
};

class PassRunner
{
public:
    explicit PassRunner(const VkContext &ctx);
    ~PassRunner();
    PassRunner(const PassRunner &) = delete;
    PassRunner &operator=(const PassRunner &) = delete;

    // Call once per recorded frame before the first draw: recycles the ring buffer and the descriptors.
    void beginFrame();
    uint64_t draws() const { return m_drawCount; }
    uint64_t m_drawCount = 0;
    // Records the draw. The target must already be in COLOR_ATTACHMENT_OPTIMAL and every texture view in
    // SHADER_READ_ONLY_OPTIMAL. Throws on a shader the runner cannot drive (more attributes than a quad has,
    // too many texture slots) or when the frame's ring buffer is full.
    void draw(VkCommandBuffer cb, const DrawRequest &request);
    // Several draws in one rendering instance: they share the first request's target, depth target, area and load operations
    // (its `clear` and `depthClear`); each has its own shader, pipeline state, uniforms, textures and geometry.
    void drawMany(VkCommandBuffer cb, std::span<const DrawRequest> requests);

    VkSampler linearClamp() const { return m_linearClamp; }
    // The 1 x 1 depth texture at the far plane (in SHADER_READ_ONLY_OPTIMAL).
    VkImageView farDepthView() const { return m_depthWhite.view(); }
    VkSampler sampler(bool nearest, bool clamp);
    // The depth comparison sampler of shadow maps (`sampler2DComparison`): lit (1) where the reference is less or equal to the stored
    // depth, linear (a 2 x 2 filter of the comparisons) or nearest, and lit outside the texture (a white border).
    VkSampler compareSampler(bool linear);
    // Uniform names no engine input, material constant or annotation could fill (collected over the run).
    const std::set<std::string> &unknownUniforms() const { return m_unknown; }

private:
    VkPipeline pipelineFor(const DrawRequest &request);
    VkDescriptorSet allocateTextureSet(const DrawRequest &request);
    void addTexturePool();

    const VkContext &m_ctx;
    VkDescriptorSetLayout m_uniformLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout m_textureLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_layout = VK_NULL_HANDLE;
    VkDescriptorPool m_uniformPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> m_texturePools; // chained: a draw takes its set from the first pool with room, from m_activePool on
    size_t m_activePool = 0;
    VkDescriptorSet m_uniformSet = VK_NULL_HANDLE;
    GpuBuffer m_ring;
    VkDeviceSize m_ringOffset = 0;
    VkDeviceSize m_uniformAlignment = 256;
    GpuImage m_white;
    GpuImage m_depthWhite; // a 1 x 1 depth texture at the far plane: what a comparison sampler reads where nothing is bound (lit)
    VkSampler m_linearClamp = VK_NULL_HANDLE;
    std::map<int, VkSampler> m_samplers;
    std::map<bool, VkSampler> m_compareSamplers;
    std::map<std::tuple<const TranslatedShader *, int, bool, int, const GeometryAttribute *, int, int>, VkPipeline> m_pipelines;
    std::map<const TranslatedShader *, std::map<VkShaderStageFlagBits, VkShaderModule>> m_modules;
    std::set<std::string> m_unknown;
};

} // namespace murasaki::scene
