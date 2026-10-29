#pragma once

// Turns a sound file the user picked into a pet audio asset (docs/pets/audio.md, "Import"). This is the one place a codec touches hostile audio, and
// it runs in a process of its own (`murasaki-petaudio --import`), never in the overlay or the daemon. libavformat and libavcodec read it
// under the same rules as the video source: only the "file" protocol, containers and decoders from lists, one audio stream, every limit checked
// against what is decoded, a deadline and cancellation through the interrupt callback.

#include "petaudiolimits.h"

#include <functional>
#include <string>
#include <vector>

namespace murasaki::audio {

struct ImportSettings {
    long long deadlineMs = 5LL * 60 * 1000; // the whole import: open, probe, read, decode, write
};

struct ImportCallbacks {
    std::function<bool()> cancelled;                 // polled between packets
    std::function<void(double fraction)> progress;   // -1 when the length is not known
};

struct ImportResult {
    bool ok = false;
    bool cancelled = false;
    bool reused = false;           // the same source had been imported: nothing was written
    std::string error;             // a name of petaudiolimits.h, alone or followed by ": " and a detail
    std::string assetId;           // SHA-256 of the source, 64 lowercase hex
    long long durationMs = 0;
    long long bytes = 0;           // the asset's size
    std::string sourceFormat;      // libavformat's name, "mp3"
    std::string sourceCodec;
    int sourceRate = 0;
    int channels = 0;
    std::vector<std::string> degraded; // informational names
};

// True when this build has libav and swresample.
bool importAvailable();
// Lower-cased and clamped copy of a file name, for the metadata: at most 80 bytes, control characters dropped.
std::string displayName(const std::string &fileName);

// Reads `source`, writes `<outDir>/<id>.wav` and `<outDir>/<id>.json` (`name` is for the metadata only). `outDir` must exist.
ImportResult importAudio(const std::string &source, const std::string &outDir, const std::string &name, const ImportSettings &settings = {},
                         const ImportCallbacks &callbacks = {});

// The demuxers audio may come from, by libavformat's name (a list of aliases matches if one does).
bool audioContainerAllowed(const char *demuxerName);
const char *audioContainerWhitelist();

} // namespace murasaki::audio
