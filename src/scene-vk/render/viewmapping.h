#pragma once

// Where the scene lands in an output (fill, fit, stretch, one part of a spanned canvas) and how big the scene target
// has to be to cover what is visible there. Pure arithmetic, no Vulkan state: the tests check it directly.

#include "bridge/protocol.h"
#include "bridge/spanview.h"
#include "util/math.h"

#include <cstdint>

namespace murasaki::scene {

struct ScaledView {
    float sizeX, sizeY;     // the scene's size in output pixels
    float offsetX, offsetY; // where its upper left corner lands
};

ScaledView scaleScene(bridge::msg::ScalingMode mode, Vec2 scene, float w, float h);

// Where the scene lands in an output of this size. When this output is one part of a spanned canvas, the scaling mode
// applies to the whole canvas (at this output's pixels per logical unit) and the output shows its rectangle of it.
ScaledView viewOf(bridge::msg::ScalingMode mode, Vec2 scene, uint32_t outputWidth, uint32_t outputHeight, const bridge::msg::SpanView &span);

struct TargetExtent {
    uint32_t width = 0;
    uint32_t height = 0;
};

// The part of the scene that is drawn, and the target it is drawn into.
struct TargetPlan {
    TargetExtent extent;
    Vec2 windowMin;      // lower left corner of the drawn rectangle, in scene units (y up)
    Vec2 windowSize;     // its size in scene units
    Vec2 presentSize;    // that rectangle's size in output pixels
    Vec2 presentOffset;  // where its upper left corner lands in the output, in pixels from the top left
    bool windowed() const { return windowSize.x > 0.0f; }
};

struct TargetPlanInput {
    bridge::msg::ScalingMode mode = bridge::msg::ScalingMode::Fill;
    Vec2 scene{1.0f, 1.0f};
    uint32_t outputWidth = 1;
    uint32_t outputHeight = 1;
    bridge::msg::SpanView span{};
    float renderScale = 1.0f;     // target pixels per output pixel (never above one pixel per scene unit)
    float marginFraction = 0.02f; // extra scene around the visible part, as a fraction of it
    float extraMargin = 0.0f;     // more of the same, as a fraction of the scene (camera shake)
    uint32_t maxDimension = 8192;
};

// The scene target covers the visible part of the scene (plus a margin) at renderScale target pixels per output pixel,
// instead of the whole scene at its own size. Edges cut by the output land on whole output pixels, so that at a
// render scale of 1 the final step is a copy and not a blur.
TargetPlan planSceneTarget(const TargetPlanInput &input);

} // namespace murasaki::scene
