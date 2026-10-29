#include "assets/imagedecode.h"

#include "stb_image.h"

#include <csetjmp>
#include <cstdio>
#include <format>

#ifdef MURASAKI_SCENE_VK_LIBJPEG
#include <jpeglib.h>
#endif
#ifdef MURASAKI_SCENE_VK_LIBDEFLATE
#include <libdeflate.h>
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace murasaki::scene {

namespace {

bool looksLikeJpeg(std::span<const uint8_t> b)
{
    return b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
}

#ifdef MURASAKI_SCENE_VK_LIBJPEG
struct JpegError {
    jpeg_error_mgr mgr;
    std::jmp_buf jump;
};

void jpegFail(j_common_ptr cinfo)
{
    std::longjmp(reinterpret_cast<JpegError *>(cinfo->err)->jump, 1);
}

void jpegQuiet(j_common_ptr) {}

// The header: false when libjpeg cannot read it (or the file is CMYK, which it cannot give as RGBA).
bool jpegHeader(std::span<const uint8_t> bytes, uint32_t &w, uint32_t &h)
{
    jpeg_decompress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpegFail;
    err.mgr.output_message = jpegQuiet;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes.data(), bytes.size());
    const bool ok = jpeg_read_header(&cinfo, TRUE) == JPEG_HEADER_OK && cinfo.jpeg_color_space != JCS_CMYK && cinfo.jpeg_color_space != JCS_YCCK;
    w = cinfo.image_width;
    h = cinfo.image_height;
    jpeg_destroy_decompress(&cinfo);
    return ok;
}

// The pixels into a buffer the caller sized from the header (so nothing allocated here is lost to a longjmp).
bool jpegPixels(std::span<const uint8_t> bytes, uint8_t *dst, uint32_t w, uint32_t h)
{
    jpeg_decompress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpegFail;
    err.mgr.output_message = jpegQuiet;
    if (setjmp(err.jump)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, bytes.data(), bytes.size());
    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK || cinfo.image_width != w || cinfo.image_height != h) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    cinfo.out_color_space = JCS_EXT_RGBA;
    jpeg_start_decompress(&cinfo);
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW row = dst + static_cast<size_t>(cinfo.output_scanline) * w * 4;
        if (jpeg_read_scanlines(&cinfo, &row, 1) != 1) {
            break;
        }
    }
    const bool complete = cinfo.output_scanline == cinfo.output_height;
    if (complete) {
        jpeg_finish_decompress(&cinfo);
    }
    jpeg_destroy_decompress(&cinfo);
    return complete;
}

// 1: decoded, 0: libjpeg cannot (the caller tries stb), -1: refused because of the limits
int decodeJpeg(std::span<const uint8_t> bytes, uint32_t maxDimension, uint64_t maxBytes, DecodedImage &out, std::string &error)
{
    uint32_t w = 0, h = 0;
    if (!jpegHeader(bytes, w, h)) {
        return 0;
    }
    if (w == 0 || h == 0 || w > maxDimension || h > maxDimension || static_cast<uint64_t>(w) * h * 4 > maxBytes) {
        error = std::format("image of {} x {} is too large", w, h);
        return -1;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    if (!jpegPixels(bytes, pixels.data(), w, h)) {
        return 0; // truncated or damaged: stb decides what to make of it
    }
    out.width = w;
    out.height = h;
    out.rgba = std::move(pixels);
    return 1;
}
#endif


#ifdef MURASAKI_SCENE_VK_LIBDEFLATE
uint32_t be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

// Undoes the PNG row filters in place, for a pixel of BPP bytes. `row` and `prev` (null for the first row) hold `bytes` bytes.
// The previous pixel's bytes are kept in locals: the loops are dependent byte to byte and memory round trips would dominate.
template <size_t BPP>
bool unfilterRowN(uint8_t filter, uint8_t *__restrict row, const uint8_t *__restrict prev, size_t bytes)
{
    switch (filter) {
    case 0:
        return true;
    case 1: {
        int left[BPP] = {};
        for (size_t i = 0; i + BPP <= bytes; i += BPP) {
            for (size_t c = 0; c < BPP; ++c) {
                left[c] = row[i + c] = static_cast<uint8_t>(row[i + c] + left[c]);
            }
        }
        return true;
    }
    case 2:
        if (prev) {
            for (size_t i = 0; i < bytes; ++i) {
                row[i] = static_cast<uint8_t>(row[i] + prev[i]);
            }
        }
        return true;
    case 3: {
        int left[BPP] = {};
        for (size_t i = 0; i + BPP <= bytes; i += BPP) {
            for (size_t c = 0; c < BPP; ++c) {
                const int up = prev ? prev[i + c] : 0;
                left[c] = row[i + c] = static_cast<uint8_t>(row[i + c] + ((left[c] + up) >> 1));
            }
        }
        return true;
    }
    case 4: {
        int left[BPP] = {}, upLeft[BPP] = {};
        for (size_t i = 0; i + BPP <= bytes; i += BPP) {
            for (size_t c = 0; c < BPP; ++c) {
                const int up = prev ? prev[i + c] : 0;
                const int a = left[c], b = up, cc = upLeft[c];
                // the nearest of a, b, c to a + b - c, ties in that order, written as selects (no branch to mispredict)
                const int pa = std::abs(b - cc), pb = std::abs(a - cc), pc = std::abs(a + b - 2 * cc);
                int pred = a, best = pa;
                const bool takeB = pb < best;
                best = takeB ? pb : best;
                pred = takeB ? b : pred;
                const bool takeC = pc < best;
                pred = takeC ? cc : pred;
                left[c] = row[i + c] = static_cast<uint8_t>(row[i + c] + pred);
                upLeft[c] = up;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool unfilterRow(uint8_t filter, uint8_t *row, const uint8_t *prev, size_t bytes, size_t bpp)
{
    switch (bpp) {
    case 1: return unfilterRowN<1>(filter, row, prev, bytes);
    case 2: return unfilterRowN<2>(filter, row, prev, bytes);
    case 3: return unfilterRowN<3>(filter, row, prev, bytes);
    default: return unfilterRowN<4>(filter, row, prev, bytes);
    }
}

// A PNG of 8 bits a channel, not interlaced, through libdeflate (several times faster than stb_image's inflate on the big
// pictures a .tex embeds). 1: decoded, 0: something this path does not do (16 bits, interlacing, colour keys, damage: the
// caller tries stb), -1: refused because of the limits.
int decodePng(std::span<const uint8_t> bytes, uint32_t maxDimension, uint64_t maxBytes, DecodedImage &out, std::string &error)
{
    static const uint8_t kSignature[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (bytes.size() < 8 + 25 || std::memcmp(bytes.data(), kSignature, 8) != 0) {
        return 0;
    }
    uint32_t w = 0, h = 0;
    int depth = 0, colorType = -1, interlace = 0;
    uint8_t palette[256][4];
    std::memset(palette, 255, sizeof(palette));
    size_t paletteSize = 0;
    bool haveHeader = false, keyed = false;
    std::vector<uint8_t> compressed;
    size_t at = 8;
    while (at + 12 <= bytes.size()) {
        const uint32_t length = be32(bytes.data() + at);
        const char *type = reinterpret_cast<const char *>(bytes.data() + at + 4);
        if (length > bytes.size() - at - 12) {
            return 0;
        }
        const uint8_t *data = bytes.data() + at + 8;
        if (!std::memcmp(type, "IHDR", 4) && length == 13) {
            w = be32(data);
            h = be32(data + 4);
            depth = data[8];
            colorType = data[9];
            interlace = data[12];
            haveHeader = true;
            if (w == 0 || h == 0 || w > maxDimension || h > maxDimension || static_cast<uint64_t>(w) * h * 4 > maxBytes) {
                error = std::format("image of {} x {} is too large", w, h);
                return -1;
            }
        } else if (!std::memcmp(type, "PLTE", 4)) {
            paletteSize = std::min<size_t>(length / 3, 256);
            for (size_t i = 0; i < paletteSize; ++i) {
                palette[i][0] = data[i * 3];
                palette[i][1] = data[i * 3 + 1];
                palette[i][2] = data[i * 3 + 2];
            }
        } else if (!std::memcmp(type, "tRNS", 4)) {
            if (colorType == 3) {
                for (size_t i = 0; i < std::min<size_t>(length, 256); ++i) {
                    palette[i][3] = data[i];
                }
            } else {
                keyed = true;
            }
        } else if (!std::memcmp(type, "IDAT", 4)) {
            compressed.insert(compressed.end(), data, data + length);
        } else if (!std::memcmp(type, "IEND", 4)) {
            break;
        }
        at += 12 + length;
    }
    if (!haveHeader || depth != 8 || interlace != 0 || keyed || compressed.empty() || (colorType != 0 && colorType != 2 && colorType != 3 && colorType != 4 && colorType != 6)
        || (colorType == 3 && paletteSize == 0)) {
        return 0;
    }
    const size_t channels = colorType == 0 || colorType == 3 ? 1 : colorType == 4 ? 2 : colorType == 2 ? 3 : 4;
    const size_t rowBytes = static_cast<size_t>(w) * channels;
    const size_t rawSize = (rowBytes + 1) * h;
    std::vector<uint8_t> raw(rawSize);
    libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    if (!d) {
        return 0;
    }
    size_t produced = 0;
    const libdeflate_result r = libdeflate_zlib_decompress(d, compressed.data(), compressed.size(), raw.data(), raw.size(), &produced);
    libdeflate_free_decompressor(d);
    if (r != LIBDEFLATE_SUCCESS || produced != rawSize) {
        return 0;
    }
    std::vector<uint8_t> pixels(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t *line = raw.data() + static_cast<size_t>(y) * (rowBytes + 1);
        const uint8_t *prev = y ? line - (rowBytes + 1) + 1 : nullptr;
        if (!unfilterRow(line[0], line + 1, prev, rowBytes, channels)) {
            return 0;
        }
        const uint8_t *src = line + 1;
        uint8_t *dst = pixels.data() + static_cast<size_t>(y) * w * 4;
        switch (colorType) {
        case 6:
            std::memcpy(dst, src, rowBytes);
            break;
        case 2:
            for (uint32_t x = 0; x < w; ++x, src += 3, dst += 4) {
                dst[0] = src[0]; dst[1] = src[1]; dst[2] = src[2]; dst[3] = 255;
            }
            break;
        case 0:
            for (uint32_t x = 0; x < w; ++x, ++src, dst += 4) {
                dst[0] = dst[1] = dst[2] = *src; dst[3] = 255;
            }
            break;
        case 4:
            for (uint32_t x = 0; x < w; ++x, src += 2, dst += 4) {
                dst[0] = dst[1] = dst[2] = src[0]; dst[3] = src[1];
            }
            break;
        default: // 3
            for (uint32_t x = 0; x < w; ++x, ++src, dst += 4) {
                const uint8_t *c = palette[*src];
                dst[0] = c[0]; dst[1] = c[1]; dst[2] = c[2]; dst[3] = c[3];
            }
            break;
        }
    }
    out.width = w;
    out.height = h;
    out.rgba = std::move(pixels);
    return 1;
}
#endif

} // namespace

bool jpegDecoderAvailable()
{
#ifdef MURASAKI_SCENE_VK_LIBJPEG
    return true;
#else
    return false;
#endif
}

bool imageSize(std::span<const uint8_t> bytes, uint32_t &width, uint32_t &height)
{
    int w = 0, h = 0, channels = 0;
    // stb memcpy's from a null buffer when given nothing, and takes the length as an int
    if (bytes.empty() || bytes.size() > static_cast<size_t>(INT32_MAX)) {
        return false;
    }
    if (!stbi_info_from_memory(bytes.data(), static_cast<int>(bytes.size()), &w, &h, &channels) || w <= 0 || h <= 0) {
        return false;
    }
    width = static_cast<uint32_t>(w);
    height = static_cast<uint32_t>(h);
    return true;
}

bool decodeImage(std::span<const uint8_t> bytes, uint32_t maxDimension, uint64_t maxBytes, DecodedImage &out, std::string &error)
{
    if (bytes.empty()) {
        error = "empty image file";
        return false;
    }
    if (bytes.size() > static_cast<size_t>(INT32_MAX)) {
        error = "image file too large";
        return false;
    }
#ifdef MURASAKI_SCENE_VK_LIBJPEG
    if (looksLikeJpeg(bytes)) {
        const int r = decodeJpeg(bytes, maxDimension, maxBytes, out, error);
        if (r == 1) {
            return true;
        }
        if (r < 0) {
            return false;
        }
    }
#else
    (void)looksLikeJpeg;
#endif
#ifdef MURASAKI_SCENE_VK_LIBDEFLATE
    {
        const int r = decodePng(bytes, maxDimension, maxBytes, out, error);
        if (r == 1) {
            return true;
        }
        if (r < 0) {
            return false;
        }
    }
#endif
    uint32_t w = 0, h = 0;
    if (!imageSize(bytes, w, h) || w > maxDimension || h > maxDimension || static_cast<uint64_t>(w) * h * 4 > maxBytes) {
        error = "the image is unreadable or too large";
        return false;
    }
    int iw = 0, ih = 0, channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &iw, &ih, &channels, 4);
    if (!pixels) {
        error = stbi_failure_reason() ? stbi_failure_reason() : "unknown";
        return false;
    }
    out.width = static_cast<uint32_t>(iw);
    out.height = static_cast<uint32_t>(ih);
    out.rgba.assign(pixels, pixels + static_cast<size_t>(iw) * static_cast<size_t>(ih) * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace murasaki::scene
