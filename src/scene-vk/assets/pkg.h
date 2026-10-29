#pragma once

// Reader for Wallpaper Engine .pkg archives (the scene.pkg of a Workshop item).
//
// Layout, as far as scenes need it: a length-prefixed version string ("PKGV0001" ... "PKGV0021"; the revisions
// differ only in the file contents they ship, not in this header), a u32 entry count, then for every entry a
// length-prefixed name followed by u32 offset and u32 length. The file data follows the table; entry offsets are
// relative to the end of the table. All integers are little-endian.
//
// The archive stays open and entries are read on demand with pread(), so a large package costs only its table.

#include "bridge/channel.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace murasaki::scene {

struct PkgEntry {
    std::string name; // forward slashes, as stored
    uint64_t offset = 0; // absolute offset in the file
    uint32_t length = 0;
};

class PkgArchive
{
public:
    // Fails (nullopt + `error`) on a missing file, a bad header or an entry that does not fit in the file.
    static std::optional<PkgArchive> open(const std::string &path, std::string *error = nullptr);

    const std::string &path() const { return m_path; }
    const std::string &version() const { return m_version; }
    const std::vector<PkgEntry> &entries() const { return m_entries; }

    // Exact name first, then a case-insensitive match (Windows authors are not careful about case).
    const PkgEntry *find(std::string_view name) const;
    std::optional<std::vector<uint8_t>> read(std::string_view name) const;
    // The first `maxBytes` of an entry (fewer when it is shorter).
    std::optional<std::vector<uint8_t>> readPrefix(std::string_view name, size_t maxBytes) const;

private:
    std::string m_path;
    std::string m_version;
    bridge::UniqueFd m_fd;
    std::vector<PkgEntry> m_entries;
    std::unordered_map<std::string, size_t> m_exact;
    std::unordered_map<std::string, size_t> m_folded;
};

// ASCII lower-casing, enough for the file names in packages.
std::string foldCase(std::string_view text);

} // namespace murasaki::scene
