#pragma once

// Murasaki's extension of the bridge protocol: SpanView, version 3. Kept out of protocol.h on purpose: that file is
// shared verbatim with the bridge producers, and a
// producer of the base protocol does not draw spanned wallpapers. It keeps announcing version 2, ignores the unknown message type, and a
// consumer never sends SpanView to a producer that announced less than kSpanViewVersion.

#include "protocol.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace murasaki::bridge {

// The Hello version of a producer that understands SpanView. A consumer accepts kBridgeVersion (the base protocol)
// up to this.
inline constexpr uint32_t kSpanViewVersion = 3;
inline constexpr MsgType kSpanViewType = static_cast<MsgType>(43);
// Geometry travels as fixed point so that the payload has no float representation to validate: 1/64 logical pixel.
inline constexpr int32_t kSpanUnit = 64;
inline constexpr int32_t kMaxSpanCanvas = 65536 * kSpanUnit;

namespace msg {

// Spanning: the wallpaper is one picture across several outputs. This viewport shows the rectangle (x, y, w, h) of a
// canvas of canvasW by canvasH, all in the compositor's logical pixels (times kSpanUnit). The producer applies its
// scaling mode to the whole canvas and draws only that rectangle, at the resolution of its own buffers. enabled 0
// goes back to drawing the whole picture in the viewport (the other fields are then ignored).
struct SpanView {
    static constexpr MsgType kType = kSpanViewType;
    uint32_t enabled; // 0 or 1
    int32_t canvasW;
    int32_t canvasH;
    int32_t x;
    int32_t y;
    int32_t w;
    int32_t h;
};
static_assert(sizeof(SpanView) == 28 && std::is_trivially_copyable_v<SpanView>);
static_assert(std::has_unique_object_representations_v<SpanView>, "SpanView must have no padding");

inline int32_t spanFixed(double logical)
{
    return static_cast<int32_t>(std::lround(std::clamp(logical, -1.0e5, 1.0e5) * kSpanUnit));
}
inline float spanLogical(int32_t fixed)
{
    return static_cast<float>(fixed) / static_cast<float>(kSpanUnit);
}

inline SpanView makeSpanView(bool enabled, double canvasW, double canvasH, double x, double y, double w, double h)
{
    return enabled ? SpanView{1, spanFixed(canvasW), spanFixed(canvasH), spanFixed(x), spanFixed(y), spanFixed(w), spanFixed(h)}
                   : SpanView{0, 0, 0, 0, 0, 0, 0};
}

inline bool validate(const SpanView &m, size_t fdCount)
{
    if (fdCount != 0 || m.enabled > 1)
        return false;
    if (m.enabled == 0)
        return true;
    // The rectangle lies inside a canvas of sane size (a hair of rounding allowed).
    constexpr int64_t slack = 1;
    return m.canvasW >= kSpanUnit && m.canvasW <= kMaxSpanCanvas && m.canvasH >= kSpanUnit && m.canvasH <= kMaxSpanCanvas
        && m.x >= 0 && m.y >= 0 && m.w > 0 && m.h > 0 && int64_t{m.x} + m.w <= int64_t{m.canvasW} + slack
        && int64_t{m.y} + m.h <= int64_t{m.canvasH} + slack;
}

} // namespace msg

} // namespace murasaki::bridge
