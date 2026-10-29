#include "assets/texcache.h"

#include <fcntl.h>
#include <lz4.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

namespace murasaki::scene {

namespace {

namespace fs = std::filesystem;

constexpr char kMagic[4] = {'M', 'T', 'C', '1'};
constexpr uintmax_t kCacheBytesLimit = 1ull << 30; // the directory is trimmed to this
constexpr int kAcceleration = 8;                   // LZ4's speed over ratio: pictures hardly compress, the time matters

std::string cacheDirectory()
{
    if (const char *off = std::getenv("MURASAKI_TEXTURE_CACHE"); off && std::string(off) == "0") {
        return {};
    }
    std::string base;
    if (const char *xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg) {
        base = xdg;
    } else if (const char *home = std::getenv("HOME"); home && *home) {
        base = std::string(home) + "/.cache";
    } else {
        return {};
    }
    return base + "/murasaki/texture-cache";
}

// 128 bits of a fast hash of the whole source (two independent 64-bit lanes, 32 bytes a round) plus its length and the decoder
// version: SHA-256 over seven megabytes of PNG cost more than the cache saved. Accidental collisions are out of reach at this
// width; the cache is the user's own and an entry is also checked against the picture's size when read.
uint64_t rotl(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

std::string keyOf(std::span<const uint8_t> source)
{
    constexpr uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, P3 = 0x165667B19E3779F9ull;
    uint64_t lane[4] = {P1 + P2, P2, 0, 0 - P1};
    uint64_t other[4] = {P3 + P1, P3, 0x27D4EB2F165667C5ull, 0 - P3};
    size_t i = 0;
    const uint8_t *d = source.data();
    for (; i + 32 <= source.size(); i += 32) {
        for (int k = 0; k < 4; ++k) {
            uint64_t v;
            std::memcpy(&v, d + i + k * 8, 8);
            lane[k] = rotl(lane[k] + v * P2, 31) * P1;
            other[k] = rotl(other[k] ^ (v * P1), 27) * P3 + P2;
        }
    }
    uint64_t tail = 0;
    for (size_t k = 0; i + k < source.size(); ++k) {
        tail = (tail << 8) | d[i + k];
        if (k % 8 == 7) {
            lane[0] = rotl(lane[0] ^ (tail * P2), 23) * P1;
            other[0] = rotl(other[0] + tail, 29) * P3;
            tail = 0;
        }
    }
    uint64_t a = rotl(lane[0], 1) + rotl(lane[1], 7) + rotl(lane[2], 12) + rotl(lane[3], 18) + tail + source.size();
    uint64_t b = rotl(other[0], 3) + rotl(other[1], 9) + rotl(other[2], 14) + rotl(other[3], 20) + (tail ^ P3) + (static_cast<uint64_t>(source.size()) << 1) + kTextureDecoderVersion;
    for (uint64_t *x : {&a, &b}) {
        *x ^= *x >> 33;
        *x *= P2;
        *x ^= *x >> 29;
        *x *= P3;
        *x ^= *x >> 32;
    }
    char hex[64];
    std::snprintf(hex, sizeof(hex), "%016llx%016llx-%zx-v%d", static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), source.size(), kTextureDecoderVersion);
    return hex;
}

// Removes the least recently used files until the directory is at three quarters of its limit.
void trim(const std::string &dir)
{
    std::error_code ec;
    std::vector<std::pair<fs::file_time_type, fs::path>> files;
    uintmax_t total = 0;
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
        if (entry.is_regular_file(ec)) {
            // a store that never finished (the process ended): its temporary file goes after an hour
            if (entry.path().filename().string().find(".tmp.") != std::string::npos) {
                if (fs::file_time_type::clock::now() - entry.last_write_time(ec) > std::chrono::hours(1)) {
                    fs::remove(entry.path(), ec);
                }
                continue;
            }
            total += entry.file_size(ec);
            files.emplace_back(entry.last_write_time(ec), entry.path());
        }
    }
    if (total <= kCacheBytesLimit) {
        return;
    }
    std::sort(files.begin(), files.end());
    for (const auto &[time, path] : files) {
        if (total <= kCacheBytesLimit * 3 / 4) {
            break;
        }
        const uintmax_t size = fs::file_size(path, ec);
        if (fs::remove(path, ec)) {
            total -= std::min(total, size);
        }
    }
}

} // namespace

bool loadCachedImage(std::span<const uint8_t> source, uint32_t maxDimension, uint64_t maxBytes, DecodedImage &out)
{
    const std::string dir = cacheDirectory();
    if (dir.empty()) {
        return false;
    }
    const std::string path = dir + "/" + keyOf(source) + ".bin";
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }
    char header[16];
    if (!in.read(header, sizeof(header)) || std::memcmp(header, kMagic, 4) != 0) {
        return false;
    }
    uint32_t w, h, compressed;
    std::memcpy(&w, header + 4, 4);
    std::memcpy(&h, header + 8, 4);
    std::memcpy(&compressed, header + 12, 4);
    const uint64_t rawSize = static_cast<uint64_t>(w) * h * 4;
    if (w == 0 || h == 0 || w > maxDimension || h > maxDimension || rawSize > maxBytes || compressed == 0 || compressed > LZ4_COMPRESSBOUND(rawSize)) {
        return false;
    }
    std::vector<uint8_t> packed(compressed);
    if (!in.read(reinterpret_cast<char *>(packed.data()), compressed) || in.peek() != std::ifstream::traits_type::eof()) {
        return false;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(rawSize));
    const int got = LZ4_decompress_safe(reinterpret_cast<const char *>(packed.data()), reinterpret_cast<char *>(pixels.data()), static_cast<int>(compressed),
                                        static_cast<int>(rawSize));
    if (got < 0 || static_cast<uint64_t>(got) != rawSize) {
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba = std::move(pixels);
    // a use: the trim removes what has not been used for longest
    const timespec times[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
    ::utimensat(AT_FDCWD, path.c_str(), times, 0);
    return true;
}

namespace {

struct StoreJob {
    std::string dir;
    std::string path;
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> rgba;
};

class StoreQueue
{
public:
    void push(StoreJob job)
    {
        std::lock_guard lock(m_mutex);
        m_jobs.push_back(std::move(job));
        ++m_pending;
        if (!m_running) {
            m_running = true;
            std::thread([this] { run(); }).detach();
        }
    }
    void flush()
    {
        std::unique_lock lock(m_mutex);
        m_idle.wait(lock, [this] { return m_pending == 0; });
    }

private:
    void run()
    {
        for (;;) {
            StoreJob job;
            {
                std::lock_guard lock(m_mutex);
                if (m_jobs.empty()) {
                    m_running = false;
                    return;
                }
                job = std::move(m_jobs.front());
                m_jobs.pop_front();
            }
            write(job);
            std::lock_guard lock(m_mutex);
            if (--m_pending == 0) {
                m_idle.notify_all();
            }
        }
    }

    static void write(const StoreJob &job)
    {
        std::error_code ec;
        fs::create_directories(job.dir, ec);
        if (ec) {
            return;
        }
        const uint64_t rawSize = static_cast<uint64_t>(job.width) * job.height * 4;
        std::vector<char> packed(static_cast<size_t>(LZ4_compressBound(static_cast<int>(rawSize))));
        const int size = LZ4_compress_fast(reinterpret_cast<const char *>(job.rgba.data()), packed.data(), static_cast<int>(rawSize),
                                           static_cast<int>(packed.size()), kAcceleration);
        if (size <= 0) {
            return;
        }
        static std::atomic<unsigned> counter{0};
        const unsigned n = counter.fetch_add(1);
        const std::string temporary = job.path + ".tmp." + std::to_string(::getpid()) + "." + std::to_string(n);
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file) {
                return;
            }
            char header[16];
            std::memcpy(header, kMagic, 4);
            std::memcpy(header + 4, &job.width, 4);
            std::memcpy(header + 8, &job.height, 4);
            const uint32_t compressed = static_cast<uint32_t>(size);
            std::memcpy(header + 12, &compressed, 4);
            file.write(header, sizeof(header));
            file.write(packed.data(), size);
            if (!file) {
                file.close();
                fs::remove(temporary, ec);
                return;
            }
        }
        fs::rename(temporary, job.path, ec);
        if (ec) {
            fs::remove(temporary, ec);
            return;
        }
        if (n % 8 == 0) {
            trim(job.dir); // now and then: the first store of a run and every few after it
        }
    }

    std::mutex m_mutex;
    std::condition_variable m_idle;
    std::deque<StoreJob> m_jobs;
    size_t m_pending = 0;
    bool m_running = false;
};

StoreQueue &queue()
{
    static StoreQueue q;
    return q;
}

} // namespace

void storeCachedImage(std::span<const uint8_t> source, const DecodedImage &image)
{
    static_assert(LZ4_MAX_INPUT_SIZE > (1u << 28));
    const std::string dir = cacheDirectory();
    const uint64_t rawSize = static_cast<uint64_t>(image.width) * image.height * 4;
    if (dir.empty() || image.rgba.size() != rawSize || rawSize == 0 || rawSize > (1ull << 28)
        || static_cast<uint64_t>(image.width) * image.height < kTextureCacheMinPixels) {
        return;
    }
    queue().push({dir, dir + "/" + keyOf(source) + ".bin", image.width, image.height, image.rgba});
}

void flushCachedImageStores()
{
    queue().flush();
}

} // namespace murasaki::scene
