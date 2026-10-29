#pragma once

// Bridge transport: SOCK_SEQPACKET send/receive with SCM_RIGHTS fds, and the memfd frame ring.
// Shared verbatim with the bridge producers: plain C++20 + Linux, no Qt.

#include "protocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace murasaki::bridge {

inline constexpr size_t kMaxPayload = kMaxPropertiesJson + kPropertiesPrefix;
inline constexpr size_t kMaxMessage = sizeof(MsgHeader) + kMaxPayload;
inline constexpr size_t kMaxFds = 16;

// Owning file descriptor.
class UniqueFd
{
public:
    UniqueFd() = default;
    explicit UniqueFd(int fd) noexcept : m_fd(fd) {}
    UniqueFd(UniqueFd &&other) noexcept : m_fd(other.release()) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;
    ~UniqueFd() { reset(); }

    int get() const noexcept { return m_fd; }
    bool valid() const noexcept { return m_fd >= 0; }
    explicit operator bool() const noexcept { return valid(); }
    int release() noexcept { return std::exchange(m_fd, -1); }
    void reset(int fd = -1) noexcept;

private:
    int m_fd = -1;
};

struct SocketPair {
    UniqueFd first;
    UniqueFd second;
};

// socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC). The caller clears CLOEXEC on the end the child inherits.
std::optional<SocketPair> makeSocketPair();

enum class ChannelStatus {
    Ok,
    WouldBlock, // nonblocking socket with nothing to read / no room to write (EAGAIN)
    Closed,     // peer closed: EOF, a zero-length datagram, EPIPE/ENOTCONN/ECONNREFUSED/ECONNRESET
    TooLarge,   // send: over the caps, nothing sent. receive: datagram over the caps, discarded
    Malformed,  // truncated datagram, bad header, or ancillary data cut off; all received fds were closed
    Error,      // other syscall failure, see errnoValue
};
// A receive that yields TooLarge or Malformed means the peer is broken or hostile and the stream can no
// longer be trusted: callers drop the connection.

struct Message {
    uint32_t type = 0; // raw MsgType value; unknown types are the caller's to ignore
    std::vector<uint8_t> payload;
    std::vector<UniqueFd> fds;
};

struct ReceiveResult {
    ChannelStatus status = ChannelStatus::Error;
    int errnoValue = 0;
    Message message; // valid only when status == Ok
};

enum class SendMode {
    NonBlocking, // MSG_DONTWAIT: never stalls the caller (the default; the render loop must not block)
    Blocking,    // wait for room in the socket buffer unless the fd itself is O_NONBLOCK
};

struct DrainResult {
    size_t handled = 0;
    ChannelStatus last = ChannelStatus::WouldBlock; // why draining stopped; WouldBlock = queue empty
};

class Channel
{
public:
    Channel() = default;
    explicit Channel(UniqueFd fd) : m_fd(std::move(fd)) {}

    bool valid() const { return m_fd.valid(); }
    int fd() const { return m_fd.get(); }
    UniqueFd takeFd() { return std::move(m_fd); }

    bool setNonblocking(bool on);

    // Sends one datagram atomically (all or nothing). The kernel duplicates the fds into the peer; the caller
    // keeps its own and always still owns them afterwards. On WouldBlock nothing was sent and the CALLER owns
    // the retry (typically: keep the message, wait for POLLOUT, send again, or skip the frame).
    ChannelStatus send(MsgType type, std::span<const uint8_t> payload, std::span<const int> fds = {},
                       SendMode mode = SendMode::NonBlocking);

    // Fixed-size messages are sent straight from the struct: no allocation.
    template <Wire T>
    ChannelStatus send(const T &m, std::span<const int> fds = {}, SendMode mode = SendMode::NonBlocking)
    {
        return send(T::kType, asBytes(m), fds, mode);
    }

    ChannelStatus sendProperties(std::string_view json, SendMode mode = SendMode::NonBlocking);

    // One message. A zero-length datagram or EOF is Closed. The returned payload is sized from the header.
    ReceiveResult receive();

    // Receives until the queue is empty, an error occurs, or maxMessages were handled, calling fn(Message &&)
    // for each. Bounding maxMessages keeps a chatty peer from starving the caller's loop.
    template <class F>
    DrainResult drain(F &&fn, size_t maxMessages)
    {
        DrainResult out;
        while (out.handled < maxMessages) {
            ReceiveResult r = receive();
            out.last = r.status;
            if (r.status != ChannelStatus::Ok) {
                return out;
            }
            ++out.handled;
            fn(std::move(r.message));
        }
        out.last = ChannelStatus::Ok;
        return out;
    }

private:
    UniqueFd m_fd;
    std::unique_ptr<uint8_t[]> m_scratch; // kMaxMessage bytes, allocated on first receive, deliberately uninitialized
};

// ---- shared-memory frame ring --------------------------------------------------------------------------

enum class ShmError {
    Ok,
    BadGeometry,       // see ringBytes()
    MemfdFailed,
    BadFd,        // fstat failed or the fd is not a regular file
    SizeMismatch, // fd size != count * stride * height
    NotSealed,    // missing F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL
    MapFailed,
};

class ShmRing
{
public:
    ShmRing() = default;
    ShmRing(ShmRing &&other) noexcept;
    ShmRing &operator=(ShmRing &&other) noexcept;
    ShmRing(const ShmRing &) = delete;
    ShmRing &operator=(const ShmRing &) = delete;
    ~ShmRing();

    // Producer: creates a sealed memfd for the geometry and maps it read-write.
    static std::optional<ShmRing> create(const ShmGeometry &g, ShmError *error = nullptr);
    // Consumer: validates the received fd against the declared geometry (size and seals) and maps it
    // read-only. Takes ownership of the fd.
    static std::optional<ShmRing> open(UniqueFd fd, const ShmGeometry &g, ShmError *error = nullptr);

    const ShmGeometry &geometry() const { return m_geo; }
    bool writable() const { return m_writable; }
    int fd() const { return m_fd.get(); } // send this to the peer
    size_t frameBytes() const { return static_cast<size_t>(m_geo.stride) * m_geo.height; }

    // Frame by index; empty span if out of range. The mutable overload is empty for read-only rings.
    std::span<const uint8_t> frame(uint32_t index) const;
    std::span<uint8_t> frame(uint32_t index);

private:
    UniqueFd m_fd;
    ShmGeometry m_geo;
    uint8_t *m_map = nullptr;
    size_t m_mapBytes = 0;
    bool m_writable = false;
};

} // namespace murasaki::bridge
