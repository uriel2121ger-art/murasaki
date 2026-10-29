#pragma once

#include "puppet/model.h"

#include <cstdint>
#include <vector>

namespace murasaki::puppet {

// How a model's frame lands on the picture the engine sends (docs/pets/puppets.md, section 19). Pure: no GPU.
//
// The picture shows the model's rest bounds, centred and scaled to fit with the aspect kept (the pet's size is the window's; the height follows the canvas aspect, plan 4.6). A model point
// (x, y), y down, goes to normalised device coordinates (x * scaleX + offsetX, y * scaleY + offsetY) with y down too (Vulkan's), and to picture pixels ((ndc + 1) / 2 * size).
struct ViewTransform {
    double scaleX = 1;
    double scaleY = 1;
    double offsetX = 0;
    double offsetY = 0;
};

// Fits `bounds` into a width x height picture. A model with no extent (empty or a point) gets the identity-sized unit view.
ViewTransform fitBounds(const Bounds &bounds, uint32_t width, uint32_t height);

struct HitMask {
    uint32_t cols = 0;
    uint32_t rows = 0;
    uint32_t cell = 8;
    std::vector<uint8_t> bits; // row 0 first, bit 0 of a byte first, cols x rows bits, the last byte padded with zeros

    bool at(uint32_t col, uint32_t row) const
    {
        if (col >= cols || row >= rows)
            return false;
        const size_t i = static_cast<size_t>(row) * cols + col;
        return (bits[i / 8] >> (i % 8)) & 1u;
    }
    size_t count() const;
};

// The footprint of a frame on the picture: a cell is set when a triangle of a drawn mesh (opacity above 0, inside a group or not) covers its centre, one of its corners (inset by a pixel) or
// has a vertex in it. It is the mesh's, not the texture's: a transparent texel of a covered triangle counts. At most kMaxHitMaskSide cells on a side (a larger picture gets larger cells).
HitMask hitMaskOf(const Frame &frame, const ModelBackend &model, const ViewTransform &view, uint32_t width, uint32_t height, uint32_t cell = 8);

// What drawing a frame costs the GPU, worked out on the CPU before anything is recorded: `pixels` is an upper bound of the area (in picture pixels) the triangles of every drawn mesh cover, from the boxes of the
// triangles (half a box where it lies inside the picture, the clipped box where it crosses an edge; no clipping of the triangle itself), plus the mask sources drawn again for each use, plus 1/kStencilClearShare of a
// picture for every stencil clear and group clear, plus the box of what a group holds when it is put on the picture; `vertices` is how many positions the frame writes (a mask source counts at each use). The work is bounded by
// kMaxFrameCostWork triangles; the items after that count as a picture each.
struct FrameCost {
    double pixels = 0;
    std::uint64_t vertices = 0;
};
FrameCost frameCost(const Frame &frame, const ModelBackend &model, const ViewTransform &view, uint32_t width, uint32_t height);
// How many times frameCost has run in this process (the engine keeps its answer while nothing that shows changes; a test counts).
std::uint64_t frameCostRuns();

// How many times hitMaskOf has run in this process (for the tests that check it is not worked out again when nothing that shows has changed).
std::uint64_t hitMaskRuns();

// When to send a mask: the first one, and then when it differs from the last sent by at least `minChangedShare` of its cells, or by any cell once `maxStaleFrames` updates have passed.
class HitMaskTracker
{
public:
    explicit HitMaskTracker(double minChangedShare = 0.02, unsigned maxStaleFrames = 30) : m_share(minChangedShare), m_stale(maxStaleFrames) {}
    // True when `mask` should be sent now (it becomes the last sent).
    bool update(const HitMask &mask);
    void reset() { m_sent = false; m_since = 0; }

private:
    double m_share;
    unsigned m_stale;
    HitMask m_last;
    bool m_sent = false;
    unsigned m_since = 0;
};

} // namespace murasaki::puppet
