#pragma once

// Offscreen render targets of the scene: the layers' ping-pong buffers, effect `fbos`, the scene itself. They are
// RGBA8 (or R8) images used both as colour attachments and as sampled textures, and the scene's depth buffer (a depth
// format: an attachment only); the pool keeps them by key, one image per key, recreated when the size changes.

#include "core/context.h"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace murasaki::scene {

struct RenderTarget {
    GpuImage image;
    VkExtent2D extent{};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED; // what the last recorded barrier left it in (all the levels, between uses)
    uint32_t mipLevels = 1;

    VkImage handle() const { return image.handle(); }
    VkImageView view() const { return image.view(); }
};

// Whether a depth target of `format` is also made sampleable (a depth-only format the device samples).
bool depthSampleable(const VkContext &ctx, VkFormat format);

class TargetPool
{
public:
    explicit TargetPool(const VkContext &ctx) : m_ctx(ctx) {}

    // The target of `key`, created (or recreated) for `extent` and `format`. The reference stays valid until clear(),
    // the pool's destruction or, for a target not asked for during `kIdleFrames` recorded frames, a beginFrame().
    // A target replaced because its size changed is not destroyed at once (a command buffer being recorded may still
    // use it): it waits `kRetireFrames` frames. `keep` exempts the target from the idle sweep.
    // A target with `mipLevels` > 1 has a chain the caller fills.
    RenderTarget &get(const std::string &key, VkExtent2D extent, VkFormat format, bool keep = false, uint32_t mipLevels = 1);
    bool has(const std::string &key) const { return m_targets.count(key) != 0; }
    void clear()
    {
        m_targets.clear();
        m_retired.clear();
    }
    size_t size() const { return m_targets.size(); }
    size_t retired() const { return m_retired.size(); }

    // Starts the recording of a frame: frees retired targets that are old enough and targets nobody asked for lately.
    // The previous frames must have completed on the GPU by now.
    void beginFrame();

    static constexpr uint64_t kIdleFrames = 8;
    static constexpr uint64_t kRetireFrames = 4;

private:
    const VkContext &m_ctx;
    struct Entry {
        std::unique_ptr<RenderTarget> target;
        uint64_t lastUsed = 0;
        bool keep = false;
    };
    std::map<std::string, Entry> m_targets;
    std::vector<std::pair<uint64_t, std::unique_ptr<RenderTarget>>> m_retired; // (frame retired, target)
    uint64_t m_frame = 0;
};

// Records the barrier that brings `target` to `layout`, ordering it after everything recorded before. With
// `discard` the old contents are not kept (the next pass overwrites every pixel).
void transition(VkCommandBuffer cb, RenderTarget &target, VkImageLayout layout, bool discard = false);

} // namespace murasaki::scene
