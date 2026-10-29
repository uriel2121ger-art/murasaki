#include "petaudioasset.h"

#include "petaudiolimits.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <set>

namespace murasaki::audio {

namespace {

void put32(std::uint8_t *p, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        p[i] = std::uint8_t(v >> (8 * i));
}
void put16(std::uint8_t *p, std::uint16_t v)
{
    p[0] = std::uint8_t(v);
    p[1] = std::uint8_t(v >> 8);
}
std::uint32_t get32(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8 | std::uint32_t(p[2]) << 16 | std::uint32_t(p[3]) << 24;
}
std::uint16_t get16(const std::uint8_t *p)
{
    return std::uint16_t(p[0] | p[1] << 8);
}

} // namespace

bool isAssetId(std::string_view id)
{
    if (id.size() != std::size_t(kPetAudioAssetIdLength))
        return false;
    return std::all_of(id.begin(), id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string assetPath(const std::string &directory, const std::string &id)
{
    if (!isAssetId(id))
        return {};
    return directory + "/" + id + ".wav";
}

std::vector<std::uint8_t> wavHeader(std::size_t frames)
{
    std::vector<std::uint8_t> h(kWavHeaderBytes, 0);
    const std::uint32_t dataBytes = std::uint32_t(frames * 4);
    std::memcpy(h.data(), "RIFF", 4);
    put32(h.data() + 4, 36 + dataBytes);
    std::memcpy(h.data() + 8, "WAVEfmt ", 8);
    put32(h.data() + 16, 16);
    put16(h.data() + 20, 1); // PCM
    put16(h.data() + 22, 2); // channels
    put32(h.data() + 24, kPetAudioRate);
    put32(h.data() + 28, kPetAudioRate * 4);
    put16(h.data() + 32, 4);  // block align
    put16(h.data() + 34, 16); // bits
    std::memcpy(h.data() + 36, "data", 4);
    put32(h.data() + 40, dataBytes);
    return h;
}

long long verifyWavHeader(const std::uint8_t *h, std::size_t fileBytes)
{
    if (fileBytes < kWavHeaderBytes || fileBytes > std::size_t(kMaxPetAudioAssetBytes))
        return -1;
    if (std::memcmp(h, "RIFF", 4) != 0 || std::memcmp(h + 8, "WAVEfmt ", 8) != 0 || std::memcmp(h + 36, "data", 4) != 0)
        return -1;
    if (get32(h + 16) != 16 || get16(h + 20) != 1 || get16(h + 22) != 2 || get32(h + 24) != std::uint32_t(kPetAudioRate)
        || get32(h + 28) != std::uint32_t(kPetAudioRate * 4) || get16(h + 32) != 4 || get16(h + 34) != 16)
        return -1;
    const std::uint32_t dataBytes = get32(h + 40);
    if (dataBytes % 4 != 0 || std::size_t(dataBytes) != fileBytes - kWavHeaderBytes || get32(h + 4) != 36 + dataBytes)
        return -1;
    const long long frames = dataBytes / 4;
    if (frames < kMinPetAudioMs * kPetAudioRate / 1000)
        return -1;
    return frames;
}

MappedClip::~MappedClip()
{
    if (m_map)
        munmap(m_map, m_mapBytes);
}

std::shared_ptr<MappedClip> MappedClip::open(const std::string &path, std::string *error)
{
    const auto fail = [&](std::string_view name) -> std::shared_ptr<MappedClip> {
        if (error)
            *error = std::string(name);
        return nullptr;
    };
    if (path.empty())
        return fail(kAudioAssetInvalid);
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return fail(errno == ENOENT ? kAudioAssetMissing : kAudioAssetInvalid);
    struct stat st {};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < off_t(kWavHeaderBytes) || st.st_size > off_t(kMaxPetAudioAssetBytes)) {
        ::close(fd);
        return fail(kAudioAssetInvalid);
    }
    void *map = mmap(nullptr, std::size_t(st.st_size), PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (map == MAP_FAILED)
        return fail(kAudioAssetInvalid);
    const long long frames = verifyWavHeader(static_cast<const std::uint8_t *>(map), std::size_t(st.st_size));
    if (frames < 0) {
        munmap(map, std::size_t(st.st_size));
        return fail(kAudioAssetInvalid);
    }
    std::shared_ptr<MappedClip> clip(new MappedClip);
    clip->m_map = map;
    clip->m_mapBytes = std::size_t(st.st_size);
    clip->m_samples = reinterpret_cast<const std::int16_t *>(static_cast<const std::uint8_t *>(map) + kWavHeaderBytes);
    clip->m_frames = std::size_t(frames);
    madvise(map, std::size_t(st.st_size), MADV_RANDOM);
    return clip;
}

void MappedClip::prefetch(std::size_t from, std::size_t frames) const
{
    if (!m_map || from >= m_frames)
        return;
    const std::size_t count = std::min(frames, m_frames - from);
    const std::size_t begin = kWavHeaderBytes + from * 4;
    const std::size_t page = std::size_t(sysconf(_SC_PAGESIZE));
    const std::size_t alignedBegin = begin / page * page;
    madvise(static_cast<std::uint8_t *>(m_map) + alignedBegin, begin + count * 4 - alignedBegin, MADV_WILLNEED);
}

WavWriter::WavWriter(std::string path) : m_path(std::move(path))
{
    const std::string part = m_path + ".part";
    m_fd = ::open(part.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (m_fd < 0)
        return;
    const std::vector<std::uint8_t> header = wavHeader(0);
    if (::write(m_fd, header.data(), header.size()) != ssize_t(header.size()))
        m_failed = true;
}

WavWriter::~WavWriter()
{
    if (m_fd >= 0)
        ::close(m_fd);
    if (!m_done)
        ::unlink((m_path + ".part").c_str());
}

bool WavWriter::append(const std::int16_t *stereo, std::size_t frames)
{
    if (!ok())
        return false;
    if ((m_frames + frames) * 4 + kWavHeaderBytes > std::size_t(kMaxPetAudioAssetBytes)) {
        m_failed = true;
        return false;
    }
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(stereo);
    std::size_t left = frames * 4;
    while (left > 0) {
        const ssize_t n = ::write(m_fd, bytes, left);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            m_failed = true;
            return false;
        }
        bytes += n;
        left -= std::size_t(n);
    }
    m_frames += frames;
    return true;
}

bool WavWriter::finish()
{
    if (!ok() || m_done)
        return false;
    const std::vector<std::uint8_t> header = wavHeader(m_frames);
    if (::pwrite(m_fd, header.data(), header.size(), 0) != ssize_t(header.size()) || ::fsync(m_fd) != 0) {
        m_failed = true;
        return false;
    }
    ::close(m_fd);
    m_fd = -1;
    if (::rename((m_path + ".part").c_str(), m_path.c_str()) != 0) {
        m_failed = true;
        return false;
    }
    m_done = true;
    return true;
}

int collectAssets(const std::string &directory, const std::vector<std::string> &keep, long long minAgeSeconds)
{
    DIR *dir = opendir(directory.c_str());
    if (!dir)
        return 0;
    const std::set<std::string> kept(keep.begin(), keep.end());
    const long long now = std::time(nullptr);
    std::vector<std::string> names;
    while (const dirent *e = readdir(dir))
        names.emplace_back(e->d_name);
    closedir(dir);
    int removed = 0;
    for (const std::string &name : names) {
        std::string id;
        for (const char *suffix : {".wav.part", ".wav", ".json"}) {
            const std::size_t n = std::strlen(suffix);
            if (name.size() == std::size_t(kPetAudioAssetIdLength) + n && name.compare(name.size() - n, n, suffix) == 0)
                id = name.substr(0, name.size() - n);
        }
        if (!isAssetId(id) || kept.count(id))
            continue;
        struct stat st {};
        const std::string path = directory + "/" + name;
        if (lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || now - st.st_mtime < minAgeSeconds)
            continue;
        if (::unlink(path.c_str()) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".wav") == 0)
            ++removed;
    }
    return removed;
}

} // namespace murasaki::audio
