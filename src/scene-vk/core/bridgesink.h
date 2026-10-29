#pragma once

// The engine side of the Murasaki bridge for a Vulkan renderer: the counterpart of bridge/producer.h (which serves
// OpenGL hosts). One viewport, kBufferCount buffers, the same message flow:
//
//     sink.start(w, h, name);                    // Hello, Viewport, DmabufBuffers or ShmBuffers
//     loop {
//         sink.service();                        // Release, RequestLinear, RequestShm; the rest goes to the handler
//         if (auto i = sink.acquire()) {         // a buffer the consumer does not hold
//             render into sink.buffers() image i; wait for the GPU; sink.buffers().finishFrame(i);
//             sink.present(*i);                  // Frame
//         }
//     }
//
// Single threaded. A buffer is "held" from the Frame message until the matching Release (same generation); the
// generation changes with every new buffer set (Resize, RequestLinear, RequestShm), so a late Release for an old
// set can never free a buffer of the current one.

#include "core/framebuffers.h"

#include "bridge/channel.h"
#include "bridge/mediaview.h"
#include "bridge/shortcutview.h"
#include "bridge/protocol.h"
#include "bridge/spanview.h"

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace murasaki::scene {

struct PropertiesJson {
    std::string json;
};

// The media state the host follows (bridge/mediaview.h), as the JSON it sent.
struct MediaJson {
    std::string json;
};

// The cover of the track, copied out of the sealed memfd that carried it. No pixels: the track has no cover.
struct MediaThumbnailData {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t generation = 0;
    std::vector<uint8_t> rgba;
};

// The icon of the application chosen for a shortcut property, copied out of the sealed memfd that carried it. No pixels: cleared.
struct ShortcutIconData {
    std::string name;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t generation = 0;
    std::vector<uint8_t> rgba;
};

// A consumer message the sink does not handle itself. Resize is delivered too: the engine decides what to do with
// the new size and then calls BridgeSink::resize().
using ControlMessage = std::variant<bridge::msg::Pause, bridge::msg::Pointer, bridge::msg::Resize, bridge::msg::Volume,
                                    bridge::msg::Scaling, bridge::msg::Fps, bridge::msg::Quit, bridge::msg::SpanView,
                                    PropertiesJson, MediaJson, MediaThumbnailData, ShortcutIconData>;

enum class ServiceResult {
    Ok,
    PeerClosed, // the consumer closed the socket
    Failed,     // protocol violation or socket error, see lastError()
};

enum class PresentResult {
    Sent,
    Skipped, // the socket was backed up: the frame was dropped and its buffer is free again
    Closed,
    Failed,
};

// bridge error codes of the Error message
inline constexpr uint32_t kErrorProtocol = 1;
inline constexpr uint32_t kErrorInit = 2;
inline constexpr uint32_t kErrorRuntime = 3;

class BridgeSink
{
public:
    // `channel` is the engine end of the socketpair; the sink makes it nonblocking.
    BridgeSink(VkContext &ctx, bridge::Channel channel);

    // Allocates the first buffer set (dma-buf unless MURASAKI_BRIDGE_FORCE_SHM is set or the device cannot export),
    // then sends Hello, Viewport and the buffers.
    bool start(uint32_t width, uint32_t height, std::string_view name);

    // The protocol version Hello announces (default: the shortcut messages' version, 5). A puppet engine announces bridge::kPuppetVersion, which adds the HitMask message. Before start().
    void setAnnouncedVersion(uint32_t version) { m_announcedVersion = version; }
    void setHandler(std::function<void(const ControlMessage &)> handler) { m_handler = std::move(handler); }
    const std::string &lastError() const { return m_error; }

    ServiceResult service(size_t maxMessages = 64);
    // Blocks until the consumer sent something or `timeout` passed.
    void waitForMessage(std::chrono::nanoseconds timeout) const;

    FrameBufferSet &buffers() { return *m_set; }
    uint32_t generation() const { return m_generation; }
    uint32_t width() const { return m_set->width(); }
    uint32_t height() const { return m_set->height(); }

    // A buffer the consumer does not hold; the same one until present() or release of the pending frame.
    std::optional<uint32_t> acquire() const;
    PresentResult present(uint32_t buffer, bool flipY = false);

    // New buffer set of another size on the same transport (new generation), announced to the consumer.
    bool resize(uint32_t width, uint32_t height);
    // True once after the set was replaced (resize or a transport change): things bound to its images are stale.
    bool consumeSetChanged() { return std::exchange(m_setChanged, false); }

    void sendError(uint32_t code, std::string_view text);
    // A message of Murasaki's extension of the protocol (producer -> consumer), best effort (bridge/puppetview.h: the hit mask of a puppet). False when the socket is gone or backed up.
    bool sendRaw(bridge::MsgType type, std::span<const uint8_t> payload);
    // Whether the scene uses the media (bridge/mediaview.h MediaWants); the consumer sends none until it knows.
    void sendMediaWants(bool wants);
    // A script asked to start the application of the shortcut property `name` (1 to 63 bytes: the caller checked). Only a request to the
    // host, which decides. False when it could not be sent (the peer predates version 5 or the socket is gone).
    bool sendShortcutLaunch(std::string_view name);
    uint32_t heldCount() const;

private:
    bool install(uint32_t width, uint32_t height, Transport wanted);
    bool sendBuffers();
    bool sendReliable(bridge::MsgType type, std::span<const uint8_t> payload, std::span<const int> fds);
    void handle(bridge::Message &&message);
    void fail(std::string text);

    VkContext &m_ctx;
    bridge::Channel m_channel;
    std::unique_ptr<FrameBufferSet> m_set;
    std::function<void(const ControlMessage &)> m_handler;
    std::array<bool, kBufferCount> m_held{};
    uint32_t m_generation = 0;
    uint32_t m_launchSerial = 0;
    uint32_t m_announcedVersion = bridge::kShortcutVersion;
    uint64_t m_sequence = 0;
    bool m_started = false;
    bool m_forceShm = false;
    bool m_setChanged = false;
    bool m_peerClosed = false;
    bool m_failed = false;
    std::string m_error;
};

} // namespace murasaki::scene
