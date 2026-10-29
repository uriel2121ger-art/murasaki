#pragma once

// Text objects: a string is rasterized with FreeType into a coverage bitmap that the renderer draws like an image layer.
//
// Layout rules (derived from Wallpaper Engine's scenes, checked against its clocks):
//   * `pointsize` is in scene units, four scene pixels to the point at scale 1: a 32 point clock on a 1920x1080 scene has
//     an em of 128 pixels. The object's scale is applied afterwards, so the bitmap is made at pointsize x 4 x scale
//     pixels and drawn 1:1 (see textPixelSize).
//   * The bitmap is the whole layout box, not the ink: the widest line by the font's ascender-to-descender height plus
//     one line height for every further line. Lines are left aligned inside it. The renderer then hangs the box on the
//     object's origin by `horizontalalign` and `verticalalign`: left puts the box's left edge on the origin and right its
//     right edge, top hangs the box below the origin and bottom stands it on the origin.
//   * `maxwidth` (scene units) wraps at spaces.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace murasaki::scene {

// Limits of one text object: a script can set any string. Longer text is cut, and the bitmap is at most this many
// pixels each way (the glyphs that would fall outside are dropped).
constexpr size_t kMaxTextCodepoints = 8192;
constexpr unsigned kMaxTextDimension = 4096;

struct TextBitmap {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> coverage; // width x height, top row first
    uint32_t lines = 0;
};

// Pixels of the em for a point size and an object scale (never below 1, never above 1024).
unsigned textPixelSize(float pointSize, float averageScale);

class TextRasterizer
{
public:
    // Null with `error` set when the bytes are not a font FreeType reads (or the engine was built without FreeType).
    static std::unique_ptr<TextRasterizer> create(std::vector<uint8_t> fontBytes, std::string *error = nullptr);
    ~TextRasterizer();
    TextRasterizer(const TextRasterizer &) = delete;
    TextRasterizer &operator=(const TextRasterizer &) = delete;

    // `maxWidthPixels` > 0 wraps lines at spaces.
    TextBitmap render(const std::string &utf8, unsigned pixelSize, float maxWidthPixels = 0.0f);

private:
    struct Impl;
    explicit TextRasterizer(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> m_impl;
};

// The file of an installed font for a name such as "systemfont_consolas" (fontconfig; a substitute when it has none).
// Empty when there is no font at all.
std::vector<uint8_t> systemFontBytes(const std::string &name);

bool textSupported();

} // namespace murasaki::scene
