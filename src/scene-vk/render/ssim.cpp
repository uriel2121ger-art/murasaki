#include "render/ssim.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace murasaki::scene {

namespace {

constexpr uint32_t kWindow = 8;
constexpr uint32_t kStep = 4;
constexpr double kC1 = (0.01 * 255.0) * (0.01 * 255.0);
constexpr double kC2 = (0.03 * 255.0) * (0.03 * 255.0);

double channelSsim(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height, int channel)
{
    const uint32_t win = std::min({kWindow, width, height});
    double total = 0.0;
    size_t count = 0;
    for (uint32_t y = 0; y + win <= height; y += kStep) {
        for (uint32_t x = 0; x + win <= width; x += kStep) {
            double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0;
            for (uint32_t j = 0; j < win; ++j) {
                const size_t row = (static_cast<size_t>(y + j) * width + x) * 4 + channel;
                for (uint32_t i = 0; i < win; ++i) {
                    const double va = a[row + i * 4], vb = b[row + i * 4];
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                }
            }
            const double n = static_cast<double>(win) * win;
            const double ma = sa / n, mb = sb / n;
            const double va = saa / n - ma * ma, vb = sbb / n - mb * mb, cov = sab / n - ma * mb;
            total += ((2 * ma * mb + kC1) * (2 * cov + kC2)) / ((ma * ma + mb * mb + kC1) * (va + vb + kC2));
            ++count;
        }
    }
    return count == 0 ? 1.0 : total / static_cast<double>(count);
}

} // namespace

double ssimRgba(const uint8_t *a, const uint8_t *b, uint32_t width, uint32_t height)
{
    if (width == 0 || height == 0) {
        return 1.0;
    }
    double lowest = 1.0;
    for (int channel = 0; channel < 4; ++channel) {
        lowest = std::min(lowest, channelSsim(a, b, width, height, channel));
    }
    return lowest;
}

int maxChannelDifference(const uint8_t *a, const uint8_t *b, size_t bytes)
{
    int worst = 0;
    for (size_t i = 0; i < bytes; ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

std::vector<uint8_t> upsampleBilinear(const uint8_t *src, uint32_t sw, uint32_t sh, uint32_t dw, uint32_t dh)
{
    std::vector<uint8_t> out(static_cast<size_t>(dw) * dh * 4);
    if (sw == 0 || sh == 0 || dw == 0 || dh == 0) {
        return out;
    }
    for (uint32_t y = 0; y < dh; ++y) {
        const float fy = std::clamp((static_cast<float>(y) + 0.5f) * static_cast<float>(sh) / static_cast<float>(dh) - 0.5f, 0.0f, static_cast<float>(sh - 1));
        const uint32_t y0 = static_cast<uint32_t>(fy), y1 = std::min(y0 + 1, sh - 1);
        const float ty = fy - static_cast<float>(y0);
        for (uint32_t x = 0; x < dw; ++x) {
            const float fx = std::clamp((static_cast<float>(x) + 0.5f) * static_cast<float>(sw) / static_cast<float>(dw) - 0.5f, 0.0f, static_cast<float>(sw - 1));
            const uint32_t x0 = static_cast<uint32_t>(fx), x1 = std::min(x0 + 1, sw - 1);
            const float tx = fx - static_cast<float>(x0);
            for (int c = 0; c < 4; ++c) {
                const auto at = [&](uint32_t px, uint32_t py) { return static_cast<float>(src[(static_cast<size_t>(py) * sw + px) * 4 + c]); };
                const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx;
                const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx;
                out[(static_cast<size_t>(y) * dw + x) * 4 + c] = static_cast<uint8_t>(std::lround(top + (bottom - top) * ty));
            }
        }
    }
    return out;
}

} // namespace murasaki::scene
