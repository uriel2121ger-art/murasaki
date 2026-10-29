#pragma once

#include "colorkey.h"
#include "framesink.h"
#include "petimport.h"

#include <QString>
#include <QStringList>

#include <functional>

namespace murasaki::import {

// The video source of the import (docs/pets/import.md, "Video"): libavformat and libavcodec turn the first video stream of a file into RGBA frames
// that go to a FrameSink. The file is the user's, so it is read as untrusted: only the "file" protocol, only the containers of kVideoContainers,
// one video stream (everything else is discarded and never decoded), at most two decoder threads, and every limit below is checked against what
// is decoded, never against what the container claims.

// The limits the user can change per import (the spec's video settings); the defaults are the plan's.
struct VideoSettings {
    int maxSide = 1024;          // the longest side of a frame; a larger video is scaled down, a smaller one is never scaled up
    double maxFps = 30;          // frames a second at most: the others are dropped by timestamp, never blended
    long long startMs = 0;       // trim: the first moment kept
    long long durationMs = 0;    // trim: how long from startMs; 0 is to the end
    int maxFrames = kMaxFrames;  // refuses (kTooManyFrames) a video that would give more
    long long deadlineMs = 10LL * 60 * 1000; // the whole import (open, probe, read, decode) may take this long; then kVideoTimeout
};

inline constexpr int kMinVideoSide = 16;
inline constexpr double kMinVideoFps = 1;
inline constexpr double kMaxVideoFps = 60;
inline constexpr long long kMaxVideoStartMs = 24LL * 3600 * 1000;
inline constexpr long long kMaxDecodedFrames = 600000;   // every frame the decoder yields, kept or dropped
inline constexpr int kMaxVideoSide = 16384;
inline constexpr long long kMaxVideoPixels = 8192LL * 4320;           // the stream's own size (8K); also the decoders' max_pixels, so no frame buffer is bigger
inline constexpr long long kMaxDecodedPixels = 300LL * 1000 * 1000 * 1000; // frames decoded times their pixels: about 145000 frames of 1080p
inline constexpr long long kMinDeadlineMs = 1;
inline constexpr long long kMaxDeadlineMs = 3600LL * 1000;

// Empty when valid, else the reason (to follow kSpecRefused's ": ").
QString validateVideoSettings(const VideoSettings &settings);

// The demuxers a video may come from, by libavformat's name (a name may list aliases, "mov,mp4,m4a,3gp,3g2,mj2": one match is enough).
bool videoContainerAllowed(const char *demuxerName);
// The same list as libavformat's format_whitelist option.
const char *videoContainerWhitelist();

// True when the library was built with libav (the whole video source is a no-op error otherwise).
bool videoSourceAvailable();

struct VideoCallbacks {
    std::function<bool()> cancelled;                     // polled between packets
    std::function<void(double fraction, int frames)> progress; // at most once a kept frame; fraction is -1 when the length is not known
};

struct VideoInfo {
    bool ok = false;
    QString error;             // a name of petimport.h, followed by ": " and the detail
    QString container;         // libavformat's name for what was probed
    QString codec;
    int width = 0;             // the stream's size
    int height = 0;
    int outWidth = 0;          // the size of the frames the settings give
    int outHeight = 0;
    double fps = 0;            // the stream's average rate, 0 when it does not say
    long long durationMs = -1; // the container's claim; -1 when it says nothing (never used for a limit)
    int estimatedFrames = 0;   // from the claim and the settings, 0 when unknown; for the preview only
    bool hasAlpha = false;     // the pixel format carries alpha and a decoder that keeps it exists
    bool hasAudio = false;     // the container has an audio stream (never decoded; for the record of keepAudio)
    QStringList degraded;      // informational names (petimport.h)
};

// Opens the file, checks the container and the stream, and reads the stream info (bounded); decodes nothing. Cancellation and the deadline reach the probe too.
VideoInfo probeVideo(const QString &file, const VideoSettings &settings, const VideoCallbacks &callbacks = {});


struct VideoOutcome {
    bool ok = false;
    bool cancelled = false;
    bool overBudget = false;   // the sink refused a frame for the byte budget
    QString error;             // a name of petimport.h, followed by ": " and the detail
    int frames = 0;
    int width = 0;
    int height = 0;
    long long totalDelayMs = 0;
    bool hasAlpha = false;
    QStringList degraded;
};

// Decodes the first video stream into `sink` (begin() once, addFrame() for each kept frame, finish() at the end). Frames are full-canvas RGBA8888 with
// straight alpha; alpha is the video's own when its pixel format has one and 255 otherwise. A frame's delay is the time to the next kept frame.
// The colour key is not applied here: wrap the sink in a KeyedSink (keyedsink.h) so that it applies to every source alike.
VideoOutcome decodeVideo(const QString &file, const VideoSettings &settings, FrameSink &sink, const VideoCallbacks &callbacks = {});

} // namespace murasaki::import
