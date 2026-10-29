#pragma once

#include <QString>

#include <optional>

class QIODevice;

namespace murasaki {

enum class AnimationFormat { Apng, Gif, Webp };

// What the headers of an animation file say. Nothing is decoded.
struct AnimationInfo {
    AnimationFormat format = AnimationFormat::Apng;
    int width = 0;  // canvas size in pixels
    int height = 0;
    int frames = 0; // 1 for a still image
    double fps = 0; // frames over the summed frame delays, rounded to 0.01; 0 for a still image
};

// True for the extensions the readers below handle: apng, gif, webp (case-insensitive, no dot).
bool isAnimationExtension(const QString &suffix);

// True when the file at `path` is an animation by its name or its content: an apng, gif or webp suffix, or a `.png` whose chunks put an `acTL`
// before the first `IDAT`, which is what makes a PNG an APNG whatever it is called (Anima Engine plays such a file, and its authors name them
// either way). A still `.png`, and one with the `acTL` after the first `IDAT` (the specification ignores it there), are not. Reads chunk headers only.
bool isAnimationFile(const QString &path);

// The readers walk the chunk/block structure with seeks and small reads: the cost is bounded by the number of
// chunks (capped at kMaxPetChunks, enough for a 2 GiB file in 4 KiB pieces, for APNG and WebP), never by the amount of pixel data, so a 300 MB file is as cheap as a small one. A malformed,
// truncated or implausible file (dimensions outside 1..32768, inconsistent frame counts, missing end marker) gives
// nullopt. The device must be seekable and open for reading.
// Delays shorter than 10 ms count as 10 ms, so fps never exceeds 100.
std::optional<AnimationInfo> readApngInfo(QIODevice *device);
// Frame delays follow browsers: a delay of 0 or 1 centisecond counts as 10.
std::optional<AnimationInfo> readGifInfo(QIODevice *device);
// Same 10 ms minimum per frame as APNG. Animated (VP8X with ANMF frames) and still (VP8, VP8L, VP8X) files.
std::optional<AnimationInfo> readWebpInfo(QIODevice *device);

// Picks the reader from the file's magic bytes, not from its extension.
std::optional<AnimationInfo> readAnimationInfo(const QString &path);

} // namespace murasaki
