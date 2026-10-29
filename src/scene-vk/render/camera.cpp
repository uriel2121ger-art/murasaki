#include "render/camera.h"

#include <algorithm>
#include <cmath>

namespace murasaki::scene {

using namespace camvec;

namespace {

bool finite(Vec3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

Vec3 normalizedOr(Vec3 v, Vec3 fallback)
{
    const float l = length(v);
    return l > 1e-6f && std::isfinite(l) ? mul(v, 1.0f / l) : fallback;
}

// The basis from a line of sight and an up: the up is made square to the line (or replaced when it runs along it).
CameraBasis basisFromAxes(Vec3 eye, Vec3 forward, Vec3 up)
{
    CameraBasis b;
    b.eye = finite(eye) ? eye : Vec3{0.0f, 0.0f, 0.0f};
    b.forward = finite(forward) ? normalizedOr(forward, {0.0f, 0.0f, -1.0f}) : Vec3{0.0f, 0.0f, -1.0f};
    Vec3 right = finite(up) ? cross(b.forward, up) : Vec3{};
    if (length(right) < 1e-6f * std::max(1.0f, finite(up) ? length(up) : 1.0f)) {
        // no usable up: the world's y, or its z when the camera looks along y
        const Vec3 fallback = std::fabs(b.forward.y) < 0.999f ? Vec3{0.0f, 1.0f, 0.0f} : Vec3{0.0f, 0.0f, b.forward.y > 0.0f ? 1.0f : -1.0f};
        right = cross(b.forward, fallback);
    }
    b.right = normalizedOr(right, {1.0f, 0.0f, 0.0f});
    b.up = normalizedOr(cross(b.right, b.forward), {0.0f, 1.0f, 0.0f});
    return b;
}

} // namespace

CameraBasis lookAtBasis(Vec3 eye, Vec3 center, Vec3 up)
{
    const Vec3 e = finite(eye) ? eye : Vec3{0.0f, 0.0f, 0.0f};
    return basisFromAxes(e, finite(center) ? sub(center, e) : Vec3{0.0f, 0.0f, -1.0f}, up);
}

CameraBasis basisFromWorld(const Mat4 &world)
{
    // the axes straight from the matrix (not through a point ahead of the eye, which loses digits far from the origin). A negative
    // scale is not undone: from the matrix alone a mirror along x cannot be told from one along z turned half round, so the columns
    // are taken as they are: the camera looks down its own -z column (a negative z scale turns it round, one on x or y does not) and
    // the picture is never mirrored (formats.md, "Camera")
    const Vec3 y{world.at(0, 1), world.at(1, 1), world.at(2, 1)};
    const Vec3 z{world.at(0, 2), world.at(1, 2), world.at(2, 2)};
    const Vec3 forward = mul(z, -1.0f);
    return basisFromAxes({world.at(0, 3), world.at(1, 3), world.at(2, 3)}, forward, y);
}

Mat4 rotationOf(const CameraBasis &b)
{
    Mat4 r = Mat4::identity();
    const Vec3 columns[3] = {b.right, b.up, mul(b.forward, -1.0f)};
    for (int c = 0; c < 3; ++c) {
        r.at(0, c) = columns[c].x;
        r.at(1, c) = columns[c].y;
        r.at(2, c) = columns[c].z;
    }
    return r;
}

Mat4 viewMatrix(const CameraBasis &b)
{
    Mat4 v = Mat4::identity();
    const Vec3 rows[3] = {b.right, b.up, mul(b.forward, -1.0f)};
    for (int r = 0; r < 3; ++r) {
        v.at(r, 0) = rows[r].x;
        v.at(r, 1) = rows[r].y;
        v.at(r, 2) = rows[r].z;
        v.at(r, 3) = -dot(rows[r], b.eye);
    }
    return v;
}

Vec3 eulerFromBasis(const CameraBasis &b)
{
    // the rotation's columns are the camera's x, y and z axes: right, up and back. With R = Rz(c) Ry(b) Rx(a):
    // R20 = -sin b, R21 = cos b sin a, R22 = cos b cos a, R10 = cos b sin c, R00 = cos b cos c
    const Vec3 back = mul(b.forward, -1.0f);
    const float r00 = b.right.x, r10 = b.right.y, r20 = b.right.z;
    const float r11 = b.up.y, r21 = b.up.z;
    const float r12 = back.y, r22 = back.z;
    const float y = std::asin(std::clamp(-r20, -1.0f, 1.0f));
    if (std::fabs(std::cos(y)) > 1e-5f) {
        return {std::atan2(r21, r22), y, std::atan2(r10, r00)};
    }
    // looking straight along the world's x: only a - c counts, so c is 0 and R = Ry(b) Rx(a)
    return {std::atan2(-r12, r11), y, 0.0f};
}

Mat4 perspectiveWindow(float fovYDegrees, Vec2 canvas, Vec2 windowLow, Vec2 windowSize, float zNear, float zFar)
{
    const float fov = clampFov(fovYDegrees) * 3.14159265358979f / 180.0f;
    const float aspect = canvas.y > 0.0f && canvas.x > 0.0f ? canvas.x / canvas.y : 16.0f / 9.0f;
    const float n = std::isfinite(zNear) && zNear > 0.0f ? zNear : 0.01f;
    const float f = std::isfinite(zFar) && zFar > n ? zFar : n + 1.0f;
    const Mat4 projection = perspectiveGl(fov, aspect, n, f);
    if (!(windowSize.x > 0.0f && windowSize.y > 0.0f && canvas.x > 0.0f && canvas.y > 0.0f)) {
        return projection;
    }
    // the canvas runs from -1 to 1 in normalized device coordinates; the window's own -1..1 is a scale and a shift of that
    // (applied to clip coordinates: x' = sx x + tx w)
    const float sx = canvas.x / windowSize.x;
    const float sy = canvas.y / windowSize.y;
    Mat4 crop = Mat4::identity();
    crop.at(0, 0) = sx;
    crop.at(0, 3) = sx - 2.0f * windowLow.x / windowSize.x - 1.0f;
    crop.at(1, 1) = sy;
    crop.at(1, 3) = sy - 2.0f * windowLow.y / windowSize.y - 1.0f;
    return crop * projection;
}

void CameraPathQueue::reset(std::vector<double> seconds, bool random, uint32_t seed)
{
    m_seconds = std::move(seconds);
    m_playable.clear();
    for (size_t i = 0; i < m_seconds.size(); ++i) {
        if (std::isfinite(m_seconds[i]) && m_seconds[i] > 0.0) {
            m_playable.push_back(i);
        }
    }
    m_total = 0.0;
    for (size_t i : m_playable) {
        m_total += m_seconds[i];
    }
    m_random = random;
    m_rng.seed(seed);
    m_index = -1;
    m_start = 0.0;
}

int CameraPathQueue::next()
{
    if (m_playable.empty()) {
        return -1;
    }
    if (m_random) {
        // another one than the path that just ended, the n-th of the others; `% n` of the generator's raw output, so a seed gives the
        // same order with any standard library
        const bool skipCurrent = m_index >= 0 && m_playable.size() > 1
            && std::find(m_playable.begin(), m_playable.end(), static_cast<size_t>(m_index)) != m_playable.end();
        size_t k = static_cast<size_t>(m_rng() % (m_playable.size() - (skipCurrent ? 1 : 0)));
        for (size_t i : m_playable) {
            if (skipCurrent && static_cast<int>(i) == m_index) {
                continue;
            }
            if (k-- == 0) {
                return static_cast<int>(i);
            }
        }
        return static_cast<int>(m_playable.front());
    }
    if (m_index >= 0) {
        for (size_t i : m_playable) {
            if (static_cast<int>(i) > m_index) {
                return static_cast<int>(i);
            }
        }
    }
    return static_cast<int>(m_playable.front()); // the first, or again from the start
}

std::pair<int, double> CameraPathQueue::at(double time)
{
    if (m_playable.empty() || !std::isfinite(time)) {
        return {-1, 0.0};
    }
    if (m_index < 0) {
        m_index = next();
        m_start = time;
    }
    if (time < m_start) {
        m_start = time; // the clock went back (a restart): the path starts over
    }
    if (!m_random && time - m_start >= m_total) {
        // in order the queue comes back to the same path after all of them: whole rounds are skipped at once
        m_start += std::floor((time - m_start) / m_total) * m_total;
    }
    // what is left ends a few paths at most in order; at random a long pause takes one step a path (bounded)
    for (int guard = 0; guard < 10000 && time - m_start >= m_seconds[static_cast<size_t>(m_index)]; ++guard) {
        m_start += m_seconds[static_cast<size_t>(m_index)];
        m_index = next();
    }
    if (time - m_start >= m_seconds[static_cast<size_t>(m_index)]) {
        m_start = time;
    }
    return {m_index, time - m_start};
}

} // namespace murasaki::scene
