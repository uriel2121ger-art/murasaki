#include "render/scenerenderer.h"

#include "assets/texfile.h"
#include "assets/imagedecode.h"
#include "util/parallel.h"
#include "render/puppetdraw.h"

#include "core/framebuffers.h"
#include "render/gpuprofiler.h"
#include "render/shake.h"
#include "render/sprite.h"
#include "render/viewmapping.h"
#include "util/log.h"


#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <set>
#include <stdexcept>

namespace murasaki::scene {

// The texture of every text layer: the string at the pixel size its point size and scale ask for, redone only when
// one of those changes (a clock's script changes the string once a second, a bound property when the user types).
void SceneRenderer::updateTexts()
{
    for (size_t i = 0; i < m_scene.layers.size(); ++i) {
        ImageLayer &layer = m_scene.layers[i];
        if (!layer.text) {
            continue;
        }
        TextSpec &spec = *layer.text;
        std::string value = spec.value;
        if (!spec.user.empty()) {
            if (const PropertyValue *p = m_props.find(spec.user); p && std::holds_alternative<std::string>(*p)) {
                value = std::get<std::string>(*p);
            }
        }
        const SceneNode &node = m_scene.nodes[layer.node];
        // the text's own pixels follow its scale in an orthographic scene (one scene unit is about a screen pixel there); under
        // a perspective camera the scale takes the box into world units, nothing like pixels, so the text is drawn at its own size
        float scale = m_scene.perspective ? 1.0f : (std::fabs(node.scale.x) + std::fabs(node.scale.y)) * 0.5f;
        if (!(scale > 0.0f)) {
            scale = 1.0f;
        }
        const unsigned pixels = textPixelSize(spec.pointSize, scale);
        TextState &state = m_texts[i];
        if (state.failed && state.fontKey == spec.font) {
            continue;
        }
        if (!state.shown.empty() || state.pixels != 0) {
            if (state.shown == value && state.pixels == pixels && state.fontKey == spec.font && state.maxWidth == spec.maxWidth) {
                continue;
            }
        }
        rasterizeText(i, value, pixels, scale);
    }
    m_scriptFlags.textChanged = false;
}

// One rasterizer per font file or name, shared by the text layers that use it. A font that is not there, or that FreeType
// cannot read, gives the system's sans-serif instead; null only when there is no font at all.
std::shared_ptr<TextRasterizer> SceneRenderer::fontFor(const std::string &font)
{
    if (const auto it = m_fonts.find(font); it != m_fonts.end()) {
        return it->second;
    }
    std::shared_ptr<TextRasterizer> raster;
    std::vector<uint8_t> bytes;
    if (font.rfind("systemfont_", 0) == 0) {
        bytes = systemFontBytes(font);
    } else if (const auto file = m_vfs.read(font)) {
        bytes = *file;
    }
    std::string error;
    if (!bytes.empty()) {
        raster = TextRasterizer::create(std::move(bytes), &error);
        if (!raster) {
            logError("text font '{}': {}; using the system font", font, error);
        }
    }
    if (!raster) {
        if (const auto it = m_fonts.find("systemfont_sans-serif"); it != m_fonts.end()) {
            raster = it->second;
        } else {
            raster = TextRasterizer::create(systemFontBytes("sans-serif"), &error);
            if (!raster) {
                logError("text: no usable font: {}", error);
            }
            m_fonts["systemfont_sans-serif"] = raster;
        }
    }
    m_fonts[font] = raster;
    return raster;
}

void SceneRenderer::rasterizeText(size_t index, const std::string &value, unsigned pixels, float scale)
{
    ImageLayer &layer = m_scene.layers[index];
    const TextSpec &spec = *layer.text;
    TextState &state = m_texts[index];
    if (!state.raster || state.fontKey != spec.font) {
        state.fontKey = spec.font;
        state.raster = fontFor(spec.font);
        if (!state.raster) {
            state.failed = true;
            return;
        }
        state.failed = false;
    }
    const TextBitmap bitmap = state.raster->render(value, pixels, spec.maxWidth > 0.0f ? spec.maxWidth * scale : 0.0f);
    state.shown = value;
    state.pixels = pixels;
    state.maxWidth = spec.maxWidth;
    // White with the glyph coverage as alpha; an empty string is one transparent texel. `padding` is empty room around the
    // text on every side (the library's boxes are the text plus twice the padding), where an effect can draw.
    const float wanted = std::isfinite(spec.padding * scale) ? spec.padding * scale : 0.0f;
    const uint32_t pad = static_cast<uint32_t>(std::clamp(std::lround(wanted), 0L, 1024L)); // (1024: a safety bound on the texture)
    const uint32_t width = bitmap.width + 2 * pad;
    const uint32_t height = bitmap.height + 2 * pad;
    Tex tex;
    tex.format = PixelFormat::Rgba8;
    tex.flags = TexFlags::ClampUVs;
    tex.textureWidth = tex.imageWidth = width;
    tex.textureHeight = tex.imageHeight = height;
    TexMip mip;
    mip.width = width;
    mip.height = height;
    mip.data.resize(static_cast<size_t>(width) * height * 4);
    for (size_t i = 0; i < mip.data.size(); i += 4) {
        mip.data[i] = mip.data[i + 1] = mip.data[i + 2] = 255;
        mip.data[i + 3] = 0;
    }
    for (uint32_t y = 0; y < bitmap.height; ++y) {
        uint8_t *row = mip.data.data() + (static_cast<size_t>(y + pad) * width + pad) * 4;
        for (uint32_t x = 0; x < bitmap.width; ++x) {
            row[x * 4 + 3] = bitmap.coverage[static_cast<size_t>(y) * bitmap.width + x];
        }
    }
    tex.mips.push_back(std::move(mip));
    std::string error;
    auto texture = GpuTexture::create(m_ctx, tex, error);
    if (!texture) {
        logError("text texture: {}", error);
        return;
    }
    m_textures["__text:" + std::to_string(layer.node)] = std::move(texture); // the previous frame is complete: the old one can go
    layer.size = {static_cast<float>(width) / scale, static_cast<float>(height) / scale};
    m_dirty = true;
}

} // namespace murasaki::scene
