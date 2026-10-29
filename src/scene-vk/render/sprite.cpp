#include "render/sprite.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

namespace {

size_t frameAt(const std::vector<TexFrame> &frames, double seconds, int forcedFrame)
{
    double total = 0.0;
    for (const TexFrame &f : frames) {
        total += std::max(0.0f, f.duration);
    }
    size_t index = 0;
    if (total > 0.0 && frames.size() > 1) {
        double t = std::fmod(std::max(0.0, seconds), total);
        for (size_t i = 0; i < frames.size(); ++i) {
            const double d = std::max(0.0f, frames[i].duration);
            if (t < d || i + 1 == frames.size()) {
                index = i;
                break;
            }
            t -= d;
        }
    }
    if (forcedFrame >= 0) {
        index = std::min<size_t>(static_cast<size_t>(forcedFrame), frames.size() - 1);
    }
    return index;
}

SpriteFrameUniforms place(const TexFrame &f, size_t index, uint32_t sheetWidth, uint32_t sheetHeight)
{
    SpriteFrameUniforms out;
    const float w = static_cast<float>(sheetWidth);
    const float h = static_cast<float>(sheetHeight);
    out.frame = index;
    out.translation = {f.x / w, f.y / h};
    out.rotation = {f.axisXx / w, f.axisXy / h, f.axisYx / w, f.axisYy / h};
    return out;
}

} // namespace

SpriteFrameUniforms spriteFrame(const std::vector<TexFrame> &frames, uint32_t sheetWidth, uint32_t sheetHeight, double seconds, int forcedFrame)
{
    if (frames.empty() || sheetWidth == 0 || sheetHeight == 0) {
        return {};
    }
    const size_t index = frameAt(frames, seconds, forcedFrame);
    return place(frames[index], index, sheetWidth, sheetHeight);
}

SpriteFrameUniforms spriteFrame(const std::vector<TexFrame> &frames, const std::vector<std::array<uint32_t, 2>> &pageSizes, double seconds, int forcedFrame)
{
    if (frames.empty() || pageSizes.empty()) {
        return {};
    }
    const size_t index = frameAt(frames, seconds, forcedFrame);
    const size_t page = std::min<size_t>(frames[index].index, pageSizes.size() - 1);
    if (pageSizes[page][0] == 0 || pageSizes[page][1] == 0) {
        return {};
    }
    SpriteFrameUniforms out = place(frames[index], index, pageSizes[page][0], pageSizes[page][1]);
    out.page = page;
    return out;
}

} // namespace murasaki::scene
