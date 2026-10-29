#pragma once

// Bridge producer: the engine side of the Murasaki bridge for a host that renders with OpenGL (the Plasma host
// mode of murasaki-surface, tests). It owns, per viewport, three RGBA8 textures with an FBO each, publishes them
// to the consumer as DMA-BUFs (EGL_MESA_image_dma_buf_export, or GBM linear buffers on request) or as a memfd
// ring, tracks which buffer the consumer still holds and answers the consumer's control messages.
//
// Qt-free C++20 + POSIX. EGL is linked, OpenGL entry points are resolved through eglGetProcAddress, so no GL
// headers or libraries are needed. GBM is optional (MURASAKI_BRIDGE_HAVE_GBM): without it RequestLinear falls back
// to shared memory.
//
// Threading: single threaded. Every call that touches OpenGL (construction, addViewport, beginFrame, endFrame,
// resize, service) needs the OpenGL context that renders the frames to be current on the calling thread, on the
// EGL display passed to the constructor. Context state the producer changes is restored before it returns.
//
// Qt host (QQuickRenderControl) notes:
//   - call endFrame() after QQuickRenderControl::endFrame(), once Qt has submitted its commands to the target;
//   - Qt renders with the origin at the bottom-left of the FBO, so pass flipY = true;
//   - service() and resize() may reallocate buffers (RequestLinear, RequestShm, resize), so they need the context
//     current too. Afterwards the textures and FBOs the caller got from beginFrame() are gone: check
//     consumeTargetsChanged() and call beginFrame() again to rebuild the render target.
//
// Frame loop:
//     producer.start();                                   // Hello, Viewport, buffers
//     loop {
//         producer.service();                             // (re-fetch targets if consumeTargetsChanged()) Release, RequestLinear/Shm, other messages -> handler
//         if (auto t = producer.beginFrame(vp)) {         // none: the consumer holds every buffer, skip the frame
//             render into t->framebuffer / t->texture;
//             producer.endFrame(vp, flipY);               // glFinish (or glReadPixels for SHM) + Frame
//         }
//     }

#include "channel.h"
#include "protocol.h"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace murasaki::bridge {

// A consumer message the producer does not handle itself. Resize is delivered too: the host decides the new size
// of its window and then calls Producer::resize().
struct PropertiesJson {
    std::string json;
};
using ControlMessage = std::variant<msg::Pause, msg::Pointer, msg::Resize, msg::Volume, msg::Scaling, msg::Fps,
                                    msg::Quit, PropertiesJson>;
using ControlHandler = std::function<void(const ControlMessage &)>;

enum class Transport {
    Dmabuf,       // textures exported with EGL_MESA_image_dma_buf_export, the driver's own layout
    DmabufLinear, // GBM linear buffers imported as EGLImages and bound to the textures
    Shm,          // memfd ring filled with glReadPixels
};

enum class ServiceResult {
    Ok,
    PeerClosed, // the consumer closed the socket
    Failed,     // protocol violation or socket error, see lastError(); an Error message was sent best effort
};

enum class FrameResult {
    Sent,
    Skipped, // nothing was sent: no frame in progress, or the socket was backed up (the buffer is free again)
    Closed,  // the consumer closed the socket
    Failed,  // see lastError()
};

// Where to render the current frame. Both objects stay valid until the next resize/transport change of the
// viewport; the caller binds them itself.
struct FrameTarget {
    uint32_t buffer = 0;
    uint32_t texture = 0;     // GL_TEXTURE_2D, RGBA8
    uint32_t framebuffer = 0; // with `texture` as colour attachment 0
    uint32_t width = 0;
    uint32_t height = 0;
};

class Producer
{
public:
    // `display` is an EGLDisplay (kept as void * so this header needs no EGL include). `channel` must outlive the
    // producer; start() makes it nonblocking.
    Producer(Channel &channel, void *display);
    ~Producer(); // frees GL objects: make the context current first
    Producer(const Producer &) = delete;
    Producer &operator=(const Producer &) = delete;

    // False when there is no current EGL context on `display` or it lacks the needed entry points.
    bool valid() const { return m_valid; }
    const std::string &lastError() const { return m_error; }

    void setControlHandler(ControlHandler handler) { m_handler = std::move(handler); }

    // Allocates the buffers (DMA-BUF, or SHM when export is unavailable or MURASAKI_BRIDGE_FORCE_SHM is set to
    // something other than "0"; MURASAKI_BRIDGE_NO_GBM makes RequestLinear fall back to SHM). Returns the viewport id, ids are 0, 1, 2... in call order.
    std::optional<uint32_t> addViewport(std::string_view name, uint32_t width, uint32_t height);

    // Sends Hello, then Viewport and the buffer set of every viewport added so far. Viewports added later are
    // announced immediately. A failure leaves the producer dead (no retry): drop it and start over.
    bool start();
    bool started() const { return m_started; }

    // Picks a buffer the consumer does not hold. Nullopt: all buffers are held (or the viewport is unknown, or
    // the connection is gone): skip this frame. Calling it again before endFrame returns the same buffer.
    std::optional<FrameTarget> beginFrame(uint32_t viewport);
    // Completes the frame (glFinish, or a read-back for SHM) and sends Frame. flipY: rows are bottom-up.
    FrameResult endFrame(uint32_t viewport, bool flipY);

    // Reads and handles pending consumer messages (at most `maxMessages`): Release, RequestLinear and RequestShm
    // here, everything else through the control handler.
    ServiceResult service(size_t maxMessages = 64);

    // New size: a new buffer set (new generation) on the same transport, announced to the consumer once started.
    // A frame in progress is abandoned.
    bool resize(uint32_t viewport, uint32_t width, uint32_t height);

    // True once if any buffer set was (re)allocated since the last call: every FrameTarget handed out earlier is
    // stale and the caller must call beginFrame() again.
    bool consumeTargetsChanged() { return std::exchange(m_targetsChanged, false); }

    void sendError(uint32_t code, std::string_view text);

    // ---- inspection -------------------------------------------------------------------------------------
    size_t viewportCount() const { return m_viewports.size(); }
    uint32_t generation(uint32_t viewport) const;
    Transport transport(uint32_t viewport) const;
    uint32_t heldCount(uint32_t viewport) const;
    bool canLinear(); // GBM render node available (opens it on first use)

private:
    struct Gl;
    struct Egl;
    struct GbmDevice;
    struct Viewport;

    Viewport *find(uint32_t id);
    const Viewport *find(uint32_t id) const;
    bool allocate(Viewport &vp, uint32_t width, uint32_t height, Transport wanted);
    bool allocateTextures(Viewport &vp);
    bool attachFramebuffers(Viewport &vp);
    bool exportTextures(Viewport &vp);
    bool importLinearTextures(Viewport &vp);
    void release(Viewport &vp);
    bool sendBuffers(Viewport &vp);
    bool sendBlocking(MsgType type, std::span<const uint8_t> payload, std::span<const int> fds);
    void switchTransport(Viewport &vp, Transport wanted);
    void handle(Message &&message);
    void fail(std::string text);
    void peerGone();

    Channel &m_channel;
    void *m_display;
    bool m_valid = false;
    bool m_started = false;
    bool m_shmOnly = false;
    bool m_dead = false;
    bool m_targetsChanged = false;
    bool m_noGbm = false;
    bool m_peerClosed = false;
    std::string m_error;
    uint64_t m_sequence = 0;
    ControlHandler m_handler;
    std::unique_ptr<Gl> m_gl;
    std::unique_ptr<Egl> m_egl;
    std::unique_ptr<GbmDevice> m_gbm;
    bool m_gbmTried = false;
    std::vector<std::unique_ptr<Viewport>> m_viewports;
};

} // namespace murasaki::bridge
