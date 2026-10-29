#pragma once

// Reader for Wallpaper Engine .tex textures.
//
// A .tex file is a chain of tagged sections, all little-endian, tags are 8 characters plus a NUL:
//
//   "TEXV0005\0" "TEXI0001\0"   container and sub-container; the only versions seen in the wild
//   i32 format                  pixel format (see PixelFormat below), i32 flags (TexFlags)
//   u32 textureWidth, textureHeight   storage size (often padded to a power of two)
//   u32 imageWidth, imageHeight       the visible picture inside the storage; shaders sample [0, image/texture]
//   u32 unknown
//   u32 extra                   only when flag bit 6 (TexFlags::ExtraHeaderWord, value 64) is set: one more word before the
//                               container tag (seen in the 28 colour-grading look-up tables of the app's own assets, where it
//                               holds four bytes that look like a colour). The flag marks a volume texture (a 32 x 32 x 32
//                               colour look-up table): its picture is the slices stacked in one 2D image, which is how it is loaded.
//   "TEXB000N\0"                image container, N = 1..4
//   u32 imageCount              (N >= 3: i32 freeImage format, N = 4: u32 isVideo; a video has an MP4 payload and
//                                is reported as such without reading further; any other TEXB0004 is read as 0003)
//   per image: u32 mipCount, then per mip:
//       u32 width, u32 height   (flag bit 6: u32 depth follows, the number of slices of a volume texture)
//       N >= 2: u32 lz4 flag, i32 uncompressed size
//       i32 stored size, then that many bytes (LZ4 block when the flag is 1)
//   optional animation section when flag IsGif is set:
//       "TEXS0001|2|3\0", u32 frameCount, (3: u32 width, u32 height), then per frame the frame number, duration and
//       the position and extent of the frame in the sheet (u32 fields in 0001, floats in 0002 and 0003)
//
// When the free-image format of a TEXB0003/4 container is not -1, a mip holds an ordinary image file (PNG, JPEG, ...)
// instead of raw pixels; it is decoded here, so callers always see pixels.
//
// Only what the renderer needs is kept: the first image with all its mips, plus the animation frames. A sprite sheet (flag
// IsGif) with several images keeps them all, as `pages` (a frame's first field is the image it lies on: the sheets of the
// library that need more than one image put the frames on up to seven), as long as the pixel data stays under the size
// limit; the pages that do not fit are counted in `droppedPages`.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace murasaki::scene {

// Pixel formats as stored in the file.
enum class TexFormat : int32_t {
    Argb8888 = 0, // bytes R, G, B, A in memory
    Rgb888 = 1,
    Rgb565 = 2,
    Dxt5 = 4,
    Dxt3 = 6,
    Dxt1 = 7,
    Rg88 = 8,
    R8 = 9,
    Rg1616f = 10,
    R16f = 11,
    Bc7 = 12,
    Rgba1010102 = 13,
    Rgba16161616f = 14,
    Rgb161616f = 15,
};

// What a mip holds after reading: the formats a GPU can sample directly.
enum class PixelFormat {
    Rgba8,
    R8,
    Rg8,
    R16f,
    Rg16f,
    Rgba16f,
    Bc1,
    Bc2,
    Bc3,
    Bc7,
    Rgb10A2, // 32 bit packed, red in the low ten bits, alpha in the top two (VK_FORMAT_A2B10G10R10_UNORM_PACK32)
};

namespace TexFlags {
inline constexpr uint32_t NoInterpolation = 1;
inline constexpr uint32_t ClampUVs = 2;
inline constexpr uint32_t IsGif = 4;
inline constexpr uint32_t ClampUVsBorder = 8;
inline constexpr uint32_t Video = 32;
inline constexpr uint32_t ExtraHeaderWord = 64;
} // namespace TexFlags

struct TexMip {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> data; // empty when the texture was read without pixel data
};

// One more image of a sprite sheet (images 1 and on); `mips[0]` gives its size, which may differ from the first image's.
struct TexPage {
    std::vector<TexMip> mips;
};

struct TexFrame {
    uint32_t index = 0;    // the image of the container the frame lies on (0 for every single-image sheet)
    float duration = 0.0f; // seconds
    float x = 0.0f;        // position of the frame in the sheet, in texels
    float y = 0.0f;
    float axisXx = 0.0f;   // the frame's x and y axes in the sheet, in texels: width and height for upright frames
    float axisXy = 0.0f;
    float axisYx = 0.0f;
    float axisYy = 0.0f;
};

struct Tex {
    int containerVersion = 0; // TEXB000N (a non-video TEXB0004 is reported as 3)
    int32_t rawFormat = 0;
    uint32_t flags = 0;
    uint32_t textureWidth = 0;
    uint32_t textureHeight = 0;
    uint32_t imageWidth = 0;
    uint32_t imageHeight = 0;
    uint32_t imageCount = 0;
    int32_t freeImageFormat = -1;
    bool isVideo = false;
    uint32_t volumeDepth = 0; // slices of a volume texture (TexFlags::ExtraHeaderWord), 0 otherwise; the mips hold them stacked
    uint32_t headerExtra = 0; // the extra header word (valid with TexFlags::ExtraHeaderWord)
    std::vector<uint8_t> video; // the MP4 stream of a video texture (TexFlags::Video): the payload of its first mip, read with the pixels
    uint64_t videoSize = 0;     // its size in bytes, known without reading it
    uint64_t videoOffset = 0;   // where it starts in the .tex bytes (always set for a video)
    std::string videoFile;      // else-than-empty: the stream is not in `video` but in this file, `videoFileOffset` bytes in (see readTexFile)
    uint64_t videoFileOffset = 0;
    PixelFormat format = PixelFormat::Rgba8;
    std::vector<TexMip> mips; // of the first image, largest first
    std::vector<TexPage> pages; // images 1 and on of a sprite sheet that fit the size limit, in order (page i is image i + 1)
    uint32_t droppedPages = 0;  // images of a sprite sheet that were left out: the size limit, or a bad one when only the header was read
    int animationVersion = 0; // TEXS000N, 0 when absent
    uint32_t sheetWidth = 0;  // TEXS0003 only
    uint32_t sheetHeight = 0;
    std::vector<TexFrame> frames;

    bool nearest() const { return (flags & TexFlags::NoInterpolation) != 0; }
    bool clampUVs() const { return (flags & (TexFlags::ClampUVs | TexFlags::ClampUVsBorder)) != 0; }
    bool animated() const { return (flags & TexFlags::IsGif) != 0; }
    bool hasVideo() const { return isVideo || (flags & TexFlags::Video) != 0; }
};

struct TexReadOptions {
    bool pixels = true; // false: walk the sections and skip pixel data (feature detection)
    // A video's stream is neither copied nor required to be in the bytes: `videoOffset` says where it starts and the bytes may end before
    // it does (a video texture is hundreds of megabytes; its header is a few dozen bytes).
    bool videoInPlace = false;
};

// Parses a whole .tex file. Fails (nullopt + `error`) on a malformed file, an unknown format, corrupt LZ4 data or
// an undecodable embedded image. A video texture carries its MP4 stream in `video` and no mips with pixels.
std::optional<Tex> parseTex(std::span<const uint8_t> bytes, const TexReadOptions &options = {}, std::string *error = nullptr);

// Bytes one mip of `width` x `height` occupies in `format`; 0 on overflow.
uint64_t pixelBytes(PixelFormat format, uint32_t width, uint32_t height);

// Whether the renderer can sample a raw format code (header-only reads let the others through).
bool texFormatSupported(int32_t rawFormat);

// Human name of a raw format code, for logs and --check reasons ("unknown" for codes the format does not define).
const char *texFormatName(int32_t rawFormat);

} // namespace murasaki::scene
