#pragma once

#include "puppet/params.h"

#include <cstdint>
#include <string>
#include <vector>

namespace murasaki::puppet {

// The model abstraction of the puppet engine (docs/pets/puppets.md, sections 12 to 16): whatever reads a model (the open reader of openformat.h; later the Cubism Core wrapper) offers the same
// things to the rest of the engine: the parameters and parts it can be driven by, the static shape of each drawable, and, for a set of parameter values, the draw list of one frame in painter's
// order. Pure and Qt-free: no GPU, no clock.
enum class BlendMode : std::uint8_t {
    Normal, Multiply, Screen, Overlay, Darken, Lighten, ColorDodge, LinearDodge, AddGlow, ColorBurn, HardLight, SoftLight, Difference, Exclusion, Subtract, Inverse, DestinationIn, ClipToLower,
    SliceFromLower
};
inline constexpr int kBlendModeCount = 19;
const char *blendModeName(BlendMode mode); // "Normal", "Multiply", ...

// A mask: the geometry of the drawable `source` (see Frame::maskSources), drawn where its alpha is above the *source's* threshold, marks the stencil with 1 (Mask) or 0 (dodge).
struct MaskRef {
    int source = -1;
    bool dodge = false;
};

enum class DrawKind { Mesh, BeginGroup, EndGroup };

// One entry of a frame's draw list. A Mesh is a drawable to draw (the shape - uvs and indices - is static: ModelBackend::shape). BeginGroup / EndGroup bracket drawables that are drawn into an off-screen
// group first; the EndGroup entry carries how the group is put on the picture (blend, opacity, tint, masks).
struct DrawItem {
    DrawKind kind = DrawKind::Mesh;
    int drawable = -1;               // index of the drawable (Mesh) or of the group (Begin/EndGroup); stable across frames
    std::vector<Vec2> positions;     // Mesh: the vertices in the canvas' units, y down, after every deformation and transform
    double opacity = 1;
    double tint[3] = {1, 1, 1};
    double screenTint[3] = {0, 0, 0};
    BlendMode blend = BlendMode::Normal;
    int texture = -1;                // albedo texture of the model, -1 none
    double maskThreshold = 0.5;      // of this drawable when it is a mask source
    std::vector<MaskRef> masks;
};

struct Frame {
    std::vector<DrawItem> items;        // painter's order, first drawn first
    std::vector<DrawItem> maskSources;  // the geometry of every drawable some mask uses, whether or not it is drawn itself
};

struct Shape {
    std::vector<Vec2> uvs;                 // one per vertex, 0..1, v down
    std::vector<std::uint16_t> indices;    // triangles
};

struct Bounds {
    double minX = 0, minY = 0, maxX = 0, maxY = 0;
};

class ModelBackend
{
public:
    virtual ~ModelBackend() = default;
    virtual std::string name() const = 0;
    // The parameters a host can set (ids, ranges, defaults) and the parts whose opacity it can set. A model's own values at rest are the defaults.
    virtual const ParameterSet &parameters() const = 0;
    virtual const PartSet &parts() const = 0;
    virtual size_t drawableCount() const = 0;
    // The id a drawable is known by in the model's own files (a Cubism hit area names a drawable by it); empty when the model has none for it.
    virtual std::string drawableId(int drawable) const { (void)drawable; return {}; }
    virtual const Shape &shape(int drawable) const = 0;
    // Where the model lies at rest, in its units.
    virtual Bounds bounds() const = 0;
    // The draw list for these values. `values` must be this model's parameter set (same ids and order; the host copies parameters() and sets the values); a set of another size is refused.
    // Returns false (frame untouched) when refused.
    virtual bool evaluate(const ParameterSet &values, const PartSet &parts, Frame &out) = 0;
};

} // namespace murasaki::puppet
