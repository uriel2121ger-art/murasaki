#pragma once

#include "colorkey.h"
#include "sheetslicer.h"

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>

namespace murasaki::import {

enum class ImportKind { Sheet, Images, Zip, Gif, Mdaa, Video };
// What happens to an image whose size is not the first one's: refuse the set, letterbox it into the largest canvas, or scale it to the first.
enum class SizePolicy { First, Fit, Scale };
enum class OutputFormat { Apng, Webp };

// The job description (docs/pets/import.md, "The job"): JSON of at most kMaxSpecBytes, strict. One function validates it for the daemon and for the
// helper, so a preview refuses exactly what the job would.
struct ImportSpec {
    ImportKind kind = ImportKind::Sheet;
    QString name;                  // shown to the user; the file name is sanitiseOutputName(name)
    QStringList sources;           // absolute paths: one for sheet, zip, gif and mdaa; two or more files, or one folder, for images
    SheetSpec sheet;               // sheet only; sheetWidth and sheetHeight are read from the image
    int frameMs = 0;               // sheet: per frame (default 100); images and zip: derived from fps unless set
    double fps = 0;                // images and zip: frames a second (default kDefaultPetFps); 0 when frameMs rules
    SizePolicy sizePolicy = SizePolicy::Fit;  // the user's answer: letterbox; "first" refuses a different size
    OutputFormat format = OutputFormat::Apng;
    int webpQuality = 100;         // webp only: 100 is lossless, 1..99 lossy
    ColorKey colorKey;             // any kind; off unless the spec turns it on
    bool keepSource = false;       // copy the source file(s) into the job folder (deleted with it)
    bool keepAudio = false;
    // video only: the VideoSettings of videosource.h (defaults are the plan's; the daemon's importVideoMaxSide and importVideoMaxFps start the forms)
    int maxSide = 1024;            // longest side; a smaller video is never scaled up
    double maxFps = 30;            // frames a second at most
    long long startMs = 0;         // trim: the first moment kept
    long long durationMs = 0;      // trim: how long; 0 is to the end
    int autoScene = -1;            // mdaa only: the scene (0-based, in actions.mddo order) a pet of the package starts in; -1 none (scenes start by hand)        // video only (task 5): keep the audio track for phase 5; stored, never played now
};

inline constexpr qsizetype kMaxSpecBytes = 2 << 20; // room for kMaxSources paths; a folder is one source
inline constexpr int kMaxSources = 20064;
inline constexpr int kMaxFrameMs = 60000;

struct SpecResult {
    std::optional<ImportSpec> spec;
    QString error; // a name of petimport.h followed by ": " and the detail
    bool ok() const { return spec.has_value(); }
};

// Keys: kind, name, sources, frameWidth, frameHeight, frameCount, startFrame, paddingLeft, paddingTop, spacingH, spacingV, columns, rows, frameMs, fps,
// sizePolicy, format ("apng"|"webp"), webpQuality, colorKey {color "RRGGBB", tolerance 0..100, softness 0..100}, keepSource, keepAudio.
// Unknown keys are ignored, a wrong type or a value out of range is an error. Paths must be absolute and free of NUL characters; they are not looked
// at here (the helper checks that they are regular files).
SpecResult parseImportSpec(const QByteArray &json);
// The spec as canonical JSON (sorted keys, defaults written), without the source paths: the form that is hashed (with the sources' content hashes) for
// the job's folder and saved in source.json, so that the same import of the same files from another place is the same folder and no path is kept.
QByteArray canonicalJson(const ImportSpec &spec);

// The frame delay in ms the spec asks for: frameMs, else 1000 / fps rounded, else the kind's default.
int effectiveFrameMs(const ImportSpec &spec);

// A file name stem from a user's name: letters, digits, '-', '_', '.' and ' ' kept, anything else (a path separator, control characters, '..' runs)
// becomes '_', at most 64 bytes, never empty and never starting with a dot or a space.
QString sanitiseOutputName(const QString &name);

// Natural order of file names ("2" before "10"), case-insensitive, ties broken by the plain comparison so that the order is total.
bool naturalLess(const QString &a, const QString &b);

} // namespace murasaki::import
