#pragma once

// Sprite sheets (an animated .tex: a grid of frames and a TEXS section with each frame's duration and place). A shader
// built with SPRITESHEET reads `g_Texture0Rotation` and `g_Texture0Translation` and computes
//     uv = translation + a_TexCoord.x * rotation.xy + a_TexCoord.y * rotation.zw
// so the quad's own 0..1 coordinates run over the current frame. The values for a frame are its position and axes in
// sheet texels divided by the sheet size (an upright frame has axes (w, 0) and (0, h)). The animation loops.

#include "assets/tex.h"

#include <array>
#include <vector>

namespace murasaki::scene {

struct SpriteFrameUniforms {
    std::array<float, 4> rotation{1.0f, 0.0f, 0.0f, 1.0f};
    std::array<float, 2> translation{0.0f, 0.0f};
    size_t frame = 0;
    size_t page = 0; // the image of the sheet the frame lies on (TexFrame::index, 0 for the overloads without page sizes)
};

// The frame shown `seconds` into the animation (looping over the sum of the durations). With no durations, or no
// frames, the first frame / the whole sheet. A `forcedFrame` of 0 or more is that frame (the last when there are fewer).
SpriteFrameUniforms spriteFrame(const std::vector<TexFrame> &frames, uint32_t sheetWidth, uint32_t sheetHeight, double seconds, int forcedFrame = -1);

// The same for a sheet on several images: a frame's place is in the texels of its own image, so the axes and the position are
// divided by that image's size (`pageSizes[page]`, width and height; a frame naming a page past the last takes the last).
SpriteFrameUniforms spriteFrame(const std::vector<TexFrame> &frames, const std::vector<std::array<uint32_t, 2>> &pageSizes, double seconds, int forcedFrame = -1);

} // namespace murasaki::scene
