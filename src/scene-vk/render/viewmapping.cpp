#include "render/viewmapping.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

ScaledView scaleScene(bridge::msg::ScalingMode mode, Vec2 scene, float w, float h)
{
    const float sx = w / scene.x;
    const float sy = h / scene.y;
    float scaleX = sx;
    float scaleY = sy;
    if (mode == bridge::msg::ScalingMode::Fill) {
        scaleX = scaleY = std::max(sx, sy);
    } else if (mode == bridge::msg::ScalingMode::Fit) {
        scaleX = scaleY = std::min(sx, sy);
    }
    const float shownW = scene.x * scaleX;
    const float shownH = scene.y * scaleY;
    return {shownW, shownH, (w - shownW) / 2.0f, (h - shownH) / 2.0f};
}

ScaledView viewOf(bridge::msg::ScalingMode mode, Vec2 scene, uint32_t outputWidth, uint32_t outputHeight, const bridge::msg::SpanView &span)
{
    const float w = static_cast<float>(outputWidth);
    const float h = static_cast<float>(outputHeight);
    if (!span.enabled || span.w <= 0 || span.h <= 0)
        return scaleScene(mode, scene, w, h);
    using bridge::msg::spanLogical;
    const float perUnitX = w / spanLogical(span.w);
    const float perUnitY = h / spanLogical(span.h);
    ScaledView view = scaleScene(mode, scene, spanLogical(span.canvasW) * perUnitX, spanLogical(span.canvasH) * perUnitY);
    view.offsetX -= spanLogical(span.x) * perUnitX;
    view.offsetY -= spanLogical(span.y) * perUnitY;
    return view;
}

namespace {

struct Axis {
    float from; // first output pixel (inclusive) of the window, as a position along the axis
    float to;
};

// The window along one axis in output pixels: the visible part of the placed scene [offset, offset + size] in an output of
// `length` pixels, grown by the margin, cut at the scene's own edges, and snapped outwards to whole pixels where it is
// cut by the output (not at the scene's edge).
Axis windowAlong(float offset, float size, float length, float margin)
{
    const float sceneFrom = offset;
    const float sceneTo = offset + size;
    float from = std::max(sceneFrom, 0.0f);
    float to = std::min(sceneTo, length);
    if (!(to > from)) {
        return {sceneFrom, sceneTo}; // nothing of the scene is visible: keep the whole thing, it is not drawn anyway
    }
    const float pad = margin * (to - from);
    from = std::max(sceneFrom, from - pad);
    to = std::min(sceneTo, to + pad);
    if (from > sceneFrom) {
        from = std::max(sceneFrom, std::floor(from));
    }
    if (to < sceneTo) {
        to = std::min(sceneTo, std::ceil(to));
    }
    return {from, to};
}

uint32_t pixelsFor(float length, float density, uint32_t maxDimension)
{
    const long n = std::lround(length * density);
    return static_cast<uint32_t>(std::clamp(n, 1l, static_cast<long>(maxDimension)));
}

} // namespace

TargetPlan planSceneTarget(const TargetPlanInput &in)
{
    const ScaledView view = viewOf(in.mode, in.scene, in.outputWidth, in.outputHeight, in.span);
    const float margin = std::max(0.0f, in.marginFraction);
    const Axis x = windowAlong(view.offsetX, view.sizeX, static_cast<float>(in.outputWidth), margin);
    const Axis y = windowAlong(view.offsetY, view.sizeY, static_cast<float>(in.outputHeight), margin);
    // the shake moves the camera by a fraction of the scene on either side: that much more is drawn, still inside the scene
    const float shakeX = std::max(0.0f, in.extraMargin) * view.sizeX;
    const float shakeY = std::max(0.0f, in.extraMargin) * view.sizeY;
    const float fromX = std::max(view.offsetX, x.from - shakeX);
    const float toX = std::min(view.offsetX + view.sizeX, x.to + shakeX);
    const float fromY = std::max(view.offsetY, y.from - shakeY);
    const float toY = std::min(view.offsetY + view.sizeY, y.to + shakeY);

    // output pixels -> scene units
    const float unitX = in.scene.x / view.sizeX;
    const float unitY = in.scene.y / view.sizeY;
    TargetPlan plan;
    plan.presentSize = {toX - fromX, toY - fromY};
    plan.presentOffset = {fromX, fromY};
    plan.windowSize = {plan.presentSize.x * unitX, plan.presentSize.y * unitY};
    // scene y points up, output y points down
    plan.windowMin = {(fromX - view.offsetX) * unitX, in.scene.y - (toY - view.offsetY) * unitY};
    // target pixels per output pixel, but never more than one per scene unit (unitX is scene units per output pixel)
    const float scale = std::max(in.renderScale, 0.01f);
    const float densityX = std::min(scale, unitX);
    const float densityY = std::min(scale, unitY);
    plan.extent.width = pixelsFor(plan.presentSize.x, densityX, in.maxDimension);
    plan.extent.height = pixelsFor(plan.presentSize.y, densityY, in.maxDimension);
    return plan;
}

} // namespace murasaki::scene
