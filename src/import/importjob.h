#pragma once

#include "importfolders.h"
#include "importspec.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace murasaki::import {

struct ImportOptions {
    QString outRoot;                                         // <data>/pets/imported: the job makes <outRoot>/<id>/ in it
    long long maxOutputBytes = 1LL << 30;
    long long maxMillis = 15LL * 60 * 1000;                  // wall time; checked between frames, ends with "time budget exceeded" before RLIMIT_CPU can kill the helper
    std::function<void(double fraction, int frames)> progress; // optional; called at most once a frame; fraction is -1 when the length is not known
    std::function<bool()> cancelled;                          // optional; polled between frames
};

struct ImportOutcome {
    bool ok = false;
    QString error;            // a name of petimport.h, followed by ": " and the detail
    bool reused = false;      // the same spec and sources had been imported: nothing was written
    QString id;               // the folder name
    QString directory;        // <outRoot>/<id>
    QString file;             // the animation inside it
    int frames = 0;
    int variants = 0;         // package imports: how many animations the folder holds (the folder is `file`)
    int width = 0;
    int height = 0;
    long long totalDelayMs = 0;
    long long bytes = 0;      // the animation's size
    QString framesSha256;     // SHA-256 over every frame's RGBA bytes, in order; what a test decodes the file against
    QStringList degraded;     // informational names (petimport.h)
};

struct ImportPreview {
    bool ok = false;
    QString error;
    int frames = 0;           // 0 when the source says nothing before it is decoded (never for sheet, images, zip)
    int width = 0;
    int height = 0;
    long long totalDelayMs = -1; // -1: not known before decoding (a GIF keeps its own delays)
    long long estimatedBytes = 0; // an upper bound: raw RGBA
    QStringList degraded;
};

// Reads headers only: nothing is decoded or written. Refuses what runImport would refuse for the same reasons that can be seen without decoding.
ImportPreview previewImport(const ImportSpec &spec);

// The whole job, in this process (the daemon runs it in murasaki-petimport behind the sandbox). Writes <outRoot>/<id>.part/, validates the result
// and renames it to <outRoot>/<id>; failure or cancellation removes the .part folder. The spec decides the id, so the same import is one folder.
ImportOutcome runImport(const ImportSpec &spec, const ImportOptions &options);

// The folder name for a spec and the fingerprints of its sources (exposed for the tests).
QString importId(const QByteArray &canonicalSpec, const QStringList &sourceSha256);

} // namespace murasaki::import
