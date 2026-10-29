#pragma once

// Decoding of the image files a scene carries (the embedded pictures of a .tex, plain png and jpg next to it) to RGBA8.
// JPEG goes through libjpeg(-turbo) when the build has it (SIMD, several times faster than stb_image on the 4000 x 6000
// pictures wallpapers like to carry); every other format, and a JPEG libjpeg cannot do (CMYK), through stb_image. The size
// is read first and checked against the caller's limits, so a small header never makes a huge allocation. Thread safe.

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace murasaki::scene {

struct DecodedImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // width x height x 4, top row first
};

// Width and height from the header; false when the bytes are no image this decoder reads.
bool imageSize(std::span<const uint8_t> bytes, uint32_t &width, uint32_t &height);

// Decodes to RGBA8. Fails (with `error`) when the image is larger than `maxDimension` on a side or `maxBytes` decoded.
bool decodeImage(std::span<const uint8_t> bytes, uint32_t maxDimension, uint64_t maxBytes, DecodedImage &out, std::string &error);

// Whether libjpeg handled the last decode of a JPEG on this thread (for tests and the timing notes).
bool jpegDecoderAvailable();

} // namespace murasaki::scene
