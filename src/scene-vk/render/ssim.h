#pragma once

// Picture comparison for the pass optimiser (render/passprobe.h): a structural similarity score over RGBA8 pictures,
// the largest per-channel difference, and the bilinear enlargement a reduced pass goes through before it is shown.

#include <cstdint>
#include <vector>

namespace murasaki::scene {

// Mean SSIM (8x8 windows, step 4, the usual constants) of two w x h RGBA8 pictures, taken per channel; the result is the
// lowest of the four channels, so a change that only touches the alpha, or only blue, still shows. 1 for equal pictures.
// Pictures smaller than a window compare as one window.
double ssimRgba(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height);

// The largest absolute difference of any channel of any pixel.
int maxChannelDifference(const uint8_t *a, const uint8_t *b, size_t bytes);

// `src` (sw x sh RGBA8) stretched to dw x dh with the bilinear filter and the pixel-centre mapping of a GPU linear blit.
std::vector<uint8_t> upsampleBilinear(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh);

} // namespace murasaki::scene
