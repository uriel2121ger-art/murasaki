#pragma once

#include "bridge/channel.h"
#include "bridge/mediaview.h"
#include "bridge/puppetview.h"
#include "bridge/shortcutview.h"
#include "bridge/spanview.h"

#include <QJsonObject>
#include <QObject>
#include <QSize>
#include <QString>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

class QSocketNotifier;

namespace murasaki {

// One buffer set of the viewport (a DmabufBuffers or ShmBuffers message), immutable once published. The render
// thread keeps a reference while it uses it, so the dma-buf fds and the mapped ring stay alive until the last
// user lets go, whatever the GUI thread has moved on to.
struct BridgeBuffers {
    enum class Kind { Dmabuf, Shm };
    static constexpr uint32_t kMaxPlanes = bridge::kMaxPlanes;

    Kind kind = Kind::Shm;
    uint32_t viewport = 0;
    uint32_t generation = 0;
    uint32_t count = 0;
    uint32_t width = 0;
    uint32_t height = 0;

    // Dmabuf: one fourcc and modifier for the whole set; fds are buffer-major, plane-minor.
    uint32_t fourcc = 0;
    uint32_t planes = 0;
    uint64_t modifier = 0;
    uint32_t offset[bridge::kMaxBuffers][kMaxPlanes] = {};
    uint32_t stride[bridge::kMaxBuffers][kMaxPlanes] = {};
    std::vector<bridge::UniqueFd> fds;

    // Shm: the ring is mapped read-only and covers all `count` frames.
    std::optional<bridge::ShmRing> ring;

    int dmabufFd(uint32_t buffer, uint32_t plane) const { return fds[size_t{buffer} * planes + plane].get(); }
};

// A frame the producer announced: which buffer of which set, and how to orient it.
struct BridgeFrame {
    std::shared_ptr<const BridgeBuffers> buffers;
    uint32_t buffer = 0;
    uint64_t seq = 0;
    bool flipY = false;

    uint32_t viewport() const { return buffers->viewport; }
    uint32_t generation() const { return buffers->generation; }
};

// What the render thread asks the GUI thread to put on the wire.
struct BridgeRequest {
    enum class Kind { Release, RequestLinear, RequestShm };
    Kind kind = Kind::Release;
    uint32_t viewport = 0;
    uint32_t buffer = 0;
    uint32_t generation = 0;
};

// The only state shared between the GUI thread (which owns the socket) and the scene-graph render thread (which
// imports buffers). Every member is guarded by one mutex; nothing here touches the socket. Requests from the
// render thread are queued and the GUI thread is woken to send them, so the channel itself stays single-threaded.
class BridgeFrameQueue
{
public:
    // ---- GUI thread ----
    // Publishes a new buffer set. Older frames can no longer be shown and are discarded.
    void setBuffers(std::shared_ptr<const BridgeBuffers> buffers);
    // Stores the newest frame; returns the frame it displaced if that one was never taken (to be released).
    std::optional<BridgeFrame> pushFrame(BridgeFrame frame);
    std::shared_ptr<const BridgeBuffers> buffers() const;
    std::vector<BridgeRequest> takeRequests();
    // `wake` is called (under the lock, from any thread) whenever a request is queued; keep it to a queued post.
    void setWake(std::function<void()> wake);
    void detach(); // after this no wake call is made and requests are dropped

    // ---- render thread ----
    std::optional<BridgeFrame> takeFrame();
    void release(const BridgeFrame &frame);
    // The set of `generation` could not be imported. The first report asks for a linear re-export, the second for
    // shared memory; further ones (and reports for sets that are no longer current) do nothing.
    void reportImportFailure(uint32_t generation);
    // A dma-buf of `generation` imported: the failure staging starts over for later sets.
    void reportImportSuccess(uint32_t generation);

private:
    void post(const BridgeRequest &request);

    mutable std::mutex m_mutex;
    std::shared_ptr<const BridgeBuffers> m_buffers;
    std::optional<BridgeFrame> m_pending;
    std::vector<BridgeRequest> m_requests;
    std::function<void()> m_wake;
    bool m_detached = false;
    int m_failureStage = 0; // 0 none, 1 linear requested, 2 shm requested
    std::optional<uint32_t> m_lastFailedGeneration;
};

// Consumer side of the bridge, Qt Core only. It lives on the GUI thread, reads the socket through a
// QSocketNotifier, validates every message, publishes buffers and frames to the BridgeFrameQueue and sends the
// control messages. Any protocol violation (bad message, wrong order, version mismatch, over-large datagram)
// or a closed socket ends the connection: `lost` is emitted once with the reason and the client goes inert.
class BridgeClient : public QObject
{
    Q_OBJECT

public:
    explicit BridgeClient(bridge::UniqueFd fd, QObject *parent = nullptr);
    ~BridgeClient() override;

    std::shared_ptr<BridgeFrameQueue> frames() const { return m_queue; }
    bool isReady() const { return m_ready; }   // Hello accepted
    bool isOpen() const { return m_channel.valid(); }
    std::optional<uint32_t> viewportId() const { return m_viewport; }
    QSize viewportSize() const { return m_viewportSize; }
    QString lastEngineError() const { return m_lastError; }
    // How the other end is called in failure reasons ("scene engine protocol violation: ...").
    void setPeerName(const QString &name) { m_peerName = name; }
    // How the socket is called in failure reasons ("invalid <name>", "<name> error").
    void setSocketName(const QString &name) { m_socketName = name; }

    void sendPause(bool paused);
    void sendVolume(uint32_t volume); // 0..128
    void sendScaling(uint32_t mode);  // msg::ScalingMode
    void sendFps(uint32_t fps);
    // The version the producer announced in its Hello (0 before it did).
    uint32_t peerVersion() const { return m_peerVersion; }
    bool supportsSpan() const { return m_peerVersion >= bridge::kSpanViewVersion; }
    // Spanning: this viewport shows (x, y, w, h) of a canvasW by canvasH canvas, logical units (see
    // bridge::msg::SpanView). Returns false, sending nothing, when enabled and the producer predates it. Turning it
    // off for such a producer is a successful no-op.
    bool sendSpanView(bool enabled, double canvasW, double canvasH, double x, double y, double w, double h);
    // The scene told us it uses the media (kMediaWantsType); false until it does.
    bool wantsMedia() const { return m_wantsMedia; }
    bool supportsMedia() const { return m_peerVersion >= bridge::kMediaVersion; }
    // The media state (bridge/mediaview.h): `json` is what the host built (bounded by kMaxMediaJson). Nothing is sent to a producer
    // that predates the media messages.
    void sendMedia(const QByteArray &json);
    // The cover of the track: width x height RGBA8 (each side 1..kMaxThumbnailSide), or none (0 x 0, no pixels). A wrong size is not sent.
    void sendMediaThumbnail(uint32_t width, uint32_t height, const QByteArray &rgba, uint32_t generation);
    bool supportsShortcut() const { return m_peerVersion >= bridge::kShortcutVersion; }
    // A puppet engine announces version 6 and sends HitMask messages (bridge/puppetview.h); a producer that announced less never gets one honoured.
    bool supportsPuppet() const { return m_peerVersion >= bridge::kPuppetVersion; }
    // The icon of the application chosen for the property `name` (bridge/shortcutview.h): width x height RGBA8, each side 1..kMaxShortcutIconSide,
    // or none (0 x 0, no pixels). A wrong size or name is not sent; nothing is sent to a producer that predates version 5.
    void sendShortcutIcon(const QByteArray &name, uint32_t width, uint32_t height, const QByteArray &rgba, uint32_t generation);
    void sendResize(uint32_t width, uint32_t height);
    void sendProperties(const QJsonObject &changed);
    void sendPointer(int x, int y, uint32_t buttons); // lossy: dropped while the socket is backed up
    void sendQuit();
    void close();

signals:
    void ready();          // Hello accepted
    void frameAvailable(); // a newer frame is waiting in frames()
    void lost(const QString &reason);
    void mediaWanted(bool wants); // the scene announced whether it uses the media
    // A script of the scene asked to start the application of the property `name` (engine.openUserShortcut). Only a request: the
    // host decides whether anything starts. `name` passed the wire checks (bridge/shortcutview.h).
    void shortcutLaunchRequested(const QByteArray &name);
    // The coarse footprint of a puppet on the picture the producer just drew: cols x rows bits of `cell` pixel cells (bit 0 of a byte first, row 0 first). Only a mask made for the buffer set that is current
    // (same generation) and whose grid is the one that set's size gives (cols = ceil(width / cell), rows likewise) is passed on; others are dropped without ending the connection.
    void hitMaskReceived(uint32_t cols, uint32_t rows, uint32_t cell, const QByteArray &bits);

private:
    void onReadable();
    void onWritable();
    void handle(bridge::Message &&message);
    void handleHello(const bridge::Message &message);
    void raiseCeiling(const QSize &size);
    void handleViewport(const bridge::Message &message);
    void handleDmabufBuffers(bridge::Message &message);
    void handleShmBuffers(bridge::Message &message);
    void handleFrame(const bridge::Message &message);
    void handleError(const bridge::Message &message);
    void violation(const QString &reason);
    void fail(const QString &reason);
    void flushRequests();
    void enqueue(bridge::MsgType type, std::vector<uint8_t> payload, std::vector<bridge::UniqueFd> fds = {});
    void flush();
    template <bridge::Wire T>
    void sendMessage(const T &m)
    {
        enqueue(T::kType, bridge::encode(m));
    }
    bool ours(uint32_t viewport) const { return m_viewport && *m_viewport == viewport; }

    struct Outgoing {
        bridge::MsgType type;
        std::vector<uint8_t> payload;
        std::vector<bridge::UniqueFd> fds; // sent with the datagram (a media cover)
    };

    bridge::Channel m_channel;
    std::unique_ptr<QSocketNotifier> m_readNotifier;
    std::unique_ptr<QSocketNotifier> m_writeNotifier;
    std::shared_ptr<BridgeFrameQueue> m_queue;
    std::deque<Outgoing> m_outbox;
    std::set<uint32_t> m_knownViewports;
    std::optional<uint32_t> m_viewport; // the first announced viewport is the one shown
    QSize m_viewportSize;
    // Largest size announced or requested so far. A buffer set is a Resize answer, not a Viewport, so its size may
    // differ from the announced one; it must stay within twice this (a producer cannot make us allocate more).
    QSize m_sizeCeiling;
    QString m_lastError;
    QString m_peerName = QStringLiteral("bridge host");
    QString m_socketName = QStringLiteral("bridge socket");
    std::optional<QSize> m_pendingResize; // asked for before the viewport was announced
    uint32_t m_peerVersion = 0;
    bool m_wantsMedia = false;
    bool m_ready = false;
    bool m_dead = false;
    bool m_lostEmitted = false;
};

} // namespace murasaki
