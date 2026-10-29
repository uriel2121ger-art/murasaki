#include "core/chunkedsubmit.h"

#include "core/context.h"

namespace murasaki::scene {

ChunkedSubmit::ChunkedSubmit(const VkContext &ctx) : m_ctx(ctx) {}

ChunkedSubmit::~ChunkedSubmit()
{
    if (!m_buffers.empty()) {
        vkFreeCommandBuffers(m_ctx.device(), m_ctx.commandPool(), static_cast<uint32_t>(m_buffers.size()), m_buffers.data());
    }
}

VkCommandBuffer ChunkedSubmit::next()
{
    if (m_used == m_buffers.size()) {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = m_ctx.commandPool();
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cb = VK_NULL_HANDLE;
        vkCheck(vkAllocateCommandBuffers(m_ctx.device(), &ai, &cb), "vkAllocateCommandBuffers");
        m_buffers.push_back(cb);
    }
    VkCommandBuffer cb = m_buffers[m_used++];
    vkCheck(vkResetCommandBuffer(cb, 0), "vkResetCommandBuffer"); // (its last frame's fence was waited for)
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkCheck(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer");
    return cb;
}

void ChunkedSubmit::submit(VkCommandBuffer cb, VkFence fence)
{
    vkCheck(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkCheck(vkQueueSubmit(m_ctx.queue(), 1, &si, fence), "vkQueueSubmit");
}

VkCommandBuffer ChunkedSubmit::begin()
{
    m_used = 0;
    return next();
}

VkCommandBuffer ChunkedSubmit::split(VkCommandBuffer cb)
{
    submit(cb, VK_NULL_HANDLE);
    return next();
}

void ChunkedSubmit::finish(VkCommandBuffer cb, VkFence fence)
{
    submit(cb, fence);
}

} // namespace murasaki::scene
