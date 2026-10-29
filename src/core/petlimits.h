#pragma once

namespace murasaki {

// The timing a new pet starts with, and what a pet stored before the field existed gets: every frame shows for 1/24 s ("it looks
// right, neither slow nor sped up"; the user's decision). Anima Engine's own rate is 15 (kAnimaPetFps in pettiming.h).
inline constexpr int kDefaultPetFps = 24;

// How long a frame shows when its file says it lasts `rawMs` milliseconds: the rule of every browser for GIF, used
// here for GIF, APNG and WebP alike so that one file plays the same whatever it is saved as. A delay of 10 ms or
// less (0 and 1 centisecond are what encoders write for "as fast as you can") counts as 100 ms; anything longer is
// kept. Assumption, not verified against the other formats' browser code: browsers apply the threshold to GIF;
// extending it to APNG and WebP is our choice. A delay that is not a number counts as tiny.
// A delay that comes out of a timestamp (FFmpeg's, in seconds times 1000) is 10.000000000000002 where the file says 10: a
// microsecond of margin keeps that rounding from turning a tiny delay into a legitimate one.
inline constexpr double kTinyFrameDelayMs = 10.0;
inline constexpr double kTinyFrameDelayMarginMs = 0.001;
inline constexpr double kTinyFrameReplacementMs = 100.0;
inline constexpr double petFrameDelayMs(double rawMs)
{
    return rawMs > kTinyFrameDelayMs + kTinyFrameDelayMarginMs ? rawMs : kTinyFrameReplacementMs;
}

// The largest animation file a pet may be: the header readers are cheap, but a decoder opens the file later.
inline constexpr long long kMaxPetFileBytes = 2LL * 1024 * 1024 * 1024;
// The largest canvas side the video path (libmpv, which uploads a frame as one GPU texture) is expected to play: the
// texture size every GPU of the last decade offers. Larger is refused; the header readers accept up to 32768.
inline constexpr int kMaxPetVideoSide = 16384;
// Chunks the header readers walk per APNG or WebP: a 2 GiB file written in 4 KiB pieces, with some room.
inline constexpr long long kMaxPetChunks = kMaxPetFileBytes / 4096 + 1024;
// Animation files of one item the check examines (an Anima item has one to three).
inline constexpr int kMaxCheckedFiles = 64;

// What the overlay's frame cache decodes at most: the item check and the SpriteDecoder read the same numbers. A pet
// beyond them still plays (the video path takes it), without the frame cache.
inline constexpr int kMaxPetFrames = 20000;
inline constexpr int kMaxPetCanvasSide = 8192; // the largest canvas, and the largest size frames are made at

// The most the sweep's probe decodes of one file (SpriteDecoder::probe), in bytes of frames.
inline constexpr long long kMaxPetProbeBytes = 768LL << 20;

// The memory the pets may use for decoded frames, in MiB: 128 by default, MURASAKI_FRAME_CACHE_MIB up to the maximum.
inline constexpr int kDefaultFrameCacheMiB = 128;
inline constexpr int kMaxFrameCacheMiB = 16384;

} // namespace murasaki
