#pragma once

// A frame recorded into several command buffers, each sent to the GPU as soon as it is full. The GPU does not stop a command
// buffer it has started, so one long frame holds every other client (the compositor among them) back until it ends; sent in
// pieces, the kernel's scheduler can run their work in between (with the low queue priority, before the rest of the frame).
//
// One queue, submission order: a barrier recorded in a later piece synchronises with the work of the earlier ones, and the
// fence of the last piece signals after all of them. Pieces are reused from frame to frame, so the caller waits for that
// fence before the next begin().

#include "core/vk.h"

#include <cstddef>
#include <vector>

namespace murasaki::scene {

class VkContext;

class ChunkedSubmit
{
public:
    explicit ChunkedSubmit(const VkContext &ctx);
    ~ChunkedSubmit();
    ChunkedSubmit(const ChunkedSubmit &) = delete;
    ChunkedSubmit &operator=(const ChunkedSubmit &) = delete;

    // The frame's first command buffer, recording.
    VkCommandBuffer begin();
    // Ends `cb`, sends it and returns the next one, recording.
    VkCommandBuffer split(VkCommandBuffer cb);
    // Ends the last one and sends it, signalling `fence`.
    void finish(VkCommandBuffer cb, VkFence fence);
    // Pieces the last frame was sent in.
    size_t pieces() const { return m_used; }

private:
    VkCommandBuffer next();
    void submit(VkCommandBuffer cb, VkFence fence);

    const VkContext &m_ctx;
    std::vector<VkCommandBuffer> m_buffers;
    size_t m_used = 0;
};

} // namespace murasaki::scene
