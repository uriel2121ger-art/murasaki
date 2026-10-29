#include "puppet/manifest.h"

#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace murasaki::puppet {

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxReasons = 16; // what one check lists at most; more would only repeat the same mistake

struct Scan {
    std::map<std::string, std::int64_t> files; // regular files by path relative to the item folder
    std::int64_t entries = 0;                  // files and folders
    std::int64_t bytes = 0;
};

bool endsWithNoCase(std::string_view text, std::string_view suffix)
{
    if (text.size() < suffix.size())
        return false;
    const std::string_view tail = text.substr(text.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(tail[i])) != suffix[i])
            return false;
    return true;
}

class Loader
{
public:
    explicit Loader(ManifestResult &result) : m_r(result) {}

    void run(const std::string &folder)
    {
        std::error_code ec;
        const fs::path root = fs::canonical(fs::path(folder), ec);
        if (folder.empty() || ec || !fs::is_directory(root, ec)) {
            add(kNotAPuppet, "not a folder");
            return;
        }
        m_r.manifest.folder = root.string();
        if (!scan(root))
            return;
        m_r.manifest.fileCount = m_scan.entries;
        m_r.manifest.totalBytes = m_scan.bytes;

        std::string model;
        std::string inp;
        int models = 0;
        int inps = 0;
        for (const auto &[path, size] : m_scan.files) {
            if (path.find('/') != std::string::npos)
                continue;
            if (path.size() > 12 && endsWithNoCase(path, ".model3.json")) {
                model = path;
                ++models;
            } else if (path.size() > 4 && endsWithNoCase(path, ".inp")) {
                inp = path;
                ++inps;
            }
        }
        if (models + inps == 0) {
            add(kNotAPuppet, "no .model3.json or .inp file");
            return;
        }
        if (models + inps > 1) {
            add(kNotAPuppet, models + inps == 2 && models == 1 ? "a .model3.json and an .inp file" : "more than one manifest or container");
            return;
        }
        if (models == 1) {
            m_r.format = Format::Cubism;
            m_r.manifest.file = model;
            if (readCubism())
                m_r.ok = m_r.reasons.empty();
            if (m_r.ok)
                m_r.degraded.push_back({kNeedsCubismRuntime, {}});
        } else {
            m_r.format = Format::Open;
            m_r.manifest.file = inp;
            readOpen();
            m_r.ok = m_r.reasons.empty();
            if (m_r.ok)
                m_r.degraded.push_back({kPayloadNotRead, {}}); // loadManifest looks at the signature only; checkItem reads the puppet and removes this
        }
    }

private:
    void add(std::string_view name, std::string detail = {})
    {
        for (const Problem &known : m_r.reasons)
            if (known.name == name && known.detail == detail)
                return; // a file named twice is said once
        if (m_r.reasons.size() < kMaxReasons)
            m_r.reasons.push_back({name, std::move(detail)});
    }

    bool tooLarge(std::string_view limit)
    {
        add(kTooLarge, std::string(limit));
        return false;
    }

    bool invalid(std::string detail)
    {
        add(kManifestInvalid, std::move(detail));
        return false;
    }

    // ---- the folder ----
    bool scan(const fs::path &root)
    {
        struct Dir {
            fs::path path;
            std::string prefix;
            int depth;
        };
        std::vector<Dir> stack{{root, {}, 0}};
        while (!stack.empty()) {
            const Dir dir = std::move(stack.back());
            stack.pop_back();
            std::error_code ec;
            fs::directory_iterator it(dir.path, fs::directory_options::none, ec);
            if (ec) {
                add(kUnsafePath, "a folder cannot be listed");
                return false;
            }
            for (const fs::directory_iterator end; it != end; it.increment(ec)) {
                if (ec) {
                    add(kUnsafePath, "a folder cannot be listed");
                    return false;
                }
                if (++m_scan.entries > kMaxItemFiles)
                    return tooLarge("kMaxItemFiles");
                const std::string name = it->path().filename().string();
                const fs::file_status status = it->symlink_status(ec);
                if (ec) {
                    add(kUnsafePath, "an entry cannot be examined");
                    return false;
                }
                const std::string relative = dir.prefix + name;
                if (fs::is_symlink(status)) {
                    add(kUnsafePath, "symbolic link: " + relative);
                    return false;
                }
                if (fs::is_directory(status)) {
                    if (dir.depth + 1 > kMaxItemDirDepth)
                        return tooLarge("kMaxItemDirDepth");
                    stack.push_back({it->path(), relative + "/", dir.depth + 1});
                } else if (fs::is_regular_file(status)) {
                    const std::uintmax_t size = it->file_size(ec);
                    if (ec) {
                        add(kUnsafePath, "an entry cannot be examined");
                        return false;
                    }
                    if (size > static_cast<std::uintmax_t>(kMaxItemBytes) || (m_scan.bytes += static_cast<std::int64_t>(size)) > kMaxItemBytes)
                        return tooLarge("kMaxItemBytes");
                    m_scan.files.emplace(relative, static_cast<std::int64_t>(size));
                } else {
                    add(kUnsafePath, "not a regular file: " + relative);
                    return false;
                }
            }
        }
        return true;
    }

    // Reads at most `maxBytes` (plus one, to tell) of a file of the item through openInItem; it must be a regular file.
    bool readFile(const std::string &relative, size_t maxBytes, std::string &out)
    {
        std::string why;
        if (!readItemFile(m_r.manifest.folder, relative, maxBytes, out, &why)) {
            add(why == "missing" ? kFileMissing : kUnsafePath, why == "missing" ? relative : why + ": " + relative);
            return false;
        }
        return true;
    }

    // ---- the Cubism manifest ----
    bool str(const json::Value *v, const std::string &what, std::string &out, bool required, size_t maxBytes = static_cast<size_t>(kMaxIdBytes))
    {
        if (!v)
            return !required || invalid("missing " + what);
        if (!v->isString())
            return invalid(what + " is not a string");
        if (v->text.size() > maxBytes)
            return invalid(what + " is too long");
        out = v->text;
        return true;
    }

    bool fade(const json::Value *v, const std::string &what, std::optional<double> &out)
    {
        if (!v)
            return true;
        if (!v->isNumber() || v->number < 0 || v->number > static_cast<double>(kMaxFadeSeconds))
            return invalid(what + " is not a time from 0 to kMaxFadeSeconds");
        out = v->number;
        return true;
    }

    bool readCubism()
    {
        Manifest &m = m_r.manifest;
        if (m_scan.files.at(m.file) > kMaxManifestBytes)
            return tooLarge("kMaxManifestBytes");
        std::string text;
        if (!readFile(m.file, static_cast<size_t>(kMaxManifestBytes), text))
            return false;
        if (static_cast<std::int64_t>(text.size()) > kMaxManifestBytes)
            return tooLarge("kMaxManifestBytes");
        const json::ParseResult parsed = json::parse(text, {kMaxJsonDepth, kMaxJsonValues, kMaxJsonStringBytes});
        if (!parsed.ok)
            return invalid(parsed.error + " at byte " + std::to_string(parsed.offset));
        const json::Value &root = parsed.value;
        if (!root.isObject())
            return invalid("the top level is not an object");

        const json::Value *version = root.find("Version");
        if (!version || !version->isNumber())
            return invalid("Version is missing or not a number");
        if (version->number != 3) {
            char buffer[32];
            std::snprintf(buffer, sizeof buffer, "%.6g", version->number);
            add(kUnsupportedManifestVersion, buffer);
            return false;
        }
        m.version = 3;

        const json::Value *refs = root.find("FileReferences");
        if (!refs || !refs->isObject())
            return invalid("FileReferences is missing or not an object");
        if (!str(refs->find("Moc"), "Moc", m.moc, true, static_cast<size_t>(kMaxPathBytes)))
            return false;
        const json::Value *textures = refs->find("Textures");
        if (!textures || !textures->isArray() || textures->size() == 0)
            return invalid("Textures is missing, empty or not a list");
        if (static_cast<std::int64_t>(textures->size()) > kMaxTextures)
            return tooLarge("kMaxTextures");
        if (!textures->numbers.empty())
            return invalid("a texture is not a string");
        for (const json::Value &item : textures->items) {
            std::string path;
            if (!str(&item, "a texture", path, true, static_cast<size_t>(kMaxPathBytes)))
                return false;
            m.textures.push_back(std::move(path));
        }
        if (!str(refs->find("Physics"), "Physics", m.physics, false, static_cast<size_t>(kMaxPathBytes)) || !str(refs->find("Pose"), "Pose", m.pose, false, static_cast<size_t>(kMaxPathBytes))
            || !str(refs->find("DisplayInfo"), "DisplayInfo", m.displayInfo, false, static_cast<size_t>(kMaxPathBytes))
            || !str(refs->find("UserData"), "UserData", m.userData, false, static_cast<size_t>(kMaxPathBytes)))
            return false;
        if (!str(refs->find("MotionSync"), "MotionSync", m.motionSync, false, static_cast<size_t>(kMaxPathBytes)))
            return false;

        if (const json::Value *expressions = refs->find("Expressions")) {
            if (!expressions->isArray())
                return invalid("Expressions is not a list");
            if (static_cast<std::int64_t>(expressions->size()) > kMaxExpressions)
                return tooLarge("kMaxExpressions");
            std::set<std::string> names;
            if (!expressions->numbers.empty())
                return invalid("an expression is not an object");
            for (const json::Value &item : expressions->items) {
                if (!item.isObject())
                    return invalid("an expression is not an object");
                ExpressionRef expression;
                if (!str(item.find("Name"), "an expression Name", expression.name, true) || !str(item.find("File"), "an expression File", expression.file, true, static_cast<size_t>(kMaxPathBytes)))
                    return false;
                if (expression.name.empty())
                    return invalid("an expression Name is empty");
                if (!names.insert(expression.name).second)
                    return invalid("an expression Name is repeated");
                m.expressions.push_back(std::move(expression));
            }
        }

        if (const json::Value *motions = refs->find("Motions")) {
            if (!motions->isObject())
                return invalid("Motions is not an object");
            if (static_cast<std::int64_t>(motions->members.size()) > kMaxMotionGroups)
                return tooLarge("kMaxMotionGroups");
            std::int64_t total = 0;
            for (const auto &[groupName, group] : motions->members) {
                if (static_cast<std::int64_t>(groupName.size()) > kMaxIdBytes)
                    return invalid("a motion group name is too long");
                if (!group.isArray())
                    return invalid("a motion group is not a list");
                if (static_cast<std::int64_t>(group.size()) > kMaxMotionsPerGroup)
                    return tooLarge("kMaxMotionsPerGroup");
                total += static_cast<std::int64_t>(group.size());
                if (total > kMaxMotions)
                    return tooLarge("kMaxMotions");
                MotionGroup out;
                out.name = groupName;
                if (!group.numbers.empty())
                    return invalid("a motion is not an object");
                for (const json::Value &item : group.items) {
                    if (!item.isObject())
                        return invalid("a motion is not an object");
                    MotionRef motion;
                    if (!str(item.find("File"), "a motion File", motion.file, true, static_cast<size_t>(kMaxPathBytes)) || !fade(item.find("FadeInTime"), "a motion FadeInTime", motion.fadeIn)
                        || !fade(item.find("FadeOutTime"), "a motion FadeOutTime", motion.fadeOut)
                        || !str(item.find("Sound"), "a motion Sound", motion.sound, false, static_cast<size_t>(kMaxPathBytes)))
                        return false;
                    // inside a motion MotionSync is the *name* of a setting of the motion-sync file, not a path
                    if (!str(item.find("MotionSync"), "a motion MotionSync", motion.motionSync, false))
                        return false;
                    out.motions.push_back(std::move(motion));
                }
                m.motionGroups.push_back(std::move(out));
            }
        }

        if (const json::Value *groups = root.find("Groups")) {
            if (!groups->isArray())
                return invalid("Groups is not a list");
            if (static_cast<std::int64_t>(groups->size()) > kMaxParameterGroups)
                return tooLarge("kMaxParameterGroups");
            if (!groups->numbers.empty())
                return invalid("a group is not an object");
            for (const json::Value &item : groups->items) {
                if (!item.isObject())
                    return invalid("a group is not an object");
                ParameterGroup group;
                if (!str(item.find("Target"), "a group Target", group.target, true) || !str(item.find("Name"), "a group Name", group.name, true))
                    return false;
                if (group.target.empty() || group.name.empty())
                    return invalid("a group Target or Name is empty");
                const json::Value *ids = item.find("Ids");
                if (!ids || !ids->isArray())
                    return invalid("a group has no list of Ids");
                if (static_cast<std::int64_t>(ids->size()) > kMaxIdsPerGroup)
                    return tooLarge("kMaxIdsPerGroup");
                if (!ids->numbers.empty())
                    return invalid("a group id is not a string");
                for (const json::Value &id : ids->items) {
                    std::string value;
                    if (!str(&id, "a group id", value, true))
                        return false;
                    group.ids.push_back(std::move(value));
                }
                m.parameterGroups.push_back(std::move(group));
            }
        }

        if (const json::Value *areas = root.find("HitAreas")) {
            if (!areas->isArray())
                return invalid("HitAreas is not a list");
            if (static_cast<std::int64_t>(areas->size()) > kMaxHitAreas)
                return tooLarge("kMaxHitAreas");
            if (!areas->numbers.empty())
                return invalid("a hit area is not an object");
            for (const json::Value &item : areas->items) {
                if (!item.isObject())
                    return invalid("a hit area is not an object");
                HitArea area;
                if (!str(item.find("Name"), "a hit area Name", area.name, true) || !str(item.find("Id"), "a hit area Id", area.id, true))
                    return false;
                m.hitAreas.push_back(std::move(area));
            }
        }

        if (const json::Value *layout = root.find("Layout")) {
            if (!layout->isObject())
                return invalid("Layout is not an object");
            for (const char *key : {"width", "height", "x", "y", "center_x", "center_y", "top", "bottom", "left", "right"})
                if (const json::Value *value = layout->find(key); value && !value->isNumber())
                    return invalid(std::string("Layout ") + key + " is not a number");
        }

        // the files it names: the syntax of every name first, then the extension, the presence and the size
        check("Moc", m.moc, ".moc3", kMaxMocBytes, "kMaxMocBytes");
        for (const std::string &texture : m.textures)
            check("a texture", texture, ".png", kMaxTextureFileBytes, "kMaxTextureFileBytes");
        if (!m.physics.empty() || refsHas(*refs, "Physics"))
            check("Physics", m.physics, ".physics3.json", kMaxPhysicsFileBytes, "kMaxPhysicsFileBytes");
        if (!m.pose.empty() || refsHas(*refs, "Pose"))
            check("Pose", m.pose, ".pose3.json", kMaxPoseFileBytes, "kMaxPoseFileBytes");
        if (!m.displayInfo.empty() || refsHas(*refs, "DisplayInfo"))
            check("DisplayInfo", m.displayInfo, ".cdi3.json", kMaxDisplayInfoFileBytes, "kMaxDisplayInfoFileBytes");
        if (!m.motionSync.empty() || refsHas(*refs, "MotionSync"))
            check("MotionSync", m.motionSync, ".motionsync3.json", kMaxMotionSyncFileBytes, "kMaxMotionSyncFileBytes");
        if (!m.userData.empty() || refsHas(*refs, "UserData"))
            check("UserData", m.userData, ".userdata3.json", kMaxUserDataFileBytes, "kMaxUserDataFileBytes");
        for (const ExpressionRef &expression : m.expressions)
            check("an expression", expression.file, ".exp3.json", kMaxExpressionFileBytes, "kMaxExpressionFileBytes");
        for (const MotionGroup &group : m.motionGroups) {
            for (const MotionRef &motion : group.motions) {
                check("a motion", motion.file, ".motion3.json", kMaxMotionFileBytes, "kMaxMotionFileBytes");
                if (!motion.sound.empty()) {
                    std::string why;
                    if (!relativePathOk(motion.sound, &why))
                        add(kUnsafePath, "a motion Sound: " + why);
                }
            }
        }
        return m_r.reasons.empty();
    }

    static bool refsHas(const json::Value &refs, const char *key) { return refs.find(key) != nullptr; }

    void check(const char *role, const std::string &path, std::string_view extension, std::int64_t maxBytes, const char *limitName)
    {
        std::string why;
        if (!relativePathOk(path, &why)) {
            add(kUnsafePath, std::string(role) + ": " + why);
            return;
        }
        if (!endsWithNoCase(path, extension)) {
            add(kUnsupportedFormat, std::string(role) + " is not a " + std::string(extension) + " file: " + path);
            return;
        }
        const auto found = m_scan.files.find(path);
        if (found == m_scan.files.end()) {
            add(kFileMissing, path);
            return;
        }
        if (found->second > maxBytes)
            add(kTooLarge, std::string(limitName) + ": " + path);
    }

    // ---- the Inochi2D container ----
    void readOpen()
    {
        Manifest &m = m_r.manifest;
        const std::int64_t size = m_scan.files.at(m.file);
        if (size > kMaxInpBytes) {
            tooLarge("kMaxInpBytes");
            return;
        }
        std::string head;
        if (!readFile(m.file, 12, head))
            return;
        head.resize(std::min<size_t>(head.size(), 12));
        if (head.size() < 12) {
            add(kUnsupportedFormat, "container too short: " + m.file);
            return;
        }
        if (head.compare(0, 8, std::string("TRNSRTS\0", 8)) == 0) {
            const std::uint64_t length = (std::uint64_t(static_cast<unsigned char>(head[8])) << 24) | (std::uint64_t(static_cast<unsigned char>(head[9])) << 16)
                | (std::uint64_t(static_cast<unsigned char>(head[10])) << 8) | std::uint64_t(static_cast<unsigned char>(head[11]));
            if (12 + length > static_cast<std::uint64_t>(size)) {
                add(kUnsupportedFormat, "the JSON length runs past the end of " + m.file);
                return;
            }
            m.version = 1;
        } else if (head.compare(0, 8, "TRNSRTS2") == 0) {
            m.version = 2;
        } else {
            add(kUnsupportedFormat, "unknown container signature: " + m.file);
        }
    }

    ManifestResult &m_r;
    Scan m_scan;
};

} // namespace

int openInItem(const std::string &folder, std::string_view relative, bool forceFallback)
{
    if (!relativePathOk(relative)) {
        errno = EINVAL;
        return -1;
    }
    const int root = ::open(folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root < 0)
        return -1;
    const std::string rel(relative);
    int fd = -1;
    if (!forceFallback) {
        open_how how{};
        how.flags = O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW;
        how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
        const long r = ::syscall(SYS_openat2, root, rel.c_str(), &how, sizeof how);
        if (r >= 0) {
            ::close(root);
            return static_cast<int>(r);
        }
        if (errno != ENOSYS && errno != EPERM && errno != E2BIG && errno != EINVAL) {
            const int saved = errno;
            ::close(root);
            errno = saved;
            return -1;
        }
    }
    // no openat2 (an old kernel, or a filter that does not know it): walk the components, none of which may be a link
    int dir = ::dup(root);
    ::close(root);
    if (dir < 0)
        return -1;
    size_t start = 0;
    while (true) {
        size_t end = rel.find('/', start);
        const bool last = end == std::string::npos;
        const std::string part = rel.substr(start, last ? std::string::npos : end - start);
        if (last) {
            fd = ::openat(dir, part.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW);
            break;
        }
        const int next = ::openat(dir, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        const int saved = errno;
        ::close(dir);
        if (next < 0) {
            errno = saved;
            return -1;
        }
        dir = next;
        start = end + 1;
    }
    const int saved = errno;
    ::close(dir);
    errno = saved;
    return fd;
}

bool readItemFile(const std::string &folder, std::string_view relative, size_t maxBytes, std::string &out, std::string *why)
{
    const auto no = [&](const char *reason) {
        if (why)
            *why = reason;
        return false;
    };
    const int fd = openInItem(folder, relative);
    if (fd < 0)
        return no(errno == ENOENT ? "missing" : "cannot be opened without following a link");
    struct stat st {};
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        return no("not a regular file");
    }
    out.clear();
    char buffer[8192];
    while (out.size() <= maxBytes) {
        const ssize_t n = ::read(fd, buffer, std::min(sizeof buffer, maxBytes + 1 - out.size()));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        out.append(buffer, static_cast<size_t>(n));
    }
    ::close(fd);
    return true;
}

bool relativePathOk(std::string_view path, std::string *why)
{
    const auto no = [&](const char *reason) {
        if (why)
            *why = reason;
        return false;
    };
    if (path.empty())
        return no("the path is empty");
    if (static_cast<std::int64_t>(path.size()) > kMaxPathBytes)
        return no("the path is too long");
    if (!json::validUtf8(path))
        return no("the path is not UTF-8");
    for (const unsigned char c : path) {
        if (c < 0x20 || c == 0x7f)
            return no("the path has a control character");
        if (c == '\\')
            return no("the path has a backslash");
    }
    if (path.front() == '/')
        return no("the path is absolute");
    if (path.size() >= 2 && path[1] == ':' && std::isalpha(static_cast<unsigned char>(path[0])))
        return no("the path starts with a drive letter");
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string_view::npos)
            end = path.size();
        const std::string_view part = path.substr(start, end - start);
        if (part.empty())
            return no("the path has an empty component");
        if (part == "." || part == "..")
            return no("the path has a . or .. component");
        if (static_cast<std::int64_t>(part.size()) > kMaxPathComponentBytes)
            return no("a component of the path is too long");
        start = end + 1;
    }
    return true;
}

ManifestResult loadManifest(const std::string &folder)
{
    ManifestResult result;
    Loader(result).run(folder);
    if (!result.ok && result.reasons.empty())
        result.reasons.push_back({kNotAPuppet, {}});
    return result;
}

} // namespace murasaki::puppet
