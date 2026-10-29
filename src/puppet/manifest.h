#pragma once

#include "puppetnames.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::puppet {

// The manifest of a puppet item: what `murasaki-puppet --check` and the daemon's validation of a folder read before anything is parsed or drawn
// (docs/pets/puppets.md, sections 3 and 8). A puppet item is a folder holding exactly one `<stem>.model3.json` (a Cubism model: the manifest names the other files) or
// exactly one `<stem>.inp` (an Inochi2D container). This checks the folder and the manifest, never the content of the files it names: sizes and names only, plus the
// container signature of an `.inp`. Qt-free (the process that reads hostile models links no GUI stack).
//
// The folder is walked without following any link: a symbolic link, a device, a pipe or a socket anywhere in it refuses the item, and so does a path the manifest names
// that is absolute, uses a backslash, a drive letter, an empty, `.` or `..` component, a control character or bytes that are not UTF-8. Every count and size is bounded by
// puppetlimits.h and checked before it is used; a value over a limit refuses the item.
enum class Format { Cubism, Open };

// A reason (the item is refused) or a degraded entry: a name of puppetnames.h, and what it is about (a file name, a limit name). text() is "<name>" or "<name>: <detail>".
struct Problem {
    std::string_view name;
    std::string detail;
    std::string text() const { return detail.empty() ? std::string(name) : std::string(name) + ": " + detail; }
};

struct ExpressionRef {
    std::string name;
    std::string file;
};

struct MotionRef {
    std::string file;
    std::optional<double> fadeIn;  // seconds, when the manifest says
    std::optional<double> fadeOut;
    std::string sound;             // named, never loaded here
    std::string motionSync;        // the *name* of a setting of the motion-sync file (not a path)
};

struct MotionGroup {
    std::string name;
    std::vector<MotionRef> motions;
};

struct ParameterGroup {
    std::string target;
    std::string name;
    std::vector<std::string> ids;
};

struct HitArea {
    std::string name;
    std::string id;
};

struct Manifest {
    std::string folder;       // the canonical path of the item folder
    std::string file;         // the manifest or container, relative to the folder
    int version = 0;          // Cubism: the manifest's Version (3). Open: the container's version (1 or 2)
    std::string moc;
    std::vector<std::string> textures;
    std::string physics;
    std::string pose;
    std::string displayInfo;
    std::string userData;
    std::string motionSync; // FileReferences.MotionSync: a path, checked like any other named file
    std::vector<ExpressionRef> expressions;
    std::vector<MotionGroup> motionGroups;
    std::vector<ParameterGroup> parameterGroups;
    std::vector<HitArea> hitAreas;
    std::int64_t fileCount = 0;  // files and folders of the item
    std::int64_t totalBytes = 0; // the size of its files
    // Filled by a deeper check (checkItem): -1 when not known
    int parameterCount = -1;
    int drawableCount = -1;
    int textureCount = -1;
    int physicsCount = -1;
    int canvasWidth = -1;  // an Inochi2D puppet at rest: the size of its bounds in model units, rounded (at least 1); -1 when not known (a Cubism model: its canvas is in the moc)
    int canvasHeight = -1;

    int motionCount() const
    {
        int n = 0;
        for (const MotionGroup &group : motionGroups)
            n += int(group.motions.size());
        return n;
    }
};

struct ManifestResult {
    bool ok = false;
    std::optional<Format> format;      // known once the folder holds exactly one manifest or container
    Manifest manifest;                 // filled as far as it was read; complete when ok
    std::vector<Problem> reasons;      // not empty when !ok
    std::vector<Problem> degraded;
};

// OPENING THE FILES OF AN ITEM (a contract for every reader: motions, expressions, physics, pose, the `.inp`, textures). The folder is validated once, by name; the file may be replaced by a link
// later, and so may a folder on its way. So a file of an item is opened only with openInItem(): `openat2` from the item's own directory with RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS |
// RESOLVE_NO_MAGICLINKS (any link on the way, the last component included, fails), or, where openat2 is not available, a walk with one `openat(O_NOFOLLOW)` per component. Never `open(folder + "/" + name)`.
// Returns a descriptor (O_RDONLY, O_NONBLOCK, close-on-exec), or -1 with errno set. `relative` must pass relativePathOk. `forceFallback` is for tests.
int openInItem(const std::string &folder, std::string_view relative, bool forceFallback = false);
// Reads at most `maxBytes` of a file opened that way (one byte more is read, to tell that it was longer); it must be a regular file. `why` is "missing" or the reason otherwise.
bool readItemFile(const std::string &folder, std::string_view relative, size_t maxBytes, std::string &out, std::string *why = nullptr);

// Reads the item in `folder` (a link to a folder is followed for the root only; the canonical path is recorded).
ManifestResult loadManifest(const std::string &folder);

// The path rules for a file a manifest names (relative to the item folder): non-empty, at most kMaxPathBytes, valid UTF-8, no control character, no backslash, not
// absolute, no drive letter, no empty, `.` or `..` component, each component at most kMaxPathComponentBytes. `why` receives the rule broken.
bool relativePathOk(std::string_view path, std::string *why = nullptr);

} // namespace murasaki::puppet
