#pragma once

#include "render/scenerenderer.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

namespace murasaki::scene {

// Helpers shared by the files that define SceneRenderer (scenerenderer*.cpp).
namespace scenerenderer_internal {

constexpr VkFormat kSdrFormat = VK_FORMAT_R8G8B8A8_UNORM;    // the buffers of an ordinary scene
constexpr VkFormat kHdrFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // ... and of an HDR scene
constexpr uint32_t kMaxTargetDimension = 8192;

inline bool isRenderTarget(const std::string &name)
{
    return name.rfind("_rt_", 0) == 0;
}

// Where the quad's origin sits for an alignment name (fractions of the size from the lower left corner).
inline Vec2 anchorFor(const std::string &alignment)
{
    float x = 0.5f;
    float y = 0.5f;
    if (alignment.find("left") != std::string::npos) {
        x = 0.0f;
    } else if (alignment.find("right") != std::string::npos) {
        x = 1.0f;
    }
    if (alignment.find("top") != std::string::npos) {
        y = 1.0f;
    } else if (alignment.find("bottom") != std::string::npos) {
        y = 0.0f;
    }
    return {x, y};
}

inline VkExtent2D scaledExtent(VkExtent2D base, int divisor)
{
    const uint32_t d = static_cast<uint32_t>(std::max(1, divisor));
    return {std::max(1u, base.width / d), std::max(1u, base.height / d)};
}

inline uint64_t mix(uint64_t h, uint64_t v)
{
    h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2);
    return h;
}

inline uint64_t mixText(uint64_t h, const std::string &s)
{
    return mix(h, std::hash<std::string>{}(s));
}

inline PassTexture fileTexture(const GpuTexture &t, PassRunner &runner)
{
    PassTexture p;
    p.view = t.view();
    p.sampler = runner.sampler(t.nearest(), t.clamp());
    p.info = {t.width(), t.height(), t.imageWidth(), t.imageHeight(), t.mipLevels()};
    return p;
}

inline PassTexture targetTexture(const RenderTarget &t, PassRunner &runner)
{
    PassTexture p;
    p.view = t.view();
    p.sampler = runner.linearClamp();
    p.info = {t.extent.width, t.extent.height, t.extent.width, t.extent.height, t.mipLevels};
    return p;
}

inline bool isSceneCopy(const std::string &name)
{
    return name == "_rt_FullFrameBuffer" || name == "_rt_MipMappedFrameBuffer";
}

// Whether a shader reads the mip count of the texture in `slot` (REFLECTION's lod = roughness * g_Texture3MipMapInfo).
inline bool readsMipInfo(const TranslatedShader &shader, uint32_t slot)
{
    const auto reads = [slot](const StageProgram &stage) {
        return std::any_of(stage.uniforms.begin(), stage.uniforms.end(),
                           [slot](const UniformInfo &u) { return u.input == EngineInput::TextureMipMapInfo && u.used && static_cast<uint32_t>(u.aux) == slot; });
    };
    return reads(shader.fragment) || reads(shader.vertex);
}

inline std::vector<uint32_t> slotsOf(const MaterialPass &pass, const std::vector<EffectBind> *binds)
{
    std::vector<int> extra;
    bool slotZeroBound = !pass.textures.empty() && !pass.textures[0].empty();
    if (binds) {
        for (const EffectBind &b : *binds) {
            extra.push_back(b.index);
            slotZeroBound = slotZeroBound || b.index == 0;
        }
        if (!slotZeroBound) {
            extra.push_back(0); // slot 0 is `previous` unless something else is bound there
        }
    }
    return boundSlotsOf(pass, extra);
}

constexpr uint64_t kRetireTextureFrames = 4;

} // namespace scenerenderer_internal

using namespace scenerenderer_internal;

} // namespace murasaki::scene
