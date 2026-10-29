#include "mediaview.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace murasaki::bridge {

namespace {

#ifdef MFD_NOEXEC_SEAL
constexpr unsigned kMfdNoexecSeal = MFD_NOEXEC_SEAL;
#else
constexpr unsigned kMfdNoexecSeal = 0x0008U;
#endif
constexpr unsigned kThumbnailSeals = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE | F_SEAL_SEAL;

} // namespace

UniqueFd sealedPixelsFd(std::span<const uint8_t> rgba)
{
    UniqueFd fd(::memfd_create("murasaki-media", MFD_CLOEXEC | MFD_ALLOW_SEALING | kMfdNoexecSeal));
    if (!fd && errno == EINVAL) {
        fd.reset(::memfd_create("murasaki-media", MFD_CLOEXEC | MFD_ALLOW_SEALING)); // before Linux 6.3
    }
    if (!fd) {
        return {};
    }
    size_t done = 0;
    while (done < rgba.size()) {
        const ssize_t n = ::write(fd.get(), rgba.data() + done, rgba.size() - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return {};
        }
        done += static_cast<size_t>(n);
    }
    if (::fcntl(fd.get(), F_ADD_SEALS, kThumbnailSeals) != 0) {
        return {};
    }
    return fd;
}

std::optional<std::vector<uint8_t>> readSealedPixels(UniqueFd fd, uint32_t width, uint32_t height)
{
    if (!fd || width < 1 || height < 1 || width > kMaxThumbnailSide || height > kMaxThumbnailSide) {
        return std::nullopt;
    }
    const uint64_t bytes = uint64_t{width} * height * 4;
    struct stat st;
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || static_cast<uint64_t>(st.st_size) != bytes) {
        return std::nullopt;
    }
    const int seals = ::fcntl(fd.get(), F_GET_SEALS);
    constexpr unsigned required = F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_WRITE;
    if (seals < 0 || (static_cast<unsigned>(seals) & required) != required) {
        return std::nullopt;
    }
    void *map = ::mmap(nullptr, bytes, PROT_READ, MAP_SHARED, fd.get(), 0);
    if (map == MAP_FAILED) {
        return std::nullopt;
    }
    std::vector<uint8_t> out(static_cast<const uint8_t *>(map), static_cast<const uint8_t *>(map) + bytes);
    ::munmap(map, bytes);
    return out;
}

} // namespace murasaki::bridge
