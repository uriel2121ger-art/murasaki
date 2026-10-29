#pragma once

// Builders for the binary formats murasaki-scene-vk reads (PKG archives, TEX textures, a tiny PNG). Fixtures are
// always made from scratch here: never copy real Workshop or Wallpaper Engine files into tests.

#include "assets/tex.h"
#include "testutil.h"

#include <lz4.h>

#include <QByteArray>
#include <QString>
#include <QTemporaryDir>

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <vector>

namespace scenevkfix {

using Bytes = std::vector<uint8_t>;

inline void put32(Bytes &b, uint32_t v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
inline void putF(Bytes &b, float f)
{
    uint32_t v;
    std::memcpy(&v, &f, 4);
    put32(b, v);
}
inline void putTag(Bytes &b, const char *tag) // 8 characters + NUL
{
    for (int i = 0; i < 8; ++i)
        b.push_back(static_cast<uint8_t>(tag[i]));
    b.push_back(0);
}
inline void putBytes(Bytes &b, const Bytes &more) { b.insert(b.end(), more.begin(), more.end()); }

inline QByteArray toQ(const Bytes &b) { return QByteArray(reinterpret_cast<const char *>(b.data()), static_cast<qsizetype>(b.size())); }

// ---- PKG ----

struct PkgFile {
    std::string name;
    std::string content;
};

inline Bytes makePkg(const std::vector<PkgFile> &files, const char *version = "PKGV0001")
{
    Bytes b;
    put32(b, static_cast<uint32_t>(std::strlen(version)));
    for (const char *p = version; *p; ++p)
        b.push_back(static_cast<uint8_t>(*p));
    put32(b, static_cast<uint32_t>(files.size()));
    uint32_t offset = 0;
    for (const auto &f : files) {
        put32(b, static_cast<uint32_t>(f.name.size()));
        for (char c : f.name)
            b.push_back(static_cast<uint8_t>(c));
        put32(b, offset);
        put32(b, static_cast<uint32_t>(f.content.size()));
        offset += static_cast<uint32_t>(f.content.size());
    }
    for (const auto &f : files)
        for (char c : f.content)
            b.push_back(static_cast<uint8_t>(c));
    return b;
}

// ---- TEX ----

struct MipSpec {
    uint32_t width = 0;
    uint32_t height = 0;
    Bytes pixels;
    bool lz4 = false;
};

struct TexSpec {
    int version = 3; // TEXB000N
    int32_t format = 0;
    uint32_t flags = 0;
    uint32_t texW = 0, texH = 0, imgW = 0, imgH = 0;
    int32_t freeImage = -1;
    uint32_t isVideo = 0;
    uint32_t volumeDepth = 0; // flags 64: the depth word after each mip's height
    uint32_t extraWord = 0; // written after the header when flags has TexFlags::ExtraHeaderWord
    std::vector<std::vector<MipSpec>> images;
    int sheetVersion = 0; // 0: no animation section
    std::vector<std::array<float, 6>> frames; // x, y, w1, w2, h2, h1 (floats for v2/v3, truncated for v1)
    std::vector<uint32_t> frameImages;        // the image each frame lies on; empty: the frame's own number (the old behaviour)
};

inline Bytes makeTex(const TexSpec &s)
{
    Bytes b;
    putTag(b, "TEXV0005");
    putTag(b, "TEXI0001");
    put32(b, static_cast<uint32_t>(s.format));
    put32(b, s.flags);
    put32(b, s.texW);
    put32(b, s.texH);
    put32(b, s.imgW);
    put32(b, s.imgH);
    put32(b, 0);
    if ((s.flags & 64u) != 0)
        put32(b, s.extraWord);
    const std::string container = "TEXB000" + std::to_string(s.version);
    putTag(b, container.c_str());
    put32(b, static_cast<uint32_t>(s.images.size()));
    if (s.version >= 3)
        put32(b, static_cast<uint32_t>(s.freeImage));
    if (s.version == 4)
        put32(b, s.isVideo);
    for (const auto &mips : s.images) {
        put32(b, static_cast<uint32_t>(mips.size()));
        for (const auto &m : mips) {
            put32(b, m.width);
            put32(b, m.height);
            if ((s.flags & 64u) != 0)
                put32(b, s.volumeDepth);
            Bytes stored = m.pixels;
            if (s.version >= 2) {
                if (m.lz4) {
                    stored.resize(static_cast<size_t>(LZ4_compressBound(static_cast<int>(m.pixels.size()))));
                    const int n = LZ4_compress_default(reinterpret_cast<const char *>(m.pixels.data()), reinterpret_cast<char *>(stored.data()),
                                                       static_cast<int>(m.pixels.size()), static_cast<int>(stored.size()));
                    stored.resize(static_cast<size_t>(n));
                }
                put32(b, m.lz4 ? 1 : 0);
                put32(b, static_cast<uint32_t>(m.pixels.size()));
            }
            put32(b, static_cast<uint32_t>(stored.size()));
            putBytes(b, stored);
        }
    }
    if (s.sheetVersion) {
        const std::string tag = "TEXS000" + std::to_string(s.sheetVersion);
        putTag(b, tag.c_str());
        put32(b, static_cast<uint32_t>(s.frames.size()));
        if (s.sheetVersion == 3) {
            put32(b, 64);
            put32(b, 32);
        }
        uint32_t index = 0;
        for (const auto &f : s.frames) {
            put32(b, index < s.frameImages.size() ? s.frameImages[index] : index);
            ++index;
            putF(b, 0.25f);
            if (s.sheetVersion == 1) {
                put32(b, static_cast<uint32_t>(f[0]));
                put32(b, static_cast<uint32_t>(f[1]));
                put32(b, static_cast<uint32_t>(f[2]));
                put32(b, 0);
                put32(b, 0);
                put32(b, static_cast<uint32_t>(f[5]));
            } else {
                for (float v : f)
                    putF(b, v);
            }
        }
    }
    return b;
}

inline Bytes rgbaPattern(uint32_t w, uint32_t h)
{
    Bytes b(static_cast<size_t>(w) * h * 4);
    for (size_t i = 0; i < b.size(); ++i)
        b[i] = static_cast<uint8_t>(i * 7 + 3);
    return b;
}

inline TexSpec simpleRgba(uint32_t w, uint32_t h)
{
    TexSpec s;
    s.texW = s.imgW = w;
    s.texH = s.imgH = h;
    s.images = {{MipSpec{w, h, rgbaPattern(w, h), false}}};
    return s;
}

// ---- a tiny PNG (stored deflate blocks) for the embedded-image path ----

inline uint32_t crc32(const uint8_t *data, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= data[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

inline void be32(Bytes &b, uint32_t v)
{
    for (int i = 3; i >= 0; --i)
        b.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

inline void pngChunk(Bytes &png, const char *type, const Bytes &data)
{
    be32(png, static_cast<uint32_t>(data.size()));
    Bytes body(type, type + 4);
    putBytes(body, data);
    putBytes(png, body);
    be32(png, crc32(body.data(), body.size()));
}

inline Bytes makePng(uint32_t w, uint32_t h, const Bytes &rgba)
{
    Bytes png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    Bytes ihdr;
    be32(ihdr, w);
    be32(ihdr, h);
    ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0}); // 8 bit RGBA
    pngChunk(png, "IHDR", ihdr);
    Bytes raw;
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0); // filter none
        raw.insert(raw.end(), rgba.begin() + static_cast<long>(y) * w * 4, rgba.begin() + static_cast<long>(y + 1) * w * 4);
    }
    Bytes z = {0x78, 0x01};
    size_t pos = 0;
    do {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        z.push_back(pos + n == raw.size() ? 1 : 0);
        z.push_back(static_cast<uint8_t>(n));
        z.push_back(static_cast<uint8_t>(n >> 8));
        z.push_back(static_cast<uint8_t>(~n));
        z.push_back(static_cast<uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + static_cast<long>(pos), raw.begin() + static_cast<long>(pos + n));
        pos += n;
    } while (pos < raw.size());
    uint32_t a = 1, b2 = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b2 = (b2 + a) % 65521;
    }
    be32(z, (b2 << 16) | a);
    pngChunk(png, "IDAT", z);
    pngChunk(png, "IEND", {});
    return png;
}

inline std::span<const uint8_t> span(const Bytes &b) { return {b.data(), b.size()}; }


using namespace Qt::StringLiterals;

// Everything one item needs: scene.json text plus a few files. Adds the texture "materials/<name>.tex".
struct Item {
    QTemporaryDir dir;
    QString root() const { return dir.path(); }
    void file(const QString &rel, const QByteArray &data) { testutil::writeFile(dir.filePath(rel), data); }
    void tex(const QString &name, const TexSpec &spec) { file(u"materials/"_s + name + u".tex"_s, toQ(makeTex(spec))); }
    // model + material pointing at texture `name`
    void image(const QString &name, const QByteArray &passExtra = {}, const QByteArray &modelExtra = {})
    {
        file(u"models/"_s + name + u".json"_s, "{\"autosize\":true,\"material\":\"materials/" + name.toUtf8() + ".json\"" + modelExtra + "}");
        file(u"materials/"_s + name + u".json"_s,
             "{\"passes\":[{\"blending\":\"translucent\",\"shader\":\"genericimage2\",\"textures\":[\"" + name.toUtf8() + "\"]" + passExtra + "}]}");
    }
};

inline QByteArray sceneJson(const QByteArray &objects, const QByteArray &general = {})
{
    return "{\"camera\":{\"center\":\"0 0 -1\",\"eye\":\"0 0 0\",\"up\":\"0 1 0\"},\"general\":{\"clearcolor\":\"0.1 0.2 0.3\","
           "\"orthogonalprojection\":{\"width\":1920,\"height\":1080}" + general + "},\"objects\":[" + objects + "]}";
}

inline QByteArray imageObject(int id, const QByteArray &name, const QByteArray &extra = {})
{
    return "{\"id\":" + QByteArray::number(id) + ",\"name\":\"" + name + "\",\"image\":\"models/" + name + ".json\","
           "\"origin\":\"960 540 0\",\"size\":\"400 300\"" + extra + "}";
}


} // namespace scenevkfix
