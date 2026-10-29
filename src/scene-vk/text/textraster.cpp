#include "text/textraster.h"

#include <algorithm>
#include <unordered_map>
#include <cmath>
#include <fstream>

#ifdef MURASAKI_SCENE_VK_TEXT

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

namespace murasaki::scene {

unsigned textPixelSize(float pointSize, float averageScale)
{
    if (!std::isfinite(pointSize) || pointSize <= 0.0f) {
        return 1;
    }
    const float scale = std::clamp(std::fabs(averageScale), 0.01f, 8.0f);
    return static_cast<unsigned>(std::clamp(std::round(pointSize * 4.0f * scale), 1.0f, 1024.0f));
}

bool textSupported()
{
    return true;
}

struct TextRasterizer::Impl {
    FT_Library library = nullptr;
    FT_Face face = nullptr;
    std::vector<uint8_t> bytes;
    ~Impl()
    {
        if (face) {
            FT_Done_Face(face);
        }
        if (library) {
            FT_Done_FreeType(library);
        }
    }
};

TextRasterizer::TextRasterizer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
TextRasterizer::~TextRasterizer() = default;

std::unique_ptr<TextRasterizer> TextRasterizer::create(std::vector<uint8_t> fontBytes, std::string *error)
{
    auto impl = std::make_unique<Impl>();
    impl->bytes = std::move(fontBytes);
    if (FT_Init_FreeType(&impl->library) != 0 || impl->bytes.empty()
        || FT_New_Memory_Face(impl->library, impl->bytes.data(), static_cast<FT_Long>(impl->bytes.size()), 0, &impl->face) != 0) {
        if (error) {
            *error = "not a font FreeType can read";
        }
        return nullptr;
    }
    FT_Select_Charmap(impl->face, FT_ENCODING_UNICODE);
    return std::unique_ptr<TextRasterizer>(new TextRasterizer(std::move(impl)));
}

namespace {

std::vector<uint32_t> decode(const std::string &s)
{
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0 && i + 3 < s.size()) {
            cp = (c & 0x07u) << 18 | (s[i + 1] & 0x3Fu) << 12 | (s[i + 2] & 0x3Fu) << 6 | (s[i + 3] & 0x3Fu);
            len = 4;
        } else if (c >= 0xE0 && i + 2 < s.size()) {
            cp = (c & 0x0Fu) << 12 | (s[i + 1] & 0x3Fu) << 6 | (s[i + 2] & 0x3Fu);
            len = 3;
        } else if (c >= 0xC0 && i + 1 < s.size()) {
            cp = (c & 0x1Fu) << 6 | (s[i + 1] & 0x3Fu);
            len = 2;
        }
        out.push_back(cp);
        i += len;
    }
    return out;
}

} // namespace

TextBitmap TextRasterizer::render(const std::string &utf8, unsigned pixelSize, float maxWidthPixels)
{
    FT_Face face = m_impl->face;
    FT_Set_Pixel_Sizes(face, 0, std::max(1u, pixelSize));
    const FT_Size_Metrics &m = face->size->metrics;
    const int ascender = static_cast<int>((m.ascender + 63) >> 6);
    const int descender = static_cast<int>(-((m.descender - 63) >> 6));
    const int lineHeight = std::max(1, static_cast<int>((m.height + 63) >> 6));

    struct Placed {
        uint32_t cp;
        int x; // pen position inside the line
    };
    // Pen positions of a line (advances and kerning) and its width.
    const auto place = [&](const std::vector<uint32_t> &cps, std::vector<Placed> *out) {
        int pen = 0;
        uint32_t previous = 0;
        for (uint32_t cp : cps) {
            const FT_UInt index = FT_Get_Char_Index(face, cp);
            if (previous && FT_HAS_KERNING(face)) {
                FT_Vector k;
                if (FT_Get_Kerning(face, FT_Get_Char_Index(face, previous), index, FT_KERNING_DEFAULT, &k) == 0) {
                    pen += static_cast<int>(k.x >> 6);
                }
            }
            if (out) {
                out->push_back({cp, pen});
            }
            pen += FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) == 0 ? static_cast<int>(face->glyph->advance.x >> 6) : 0;
            previous = cp;
        }
        return pen;
    };

    // paragraphs by newline, each wrapped at spaces. Where a line breaks is decided from the plain advances of the glyphs
    // (kerning ignored, each advance looked up once), which keeps wrapping linear in the length of the text; the
    // layout that is drawn still uses the kerned pen positions.
    std::unordered_map<uint32_t, int> advances;
    const auto advanceOf = [&](uint32_t cp) {
        const auto it = advances.find(cp);
        if (it != advances.end()) {
            return it->second;
        }
        const FT_UInt index = FT_Get_Char_Index(face, cp);
        const int a = FT_Load_Glyph(face, index, FT_LOAD_DEFAULT) == 0 ? static_cast<int>(face->glyph->advance.x >> 6) : 0;
        advances.emplace(cp, a);
        return a;
    };
    std::vector<std::vector<uint32_t>> lines;
    std::vector<uint32_t> paragraph;
    const auto wrap = [&] {
        if (maxWidthPixels <= 0.0f) {
            lines.push_back(paragraph);
            return;
        }
        std::vector<uint32_t> line;
        long lineWidth = 0;
        size_t i = 0;
        while (i < paragraph.size()) {
            size_t j = i;
            long wordWidth = 0;
            while (j < paragraph.size() && paragraph[j] != ' ') {
                wordWidth += advanceOf(paragraph[j]);
                ++j;
            }
            const long joined = lineWidth + (line.empty() ? 0 : advanceOf(' ')) + wordWidth;
            if (!line.empty() && static_cast<float>(joined) > maxWidthPixels) {
                lines.push_back(std::move(line));
                line.assign(paragraph.begin() + static_cast<std::ptrdiff_t>(i), paragraph.begin() + static_cast<std::ptrdiff_t>(j));
                lineWidth = wordWidth;
            } else {
                if (!line.empty()) {
                    line.push_back(' ');
                }
                line.insert(line.end(), paragraph.begin() + static_cast<std::ptrdiff_t>(i), paragraph.begin() + static_cast<std::ptrdiff_t>(j));
                lineWidth = joined;
            }
            i = j + 1;
        }
        lines.push_back(std::move(line));
    };
    std::vector<uint32_t> text = decode(utf8);
    if (text.size() > kMaxTextCodepoints) {
        text.resize(kMaxTextCodepoints);
    }
    for (uint32_t cp : text) {
        if (cp == '\n') {
            wrap();
            paragraph.clear();
        } else if (cp != '\r') {
            paragraph.push_back(cp);
        }
    }
    wrap();
    // the bitmap stays within kMaxTextDimension each way: the lines that would not fit are left out
    const size_t maxLines = std::max<size_t>(1, static_cast<size_t>(std::max(0, static_cast<int>(kMaxTextDimension) - ascender - descender) / lineHeight) + 1);
    if (lines.size() > maxLines) {
        lines.resize(maxLines);
    }

    std::vector<std::vector<Placed>> placed(lines.size());
    int boxWidth = 0;
    for (size_t li = 0; li < lines.size(); ++li) {
        boxWidth = std::max(boxWidth, place(lines[li], &placed[li]));
    }

    TextBitmap out;
    out.lines = static_cast<uint32_t>(lines.size());
    out.width = static_cast<uint32_t>(std::clamp(boxWidth, 1, static_cast<int>(kMaxTextDimension)));
    out.height = static_cast<uint32_t>(std::clamp(ascender + descender + (static_cast<int>(lines.size()) - 1) * lineHeight, 1, static_cast<int>(kMaxTextDimension)));
    out.coverage.assign(static_cast<size_t>(out.width) * out.height, 0);
    for (size_t li = 0; li < placed.size(); ++li) {
        const int baseline = ascender + static_cast<int>(li) * lineHeight;
        for (const Placed &g : placed[li]) {
            if (FT_Load_Char(face, g.cp, FT_LOAD_RENDER) != 0) {
                continue;
            }
            const FT_GlyphSlot slot = face->glyph;
            const FT_Bitmap &bm = slot->bitmap;
            // only 8-bit coverage and 1-bit bitmaps (bitmap-only fonts) carry glyph shapes; colour bitmaps are skipped
            if ((bm.pixel_mode != FT_PIXEL_MODE_GRAY && bm.pixel_mode != FT_PIXEL_MODE_MONO) || !bm.buffer) {
                continue;
            }
            const int x0 = g.x + slot->bitmap_left;
            const int y0 = baseline - slot->bitmap_top;
            for (unsigned row = 0; row < bm.rows; ++row) {
                // a negative pitch means the rows run upwards from `buffer`; signed arithmetic follows either
                const unsigned char *src = bm.buffer + static_cast<std::ptrdiff_t>(row) * bm.pitch;
                for (unsigned col = 0; col < bm.width; ++col) {
                    const int x = x0 + static_cast<int>(col);
                    const int y = y0 + static_cast<int>(row);
                    if (x < 0 || y < 0 || x >= static_cast<int>(out.width) || y >= static_cast<int>(out.height)) {
                        continue;
                    }
                    const uint8_t value = bm.pixel_mode == FT_PIXEL_MODE_MONO ? ((src[col >> 3] >> (7 - (col & 7))) & 1 ? 255 : 0) : src[col];
                    uint8_t &dst = out.coverage[static_cast<size_t>(y) * out.width + static_cast<size_t>(x)];
                    dst = std::max(dst, value);
                }
            }
        }
    }
    return out;
}

std::vector<uint8_t> systemFontBytes(const std::string &name)
{
    std::string family = name;
    const std::string prefix = "systemfont_";
    if (family.rfind(prefix, 0) == 0) {
        family = family.substr(prefix.size());
    }
    std::vector<uint8_t> bytes;
    if (!FcInit()) {
        return bytes;
    }
    FcPattern *pattern = FcPatternCreate();
    FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(family.c_str()));
    FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
    FcDefaultSubstitute(pattern);
    FcResult result;
    FcPattern *match = FcFontMatch(nullptr, pattern, &result);
    if (match) {
        FcChar8 *file = nullptr;
        if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file) {
            std::ifstream in(reinterpret_cast<const char *>(file), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        FcPatternDestroy(match);
    }
    FcPatternDestroy(pattern);
    return bytes;
}

} // namespace murasaki::scene

#else

namespace murasaki::scene {

unsigned textPixelSize(float, float)
{
    return 1;
}
bool textSupported()
{
    return false;
}
struct TextRasterizer::Impl {};
TextRasterizer::TextRasterizer(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
TextRasterizer::~TextRasterizer() = default;
std::unique_ptr<TextRasterizer> TextRasterizer::create(std::vector<uint8_t>, std::string *error)
{
    if (error) {
        *error = "built without FreeType";
    }
    return nullptr;
}
TextBitmap TextRasterizer::render(const std::string &, unsigned, float)
{
    return {};
}
std::vector<uint8_t> systemFontBytes(const std::string &)
{
    return {};
}

} // namespace murasaki::scene

#endif
