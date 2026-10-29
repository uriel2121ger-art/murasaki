#include "render/passprobe.h"

#include "render/ssim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace murasaki::scene {

namespace {

// Two clock readings far apart: a pass that is the identity at both is taken to be one (the clock only ever moves a
// pass's effect, it does not switch it on and off at these values).
constexpr double kProbeTimes[2] = {0.37, 5.91};
// A pass that reads the clock can be the identity at a few readings and not at others (a pulse, a flash every few seconds):
// it is judged over a spread of readings across a minute (step chosen so no common period lines up with it).
constexpr int kClockProbes = 24;
constexpr double kClockStep = 2.6137;

bool readsClock(const TranslatedShader &shader)
{
    const auto time = [](const UniformInfo &u) { return u.used && (u.input == EngineInput::Time || u.input == EngineInput::Daytime); };
    return std::any_of(shader.vertex.uniforms.begin(), shader.vertex.uniforms.end(), time)
        || std::any_of(shader.fragment.uniforms.begin(), shader.fragment.uniforms.end(), time);
}
constexpr float kRegionMargin = 0.03f;
constexpr int kIdentityTolerance = 2;      // of 255
constexpr double kIdentitySsim = 0.999;

float hash(uint32_t x, uint32_t y)
{
    uint32_t h = x * 374761393u + y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return static_cast<float>((h ^ (h >> 16)) & 0xffffu) / 65535.0f;
}

} // namespace

bool probeable(const TranslatedShader &shader)
{
    const auto bad = [](const UniformInfo &u) {
        return u.used && (u.input == EngineInput::PointerPosition || u.input == EngineInput::PointerPositionLast
                          || u.input == EngineInput::PointerState || u.input == EngineInput::AudioSpectrum);
    };
    return std::none_of(shader.vertex.uniforms.begin(), shader.vertex.uniforms.end(), bad)
        && std::none_of(shader.fragment.uniforms.begin(), shader.fragment.uniforms.end(), bad);
}

std::vector<uint8_t> probePicture(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> p(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(width), v = static_cast<float>(y) / static_cast<float>(height);
            // smooth colour field + a few soft blobs
            float r = 0.5f + 0.4f * std::sin(u * 6.0f + v * 2.0f), g = 0.5f + 0.4f * std::sin(v * 5.0f - u), b = 0.5f + 0.4f * std::cos(u * 3.0f + v * 4.0f);
            const float blob = std::exp(-((u - 0.3f) * (u - 0.3f) + (v - 0.6f) * (v - 0.6f)) * 40.0f);
            r += blob * 0.5f;
            b -= blob * 0.3f;
            // hard edges: a bar and a disc
            if (u > 0.55f && u < 0.62f && v > 0.1f && v < 0.9f) {
                r = 1.0f, g = 1.0f, b = 0.1f;
            }
            if ((u - 0.78f) * (u - 0.78f) + (v - 0.35f) * (v - 0.35f) < 0.015f) {
                r = 0.05f, g = 0.1f, b = 0.9f;
            }
            // fine texture in the lower-left quarter
            if (u < 0.45f && v > 0.65f) {
                const float n = hash(x, y);
                r = g = b = 0.2f + 0.6f * n;
            }
            // alpha: opaque, a soft fade at the right, a transparent patch
            float a = 1.0f - std::clamp((u - 0.8f) * 5.0f, 0.0f, 1.0f) * 0.7f;
            if (u > 0.05f && u < 0.2f && v > 0.05f && v < 0.25f) {
                a = 0.0f;
            }
            uint8_t *px = &p[(static_cast<size_t>(y) * width + x) * 4];
            px[0] = static_cast<uint8_t>(std::clamp(r, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[1] = static_cast<uint8_t>(std::clamp(g, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[2] = static_cast<uint8_t>(std::clamp(b, 0.0f, 1.0f) * 255.0f + 0.5f);
            px[3] = static_cast<uint8_t>(a * 255.0f + 0.5f);
        }
    }
    return p;
}

std::vector<uint8_t> noisePicture(uint32_t width, uint32_t height)
{
    std::vector<uint8_t> p(static_cast<size_t>(width) * height * 4);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            uint8_t *px = &p[(static_cast<size_t>(y) * width + x) * 4];
            px[0] = static_cast<uint8_t>(hash(x, y) * 255.0f);
            px[1] = static_cast<uint8_t>(hash(x + 7919, y + 104729) * 255.0f);
            px[2] = static_cast<uint8_t>(hash(x + 15485863, y + 32452843) * 255.0f);
            px[3] = static_cast<uint8_t>(64.0f + hash(x + 49979687, y + 86028121) * 191.0f);
        }
    }
    return p;
}

void PassProbe::ensureInput(Input &input, bool noise, uint32_t width, uint32_t height)
{
    if (input.texture && input.texture->width() == width && input.texture->height() == height) {
        return;
    }
    std::string error;
    input.texture = GpuTexture::createDynamic(m_ctx, width, height, false, true, error);
    if (!input.texture) {
        throw VkException("probe picture: " + error);
    }
    input.pixels = noise ? noisePicture(width, height) : probePicture(width, height);
    input.texture->update(m_ctx, input.pixels.data());
}

RenderTarget &PassProbe::targetOf(Slot &slot, VkExtent2D extent)
{
    RenderTarget &t = slot.target;
    if (!t.image.handle() || t.extent.width != extent.width || t.extent.height != extent.height) {
        t = RenderTarget{};
        t.extent = extent;
        t.format = VK_FORMAT_R8G8B8A8_UNORM;
        t.image = m_ctx.createImage(t.format, extent.width, extent.height, 1,
                                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    }
    return t;
}

std::vector<uint8_t> PassProbe::render(const DrawRequest &request, Slot &slot, VkExtent2D extent)
{
    RenderTarget &target = targetOf(slot, extent);
    const size_t bytes = static_cast<size_t>(extent.width) * extent.height * 4;
    if (!m_readback || m_readback.size() < bytes) {
        m_readback = m_ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
    }
    GpuBuffer &out = m_readback;
    DrawRequest req = request;
    req.target = target.view();
    req.targetFormat = target.format;
    req.extent = extent;
    req.clear = true;
    req.blend = false;
    m_ctx.submitAndWait([&](VkCommandBuffer cb) {
        transition(cb, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, true);
        m_runner.draw(cb, req);
        transition(cb, target, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {extent.width, extent.height, 1};
        vkCmdCopyImageToBuffer(cb, target.handle(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out.handle(), 1, &region);
    });
    out.invalidate();
    std::vector<uint8_t> pixels(bytes);
    std::memcpy(pixels.data(), out.mapped(), bytes);
    return pixels;
}

std::optional<ProbeOutcome> PassProbe::measure(DrawRequest request, const ProbeOptions &options)
{
    const VkExtent2D real = request.extent;
    if (real.width < 16 || real.height < 16) {
        return std::nullopt;
    }
    const uint32_t width = std::min(real.width, options.maxWidth);
    const uint32_t height = std::max<uint32_t>(16, static_cast<uint32_t>(std::lround(static_cast<double>(width) * real.height / real.width)));
    static const int dbg = std::getenv("MURASAKI_SCENE_VK_PASS_OPT_DEBUG") ? std::atoi(std::getenv("MURASAKI_SCENE_VK_PASS_OPT_DEBUG")) : 0;
    if (dbg == 1) {
        return std::nullopt;
    }
    try {
        ensureInput(m_noise, true, width, height);
        if (dbg == 2) {
            return std::nullopt;
        }
        request.textures[0].sampler = m_runner.linearClamp();
        // (the sizes the shader sees stay those of the real picture: a kernel is a number of its texels)
        ++m_probes;

        ProbeOutcome outcome;
        // identity and changed region, on the noise picture
        request.textures[0].view = m_noise.texture->view();
        bool identity = options.testIdentity;
        int minX = static_cast<int>(width), minY = static_cast<int>(height), maxX = -1, maxY = -1;
        std::vector<double> times(std::begin(kProbeTimes), std::end(kProbeTimes));
        if (request.shader && readsClock(*request.shader)) {
            times.clear();
            for (int k = 0; k < kClockProbes; ++k) {
                times.push_back(0.37 + k * kClockStep);
            }
        }
        size_t step = 0;
        for (const double time : times) {
            request.uniforms.inputs.time = time;
            request.uniforms.inputs.daytime = static_cast<float>(std::fmod(static_cast<double>(step++) * 0.1373, 1.0));
            const std::vector<uint8_t> out = render(request, m_full, {width, height});
            if (dbg == 3) {
                return std::nullopt;
            }
            if (const char *dump = std::getenv("MURASAKI_SCENE_VK_PASS_OPT_DUMP")) {
                if (FILE *f = std::fopen((std::string(dump) + "/probe-" + std::to_string(m_probes) + ".pam").c_str(), "wb")) {
                    std::fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", width, height);
                    std::fwrite(out.data(), 1, out.size(), f);
                    std::fclose(f);
                }
            }
            outcome.maxDiff = std::max(outcome.maxDiff, maxChannelDifference(out.data(), m_noise.pixels.data(), out.size()));
            if (identity) {
                identity = outcome.maxDiff <= kIdentityTolerance
                    && ssimRgba(out.data(), m_noise.pixels.data(), width, height) >= kIdentitySsim;
            }
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const size_t at = (static_cast<size_t>(y) * width + x) * 4;
                    for (int c = 0; c < 4; ++c) {
                        if (std::abs(static_cast<int>(out[at + c]) - static_cast<int>(m_noise.pixels[at + c])) > kIdentityTolerance) {
                            minX = std::min(minX, static_cast<int>(x)), maxX = std::max(maxX, static_cast<int>(x));
                            minY = std::min(minY, static_cast<int>(y)), maxY = std::max(maxY, static_cast<int>(y));
                            break;
                        }
                    }
                }
            }
        }
        if (identity) {
            outcome.identity = true;
            return outcome;
        }
        if (maxX >= 0) {
            outcome.region = {std::max(0.0f, static_cast<float>(minX) / static_cast<float>(width) - kRegionMargin),
                              std::max(0.0f, static_cast<float>(minY) / static_cast<float>(height) - kRegionMargin),
                              std::min(1.0f, static_cast<float>(maxX + 1) / static_cast<float>(width) + kRegionMargin),
                              std::min(1.0f, static_cast<float>(maxY + 1) / static_cast<float>(height) + kRegionMargin)};
        }
        // reductions, on the structured picture
        if (options.factors.empty()) {
            return outcome;
        }
        ensureInput(m_detail, false, width, height);
        request.textures[0].view = m_detail.texture->view();
        std::vector<uint8_t> full[2];
        for (int i = 0; i < 2; ++i) {
            request.uniforms.inputs.time = kProbeTimes[i];
            full[i] = render(request, m_full, {width, height});
        }
        double best = 0.0;
        for (const uint32_t factor : options.factors) {
            const VkExtent2D small{std::max<uint32_t>(8, width / factor), std::max<uint32_t>(8, height / factor)};
            double worst = 1.0;
            for (int i = 0; i < 2; ++i) {
                request.uniforms.inputs.time = kProbeTimes[i];
                const std::vector<uint8_t> reduced = render(request, m_reduced, small);
                const std::vector<uint8_t> back = upsampleBilinear(reduced.data(), small.width, small.height, width, height);
                worst = std::min(worst, ssimRgba(full[i].data(), back.data(), width, height));
            }
            if (worst >= options.gate) {
                outcome.factor = factor;
                outcome.ssim = worst;
                return outcome;
            }
            best = std::max(best, worst);
        }
        outcome.ssim = best;
        return outcome;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}

} // namespace murasaki::scene
