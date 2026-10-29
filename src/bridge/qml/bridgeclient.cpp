#include "bridgeclient.h"

#include <QJsonDocument>
#include <QMetaObject>
#include <QSocketNotifier>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string_view>
#include <utility>

namespace murasaki {

using namespace bridge;

namespace {
constexpr size_t kMaxOutbox = 512;
// One buffer is always on screen, so a producer needs a second one to render into.
constexpr uint32_t kMinBuffers = 2;
constexpr size_t kMaxViewports = 8; // the engine announces one per screen it renders; more is a misbehaving peer
constexpr size_t kDrainBatch = 64; // messages per notifier wake-up; the level-triggered notifier fires again

// Generations wrap, so "newer" is a signed distance.
bool newerGeneration(uint32_t candidate, uint32_t current)
{
    return static_cast<int32_t>(candidate - current) > 0;
}
} // namespace

// ---- BridgeFrameQueue -----------------------------------------------------------------------------------

void BridgeFrameQueue::setBuffers(std::shared_ptr<const BridgeBuffers> buffers)
{
    std::lock_guard lock(m_mutex);
    if (m_buffers && buffers && m_buffers->kind != buffers->kind) {
        m_failureStage = 0; // a switch DMA-BUF <-> SHM starts a new negotiation
        m_lastFailedGeneration.reset();
    }
    m_buffers = std::move(buffers);
    m_pending.reset();
}

std::optional<BridgeFrame> BridgeFrameQueue::pushFrame(BridgeFrame frame)
{
    std::lock_guard lock(m_mutex);
    std::optional<BridgeFrame> displaced = std::move(m_pending);
    m_pending = std::move(frame);
    return displaced;
}

std::shared_ptr<const BridgeBuffers> BridgeFrameQueue::buffers() const
{
    std::lock_guard lock(m_mutex);
    return m_buffers;
}

std::vector<BridgeRequest> BridgeFrameQueue::takeRequests()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_requests, {});
}

void BridgeFrameQueue::setWake(std::function<void()> wake)
{
    std::lock_guard lock(m_mutex);
    m_wake = std::move(wake);
}

void BridgeFrameQueue::detach()
{
    std::lock_guard lock(m_mutex);
    m_detached = true;
    m_wake = nullptr;
    m_requests.clear();
}

std::optional<BridgeFrame> BridgeFrameQueue::takeFrame()
{
    std::lock_guard lock(m_mutex);
    return std::exchange(m_pending, std::nullopt);
}

void BridgeFrameQueue::post(const BridgeRequest &request)
{
    // Caller holds m_mutex.
    if (m_detached)
        return;
    m_requests.push_back(request);
    if (m_wake)
        m_wake();
}

void BridgeFrameQueue::release(const BridgeFrame &frame)
{
    std::lock_guard lock(m_mutex);
    post({BridgeRequest::Kind::Release, frame.viewport(), frame.buffer, frame.generation()});
}

void BridgeFrameQueue::reportImportFailure(uint32_t generation)
{
    std::lock_guard lock(m_mutex);
    if (!m_buffers || m_buffers->generation != generation || m_lastFailedGeneration == generation
        || m_buffers->kind != BridgeBuffers::Kind::Dmabuf || m_failureStage >= 2)
        return;
    m_lastFailedGeneration = generation;
    ++m_failureStage;
    post({m_failureStage == 1 ? BridgeRequest::Kind::RequestLinear : BridgeRequest::Kind::RequestShm,
          m_buffers->viewport, 0, generation});
}

void BridgeFrameQueue::reportImportSuccess(uint32_t generation)
{
    std::lock_guard lock(m_mutex);
    if (m_buffers && m_buffers->generation == generation) {
        m_failureStage = 0;
        m_lastFailedGeneration.reset();
    }
}

// ---- BridgeClient ---------------------------------------------------------------------------------

BridgeClient::BridgeClient(UniqueFd fd, QObject *parent)
    : QObject(parent)
    , m_channel(std::move(fd))
    , m_queue(std::make_shared<BridgeFrameQueue>())
{
    if (!m_channel.valid() || !m_channel.setNonblocking(true)) {
        m_dead = true;
        QMetaObject::invokeMethod(this, [this] { fail(QStringLiteral("invalid ") + m_socketName); },
                                  Qt::QueuedConnection);
        return;
    }
    m_readNotifier = std::make_unique<QSocketNotifier>(m_channel.fd(), QSocketNotifier::Read);
    connect(m_readNotifier.get(), &QSocketNotifier::activated, this, &BridgeClient::onReadable);
    m_writeNotifier = std::make_unique<QSocketNotifier>(m_channel.fd(), QSocketNotifier::Write);
    m_writeNotifier->setEnabled(false);
    connect(m_writeNotifier.get(), &QSocketNotifier::activated, this, &BridgeClient::onWritable);
    // Render-thread requests arrive from any thread; the wake is only a queued post to this object.
    m_queue->setWake([this] { QMetaObject::invokeMethod(this, &BridgeClient::flushRequests, Qt::QueuedConnection); });
}

BridgeClient::~BridgeClient()
{
    m_queue->detach();
}

void BridgeClient::close()
{
    m_dead = true;
    // close() may run inside a notifier's own activated() signal, so the notifiers are deleted later.
    for (auto *notifier : {m_readNotifier.release(), m_writeNotifier.release()}) {
        if (notifier) {
            notifier->setEnabled(false);
            notifier->deleteLater();
        }
    }
    m_outbox.clear();
    m_queue->detach();
    m_channel = Channel();
}

void BridgeClient::fail(const QString &reason)
{
    close();
    if (m_lostEmitted)
        return;
    m_lostEmitted = true;
    emit lost(reason);
}

void BridgeClient::violation(const QString &reason)
{
    fail(m_peerName + QStringLiteral(" protocol violation: ") + reason);
}

void BridgeClient::onReadable()
{
    if (m_dead)
        return;
    const DrainResult result = m_channel.drain([this](Message &&m) {
        if (!m_dead)
            handle(std::move(m));
    }, kDrainBatch);
    if (m_dead)
        return;
    switch (result.last) {
    case ChannelStatus::Ok:
    case ChannelStatus::WouldBlock:
        break;
    case ChannelStatus::Closed: {
        QString reason = m_peerName + QStringLiteral(" closed the connection");
        if (!lastEngineError().isEmpty())
            reason += QStringLiteral(": ") + lastEngineError();
        fail(reason);
        break;
    }
    case ChannelStatus::TooLarge:
    case ChannelStatus::Malformed:
        violation(QStringLiteral("malformed or oversize datagram"));
        break;
    case ChannelStatus::Error:
        fail(m_socketName + QStringLiteral(" error"));
        break;
    }
}

void BridgeClient::handle(Message &&message)
{
    const auto type = static_cast<MsgType>(message.type);
    if (!m_ready && type != MsgType::Hello) {
        // Unknown types are ignored, but nothing real may precede Hello.
        switch (type) {
        case MsgType::Viewport:
        case MsgType::DmabufBuffers:
        case MsgType::ShmBuffers:
        case MsgType::Frame:
            violation(QStringLiteral("message before Hello"));
            return;
        default:
            break;
        }
    }
    switch (type) {
    case MsgType::Hello:
        handleHello(message);
        break;
    case MsgType::Viewport:
        handleViewport(message);
        break;
    case MsgType::DmabufBuffers:
        handleDmabufBuffers(message);
        break;
    case MsgType::ShmBuffers:
        handleShmBuffers(message);
        break;
    case MsgType::Frame:
        handleFrame(message);
        break;
    case MsgType::Error:
        handleError(message);
        break;
    default:
        if (message.type == static_cast<uint32_t>(kMediaWantsType) && m_peerVersion >= kMediaVersion) {
            msg::MediaWants wants{};
            if (decode(message.payload, wants) && msg::validate(wants, message.fds.size()) && (wants.wants != 0) != m_wantsMedia) {
                m_wantsMedia = wants.wants != 0;
                emit mediaWanted(m_wantsMedia);
            }
        }
        if (message.type == static_cast<uint32_t>(kShortcutLaunchType) && m_peerVersion >= kShortcutVersion) {
            msg::ShortcutLaunch launch{};
            if (decode(message.payload, launch) && msg::validate(launch, message.fds.size())) {
                emit shortcutLaunchRequested(QByteArray(launch.name));
            }
        }
        if (message.type == static_cast<uint32_t>(kHitMaskType) && m_peerVersion >= kPuppetVersion && message.fds.empty()) {
            const auto mask = decodeHitMask(message.payload);
            const auto current = m_queue->buffers();
            if (mask && current && mask->header.generation == current->generation) {
                const HitMaskHeader &h = mask->header;
                const auto cells = [](uint32_t size, uint32_t cell) { return static_cast<uint32_t>((static_cast<uint64_t>(size) + cell - 1) / cell); }; // (64 bit: no wrap whatever the size)
                if (h.cols == cells(current->width, h.cell) && h.rows == cells(current->height, h.cell))
                    emit hitMaskReceived(h.cols, h.rows, h.cell, QByteArray(reinterpret_cast<const char *>(mask->bits.data()), static_cast<qsizetype>(mask->bits.size())));
            }
        }
        break; // unknown or consumer-direction types are ignored
    }
}

void BridgeClient::handleHello(const Message &message)
{
    msg::Hello hello{};
    if (!decode(message.payload, hello) || !msg::validate(hello, message.fds.size())) {
        violation(QStringLiteral("bad Hello"));
        return;
    }
    if (m_ready) {
        violation(QStringLiteral("duplicate Hello"));
        return;
    }
    // The base protocol, or the one with SpanView, the media messages, the shortcut messages and the puppet's HitMask (6); a producer of the base protocol announces the base one.
    if (hello.version < kBridgeVersion || hello.version > kPuppetVersion) {
        violation(QStringLiteral("bridge version %1, expected %2 to %3")
                      .arg(hello.version)
                      .arg(kBridgeVersion)
                      .arg(kPuppetVersion));
        return;
    }
    m_peerVersion = hello.version;
    m_ready = true;
    emit ready();
}

void BridgeClient::raiseCeiling(const QSize &size)
{
    m_sizeCeiling = m_sizeCeiling.expandedTo(size);
}

void BridgeClient::handleViewport(const Message &message)
{
    msg::Viewport vp{};
    if (!decode(message.payload, vp) || !msg::validate(vp, message.fds.size()) || vp.width < 1 || vp.height < 1
        || vp.width > kMaxDimension || vp.height > kMaxDimension) {
        violation(QStringLiteral("bad Viewport"));
        return;
    }
    if (!m_knownViewports.contains(vp.id) && m_knownViewports.size() >= kMaxViewports) {
        violation(QStringLiteral("too many viewports"));
        return;
    }
    m_knownViewports.insert(vp.id);
    if (!m_viewport)
        m_viewport = vp.id;
    if (ours(vp.id)) {
        m_viewportSize = QSize(static_cast<int>(vp.width), static_cast<int>(vp.height));
        raiseCeiling(m_viewportSize);
        if (m_pendingResize) {
            const QSize wanted = *std::exchange(m_pendingResize, std::nullopt);
            if (wanted != m_viewportSize)
                sendResize(static_cast<uint32_t>(wanted.width()), static_cast<uint32_t>(wanted.height()));
        }
    }
}

void BridgeClient::handleDmabufBuffers(Message &message)
{
    msg::DmabufBuffers m{};
    if (!decode(message.payload, m) || !msg::validate(m, message.fds.size()) || !m_knownViewports.contains(m.viewport)) {
        violation(QStringLiteral("bad DmabufBuffers"));
        return;
    }
    if (!ours(m.viewport))
        return;
    if (m.count < kMinBuffers) {
        violation(QStringLiteral("a buffer set needs at least %1 buffers").arg(kMinBuffers));
        return;
    }
    if (static_cast<qint64>(m.width) > 2 * m_sizeCeiling.width()
        || static_cast<qint64>(m.height) > 2 * m_sizeCeiling.height()) {
        violation(QStringLiteral("buffer set larger than twice the viewport"));
        return;
    }
    if (const auto current = m_queue->buffers(); current && !newerGeneration(m.generation, current->generation)) {
        violation(QStringLiteral("buffer generation went backwards"));
        return;
    }
    auto set = std::make_shared<BridgeBuffers>();
    set->kind = BridgeBuffers::Kind::Dmabuf;
    set->viewport = m.viewport;
    set->generation = m.generation;
    set->count = m.count;
    set->width = m.width;
    set->height = m.height;
    set->fourcc = m.fourcc;
    set->planes = m.planes;
    set->modifier = m.modifier;
    for (uint32_t b = 0; b < m.count; ++b) {
        for (uint32_t p = 0; p < m.planes; ++p) {
            set->offset[b][p] = m.plane[b][p].offset;
            set->stride[b][p] = m.plane[b][p].stride;
        }
    }
    set->fds = std::move(message.fds);
    m_queue->setBuffers(std::move(set));
}

void BridgeClient::handleShmBuffers(Message &message)
{
    msg::ShmBuffers m{};
    if (!decode(message.payload, m) || !msg::validate(m, message.fds.size()) || !m_knownViewports.contains(m.viewport)) {
        violation(QStringLiteral("bad ShmBuffers"));
        return;
    }
    if (!ours(m.viewport))
        return;
    if (m.count < kMinBuffers) {
        violation(QStringLiteral("a buffer set needs at least %1 buffers").arg(kMinBuffers));
        return;
    }
    if (static_cast<qint64>(m.width) > 2 * m_sizeCeiling.width()
        || static_cast<qint64>(m.height) > 2 * m_sizeCeiling.height()) {
        violation(QStringLiteral("buffer set larger than twice the viewport"));
        return;
    }
    if (const auto current = m_queue->buffers(); current && !newerGeneration(m.generation, current->generation)) {
        violation(QStringLiteral("buffer generation went backwards"));
        return;
    }
    ShmError error = ShmError::Ok;
    auto ring = ShmRing::open(std::move(message.fds.front()), {m.count, m.width, m.height, m.stride}, &error);
    if (!ring) {
        violation(QStringLiteral("unusable shared memory ring (error %1)").arg(static_cast<int>(error)));
        return;
    }
    auto set = std::make_shared<BridgeBuffers>();
    set->kind = BridgeBuffers::Kind::Shm;
    set->viewport = m.viewport;
    set->generation = m.generation;
    set->count = m.count;
    set->width = m.width;
    set->height = m.height;
    set->ring = std::move(ring);
    m_queue->setBuffers(std::move(set));
}

void BridgeClient::handleFrame(const Message &message)
{
    msg::Frame m{};
    if (!decode(message.payload, m) || !msg::validate(m, message.fds.size()) || !m_knownViewports.contains(m.viewport)) {
        violation(QStringLiteral("bad Frame"));
        return;
    }
    if (!ours(m.viewport))
        return;
    const auto set = m_queue->buffers();
    // Frames of an older (or not yet announced) generation are stale: dropped, and nothing is released since the
    // producer ignores a Release for a ring it no longer has.
    if (!set || set->generation != m.generation)
        return;
    if (m.buffer >= set->count) {
        violation(QStringLiteral("Frame buffer index out of range"));
        return;
    }
    BridgeFrame frame{set, m.buffer, m.seq, m.flipY != 0};
    if (auto displaced = m_queue->pushFrame(std::move(frame)))
        sendMessage(msg::Release{displaced->viewport(), displaced->buffer, displaced->generation()}); // never shown
    emit frameAvailable();
}

void BridgeClient::handleError(const Message &message)
{
    msg::Error m{};
    if (!decode(message.payload, m) || !msg::validate(m, message.fds.size())) {
        violation(QStringLiteral("bad Error"));
        return;
    }
    m_lastError = QString::fromUtf8(getString(m.text).data(), static_cast<qsizetype>(getString(m.text).size()));
}

void BridgeClient::flushRequests()
{
    if (m_dead)
        return;
    for (const BridgeRequest &r : m_queue->takeRequests()) {
        switch (r.kind) {
        case BridgeRequest::Kind::Release:
            sendMessage(msg::Release{r.viewport, r.buffer, r.generation});
            break;
        case BridgeRequest::Kind::RequestLinear:
            sendMessage(msg::RequestLinear{r.viewport});
            break;
        case BridgeRequest::Kind::RequestShm:
            sendMessage(msg::RequestShm{r.viewport});
            break;
        }
    }
}

// ---- sending -------------------------------------------------------------------------------------------

void BridgeClient::enqueue(MsgType type, std::vector<uint8_t> payload, std::vector<UniqueFd> fds)
{
    if (m_dead || !m_channel.valid())
        return;
    if (m_outbox.size() >= kMaxOutbox) {
        fail(m_peerName + QStringLiteral(" is not reading its socket"));
        return;
    }
    m_outbox.push_back({type, std::move(payload), std::move(fds)});
    flush();
}

void BridgeClient::flush()
{
    while (!m_dead && !m_outbox.empty()) {
        const Outgoing &front = m_outbox.front();
        std::vector<int> fds;
        for (const UniqueFd &fd : front.fds)
            fds.push_back(fd.get());
        const ChannelStatus status = m_channel.send(front.type, front.payload, fds);
        if (status == ChannelStatus::WouldBlock) {
            if (m_writeNotifier)
                m_writeNotifier->setEnabled(true);
            return;
        }
        if (status == ChannelStatus::Closed) {
            // The read side reports the closed peer with the engine's last words; just stop writing.
            m_outbox.clear();
            break;
        }
        m_outbox.pop_front(); // sent, or an unsendable message that retrying would not fix
    }
    if (m_writeNotifier)
        m_writeNotifier->setEnabled(false);
}

void BridgeClient::onWritable()
{
    flush();
}

void BridgeClient::sendPause(bool paused)
{
    sendMessage(msg::Pause{paused ? 1u : 0u});
}

void BridgeClient::sendVolume(uint32_t volume)
{
    sendMessage(msg::Volume{std::min<uint32_t>(volume, 128)});
}

void BridgeClient::sendScaling(uint32_t mode)
{
    sendMessage(msg::Scaling{mode});
}

bool BridgeClient::sendSpanView(bool enabled, double canvasW, double canvasH, double x, double y, double w, double h)
{
    if (!supportsSpan())
        return !enabled;
    const msg::SpanView view = msg::makeSpanView(enabled, canvasW, canvasH, x, y, w, h);
    if (!msg::validate(view, 0))
        return false;
    sendMessage(view);
    return true;
}

void BridgeClient::sendFps(uint32_t fps)
{
    sendMessage(msg::Fps{fps});
}

void BridgeClient::sendResize(uint32_t width, uint32_t height)
{
    raiseCeiling(QSize(static_cast<int>(std::min(width, kMaxDimension)), static_cast<int>(std::min(height, kMaxDimension))));
    if (!m_viewport) {
        m_pendingResize = QSize(static_cast<int>(width), static_cast<int>(height));
        return;
    }
    sendMessage(msg::Resize{*m_viewport, std::clamp<uint32_t>(width, 1, kMaxDimension),
                            std::clamp<uint32_t>(height, 1, kMaxDimension)});
}

void BridgeClient::sendMedia(const QByteArray &json)
{
    if (!supportsMedia() || json.size() > static_cast<qsizetype>(kMaxMediaJson))
        return;
    auto payload = encodeProperties(std::string_view(json.constData(), static_cast<size_t>(json.size())));
    if (payload) {
        std::erase_if(m_outbox, [](const Outgoing &o) { return o.type == kMediaType; }); // the newest state replaces one still waiting
        enqueue(kMediaType, std::move(*payload));
    }
}

void BridgeClient::sendMediaThumbnail(uint32_t width, uint32_t height, const QByteArray &rgba, uint32_t generation)
{
    if (!supportsMedia())
        return;
    std::erase_if(m_outbox, [](const Outgoing &o) { return o.type == kMediaThumbnailType; }); // (a cover is up to 1 MiB: none piles up)
    const msg::MediaThumbnail header{width, height, generation, 0};
    if (width == 0 && height == 0) {
        enqueue(kMediaThumbnailType, std::vector<uint8_t>(asBytes(header).begin(), asBytes(header).end()));
        return;
    }
    if (!msg::validate(header, 1) || static_cast<uint64_t>(rgba.size()) != uint64_t{width} * height * 4)
        return;
    UniqueFd fd = sealedPixelsFd({reinterpret_cast<const uint8_t *>(rgba.constData()), static_cast<size_t>(rgba.size())});
    if (!fd)
        return;
    std::vector<UniqueFd> fds;
    fds.push_back(std::move(fd));
    enqueue(kMediaThumbnailType, std::vector<uint8_t>(asBytes(header).begin(), asBytes(header).end()), std::move(fds));
}

void BridgeClient::sendShortcutIcon(const QByteArray &name, uint32_t width, uint32_t height, const QByteArray &rgba, uint32_t generation)
{
    if (!supportsShortcut() || name.isEmpty() || name.size() >= static_cast<qsizetype>(bridge::kShortcutNameBytes))
        return;
    msg::ShortcutIcon header{width, height, generation, 0, {}};
    msg::setShortcutName(header, std::string_view(name.constData(), static_cast<size_t>(name.size())));
    if (!msg::validate(header, width == 0 && height == 0 ? 0 : 1))
        return;
    const auto bytes = std::vector<uint8_t>(asBytes(header).begin(), asBytes(header).end());
    // (one icon per slot waits: a newer one replaces it)
    std::erase_if(m_outbox, [&](const Outgoing &o) {
        return o.type == kShortcutIconType && o.payload.size() == bytes.size()
            && std::memcmp(o.payload.data() + offsetof(msg::ShortcutIcon, name), header.name, kShortcutNameBytes) == 0;
    });
    if (width == 0 && height == 0) {
        enqueue(kShortcutIconType, bytes);
        return;
    }
    if (static_cast<uint64_t>(rgba.size()) != uint64_t{width} * height * 4)
        return;
    UniqueFd fd = sealedPixelsFd({reinterpret_cast<const uint8_t *>(rgba.constData()), static_cast<size_t>(rgba.size())});
    if (!fd)
        return;
    std::vector<UniqueFd> fds;
    fds.push_back(std::move(fd));
    enqueue(kShortcutIconType, bytes, std::move(fds));
}

void BridgeClient::sendProperties(const QJsonObject &changed)
{
    if (changed.isEmpty())
        return;
    const QByteArray json = QJsonDocument(changed).toJson(QJsonDocument::Compact);
    auto payload = encodeProperties(std::string_view(json.constData(), static_cast<size_t>(json.size())));
    if (payload)
        enqueue(MsgType::Properties, std::move(*payload));
}

void BridgeClient::sendPointer(int x, int y, uint32_t buttons)
{
    if (!m_viewport || !m_outbox.empty())
        return;
    constexpr int lim = static_cast<int>(kMaxDimension);
    sendMessage(msg::Pointer{*m_viewport, std::clamp(x, -lim, lim), std::clamp(y, -lim, lim), buttons & 7u});
}

void BridgeClient::sendQuit()
{
    sendMessage(msg::Quit{0});
}

} // namespace murasaki
