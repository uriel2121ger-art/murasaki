#pragma once

// Measuring an effect pass before trusting a shortcut for it. The scene renderer asks, for a pass drawn as
// "the picture so far -> the next buffer", two questions that no list of shader names can answer, because the answer
// depends on the instance's constants, combos and masks:
//   - is it the identity (strength zero, an empty mask, a combo that turns the effect off)? Then it is not drawn.
//   - does it survive being drawn at a fraction of the size and enlarged (a blur, a glow, a soft distortion)? Then it is.
// The pass is drawn for real, as the frame would draw it, into private targets with a synthetic picture in place of the
// picture so far, and the results are compared on the CPU (render/ssim.h). The synthetic picture has smooth areas, hard
// edges, fine texture and a varying alpha, so a pass that only misbehaves on detail shows.

#include "render/passes.h"
#include "render/targets.h"
#include "render/texture.h"

#include <memory>
#include <optional>
#include <vector>

namespace murasaki::scene {

// The part of the picture a pass changes, as fractions of its width and height (the union over the probe's clock readings,
// widened by a margin). A pass that leaves most of the picture alone (a displacement gated by a mask) only needs drawing there.
struct ProbeRegion {
    float x0 = 0.0f, y0 = 0.0f, x1 = 1.0f, y1 = 1.0f;
    float area() const { return (x1 - x0) * (y1 - y0); }
};

struct ProbeOutcome {
    bool identity = false;  // the pass changes nothing (to within the rounding of 8 bits)
    uint32_t factor = 1;    // the largest of the offered reductions that scored above the gate; 1: none did
    double ssim = 1.0;      // the score of that reduction (the best one when none passed)
    int maxDiff = 0;        // the largest channel difference from the input the noise picture showed
    ProbeRegion region;     // where the pass changes the picture (everything when it changes it everywhere)
};

struct ProbeOptions {
    bool testIdentity = true;
    std::vector<uint32_t> factors;  // reductions to try, largest first
    double gate = 0.995;            // lowest score a reduction may have
    uint32_t maxWidth = 960;        // the probe picture's width cap (its height follows the pass's aspect ratio)
};

// Whether a shader's result depends on things a probe cannot reproduce (the pointer, the audio): such a pass is never judged.
bool probeable(const TranslatedShader &shader);

class PassProbe
{
public:
    PassProbe(const VkContext &ctx, PassRunner &runner) : m_ctx(ctx), m_runner(runner) {}

    // `request` is the pass as the frame would draw it with its slot 0 holding the picture so far (only the target, its
    // extent and slot 0 are replaced). Every other texture must be a file texture (already on the GPU). Draws with the
    // runner but records nothing in a frame: it submits its own command buffers and waits, so it may be called while a
    // frame is being recorded. Nullopt when the GPU work failed.
    std::optional<ProbeOutcome> measure(DrawRequest request, const ProbeOptions &options);

    uint64_t probes() const { return m_probes; }

private:
    struct Slot {
        RenderTarget target;
    };
    RenderTarget &targetOf(Slot &slot, VkExtent2D extent);
    std::vector<uint8_t> render(const DrawRequest &request, Slot &slot, VkExtent2D extent);
    struct Input {
        std::unique_ptr<GpuTexture> texture;
        std::vector<uint8_t> pixels;
    };
    void ensureInput(Input &input, bool noise, uint32_t width, uint32_t height);

    const VkContext &m_ctx;
    PassRunner &m_runner;
    Input m_detail; // the structured picture the reductions are judged on
    Input m_noise;  // per-pixel noise: any change of any pixel shows, which is what the identity and the changed region need
    Slot m_full, m_reduced;
    GpuBuffer m_readback;
    uint64_t m_probes = 0;
};

// The synthetic pictures (RGBA8, straight alpha), deterministic: smooth areas, hard edges, fine texture and a varying alpha /
// noise in every channel of every pixel.
std::vector<uint8_t> probePicture(uint32_t width, uint32_t height);
std::vector<uint8_t> noisePicture(uint32_t width, uint32_t height);

} // namespace murasaki::scene
