#include "assets/pkg.h"

#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstring>

namespace murasaki::scene {

namespace {

constexpr uint32_t kMaxVersionLength = 64;
constexpr uint32_t kMaxEntries = 1u << 20;
constexpr uint32_t kMaxNameLength = 4096;

class FileReader
{
public:
    FileReader(int fd, uint64_t size) : m_fd(fd), m_size(size) {}

    bool bytes(void *out, size_t n)
    {
        if (m_pos + n > m_size) {
            return false;
        }
        size_t done = 0;
        while (done < n) {
            const ssize_t r = ::pread(m_fd, static_cast<char *>(out) + done, n - done, static_cast<off_t>(m_pos + done));
            if (r <= 0) {
                return false;
            }
            done += static_cast<size_t>(r);
        }
        m_pos += n;
        return true;
    }
    bool u32(uint32_t &out) { return bytes(&out, sizeof(out)); }
    bool string(uint32_t maxLength, std::string &out)
    {
        uint32_t length = 0;
        if (!u32(length) || length > maxLength) {
            return false;
        }
        out.resize(length);
        return length == 0 || bytes(out.data(), length);
    }
    uint64_t position() const { return m_pos; }

private:
    int m_fd;
    uint64_t m_size;
    uint64_t m_pos = 0;
};

} // namespace

std::string foldCase(std::string_view text)
{
    std::string out(text);
    for (char &c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

std::optional<PkgArchive> PkgArchive::open(const std::string &path, std::string *error)
{
    const auto fail = [&](const std::string &what) -> std::optional<PkgArchive> {
        if (error) {
            *error = path + ": " + what;
        }
        return std::nullopt;
    };
    bridge::UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
    if (!fd) {
        return fail(std::strerror(errno));
    }
    struct stat st{};
    if (::fstat(fd.get(), &st) != 0 || !S_ISREG(st.st_mode)) {
        return fail("not a regular file");
    }
    const auto size = static_cast<uint64_t>(st.st_size);
    FileReader reader(fd.get(), size);

    PkgArchive archive;
    archive.m_path = path;
    if (!reader.string(kMaxVersionLength, archive.m_version) || archive.m_version.rfind("PKGV", 0) != 0) {
        return fail("not a Wallpaper Engine package (bad header)");
    }
    uint32_t count = 0;
    if (!reader.u32(count) || count > kMaxEntries) {
        return fail("bad entry count");
    }
    archive.m_entries.reserve(count);
    std::vector<std::pair<uint32_t, uint32_t>> raw; // offset, length relative to the data start
    raw.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        PkgEntry entry;
        uint32_t offset = 0;
        if (!reader.string(kMaxNameLength, entry.name) || !reader.u32(offset) || !reader.u32(entry.length)) {
            return fail("truncated file table");
        }
        for (char &c : entry.name) {
            if (c == '\\') {
                c = '/';
            }
        }
        raw.emplace_back(offset, entry.length);
        archive.m_entries.push_back(std::move(entry));
    }
    const uint64_t dataStart = reader.position();
    for (size_t i = 0; i < archive.m_entries.size(); ++i) {
        PkgEntry &entry = archive.m_entries[i];
        entry.offset = dataStart + raw[i].first;
        if (entry.offset + entry.length > size) {
            return fail("entry '" + entry.name + "' lies outside the file");
        }
        archive.m_exact.emplace(entry.name, i);
        archive.m_folded.emplace(foldCase(entry.name), i);
    }
    archive.m_fd = std::move(fd);
    return archive;
}

const PkgEntry *PkgArchive::find(std::string_view name) const
{
    if (auto it = m_exact.find(std::string(name)); it != m_exact.end()) {
        return &m_entries[it->second];
    }
    if (auto it = m_folded.find(foldCase(name)); it != m_folded.end()) {
        return &m_entries[it->second];
    }
    return nullptr;
}

std::optional<std::vector<uint8_t>> PkgArchive::read(std::string_view name) const
{
    const PkgEntry *entry = find(name);
    if (!entry) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(entry->length);
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t r = ::pread(m_fd.get(), data.data() + done, data.size() - done, static_cast<off_t>(entry->offset + done));
        if (r <= 0) {
            return std::nullopt;
        }
        done += static_cast<size_t>(r);
    }
    return data;
}

std::optional<std::vector<uint8_t>> PkgArchive::readPrefix(std::string_view name, size_t maxBytes) const
{
    const PkgEntry *entry = find(name);
    if (!entry) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(std::min<size_t>(entry->length, maxBytes));
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t r = ::pread(m_fd.get(), data.data() + done, data.size() - done, static_cast<off_t>(entry->offset + done));
        if (r <= 0) {
            return std::nullopt;
        }
        done += static_cast<size_t>(r);
    }
    return data;
}

} // namespace murasaki::scene
