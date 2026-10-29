#pragma once

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>

namespace murasaki::import {

// One animation of the package, written into the job folder under `file`.
struct MdaaVariant {
    QString name;   // shown to the user: the file's name without its extension
    QString file;   // the file name in the job folder
    QString member; // the member it came from (normalised name), for matching actions.mddo lines
    int frames = 0;
    int width = 0;
    int height = 0;
};

// One accepted line of actions.mddo: show the animation `variant` (and move there), or hide the pet.
struct MdaaScene {
    int line = 0;
    QString variant; // a MdaaVariant::name; empty for a hide line that names no animation
    bool show = false;
    bool hasPosition = false;
    double x = 0;
    double y = 0;
};

struct MdaaResult {
    QString error;             // a name of petimport.h (+ ": detail"); the package is refused as a whole
    QList<MdaaVariant> variants;
    QString preview;           // "preview.png" in the job folder, or empty
    QList<MdaaScene> scenes;
    QStringList notes;         // lines of actions.mddo refused, members skipped (each starts with a name)
    QStringList degraded;      // informational names
    QString executableName;    // core.exe: hashed and dropped
    QString executableSha256;
    QStringList programs;      // other programs found, named and ignored
    bool triggersPack = false;
    QJsonObject pack;          // a triggers pack's pack.json and meta.json as bounded data (triggerspack.h); empty when there is none or it was refused
    QJsonObject screenRefs;    // {status, regions: [{region, status, sourceFrames, frames: [{file, clipIndex}]}]}: the reference pictures kept for the screen triggers (screenframes.h); empty when the pack has no region
    int ignored = 0;           // members that are not used
    bool ok() const { return error.isEmpty(); }
};

// What an import of a package may use: bytes written to the job folder (counted as they are written, not after) and wall time (checked between members).
struct MdaaBudget {
    long long maxBytes = 1LL << 30;
    long long maxMillis = 15LL * 60 * 1000;
};

// Imports an .mdaa package (docs/pets/import.md, "Packages"): a ZIP read with BoundedZip. The animations it holds are written, by us and under names we
// choose, into `folder` (which must exist and be empty): never a member's own name as a path. miniatura.png becomes preview.png; actions.mddo is parsed
// by the strict MddoParser into scenes that name animations of the package; core.exe and every other program are never extracted, never started
// (core.exe is only hashed). Stops early with "cancelled" when `cancelled` says so.
MdaaResult importMdaa(const QString &zipPath, const QString &folder, const std::function<bool()> &cancelled = {}, const std::function<void(double)> &progress = {},
                       const MdaaBudget &budget = {});

// What importMdaa would find, without writing: the number of animations the first bytes of the members say, and whether a program is inside.
struct MdaaPreview {
    QString error;
    int animations = 0;
    bool hasExecutable = false;
    bool triggersPack = false;
    QStringList degraded;
    bool ok() const { return error.isEmpty(); }
};
MdaaPreview previewMdaa(const QString &zipPath);

// The "mdaa" object of source.json: variants, scenes, notes, the executable's hash, the programs found.
QJsonObject mdaaRecord(const MdaaResult &result);

} // namespace murasaki::import
