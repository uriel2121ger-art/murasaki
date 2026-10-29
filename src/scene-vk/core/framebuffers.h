#pragma once

// The images a viewport renders into and hands to the bridge consumer: a set of kCount RGBA8 images (the bridge's
// kMaxBuffers) that leave the process in one of three ways.
//
//   Dmabuf        exportable VkImages with a DRM format modifier chosen by the driver among the ones it can render
//                 to and export (tiled first). The consumer imports them with EGL.
//   DmabufLinear  the same with the LINEAR modifier, for consumers that cannot import a tiled layout
//                 (the consumer asks for it with RequestLinear).
//   Shm           plain device images; every finished frame is copied through a host-visible buffer into a sealed
//                 memfd ring (RequestShm, or the fallback when the device cannot export).
//
// Bytes are R, G, B, A in memory order (VK_FORMAT_R8G8B8A8_UNORM, DRM fourcc 'AB24'), which is also what the
// shared-memory protocol calls kFormatRgba8.
//
// Synchronization is deliberately simple: the engine waits for the frame's fence before it sends Frame, so a
// consumer never sees a buffer the GPU is still writing (the same rule as glFinish in the GL producer). The
// release barrier still hands dma-bufs over to the foreign queue family so implicit sync stays correct.

#include "core/context.h"

#include "bridge/channel.h"
#include "bridge/protocol.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace murasaki::scene {

enum class Transport { Dmabuf, DmabufLinear, Shm };

inline constexpr VkFormat kExportFormat = VK_FORMAT_R8G8B8A8_UNORM;
inline constexpr uint32_t kFourccAbgr8888 = 0x34324241; // 'AB24'
inline constexpr uint32_t kBufferCount = bridge::kMaxBuffers;

class FrameBufferSet
{
public:
    // Fails (null + `error`) when the transport cannot be satisfied: the caller then tries the next one.
    static std::unique_ptr<FrameBufferSet> create(VkContext &ctx, uint32_t width, uint32_t height, Transport transport,
                                                  std::string &error);
    ~FrameBufferSet();
    FrameBufferSet(const FrameBufferSet &) = delete;
    FrameBufferSet &operator=(const FrameBufferSet &) = delete;

    Transport transport() const { return m_transport; }
    bool isDmabuf() const { return m_transport != Transport::Shm; }
    uint32_t width() const { return m_width; }
    uint32_t height() const { return m_height; }

    // ---- what the consumer is told ----
    uint32_t fourcc() const { return kFourccAbgr8888; }
    uint64_t modifier() const { return m_modifier; }
    // Plane layout (offset, row pitch) of buffer `index`; dma-buf only.
    bridge::msg::DmabufBuffers::Plane plane(uint32_t index) const { return m_slots[index].plane; }
    // One dma-buf fd per buffer (a single memory plane each). The set keeps ownership.
    std::array<int, kBufferCount> dmabufFds() const;
    // Shared-memory transport: geometry and fd of the ring (valid when !isDmabuf()).
    const bridge::ShmRing &ring() const { return *m_ring; }

    // ---- rendering ----
    VkImage image(uint32_t index) const { return m_slots[index].image; }
    VkImageView view(uint32_t index) const { return m_slots[index].view; }
    VkExtent2D extent() const { return {m_width, m_height}; }

    // Moves buffer `index` to `layout`, discarding its contents (every frame is drawn in full).
    void cmdAcquire(VkCommandBuffer cb, uint32_t index, VkImageLayout layout);
    // After the last write (the image is in `layout`): makes the frame ready for the consumer. Dma-buf: ownership
    // goes to the foreign queue family. Shm: copies the image into the staging buffer.
    void cmdRelease(VkCommandBuffer cb, uint32_t index, VkImageLayout layout);
    // After the frame's fence has signalled. Shm: copies the staging buffer into the ring slot. No-op for dma-bufs.
    void finishFrame(uint32_t index);

private:
    struct Slot {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        GpuImage owned; // Shm: the image lives in VMA instead
        bridge::UniqueFd fd;
        bridge::msg::DmabufBuffers::Plane plane{};
    };

    FrameBufferSet(VkContext &ctx, uint32_t width, uint32_t height, Transport transport);
    void createDmabuf(const std::vector<uint64_t> &modifiers);
    void createShm();
    void destroy();

    VkContext &m_ctx;
    uint32_t m_width;
    uint32_t m_height;
    Transport m_transport;
    uint64_t m_modifier = 0;
    std::array<Slot, kBufferCount> m_slots;
    GpuBuffer m_staging;
    std::optional<bridge::ShmRing> m_ring;
};

} // namespace murasaki::scene
