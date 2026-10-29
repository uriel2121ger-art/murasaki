#include "colorkey.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace murasaki::import {

namespace {

const std::array<float, 256> &linearTable()
{
    static const std::array<float, 256> table = [] {
        std::array<float, 256> t{};
        for (int i = 0; i < 256; ++i) {
            const double c = i / 255.0;
            t[size_t(i)] = float(c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4));
        }
        return t;
    }();
    return table;
}

} // namespace

void applyColorKey(uchar *rgba, int width, int height, qsizetype stride, const ColorKey &key)
{
    if (!key.enabled || width <= 0 || height <= 0)
        return;
    const auto &lin = linearTable();
    const float kr = lin[key.red], kg = lin[key.green], kb = lin[key.blue];
    const double tol = std::clamp(key.tolerance, 0.0, 1.0);
    const double soft = std::clamp(key.softness, 0.0, 1.0);
    const double maxDistance = std::sqrt(3.0);
    for (int y = 0; y < height; ++y) {
        uchar *p = rgba + y * stride;
        for (int x = 0; x < width; ++x, p += 4) {
            const double dr = lin[p[0]] - kr, dg = lin[p[1]] - kg, db = lin[p[2]] - kb;
            const double d = std::sqrt(dr * dr + dg * dg + db * db) / maxDistance;
            if (d <= tol) {
                p[3] = 0;
                continue;
            }
            if (soft <= 0 || d >= tol + soft)
                continue;
            const double f = (d - tol) / soft; // 0..1, the share of the pixel that is foreground
            p[3] = uchar(std::lround(p[3] * f));
            // spill: the pixel is f of the foreground colour and 1 - f of the key colour; take the key out
            const int channels[3] = {key.red, key.green, key.blue};
            for (int c = 0; c < 3; ++c)
                p[c] = uchar(std::clamp(std::lround((p[c] - (1.0 - f) * channels[c]) / f), 0L, 255L));
        }
    }
}

} // namespace murasaki::import
