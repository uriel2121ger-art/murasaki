#include "render/shadows.h"

#include "render/camera.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace murasaki::scene {

namespace {

constexpr float kPi = 3.14159265358979f;
// what WE's shipped helper takes as a third of the block's height (CalculateProjectedCoordsPoint's viewport scale)
constexpr float kPointRowScale = 0.3333f;

using camvec::add;
using camvec::dot;
using camvec::mul;
using camvec::sub;

Vec3 transformPoint(const Mat4 &m, Vec3 p)
{
    const float w = m.at(3, 0) * p.x + m.at(3, 1) * p.y + m.at(3, 2) * p.z + m.at(3, 3);
    const float s = std::fabs(w) > 1e-30f ? 1.0f / w : 1.0f;
    return {(m.at(0, 0) * p.x + m.at(0, 1) * p.y + m.at(0, 2) * p.z + m.at(0, 3)) * s,
            (m.at(1, 0) * p.x + m.at(1, 1) * p.y + m.at(1, 2) * p.z + m.at(1, 3)) * s,
            (m.at(2, 0) * p.x + m.at(2, 1) * p.y + m.at(2, 2) * p.z + m.at(2, 3)) * s};
}

std::array<Vec3, 8> cornersOf(const ShadowBounds &b)
{
    std::array<Vec3, 8> c;
    for (size_t i = 0; i < 8; ++i) {
        c[i] = {(i & 1) ? b.max.x : b.min.x, (i & 2) ? b.max.y : b.min.y, (i & 4) ? b.max.z : b.min.z};
    }
    return c;
}

// The nearest and farthest distances of the casters' box from a point (0 for the nearest when the point is inside).
void distancesTo(const ShadowBounds &b, Vec3 p, float &nearest, float &farthest)
{
    const Vec3 q{std::clamp(p.x, b.min.x, b.max.x), std::clamp(p.y, b.min.y, b.max.y), std::clamp(p.z, b.min.z, b.max.z)};
    nearest = camvec::length(sub(q, p));
    farthest = 0.0f;
    for (const Vec3 &c : cornersOf(b)) {
        farthest = std::max(farthest, camvec::length(sub(c, p)));
    }
}

// A value rounded up (or down) to an eighth of the power of two below it: a range that drifts keeps its ends for a while.
float roundToStep(float v, bool up)
{
    if (!(v > 0.0f) || !std::isfinite(v)) {
        return v;
    }
    const float step = std::exp2(std::floor(std::log2(v)) - 3.0f);
    return (up ? std::ceil(v / step) : std::floor(v / step)) * step;
}

// The depth range a light at `origin` that reaches `radius` needs for the casters: as far as the farthest caster (or the radius when
// that is nearer), from half the nearest one's distance (never nearer than a ten-thousandth of the far end, for the depth precision).
void depthRange(const ShadowBounds &casters, Vec3 origin, float radius, float &zNear, float &zFar)
{
    float nearest = 0.0f, farthest = 0.0f;
    distancesTo(casters, origin, nearest, farthest);
    zFar = std::max(std::min(radius, roundToStep(farthest * 1.01f, true)), 1e-3f);
    zNear = std::clamp(roundToStep(nearest * 0.5f, false), zFar * 1e-4f, zFar * 0.5f);
}

Mat4 rowsMatrix(Vec3 r, Vec3 u, Vec3 b, Vec3 origin)
{
    Mat4 m = Mat4::identity();
    const Vec3 rows[3] = {r, u, b};
    for (int i = 0; i < 3; ++i) {
        m.at(i, 0) = rows[i].x;
        m.at(i, 1) = rows[i].y;
        m.at(i, 2) = rows[i].z;
        m.at(i, 3) = -dot(rows[i], origin);
    }
    return m;
}

using Block = ShadowBlock;

// Four cells wide, as many rows as needed: each block goes to the first place it fits, scanning the rows from the top and each row
// from the left.
class Grid
{
public:
    explicit Grid(uint32_t cell) : m_cell(cell) {}
    Block take(uint32_t wide, uint32_t high)
    {
        for (uint32_t row = 0;; ++row) {
            for (uint32_t col = 0; col + wide <= 4; ++col) {
                if (fits(col, row, wide, high)) {
                    for (uint32_t r = row; r < row + high; ++r) {
                        if (m_used.size() <= r) {
                            m_used.resize(r + 1, {});
                        }
                        for (uint32_t c = col; c < col + wide; ++c) {
                            m_used[r][c] = true;
                        }
                    }
                    return {static_cast<float>(col * m_cell), static_cast<float>(row * m_cell), static_cast<float>(wide * m_cell),
                            static_cast<float>(high * m_cell)};
                }
            }
        }
    }
    uint32_t height() const { return std::max<uint32_t>(1, static_cast<uint32_t>(m_used.size())) * m_cell; }

private:
    bool fits(uint32_t col, uint32_t row, uint32_t wide, uint32_t high) const
    {
        for (uint32_t r = row; r < row + high; ++r) {
            for (uint32_t c = col; c < col + wide; ++c) {
                if (r < m_used.size() && m_used[r][c]) {
                    return false;
                }
            }
        }
        return true;
    }
    uint32_t m_cell;
    std::vector<std::array<bool, 4>> m_used;
};

} // namespace

float shadowPointCompensation(int quality)
{
    return quality <= 2 ? 0.47f : quality == 3 ? 0.48f : 0.49f;
}

void ShadowBounds::add(Vec3 p)
{
    if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) {
        return;
    }
    if (empty) {
        min = max = p;
        empty = false;
        return;
    }
    min = {std::min(min.x, p.x), std::min(min.y, p.y), std::min(min.z, p.z)};
    max = {std::max(max.x, p.x), std::max(max.y, p.y), std::max(max.z, p.z)};
}

Mat4 depthGlToZeroOne()
{
    Mat4 out = Mat4::identity();
    out.at(2, 2) = 0.5f;
    out.at(2, 3) = 0.5f; // z' = (z + w) / 2
    return out;
}

ShadowPlan planShadows(const Scene &scene, const PropertyStore &props, const LightUniforms &lights, const ShadowCamera &camera,
                       const ShadowBounds &casters, const ShadowSettings &settings)
{
    ShadowPlan plan;
    const LightCounts &counts = lights.counts;
    if (counts.pointShadow + counts.spotShadow + counts.directionalShadow == 0) {
        return plan;
    }
    Grid grid(settings.cell);
    std::vector<Block> blocks; // in plan.lights order; a directional light takes three
    const float s = 2.0f * shadowPointCompensation(settings.quality);
    const auto lit = [](const Vec4 &color) { return color.x != 0.0f || color.y != 0.0f || color.z != 0.0f; };

    // point lights: the six faces of the shipped helper (the axes each face's view takes, +x -x +y -y +z -z)
    static const std::array<std::array<Vec3, 3>, 6> kFaces{{
        {{{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}},
        {{{0, 0, -1}, {0, 1, 0}, {1, 0, 0}}},
        {{{1, 0, 0}, {0, 0, 1}, {0, -1, 0}}},
        {{{1, 0, 0}, {0, 0, -1}, {0, 1, 0}}},
        {{{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}}},
        {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}},
    }};
    for (int i = 0; i < counts.pointShadow; ++i) {
        const Block block = grid.take(2, 3);
        blocks.push_back(block);
        const Vec3 origin{lights.pointOrigin[i].x, lights.pointOrigin[i].y, lights.pointOrigin[i].z};
        float zNear = 0.0f, zFar = 0.0f;
        depthRange(casters, origin, lights.pointColor[i].w, zNear, zFar);
        const float px = -zFar / (zFar - zNear);
        const float py = -zNear * zFar / (zFar - zNear);
        plan.uniforms.pointProjection[i] = {px, py, -1.0f, 0.0f};
        ShadowLightViews views;
        views.block = block;
        if (lit(lights.pointColor[i]) && !casters.empty) {
            // the caster's clip space: x and y widened by the compensation, depth px * z + py over w = -z, then taken to OpenGL's range
            Mat4 project;
            project.at(0, 0) = s;
            project.at(1, 1) = s;
            project.at(2, 2) = 2.0f * px + 1.0f;
            project.at(2, 3) = 2.0f * py;
            project.at(3, 2) = -1.0f;
            for (size_t f = 0; f < kFaces.size(); ++f) {
                ShadowView v;
                v.viewProjection = project * rowsMatrix(kFaces[f][0], kFaces[f][1], kFaces[f][2], origin);
                v.width = block.width * 0.5f;
                v.height = block.height * kPointRowScale;
                v.x = block.x + static_cast<float>(f & 1) * v.width;
                v.y = block.y + static_cast<float>(f >> 1) * v.height;
                views.views.push_back(v);
            }
        }
        plan.lights.push_back(std::move(views));
    }

    // spots: a perspective picture along the cone, as wide as the outer cone (at most 170 degrees)
    for (int i = 0; i < counts.spotShadow; ++i) {
        const Block block = grid.take(2, 2);
        blocks.push_back(block);
        const Vec3 origin{lights.spotOrigin[i].x, lights.spotOrigin[i].y, lights.spotOrigin[i].z};
        const Vec3 forward{lights.spotDirection[i].x, lights.spotDirection[i].y, lights.spotDirection[i].z};
        const float outer = std::acos(std::clamp(lights.spotDirection[i].w, -1.0f, 1.0f));
        const float fov = std::clamp(2.0f * outer * 1.02f, kPi / 180.0f, 170.0f * kPi / 180.0f);
        float zNear = 0.0f, zFar = 0.0f;
        depthRange(casters, origin, lights.spotColor[i].w, zNear, zFar);
        const Mat4 viewProjection = perspectiveGl(fov, 1.0f, zNear, zFar) * viewMatrix(lookAtBasis(origin, add(origin, forward), {0.0f, 1.0f, 0.0f}));
        plan.uniforms.spotProjection[i] = depthGlToZeroOne() * viewProjection;
        ShadowLightViews views;
        if (lit(lights.spotColor[i]) && !casters.empty) {
            views.views.push_back({viewProjection, block.x, block.y, block.width, block.height});
        }
        plan.lights.push_back(std::move(views));
    }

    // directional lights: three cascades along the camera's view
    const Mat4 cameraInverse = inverse(camera.viewProjection);
    std::array<Vec3, 4> nearCorners, farCorners;
    for (size_t c = 0; c < 4; ++c) {
        const float x = (c & 1) ? 1.0f : -1.0f;
        const float y = (c & 2) ? 1.0f : -1.0f;
        nearCorners[c] = transformPoint(cameraInverse, {x, y, -1.0f});
        farCorners[c] = transformPoint(cameraInverse, {x, y, 1.0f});
    }
    const float clipNear = camera.clipNear >= 0.0f ? camera.clipNear : camera.nearDistance;
    const float clipFar = camera.clipFar >= 0.0f ? camera.clipFar : camera.farDistance;
    const float depthSpan = std::max(clipFar - clipNear, 1e-6f);
    std::vector<int> directionalSlots(static_cast<size_t>(counts.directionalShadow), -1);
    {
        const std::vector<LightSlot> slots = lightSlots(scene);
        for (size_t l = 0; l < scene.lights.size(); ++l) {
            if (scene.lights[l].type == LightType::Directional && slots[l].shadow && slots[l].slot >= 0 && slots[l].slot < counts.directionalShadow) {
                directionalSlots[static_cast<size_t>(slots[l].slot)] = static_cast<int>(l);
            }
        }
    }
    for (int i = 0; i < counts.directionalShadow; ++i) {
        std::array<float, 3> distances = kDefaultCascadeDistances;
        if (const int l = directionalSlots[static_cast<size_t>(i)]; l >= 0) {
            const SceneLight &light = scene.lights[static_cast<size_t>(l)];
            std::array<float, 3> set{};
            bool any = false;
            for (size_t k = 0; k < 3; ++k) {
                const float d = resolve(light.cascadeDistances[k], props);
                set[k] = std::isfinite(d) && d > 0.0f ? d : 0.0f;
                any = any || set[k] > 0.0f;
            }
            if (any) {
                distances = set;
            }
        }
        std::sort(distances.begin(), distances.end());
        const Vec3 toLight{lights.directionalDirection[i].x, lights.directionalDirection[i].y, lights.directionalDirection[i].z};
        const float toLightLength = camvec::length(toLight);
        const Vec3 shine = toLightLength > 1e-12f ? mul(toLight, -1.0f / toLightLength) : Vec3{0.0f, 0.0f, -1.0f};
        const Mat4 lightView = viewMatrix(lookAtBasis({0.0f, 0.0f, 0.0f}, shine, {0.0f, 1.0f, 0.0f}));
        float casterHigh = -std::numeric_limits<float>::max();
        if (!casters.empty) {
            for (const Vec3 &c : cornersOf(casters)) {
                casterHigh = std::max(casterHigh, transformPoint(lightView, c).z);
            }
        }
        ShadowLightViews views;
        float from = camera.nearDistance;
        for (int k = 0; k < kShadowCascades; ++k) {
            const uint32_t cells = k + 1 < kShadowCascades ? 2 : 1;
            const Block block = grid.take(cells, cells);
            blocks.push_back(block);
            const float to = std::max(distances[static_cast<size_t>(k)], from);
            const float ta = std::clamp((from - clipNear) / depthSpan, 0.0f, 1.0f);
            const float tb = std::clamp((to - clipNear) / depthSpan, 0.0f, 1.0f);
            from = to;
            std::array<Vec3, 8> slice;
            for (size_t c = 0; c < 4; ++c) {
                const Vec3 along = sub(farCorners[c], nearCorners[c]);
                slice[c] = transformPoint(lightView, add(nearCorners[c], mul(along, ta)));
                slice[c + 4] = transformPoint(lightView, add(nearCorners[c], mul(along, tb)));
            }
            Vec3 centre{0.0f, 0.0f, 0.0f};
            for (const Vec3 &p : slice) {
                centre = add(centre, mul(p, 1.0f / 8.0f));
            }
            float radius = 1e-3f;
            float low = std::numeric_limits<float>::max(), high = -std::numeric_limits<float>::max();
            for (const Vec3 &p : slice) {
                radius = std::max(radius, camvec::length(sub(p, centre)));
                low = std::min(low, p.z);
                high = std::max(high, p.z);
            }
            // the box's centre on the atlas's pixels: a camera that moves keeps the same pixels under the same places
            const float texel = 2.0f * radius / block.width;
            centre.x = std::floor(centre.x / texel) * texel;
            centre.y = std::floor(centre.y / texel) * texel;
            high = std::max({high, casterHigh, centre.z + radius});
            low = std::min(low, centre.z - radius);
            // the depth range in steps of the radius: a camera or a caster that drifts keeps the depth mapping (and the picture) as it is
            high = std::ceil(high / radius + 0.01f) * radius;
            low = std::floor(low / radius - 0.01f) * radius;
            plan.uniforms.cascadeTexel[static_cast<size_t>(i * kShadowCascades + k)] = {texel, 0.0f, 0.0f, 0.0f};
            const Mat4 viewProjection = orthographicGl(centre.x - radius, centre.x + radius, centre.y - radius, centre.y + radius, -high, -low) * lightView;
            plan.uniforms.cascadeProjection[static_cast<size_t>(i * kShadowCascades + k)] = depthGlToZeroOne() * viewProjection;
            if (lit(lights.directionalColor[i]) && !casters.empty) {
                views.views.push_back({viewProjection, block.x, block.y, block.width, block.height});
            }
        }
        plan.lights.push_back(std::move(views));
    }

    plan.atlasWidth = 4 * settings.cell;
    plan.atlasHeight = grid.height();
    const float aw = static_cast<float>(plan.atlasWidth), ah = static_cast<float>(plan.atlasHeight);
    const auto uv = [&](const Block &b) { return Vec4{b.x / aw, b.y / ah, b.width / aw, b.height / ah}; };
    size_t b = 0;
    for (int i = 0; i < counts.pointShadow; ++i) {
        plan.uniforms.pointTransform[static_cast<size_t>(i)] = uv(blocks[b++]);
    }
    for (int i = 0; i < counts.spotShadow; ++i) {
        plan.uniforms.spotTransform[static_cast<size_t>(i)] = uv(blocks[b++]);
    }
    for (int i = 0; i < counts.directionalShadow * kShadowCascades; ++i) {
        plan.uniforms.cascadeTransform[static_cast<size_t>(i)] = uv(blocks[b++]);
    }
    return plan;
}

} // namespace murasaki::scene
