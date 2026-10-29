#include "assets/tex.h"

#include <lz4.h>
#include "assets/imagedecode.h"
#include "assets/texcache.h"
#include "util/parallel.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace murasaki::scene {

namespace {

constexpr uint32_t kMaxMips = 32;
constexpr uint32_t kMaxImages = 4096;
constexpr uint32_t kMaxFrames = 100000;
constexpr uint32_t kMaxDimension = 32768;
constexpr uint64_t kMaxMipBytes = 1ull << 29;       // 512 MiB
constexpr uint64_t kMaxDecodedImageBytes = 1ull << 28; // 256 MiB: one embedded image, decoded to RGBA8
constexpr uint64_t kMaxVideoBytes = 1ull << 27;         // 128 MiB: the MP4 a video texture carries
constexpr uint64_t kMaxTextureBytes = 1ull << 29;      // 512 MiB: all mips of one texture together

// Bounds-checked little-endian reader; every read past the end latches `ok` to false.
class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data) : m_data(data) {}

    bool ok() const { return m_ok; }
    size_t remaining() const { return m_data.size() - m_pos; }
    size_t position() const { return m_pos; }

    uint32_t u32()
    {
        uint32_t v = 0;
        read(&v, sizeof(v));
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32()
    {
        float v = 0;
        read(&v, sizeof(v));
        return v;
    }
    // A 9 byte tag ("TEXV0005\0"); returns its 8 visible characters.
    std::string tag()
    {
        char buf[9] = {};
        read(buf, sizeof(buf));
        return std::string(buf, buf[8] == '\0' ? 8 : 9);
    }
    std::span<const uint8_t> bytes(size_t n)
    {
        if (n > remaining()) {
            m_ok = false;
            return {};
        }
        auto s = m_data.subspan(m_pos, n);
        m_pos += n;
        return s;
    }

private:
    void read(void *out, size_t n)
    {
        if (!m_ok || n > remaining()) {
            m_ok = false;
            std::memset(out, 0, n);
            return;
        }
        std::memcpy(out, m_data.data() + m_pos, n);
        m_pos += n;
    }

    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

// A tag with anything that is not plain text shown as '?', for refusal reasons.
std::string printable(const std::string &tag)
{
    std::string out;
    for (char c : tag) {
        out += (c >= 0x20 && c < 0x7f) ? c : '?';
    }
    return out;
}

// How a stored mip is widened before upload (formats Vulkan cannot sample as they are stored).
enum class Expand { None, Rgb888, Rgb565, Rgb16f };

bool pixelFormatFor(int32_t raw, PixelFormat &out, Expand &expand)
{
    expand = Expand::None;
    switch (static_cast<TexFormat>(raw)) {
    case TexFormat::Argb8888: out = PixelFormat::Rgba8; return true;
    case TexFormat::Rgb888: out = PixelFormat::Rgba8; expand = Expand::Rgb888; return true;
    case TexFormat::Rgb565: out = PixelFormat::Rgba8; expand = Expand::Rgb565; return true;
    case TexFormat::Dxt5: out = PixelFormat::Bc3; return true;
    case TexFormat::Dxt3: out = PixelFormat::Bc2; return true;
    case TexFormat::Dxt1: out = PixelFormat::Bc1; return true;
    case TexFormat::Rg88: out = PixelFormat::Rg8; return true;
    case TexFormat::R8: out = PixelFormat::R8; return true;
    case TexFormat::Rg1616f: out = PixelFormat::Rg16f; return true;
    case TexFormat::R16f: out = PixelFormat::R16f; return true;
    case TexFormat::Bc7: out = PixelFormat::Bc7; return true;
    case TexFormat::Rgba1010102: out = PixelFormat::Rgb10A2; return true;
    case TexFormat::Rgba16161616f: out = PixelFormat::Rgba16f; return true;
    case TexFormat::Rgb161616f: out = PixelFormat::Rgba16f; expand = Expand::Rgb16f; return true;
    }
    return false;
}

// Bytes per pixel of a mip as stored in the file, for the formats that are widened.
uint64_t storedPixelBytes(Expand expand)
{
    switch (expand) {
    case Expand::Rgb888: return 3;
    case Expand::Rgb565: return 2;
    case Expand::Rgb16f: return 6;
    case Expand::None: break;
    }
    return 0;
}

std::vector<uint8_t> expandPixels(const std::vector<uint8_t> &src, uint32_t width, uint32_t height, Expand expand)
{
    const size_t pixels = static_cast<size_t>(width) * height;
    if (expand == Expand::Rgb16f) {
        // three half floats become four, alpha 1.0 (0x3C00)
        std::vector<uint8_t> out(pixels * 8);
        for (size_t i = 0; i < pixels; ++i) {
            std::memcpy(&out[i * 8], &src[i * 6], 6);
            out[i * 8 + 6] = 0x00;
            out[i * 8 + 7] = 0x3C;
        }
        return out;
    }
    const bool rgb565 = expand == Expand::Rgb565;
    std::vector<uint8_t> out(pixels * 4, 255);
    for (size_t i = 0; i < pixels; ++i) {
        if (rgb565) {
            const uint16_t v = static_cast<uint16_t>(src[i * 2] | (src[i * 2 + 1] << 8));
            out[i * 4 + 0] = static_cast<uint8_t>(((v >> 11) & 31) * 255 / 31);
            out[i * 4 + 1] = static_cast<uint8_t>(((v >> 5) & 63) * 255 / 63);
            out[i * 4 + 2] = static_cast<uint8_t>((v & 31) * 255 / 31);
        } else {
            out[i * 4 + 0] = src[i * 3 + 0];
            out[i * 4 + 1] = src[i * 3 + 1];
            out[i * 4 + 2] = src[i * 3 + 2];
        }
    }
    return out;
}

} // namespace

uint64_t pixelBytes(PixelFormat format, uint32_t width, uint32_t height)
{
    const uint64_t w = width;
    const uint64_t h = height;
    const uint64_t blocks = ((w + 3) / 4) * ((h + 3) / 4);
    switch (format) {
    case PixelFormat::Rgba8:
    case PixelFormat::Rgb10A2: return w * h * 4;
    case PixelFormat::R8: return w * h;
    case PixelFormat::Rg8: return w * h * 2;
    case PixelFormat::R16f: return w * h * 2;
    case PixelFormat::Rg16f: return w * h * 4;
    case PixelFormat::Rgba16f: return w * h * 8;
    case PixelFormat::Bc1: return blocks * 8;
    case PixelFormat::Bc2:
    case PixelFormat::Bc3:
    case PixelFormat::Bc7: return blocks * 16;
    }
    return 0;
}

bool texFormatSupported(int32_t rawFormat)
{
    PixelFormat format;
    Expand expand;
    return pixelFormatFor(rawFormat, format, expand);
}

const char *texFormatName(int32_t rawFormat)
{
    switch (static_cast<TexFormat>(rawFormat)) {
    case TexFormat::Argb8888: return "RGBA8888";
    case TexFormat::Rgb888: return "RGB888";
    case TexFormat::Rgb565: return "RGB565";
    case TexFormat::Dxt5: return "DXT5";
    case TexFormat::Dxt3: return "DXT3";
    case TexFormat::Dxt1: return "DXT1";
    case TexFormat::Rg88: return "RG88";
    case TexFormat::R8: return "R8";
    case TexFormat::Rg1616f: return "RG1616f";
    case TexFormat::R16f: return "R16f";
    case TexFormat::Bc7: return "BC7";
    case TexFormat::Rgba1010102: return "RGBA1010102";
    case TexFormat::Rgba16161616f: return "RGBA16161616f";
    case TexFormat::Rgb161616f: return "RGB161616f";
    }
    return "unknown";
}

std::optional<Tex> parseTex(std::span<const uint8_t> bytes, const TexReadOptions &options, std::string *error)
{
    const auto fail = [&](const std::string &what) -> std::optional<Tex> {
        if (error) {
            *error = what;
        }
        return std::nullopt;
    };
    Reader r(bytes);
    Tex tex;

    const std::string outer = r.tag();
    const std::string inner = r.tag();
    if (!r.ok() || outer != "TEXV0005" || inner != "TEXI0001") {
        return fail(std::format("unsupported texture container {}/{} (only TEXV0005/TEXI0001 exist in the library)", printable(outer), printable(inner)));
    }
    tex.rawFormat = r.i32();
    tex.flags = r.u32();
    tex.textureWidth = r.u32();
    tex.textureHeight = r.u32();
    tex.imageWidth = r.u32();
    tex.imageHeight = r.u32();
    r.u32(); // unknown
    if ((tex.flags & TexFlags::ExtraHeaderWord) != 0) {
        tex.headerExtra = r.u32();
    }
    if (!r.ok()) {
        return fail("truncated texture header");
    }
    if (tex.textureWidth == 0 || tex.textureHeight == 0 || tex.textureWidth > kMaxDimension || tex.textureHeight > kMaxDimension) {
        return fail("bad texture size");
    }

    const std::string container = r.tag();
    if (container.size() != 8 || container.rfind("TEXB000", 0) != 0 || container[7] < '1' || container[7] > '4') {
        return fail(std::format("unsupported image container '{}' (TEXB0001 to TEXB0004 are read)", printable(container)));
    }
    tex.containerVersion = container[7] - '0';
    tex.imageCount = r.u32();
    if (tex.containerVersion >= 3) {
        tex.freeImageFormat = r.i32();
    }
    if (tex.containerVersion == 4) {
        tex.isVideo = r.u32() == 1;
    }
    if (!r.ok() || tex.imageCount == 0 || tex.imageCount > kMaxImages) {
        return fail("bad image count");
    }
    if (tex.isVideo) {
        // The payload is an MP4 stream, which no renderer here plays: callers learn it is a video and nothing else.
        return tex;
    }
    // TEXB0004 only adds per-mip editor data to videos; a TEXB0004 picture is laid out exactly like TEXB0003.
    if (tex.containerVersion == 4) {
        tex.containerVersion = 3;
    }

    uint64_t totalBytes = 0; // pixel data held so far, bounded by kMaxTextureBytes
    const bool embedded = tex.containerVersion >= 3 && tex.freeImageFormat != -1;
    Expand expand = Expand::None;
    const bool volume = (tex.flags & TexFlags::ExtraHeaderWord) != 0;
    const bool video = (tex.flags & TexFlags::Video) != 0;
    if (!embedded && !tex.isVideo && !video && !pixelFormatFor(tex.rawFormat, tex.format, expand)) {
        // Unsupported formats still parse their section layout when the caller only wants the header.
        if (options.pixels) {
            return fail(std::format("unsupported texture format {} ({})", tex.rawFormat, texFormatName(tex.rawFormat)));
        }
    }

    struct DecodeJob {
        size_t page; // 0: the first image (tex.mips), else tex.pages[page - 1]
        size_t mip;
        std::vector<uint8_t> bytes;
    };
    std::vector<DecodeJob> decodeJobs;
    // A sprite sheet keeps every image (its frames name the one they lie on); the images that would take the texture over its limit,
    // and all those after them, are left out and counted.
    const bool wantPages = tex.animated() && !video && !volume;
    bool stopPages = false;
    for (uint32_t image = 0; image < tex.imageCount; ++image) {
        const uint32_t mipCount = r.u32();
        if (!r.ok() || mipCount > kMaxMips) {
            return fail("bad mip count");
        }
        TexPage page;
        const bool pageImage = image != 0 && wantPages;
        const uint64_t pageStartBytes = totalBytes;
        bool dropPage = pageImage && stopPages;
        // True when `totalBytes` is over the limit and the image being read is a page that can be left out instead of failing.
        const auto dropIfPage = [&]() {
            if (!pageImage) {
                return false;
            }
            dropPage = true;
            stopPages = true;
            totalBytes = pageStartBytes;
            decodeJobs.erase(std::remove_if(decodeJobs.begin(), decodeJobs.end(), [&](const DecodeJob &j) { return j.page == image; }), decodeJobs.end());
            page.mips.clear();
            return true;
        };
        std::vector<TexMip> &dest = pageImage ? page.mips : tex.mips;
        for (uint32_t m = 0; m < mipCount; ++m) {
            TexMip mip;
            mip.width = r.u32();
            mip.height = r.u32();
            if (volume) {
                const uint32_t depth = r.u32(); // slices of a volume; the picture holds them one below the other
                if (image == 0 && m == 0) {
                    tex.volumeDepth = depth;
                }
            }
            uint32_t lz4 = 0;
            int32_t storedUncompressed = 0;
            if (tex.containerVersion >= 2) {
                lz4 = r.u32();
                storedUncompressed = r.i32();
            }
            const int32_t storedSize = r.i32();
            const bool streamInPlace = options.videoInPlace && video && image == 0 && m == 0;
            if (streamInPlace && r.ok() && storedSize > 0) {
                // the stream stays where it is: its place and size are all a caller needs, and nothing after it matters to a video
                tex.videoSize = static_cast<uint64_t>(storedSize);
                tex.videoOffset = r.position();
                tex.mips.push_back({mip.width, mip.height, {}});
                return tex;
            }
            if (!r.ok() || storedSize < 0 || static_cast<size_t>(storedSize) > r.remaining()) {
                return fail("truncated mip data");
            }
            if (tex.containerVersion < 2 || lz4 == 0) {
                storedUncompressed = storedSize;
            }
            const auto stored = r.bytes(static_cast<size_t>(storedSize));
            if (!r.ok()) {
                return fail("truncated mip data");
            }
            if (video && image == 0 && m == 0) {
                // the stream of a video texture: its size is known to a header read, the bytes only to a full one
                tex.videoSize = static_cast<uint64_t>(storedSize);
                tex.videoOffset = r.position() - static_cast<size_t>(storedSize);
                if (options.pixels) {
                    if (tex.videoSize > kMaxVideoBytes) {
                        return fail("video stream over the size limit");
                    }
                    tex.video.assign(stored.begin(), stored.end());
                }
                tex.mips.push_back({mip.width, mip.height, {}});
                continue;
            }
            if (video) {
                continue; // a video has no other pictures
            }
            if (dropPage) {
                continue;
            }
            if ((image != 0 && !pageImage) || !options.pixels) {
                // Only the first image is used (a picture other than a sprite sheet), and feature detection never needs pixels.
                // Dimensions still count. A sprite sheet's images are counted against the limit all the same, so that a header read
                // knows which pages a full read would leave out.
                if (wantPages && !embedded && storedUncompressed > 0) {
                    totalBytes += static_cast<uint64_t>(storedUncompressed);
                    if (totalBytes > kMaxTextureBytes && dropIfPage()) {
                        continue;
                    }
                }
                if ((image == 0 || pageImage) && m < kMaxMips) {
                    dest.push_back({mip.width, mip.height, {}});
                }
                continue;
            }
            if (mip.width == 0 || mip.height == 0 || mip.width > kMaxDimension || mip.height > kMaxDimension) {
                return fail("bad mip size");
            }
            if (storedUncompressed < 0 || static_cast<uint64_t>(storedUncompressed) > kMaxMipBytes) {
                return fail("bad mip size in bytes");
            }

            // LZ4 cannot expand a block more than about 255 times: anything claiming more is corrupt, and would
            // only make us allocate a large buffer for nothing.
            if (lz4 == 1 && static_cast<uint64_t>(storedUncompressed) > static_cast<uint64_t>(storedSize) * 255 + 16) {
                return fail("implausible LZ4 expansion in a mip");
            }
            totalBytes += static_cast<uint64_t>(storedUncompressed);
            if (totalBytes > kMaxTextureBytes) {
                if (dropIfPage()) {
                    continue;
                }
                return fail("texture data exceeds the size limit");
            }

            std::vector<uint8_t> raw;
            if (lz4 == 1) {
                raw.resize(static_cast<size_t>(storedUncompressed));
                const int got = LZ4_decompress_safe(reinterpret_cast<const char *>(stored.data()), reinterpret_cast<char *>(raw.data()),
                                                    storedSize, storedUncompressed);
                if (got < 0 || got != storedUncompressed) {
                    return fail("corrupt LZ4 data in a mip");
                }
            } else {
                raw.assign(stored.begin(), stored.end());
            }

            if (embedded) {
                // The size is read first: a few bytes of header must not make us allocate gigabytes.
                uint32_t w = 0, h = 0;
                if (!imageSize(raw, w, h) || w > kMaxDimension || h > kMaxDimension || static_cast<uint64_t>(w) * h * 4 > kMaxDecodedImageBytes) {
                    return fail(std::format("embedded image (free image format {}) is unreadable or too large", tex.freeImageFormat));
                }
                totalBytes += static_cast<uint64_t>(w) * h * 4;
                if (totalBytes > kMaxTextureBytes) {
                    if (dropIfPage()) {
                        continue;
                    }
                    return fail("texture data exceeds the size limit");
                }
                // decoded after the walk, together with the other mips (they are independent pictures)
                decodeJobs.push_back({image, dest.size(), std::move(raw)});
                mip.width = w;
                mip.height = h;
                tex.format = PixelFormat::Rgba8;
            } else if (expand != Expand::None) {
                const uint64_t need = static_cast<uint64_t>(mip.width) * mip.height * storedPixelBytes(expand);
                if (raw.size() < need) {
                    return fail("mip is smaller than its size implies");
                }
                mip.data = expandPixels(raw, mip.width, mip.height, expand);
            } else {
                if (raw.size() < pixelBytes(tex.format, mip.width, mip.height)) {
                    return fail("mip is smaller than its size implies");
                }
                mip.data = std::move(raw);
                mip.data.resize(static_cast<size_t>(pixelBytes(tex.format, mip.width, mip.height)));
            }
            dest.push_back(std::move(mip));
        }
        if (pageImage) {
            if (dropPage) {
                ++tex.droppedPages;
            } else {
                tex.pages.push_back(std::move(page));
            }
        }
    }

    if (!decodeJobs.empty()) {
        std::vector<std::string> failures(decodeJobs.size());
        parallelFor(decodeJobs.size(), [&](size_t i) {
            DecodedImage image;
            // a big picture decoded before comes back from the cache; otherwise it is decoded and remembered
            bool ready = loadCachedImage(decodeJobs[i].bytes, kMaxDimension, kMaxDecodedImageBytes, image);
            if (!ready && decodeImage(decodeJobs[i].bytes, kMaxDimension, kMaxDecodedImageBytes, image, failures[i])) {
                storeCachedImage(decodeJobs[i].bytes, image);
                ready = true;
            }
            if (ready) {
                TexMip &m = decodeJobs[i].page == 0 ? tex.mips[decodeJobs[i].mip] : tex.pages[decodeJobs[i].page - 1].mips[decodeJobs[i].mip];
                m.width = image.width;
                m.height = image.height;
                m.data = std::move(image.rgba);
            }
            decodeJobs[i].bytes = {}; // the compressed picture is not needed again
        });
        for (const std::string &why : failures) {
            if (!why.empty()) {
                return fail(std::format("cannot decode the embedded image (free image format {}): {}", tex.freeImageFormat, why));
            }
        }
    }

    if (embedded && (tex.flags & TexFlags::ExtraHeaderWord) != 0 && !tex.mips.empty() && !tex.mips[0].data.empty()) {
        // These files describe their picture with sizes that do not match the embedded image (a 32 x 1024 strip announced as
        // 1024 x 32): the decoded picture is the truth, and fills its storage.
        tex.textureWidth = tex.imageWidth = tex.mips[0].width;
        tex.textureHeight = tex.imageHeight = tex.mips[0].height;
    }

    if (tex.animated() && r.remaining() >= 9) {
        const std::string sheet = r.tag();
        if (sheet.size() == 8 && sheet.rfind("TEXS000", 0) == 0 && sheet[7] >= '1' && sheet[7] <= '3') {
            tex.animationVersion = sheet[7] - '0';
            const uint32_t count = r.u32();
            if (tex.animationVersion == 3) {
                tex.sheetWidth = r.u32();
                tex.sheetHeight = r.u32();
            }
            if (!r.ok() || count > kMaxFrames) {
                return fail("bad animation section");
            }
            for (uint32_t i = 0; i < count; ++i) {
                TexFrame f;
                f.index = r.u32();
                f.duration = r.f32();
                if (tex.animationVersion == 1) {
                    f.x = static_cast<float>(r.u32());
                    f.y = static_cast<float>(r.u32());
                    f.axisXx = static_cast<float>(r.u32());
                    r.u32();
                    r.u32();
                    f.axisYy = static_cast<float>(r.u32());
                } else {
                    f.x = r.f32();
                    f.y = r.f32();
                    f.axisXx = r.f32();
                    f.axisXy = r.f32();
                    f.axisYx = r.f32();
                    f.axisYy = r.f32();
                }
                if (!r.ok()) {
                    return fail("truncated animation section");
                }
                tex.frames.push_back(f);
            }
        } else {
            return fail("unknown animation section '" + sheet + "'");
        }
    }
    return tex;
}

} // namespace murasaki::scene
