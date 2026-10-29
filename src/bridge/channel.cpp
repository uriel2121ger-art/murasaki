#include "channel.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

namespace murasaki::bridge {

void UniqueFd::reset(int fd) noexcept
{
    if (m_fd >= 0) {
        ::close(m_fd); // close() is not retried on EINTR: the fd is released either way on Linux
    }
    m_fd = fd;
}

std::optional<SocketPair> makeSocketPair()
{
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, fds) != 0) {
        return std::nullopt;
    }
    return SocketPair{UniqueFd(fds[0]), UniqueFd(fds[1])};
}

bool Channel::setNonblocking(bool on)
{
    const int flags = ::fcntl(m_fd.get(), F_GETFL);
    if (flags < 0) {
        return false;
    }
    const int wanted = on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return wanted == flags || ::fcntl(m_fd.get(), F_SETFL, wanted) == 0;
}

ChannelStatus Channel::send(MsgType type, std::span<const uint8_t> payload, std::span<const int> fds, SendMode mode)
{
    if (!m_fd.valid()) {
        return ChannelStatus::Error;
    }
    if (payload.size() > kMaxPayload || fds.size() > kMaxFds) {
        return ChannelStatus::TooLarge;
    }

    MsgHeader header{static_cast<uint32_t>(type), static_cast<uint32_t>(payload.size())};
    iovec iov[2] = {{&header, sizeof(header)}, {const_cast<uint8_t *>(payload.data()), payload.size()}};

    alignas(cmsghdr) char control[CMSG_SPACE(kMaxFds * sizeof(int))];
    msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = payload.empty() ? 1 : 2;
    if (!fds.empty()) {
        std::memset(control, 0, sizeof(control));
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(fds.size() * sizeof(int));
        cmsghdr *cm = CMSG_FIRSTHDR(&msg);
        cm->cmsg_level = SOL_SOCKET;
        cm->cmsg_type = SCM_RIGHTS;
        cm->cmsg_len = CMSG_LEN(fds.size() * sizeof(int));
        std::memcpy(CMSG_DATA(cm), fds.data(), fds.size() * sizeof(int));
    }

    for (;;) {
        const ssize_t n = ::sendmsg(m_fd.get(), &msg, MSG_NOSIGNAL | (mode == SendMode::NonBlocking ? MSG_DONTWAIT : 0));
        if (n >= 0) {
            // SEQPACKET is atomic: a short count would mean a broken kernel contract.
            return static_cast<size_t>(n) == sizeof(header) + payload.size() ? ChannelStatus::Ok : ChannelStatus::Error;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return ChannelStatus::WouldBlock;
        }
        if (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == ECONNREFUSED) {
            return ChannelStatus::Closed;
        }
        return ChannelStatus::Error;
    }
}

ChannelStatus Channel::sendProperties(std::string_view json, SendMode mode)
{
    const auto bytes = encodeProperties(json);
    if (!bytes) {
        return ChannelStatus::TooLarge;
    }
    return send(MsgType::Properties, *bytes, {}, mode);
}

ReceiveResult Channel::receive()
{
    ReceiveResult result;
    if (!m_fd.valid()) {
        return result;
    }

    if (!m_scratch) {
        m_scratch.reset(new uint8_t[kMaxMessage]); // default-init: no 64 KiB zero fill
    }
    uint8_t *const buffer = m_scratch.get();
    iovec iov{buffer, kMaxMessage};
    alignas(cmsghdr) char control[CMSG_SPACE(kMaxFds * sizeof(int))];
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control;
    msg.msg_controllen = sizeof(control);

    ssize_t n;
    for (;;) {
        msg.msg_controllen = sizeof(control);
        n = ::recvmsg(m_fd.get(), &msg, MSG_CMSG_CLOEXEC);
        if (n >= 0 || errno != EINTR) {
            break;
        }
    }
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            result.status = ChannelStatus::WouldBlock;
        } else if (errno == ECONNRESET || errno == ENOTCONN) {
            result.status = ChannelStatus::Closed;
        } else {
            result.status = ChannelStatus::Error;
            result.errnoValue = errno;
        }
        return result;
    }

    // Take ownership of every received fd first so that any rejection below closes them.
    std::vector<UniqueFd> fds;
    for (cmsghdr *cm = CMSG_FIRSTHDR(&msg); cm != nullptr; cm = CMSG_NXTHDR(&msg, cm)) {
        if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS) {
            continue;
        }
        const size_t count = (cm->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (size_t i = 0; i < count; ++i) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(cm) + i * sizeof(int), sizeof(int));
            fds.emplace_back(fd);
        }
    }

    if (n == 0) {
        result.status = ChannelStatus::Closed;
        return result;
    }
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0) {
        result.status = (msg.msg_flags & MSG_TRUNC) != 0 ? ChannelStatus::TooLarge : ChannelStatus::Malformed;
        return result;
    }
    if (static_cast<size_t>(n) < sizeof(MsgHeader)) {
        result.status = ChannelStatus::Malformed;
        return result;
    }
    MsgHeader header;
    std::memcpy(&header, buffer, sizeof(header));
    if (header.size > kMaxPayload) {
        result.status = ChannelStatus::TooLarge;
        return result;
    }
    if (static_cast<size_t>(n) - sizeof(MsgHeader) != header.size) {
        result.status = ChannelStatus::Malformed;
        return result;
    }
    if (fds.size() > kMaxFds) {
        result.status = ChannelStatus::TooLarge;
        return result;
    }

    result.message.type = header.type;
    result.message.payload.assign(buffer + sizeof(MsgHeader), buffer + sizeof(MsgHeader) + header.size);
    result.message.fds = std::move(fds);
    result.status = ChannelStatus::Ok;
    return result;
}

// ---- ShmRing -------------------------------------------------------------------------------------------

namespace {
#ifdef MFD_NOEXEC_SEAL
constexpr unsigned kMfdNoexecSeal = MFD_NOEXEC_SEAL;
#else
constexpr unsigned kMfdNoexecSeal = 0x0008U;
#endif
constexpr unsigned kRequiredSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL;

void setError(ShmError *out, ShmError value)
{
    if (out != nullptr) {
        *out = value;
    }
}
} // namespace

ShmRing::ShmRing(ShmRing &&other) noexcept
    : m_fd(std::move(other.m_fd)), m_geo(other.m_geo), m_map(std::exchange(other.m_map, nullptr)),
      m_mapBytes(std::exchange(other.m_mapBytes, 0)), m_writable(other.m_writable)
{
}

ShmRing &ShmRing::operator=(ShmRing &&other) noexcept
{
    if (this != &other) {
        if (m_map != nullptr) {
            ::munmap(m_map, m_mapBytes);
        }
        m_fd = std::move(other.m_fd);
        m_geo = other.m_geo;
        m_map = std::exchange(other.m_map, nullptr);
        m_mapBytes = std::exchange(other.m_mapBytes, 0);
        m_writable = other.m_writable;
    }
    return *this;
}

ShmRing::~ShmRing()
{
    if (m_map != nullptr) {
        ::munmap(m_map, m_mapBytes);
    }
}

std::optional<ShmRing> ShmRing::create(const ShmGeometry &g, ShmError *error)
{
    setError(error, ShmError::Ok);
    const uint64_t bytes = ringBytes(g);
    if (bytes == 0) {
        setError(error, ShmError::BadGeometry);
        return std::nullopt;
    }
    // MFD_NOEXEC_SEAL (Linux 6.3+) keeps the ring non-executable; older kernels reject it with EINVAL.
    UniqueFd fd(::memfd_create("murasaki-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING | kMfdNoexecSeal));
    if (!fd && errno == EINVAL) {
        fd.reset(::memfd_create("murasaki-ring", MFD_CLOEXEC | MFD_ALLOW_SEALING));
    }
    if (!fd || ::ftruncate(fd.get(), static_cast<off_t>(bytes)) != 0
        || ::fcntl(fd.get(), F_ADD_SEALS, kRequiredSeals) != 0) {
        setError(error, ShmError::MemfdFailed);
        return std::nullopt;
    }
    void *map = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd.get(), 0);
    if (map == MAP_FAILED) {
        setError(error, ShmError::MapFailed);
        return std::nullopt;
    }
    ShmRing ring;
    ring.m_fd = std::move(fd);
    ring.m_geo = g;
    ring.m_map = static_cast<uint8_t *>(map);
    ring.m_mapBytes = bytes;
    ring.m_writable = true;
    return ring;
}

std::optional<ShmRing> ShmRing::open(UniqueFd fd, const ShmGeometry &g, ShmError *error)
{
    setError(error, ShmError::Ok);
    const uint64_t bytes = ringBytes(g);
    if (bytes == 0) {
        setError(error, ShmError::BadGeometry);
        return std::nullopt;
    }
    struct stat st;
    if (!fd || ::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        setError(error, ShmError::BadFd);
        return std::nullopt;
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) != bytes) {
        setError(error, ShmError::SizeMismatch);
        return std::nullopt;
    }
    const int seals = ::fcntl(fd.get(), F_GET_SEALS);
    if (seals < 0 || (static_cast<unsigned>(seals) & kRequiredSeals) != kRequiredSeals) {
        setError(error, ShmError::NotSealed);
        return std::nullopt;
    }
    void *map = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd.get(), 0);
    if (map == MAP_FAILED) {
        setError(error, ShmError::MapFailed);
        return std::nullopt;
    }
    ShmRing ring;
    ring.m_fd = std::move(fd);
    ring.m_geo = g;
    ring.m_map = static_cast<uint8_t *>(map);
    ring.m_mapBytes = bytes;
    ring.m_writable = false;
    return ring;
}

std::span<const uint8_t> ShmRing::frame(uint32_t index) const
{
    if (m_map == nullptr || index >= m_geo.count) {
        return {};
    }
    return {m_map + static_cast<size_t>(index) * frameBytes(), frameBytes()};
}

std::span<uint8_t> ShmRing::frame(uint32_t index)
{
    if (m_map == nullptr || !m_writable || index >= m_geo.count) {
        return {};
    }
    return {m_map + static_cast<size_t>(index) * frameBytes(), frameBytes()};
}

} // namespace murasaki::bridge
