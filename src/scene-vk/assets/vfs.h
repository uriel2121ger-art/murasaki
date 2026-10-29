#pragma once

// The file system the scene sees. Scene files name their resources by relative paths ("materials/foo.json",
// "shaders/genericimage2.frag"); the same path can live in the item folder, in the item's scene.pkg, or in the
// Wallpaper Engine installation's assets/ directory. Sources are searched in the order they were added, which for an
// item is: item folder, packages found in it, then the assets directory.

#include "assets/pkg.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace murasaki::scene {

class Vfs
{
public:
    // Directory listings by folded name, filled on demand by lookups (so a Vfs is not for concurrent use).
    using ListingCache = std::unordered_map<std::string, std::unordered_map<std::string, std::string>>;

    void addDirectory(std::string path);
    void addPackage(std::shared_ptr<PkgArchive> package);

    // Whole file, or nullopt. A path with ".." components never matches. A directory source falls back to a
    // case-insensitive match, like a package does.
    std::optional<std::vector<uint8_t>> read(std::string_view path) const;
    bool exists(std::string_view path) const;

    // Where a file lives on disk: a region of a package or a whole file. Lets a very big file (a video texture of hundreds of megabytes)
    // be read where it is, in the pieces that are needed, instead of held in memory.
    struct Location {
        std::string file;
        uint64_t offset = 0;
        uint64_t length = 0;
    };
    std::optional<Location> locate(std::string_view path) const;
    // The first `maxBytes` of a file (fewer when it is shorter).
    std::optional<std::vector<uint8_t>> readPrefix(std::string_view path, size_t maxBytes) const;

    // Text convenience: same as read() but as a string.
    std::optional<std::string> readText(std::string_view path) const;

    // Item setup: the item directory, then scene.pkg (or, failing that, the first *.pkg in it) from that directory,
    // then `assetsDir` when it is not empty, then each folder of `assetsDir/effects/` (every shipped effect has its own
    // materials/ and shaders/ tree). Returns the Vfs; `error` gets a note for a package that failed to open.
    static Vfs forItem(const std::string &itemDir, const std::string &assetsDir, std::string *error = nullptr);

private:
    struct Source {
        std::string directory;                 // set for a directory source
        std::shared_ptr<PkgArchive> package;   // set for a package source
    };
    std::vector<Source> m_sources;
    mutable ListingCache m_listings;
};

// Cleans a scene path: backslashes become slashes, "./" and duplicate slashes go, a leading slash goes. Returns an
// empty string for an unusable path (".." component or empty).
std::string normalizeAssetPath(std::string_view path);

// Where the Wallpaper Engine "assets" directory is. `hint` wins when it names an existing directory; otherwise the
// Steam libraries are searched: $MURASAKI_STEAM_ROOT, the Steam root implied by an item path under
// ".../steamapps/workshop/content/", and the usual per-user Steam locations. Empty when nothing was found.
std::string findAssetsDir(const std::string &hint, const std::string &itemDir);

} // namespace murasaki::scene
