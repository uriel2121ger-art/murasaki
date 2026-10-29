#include "render/shake.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace murasaki::scene {

namespace {

float lattice(int64_t i, uint32_t seed)
{
    uint32_t h = static_cast<uint32_t>(i) * 374761393u + seed * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h & 0xFFFFFFu) / 8388607.5f - 1.0f; // -1..1
}

float noise(double t, uint32_t seed)
{
    const double base = std::floor(t);
    const float f = static_cast<float>(t - base);
    const float s = f * f * (3.0f - 2.0f * f);
    const float a = lattice(static_cast<int64_t>(base), seed);
    const float b = lattice(static_cast<int64_t>(base) + 1, seed);
    return a + (b - a) * s;
}

float wander(double time, float roughness, float speed, uint32_t seed)
{
    const float r = std::clamp(roughness, 0.0f, 2.0f);
    const double t = time * static_cast<double>(std::max(speed, 0.0f));
    const float w1 = 1.0f, w2 = 0.5f * r, w3 = 0.25f * r;
    return (noise(t, seed) * w1 + noise(t * 2.1 + 17.0, seed + 1) * w2 + noise(t * 4.3 + 41.0, seed + 2) * w3) / (w1 + w2 + w3);
}

} // namespace

Vec2 cameraShake(double time, float amplitude, float roughness, float speed, Vec2 size)
{
    if (!(amplitude > 0.0f)) {
        return {0.0f, 0.0f};
    }
    const float scale = amplitude * 0.01f;
    return {wander(time, roughness, speed, 11u) * scale * size.x, wander(time, roughness, speed, 71u) * scale * size.y};
}

} // namespace murasaki::scene
