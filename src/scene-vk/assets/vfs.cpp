#include "assets/vfs.h"

#include "util/log.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace murasaki::scene {

namespace fs = std::filesystem;

namespace {

// No scene file is anywhere near this: a bigger one is a mistake or an attack, not a wallpaper.
constexpr uint64_t kMaxFileBytes = 512ull << 20;

std::optional<std::vector<uint8_t>> readFile(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || static_cast<uint64_t>(st.st_size) > kMaxFileBytes) {
        return std::nullopt;
    }
    std::vector<uint8_t> data(static_cast<size_t>(st.st_size));
    in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!in && !data.empty()) {
        return std::nullopt;
    }
    return data;
}

using Listing = std::unordered_map<std::string, std::string>; // folded name -> real name

// The folded names of a directory, read once per Vfs (a scene asks for hundreds of paths in the same few folders).
const Listing &listing(Vfs::ListingCache &cache, const std::string &directory)
{
    const auto found = cache.find(directory);
    if (found != cache.end()) {
        return found->second;
    }
    Listing names;
    if (DIR *dir = ::opendir(directory.c_str())) {
        while (const dirent *e = ::readdir(dir)) {
            names.emplace(foldCase(e->d_name), e->d_name); // the first spelling wins when two differ only by case
        }
        ::closedir(dir);
    }
    return cache.emplace(directory, std::move(names)).first->second;
}

// The real path of `path` under `root` (spelled as the directory spells it), or empty.
std::string resolveInDirectory(Vfs::ListingCache &cache, const std::string &root, const std::string &path)
{
    struct stat st{};
    const std::string direct = root + "/" + path;
    if (::stat(direct.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
        return direct;
    }
    // Case-insensitive walk, component by component.
    std::string current = root;
    size_t start = 0;
    while (start <= path.size()) {
        const size_t slash = path.find('/', start);
        const std::string component = path.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const Listing &names = listing(cache, current);
        const auto it = names.find(foldCase(component));
        if (it == names.end()) {
            return {};
        }
        current += "/" + it->second;
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    return ::stat(current.c_str(), &st) == 0 && S_ISREG(st.st_mode) ? current : std::string();
}

std::optional<std::vector<uint8_t>> readFromDirectory(Vfs::ListingCache &cache, const std::string &root, const std::string &path)
{
    if (auto data = readFile(root + "/" + path)) {
        return data;
    }
    const std::string real = resolveInDirectory(cache, root, path);
    return real.empty() ? std::nullopt : readFile(real);
}

} // namespace

std::string normalizeAssetPath(std::string_view path)
{
    std::string out;
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find_first_of("/\\", start);
        if (end == std::string_view::npos) {
            end = path.size();
        }
        const std::string_view part = path.substr(start, end - start);
        if (part == "..") {
            return {};
        }
        if (!part.empty() && part != ".") {
            if (!out.empty()) {
                out += '/';
            }
            out += part;
        }
        start = end + 1;
    }
    return out;
}

void Vfs::addDirectory(std::string path)
{
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    m_sources.push_back({std::move(path), nullptr});
}

void Vfs::addPackage(std::shared_ptr<PkgArchive> package)
{
    m_sources.push_back({{}, std::move(package)});
}

std::optional<std::vector<uint8_t>> Vfs::read(std::string_view path) const
{
    const std::string clean = normalizeAssetPath(path);
    if (clean.empty()) {
        return std::nullopt;
    }
    for (const Source &source : m_sources) {
        if (source.package) {
            if (auto data = source.package->read(clean)) {
                return data;
            }
        } else if (auto data = readFromDirectory(m_listings, source.directory, clean)) {
            return data;
        }
    }
    return std::nullopt;
}

std::optional<Vfs::Location> Vfs::locate(std::string_view path) const
{
    const std::string clean = normalizeAssetPath(path);
    if (clean.empty()) {
        return std::nullopt;
    }
    for (const Source &source : m_sources) {
        if (source.package) {
            if (const PkgEntry *entry = source.package->find(clean)) {
                return Location{source.package->path(), entry->offset, entry->length};
            }
        } else if (const std::string real = resolveInDirectory(m_listings, source.directory, clean); !real.empty()) {
            struct stat st{};
            if (::stat(real.c_str(), &st) == 0) {
                return Location{real, 0, static_cast<uint64_t>(st.st_size)};
            }
        }
    }
    return std::nullopt;
}

std::optional<std::vector<uint8_t>> Vfs::readPrefix(std::string_view path, size_t maxBytes) const
{
    const std::string clean = normalizeAssetPath(path);
    if (clean.empty()) {
        return std::nullopt;
    }
    for (const Source &source : m_sources) {
        if (source.package) {
            if (auto data = source.package->readPrefix(clean, maxBytes)) {
                return data;
            }
        } else if (const std::string real = resolveInDirectory(m_listings, source.directory, clean); !real.empty()) {
            std::ifstream in(real, std::ios::binary);
            if (!in) {
                continue;
            }
            std::vector<uint8_t> data(maxBytes);
            in.read(reinterpret_cast<char *>(data.data()), static_cast<std::streamsize>(data.size()));
            data.resize(static_cast<size_t>(in.gcount()));
            return data;
        }
    }
    return std::nullopt;
}

bool Vfs::exists(std::string_view path) const
{
    const std::string clean = normalizeAssetPath(path);
    if (clean.empty()) {
        return false;
    }
    for (const Source &source : m_sources) {
        if (source.package) {
            if (source.package->find(clean)) {
                return true;
            }
        } else if (readFromDirectory(m_listings, source.directory, clean)) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> Vfs::readText(std::string_view path) const
{
    auto data = read(path);
    if (!data) {
        return std::nullopt;
    }
    return std::string(data->begin(), data->end());
}

Vfs Vfs::forItem(const std::string &itemDir, const std::string &assetsDir, std::string *error)
{
    Vfs vfs;
    vfs.addDirectory(itemDir);
    std::vector<std::string> packages;
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(itemDir, ec)) {
        if (entry.is_regular_file(ec) && foldCase(entry.path().extension().string()) == ".pkg") {
            packages.push_back(entry.path().string());
        }
    }
    // scene.pkg first, the rest in name order so the result does not depend on readdir.
    std::sort(packages.begin(), packages.end(), [](const std::string &a, const std::string &b) {
        const bool sa = fs::path(a).filename() == "scene.pkg";
        const bool sb = fs::path(b).filename() == "scene.pkg";
        return sa != sb ? sa : a < b;
    });
    for (const std::string &path : packages) {
        std::string openError;
        if (auto archive = PkgArchive::open(path, &openError)) {
            vfs.addPackage(std::make_shared<PkgArchive>(std::move(*archive)));
        } else if (error) {
            *error += (error->empty() ? "" : "; ") + openError;
        }
    }
    if (!assetsDir.empty()) {
        vfs.addDirectory(assetsDir);
        // Every shipped effect carries its own materials/ and shaders/ trees (assets/effects/<name>/materials/...),
        // which scenes (and the packages made from them) name without the effect folder: they are roots too, after
        // everything else, so an item's own copy always wins.
        std::vector<std::string> effectRoots;
        for (const auto &entry : fs::directory_iterator(fs::path(assetsDir) / "effects", ec)) {
            if (entry.is_directory(ec)) {
                effectRoots.push_back(entry.path().string());
            }
        }
        std::sort(effectRoots.begin(), effectRoots.end());
        for (std::string &root : effectRoots) {
            vfs.addDirectory(std::move(root));
        }
    }
    return vfs;
}

std::string findAssetsDir(const std::string &hint, const std::string &itemDir)
{
    std::error_code ec;
    const auto isDir = [&](const fs::path &p) { return fs::is_directory(p, ec); };
    if (!hint.empty() && isDir(hint)) {
        return hint;
    }
    std::vector<fs::path> roots;
    if (const char *env = std::getenv("MURASAKI_STEAM_ROOT"); env && *env) {
        roots.emplace_back(env);
    }
    // An item under <steam>/steamapps/workshop/content/431960/<id> tells where its Steam library is.
    const std::string marker = "/steamapps/workshop/content/";
    if (const size_t at = itemDir.find(marker); at != std::string::npos) {
        roots.emplace_back(itemDir.substr(0, at));
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        for (const char *rel : {".steam/steam", ".steam/debian-installation", ".local/share/Steam",
                                ".var/app/com.valvesoftware.Steam/.local/share/Steam"}) {
            roots.emplace_back(fs::path(home) / rel);
        }
    }
    for (const fs::path &root : roots) {
        const fs::path candidate = root / "steamapps/common/wallpaper_engine/assets";
        if (isDir(candidate)) {
            return candidate.string();
        }
    }
    return {};
}

} // namespace murasaki::scene
