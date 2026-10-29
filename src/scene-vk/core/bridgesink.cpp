#include "core/bridgesink.h"

#include "util/log.h"

#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace murasaki::scene {

namespace msg = bridge::msg;

namespace {

bool forceShmFromEnvironment()
{
    const char *forced = std::getenv("MURASAKI_BRIDGE_FORCE_SHM");
    return forced && forced[0] != '\0' && std::strcmp(forced, "0") != 0;
}

const char *transportName(Transport t)
{
    switch (t) {
    case Transport::Dmabuf: return "dma-buf";
    case Transport::DmabufLinear: return "linear dma-buf";
    case Transport::Shm: return "shared memory";
    }
    return "?";
}

} // namespace

BridgeSink::BridgeSink(VkContext &ctx, bridge::Channel channel) : m_ctx(ctx), m_channel(std::move(channel))
{
    m_forceShm = forceShmFromEnvironment();
    if (m_forceShm) {
        logInfo("MURASAKI_BRIDGE_FORCE_SHM is set, frames travel through shared memory");
    }
}

bool BridgeSink::start(uint32_t width, uint32_t height, std::string_view name)
{
    if (!m_channel.valid() || !m_channel.setNonblocking(true)) {
        m_error = "cannot use the bridge socket";
        return false;
    }
    const Transport wanted = (m_forceShm || !m_ctx.dmabufExport()) ? Transport::Shm : Transport::Dmabuf;
    if (!install(width, height, wanted)) {
        return false;
    }

    msg::Hello hello{};
    hello.version = m_announcedVersion; // by default the base protocol plus SpanView, the media messages and the shortcut ones
    hello.pid = static_cast<uint32_t>(::getpid());
    if (m_channel.send(hello) != bridge::ChannelStatus::Ok) {
        m_error = "cannot send Hello over the bridge socket";
        return false;
    }
    msg::Viewport viewport{};
    viewport.id = 0;
    viewport.width = width;
    viewport.height = height;
    bridge::setString(viewport.name, name);
    if (m_channel.send(viewport) != bridge::ChannelStatus::Ok) {
        m_error = "cannot send Viewport over the bridge socket";
        return false;
    }
    m_started = true;
    return sendBuffers();
}

bool BridgeSink::install(uint32_t width, uint32_t height, Transport wanted)
{
    std::string error;
    std::unique_ptr<FrameBufferSet> set = FrameBufferSet::create(m_ctx, width, height, wanted, error);
    // Fall back one step at a time: tiled dma-buf, linear dma-buf, shared memory.
    if (!set && wanted == Transport::Dmabuf) {
        logError("cannot export {} buffers ({}), trying linear", transportName(wanted), error);
        wanted = Transport::DmabufLinear;
        set = FrameBufferSet::create(m_ctx, width, height, wanted, error);
    }
    if (!set && wanted != Transport::Shm) {
        logError("cannot export {} buffers ({}), using shared memory", transportName(wanted), error);
        wanted = Transport::Shm;
        set = FrameBufferSet::create(m_ctx, width, height, wanted, error);
    }
    if (!set) {
        m_error = "cannot allocate the render targets: " + error;
        return false;
    }
    m_set = std::move(set);
    ++m_generation;
    m_held.fill(false);
    m_setChanged = true;
    logInfo("{}x{} buffers over {}, generation {}", width, height, transportName(m_set->transport()), m_generation);
    return m_started ? sendBuffers() : true;
}

bool BridgeSink::sendReliable(bridge::MsgType type, std::span<const uint8_t> payload, std::span<const int> fds)
{
    // The fds stay valid for the whole call, so wait for room in the socket instead of dropping the message.
    bridge::ChannelStatus status = m_channel.send(type, payload, fds);
    for (int retry = 0; status == bridge::ChannelStatus::WouldBlock && retry < 200; ++retry) {
        pollfd writable{m_channel.fd(), POLLOUT, 0};
        ::poll(&writable, 1, 10);
        status = m_channel.send(type, payload, fds);
    }
    if (status == bridge::ChannelStatus::Closed) {
        m_peerClosed = true;
    }
    return status == bridge::ChannelStatus::Ok;
}

bool BridgeSink::sendBuffers()
{
    bool ok = false;
    if (m_set->isDmabuf()) {
        msg::DmabufBuffers buffers{};
        buffers.viewport = 0;
        buffers.count = kBufferCount;
        buffers.planes = 1;
        buffers.fourcc = m_set->fourcc();
        buffers.width = m_set->width();
        buffers.height = m_set->height();
        buffers.generation = m_generation;
        buffers.modifier = m_set->modifier();
        for (uint32_t b = 0; b < kBufferCount; ++b) {
            buffers.plane[b][0] = m_set->plane(b);
        }
        const std::array<int, kBufferCount> fds = m_set->dmabufFds();
        ok = sendReliable(msg::DmabufBuffers::kType, bridge::asBytes(buffers), fds);
        logInfo("publishing dma-bufs: fourcc 0x{:x} modifier 0x{:x} {}x{} generation {}", buffers.fourcc, buffers.modifier,
                buffers.width, buffers.height, buffers.generation);
    } else {
        msg::ShmBuffers buffers{};
        buffers.viewport = 0;
        buffers.count = kBufferCount;
        buffers.width = m_set->width();
        buffers.height = m_set->height();
        buffers.stride = m_set->width() * 4;
        buffers.format = bridge::kFormatRgba8;
        buffers.generation = m_generation;
        const int fd = m_set->ring().fd();
        ok = sendReliable(msg::ShmBuffers::kType, bridge::asBytes(buffers), std::span<const int>(&fd, 1));
    }
    if (!ok && m_error.empty()) {
        m_error = m_peerClosed ? "the bridge peer closed the socket" : "cannot send the buffers over the bridge socket";
    }
    return ok;
}

bool BridgeSink::resize(uint32_t width, uint32_t height)
{
    if (width == m_set->width() && height == m_set->height()) {
        return true;
    }
    return install(width, height, m_set->transport());
}

void BridgeSink::sendMediaWants(bool wants)
{
    if (m_failed || m_peerClosed) {
        return;
    }
    m_channel.send(bridge::msg::MediaWants{wants ? 1u : 0u});
}

bool BridgeSink::sendShortcutLaunch(std::string_view name)
{
    if (m_failed || m_peerClosed || name.empty() || name.size() >= bridge::kShortcutNameBytes) {
        return false;
    }
    bridge::msg::ShortcutLaunch launch{};
    launch.serial = ++m_launchSerial;
    bridge::msg::setShortcutName(launch, name);
    if (!bridge::msg::validate(launch, 0)) {
        return false;
    }
    return m_channel.send(launch) == bridge::ChannelStatus::Ok;
}

bool BridgeSink::sendRaw(bridge::MsgType type, std::span<const uint8_t> payload)
{
    if (m_failed || m_peerClosed)
        return false;
    return m_channel.send(type, payload) == bridge::ChannelStatus::Ok;
}

void BridgeSink::sendError(uint32_t code, std::string_view text)
{
    if (!m_channel.valid()) {
        return;
    }
    msg::Error error{};
    error.code = code;
    bridge::setString(error.text, text);
    m_channel.send(error); // best effort, the peer may be gone already
}

void BridgeSink::fail(std::string text)
{
    logError("bridge: {}", text);
    sendError(kErrorProtocol, text);
    m_error = std::move(text);
    m_failed = true;
}

std::optional<uint32_t> BridgeSink::acquire() const
{
    for (uint32_t i = 0; i < kBufferCount; ++i) {
        if (!m_held[i]) {
            return i;
        }
    }
    return std::nullopt;
}

uint32_t BridgeSink::heldCount() const
{
    return static_cast<uint32_t>(std::count(m_held.begin(), m_held.end(), true));
}

PresentResult BridgeSink::present(uint32_t buffer, bool flipY)
{
    msg::Frame frame{};
    frame.viewport = 0;
    frame.buffer = buffer;
    frame.seq = ++m_sequence;
    frame.flipY = flipY ? 1 : 0;
    frame.generation = m_generation;
    switch (m_channel.send(frame)) {
    case bridge::ChannelStatus::Ok:
        m_held[buffer] = true;
        return PresentResult::Sent;
    case bridge::ChannelStatus::WouldBlock:
        return PresentResult::Skipped;
    case bridge::ChannelStatus::Closed:
        m_peerClosed = true;
        return PresentResult::Closed;
    default:
        m_error = "cannot send a frame over the bridge socket";
        m_failed = true;
        return PresentResult::Failed;
    }
}

void BridgeSink::waitForMessage(std::chrono::nanoseconds timeout) const
{
    if (!m_channel.valid()) {
        return;
    }
    const auto ns = std::max(timeout, std::chrono::nanoseconds::zero());
    const timespec limit{static_cast<time_t>(ns.count() / 1000000000), static_cast<long>(ns.count() % 1000000000)};
    pollfd pending{m_channel.fd(), POLLIN, 0};
    ::ppoll(&pending, 1, &limit, nullptr);
}

ServiceResult BridgeSink::service(size_t maxMessages)
{
    if (m_peerClosed) {
        return ServiceResult::PeerClosed;
    }
    const bridge::DrainResult result = m_channel.drain(
        [this](bridge::Message &&message) {
            if (!m_failed) {
                handle(std::move(message));
            }
        },
        maxMessages);
    if (m_failed) {
        return ServiceResult::Failed;
    }
    switch (result.last) {
    case bridge::ChannelStatus::Ok:
    case bridge::ChannelStatus::WouldBlock:
        return m_peerClosed ? ServiceResult::PeerClosed : ServiceResult::Ok;
    case bridge::ChannelStatus::Closed:
        m_peerClosed = true;
        return ServiceResult::PeerClosed;
    case bridge::ChannelStatus::TooLarge:
    case bridge::ChannelStatus::Malformed:
        fail("malformed message from the bridge peer");
        return ServiceResult::Failed;
    default:
        m_error = "cannot read from the bridge socket";
        m_failed = true;
        return ServiceResult::Failed;
    }
}

void BridgeSink::handle(bridge::Message &&message)
{
    const std::span<const uint8_t> payload{message.payload};
    const size_t fdCount = message.fds.size();

    // Decodes and validates a fixed size message; anything that does not pass is a protocol violation.
    const auto read = [&](auto &out) -> bool {
        if (!bridge::decode(payload, out) || !msg::validate(out, fdCount)) {
            fail("invalid message from the bridge peer");
            return false;
        }
        return true;
    };
    const auto forward = [&](auto &out) {
        if (read(out) && m_handler) {
            m_handler(ControlMessage(out));
        }
    };

    // Murasaki's own extension of the protocol, outside the enum shared by the bridge producers.
    if (message.type == static_cast<uint32_t>(bridge::kSpanViewType)) {
        msg::SpanView m{};
        forward(m);
        return;
    }

    if (message.type == static_cast<uint32_t>(bridge::kMediaType)) {
        // untrusted text: only its size is judged here, the engine parses it as it would any JSON
        const auto json = fdCount == 0 ? bridge::decodeProperties(payload) : std::nullopt;
        if (!json || json->size() > bridge::kMaxMediaJson) {
            fail("invalid Media message from the bridge peer");
            return;
        }
        if (m_handler) {
            m_handler(ControlMessage(MediaJson{std::string(*json)}));
        }
        return;
    }
    if (message.type == static_cast<uint32_t>(bridge::kMediaThumbnailType)) {
        bridge::msg::MediaThumbnail m{};
        if (!bridge::decode(payload, m) || !bridge::msg::validate(m, fdCount)) {
            fail("invalid MediaThumbnail message from the bridge peer");
            return;
        }
        MediaThumbnailData data{m.width, m.height, m.generation, {}};
        if (m.width != 0) {
            // a cover that is not a sealed memfd of the right size is dropped (the track shows no cover), the engine carries on
            auto pixels = bridge::readSealedPixels(std::move(message.fds[0]), m.width, m.height);
            if (!pixels) {
                data = MediaThumbnailData{0, 0, m.generation, {}};
            } else {
                data.rgba = std::move(*pixels);
            }
        }
        if (m_handler) {
            m_handler(ControlMessage(std::move(data)));
        }
        return;
    }

    if (message.type == static_cast<uint32_t>(bridge::kShortcutIconType)) {
        bridge::msg::ShortcutIcon m{};
        if (!bridge::decode(payload, m) || !bridge::msg::validate(m, fdCount)) {
            fail("invalid ShortcutIcon message from the bridge peer");
            return;
        }
        ShortcutIconData data{std::string(m.name), m.width, m.height, m.generation, {}};
        if (m.width != 0) {
            // an icon that is not a sealed memfd of the right size is dropped (the slot keeps its default), the engine carries on
            auto pixels = bridge::readSealedPixels(std::move(message.fds[0]), m.width, m.height);
            if (!pixels) {
                data.width = data.height = 0;
            } else {
                data.rgba = std::move(*pixels);
            }
        }
        if (m_handler) {
            m_handler(ControlMessage(std::move(data)));
        }
        return;
    }

    switch (static_cast<bridge::MsgType>(message.type)) {
    case bridge::MsgType::Release: {
        msg::Release release{};
        if (!read(release)) {
            return;
        }
        if (release.viewport != 0 || release.buffer >= kBufferCount) {
            fail("Release refers to an unknown viewport or buffer");
            return;
        }
        // A Release of an older buffer set must never free a buffer of the current one.
        if (release.generation == m_generation) {
            m_held[release.buffer] = false;
        }
        break;
    }
    case bridge::MsgType::RequestLinear:
    case bridge::MsgType::RequestShm: {
        msg::RequestShm request{}; // both only carry the viewport id
        if (!read(request)) {
            return;
        }
        if (request.viewport != 0) {
            fail("transport request refers to an unknown viewport");
            return;
        }
        const Transport wanted = static_cast<bridge::MsgType>(message.type) == bridge::MsgType::RequestShm
            ? Transport::Shm
            : Transport::DmabufLinear;
        if (m_set->transport() != wanted && !(m_forceShm && wanted != Transport::Shm)) {
            if (!install(m_set->width(), m_set->height(), wanted)) {
                fail("cannot allocate the render targets for the requested transport");
            }
        }
        break;
    }
    case bridge::MsgType::Pause: {
        msg::Pause m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Volume: {
        msg::Volume m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Pointer: {
        msg::Pointer m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Resize: {
        msg::Resize m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Scaling: {
        msg::Scaling m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Fps: {
        msg::Fps m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Quit: {
        msg::Quit m{};
        forward(m);
        break;
    }
    case bridge::MsgType::Properties: {
        const auto json = fdCount == 0 ? bridge::decodeProperties(payload) : std::nullopt;
        if (!json) {
            fail("invalid Properties message from the bridge peer");
            return;
        }
        if (m_handler) {
            m_handler(ControlMessage(PropertiesJson{std::string(*json)}));
        }
        break;
    }
    default:
        // Unknown (or producer-only) types are ignored; their fds close with the message.
        break;
    }
}

} // namespace murasaki::scene
