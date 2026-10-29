#pragma once

// Tiny vector and matrix types for the scene transforms. Matrices are column-major like GLSL, so a Mat4 can be
// pushed to a shader as is.

#include <array>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

namespace murasaki::scene {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

struct Vec4 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
};

struct Mat4 {
    std::array<float, 16> m{}; // m[column * 4 + row]

    static Mat4 identity()
    {
        Mat4 r;
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }
    float &at(int row, int col) { return m[static_cast<size_t>(col) * 4 + static_cast<size_t>(row)]; }
    float at(int row, int col) const { return m[static_cast<size_t>(col) * 4 + static_cast<size_t>(row)]; }
};

inline Mat4 operator*(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.at(row, k) * b.at(k, col);
            }
            r.at(row, col) = sum;
        }
    }
    return r;
}

inline Mat4 translation(Vec3 t)
{
    Mat4 r = Mat4::identity();
    r.at(0, 3) = t.x;
    r.at(1, 3) = t.y;
    r.at(2, 3) = t.z;
    return r;
}

inline Mat4 scaling(Vec3 s)
{
    Mat4 r = Mat4::identity();
    r.at(0, 0) = s.x;
    r.at(1, 1) = s.y;
    r.at(2, 2) = s.z;
    return r;
}

inline Mat4 rotationX(float a)
{
    Mat4 r = Mat4::identity();
    r.at(1, 1) = std::cos(a);
    r.at(1, 2) = -std::sin(a);
    r.at(2, 1) = std::sin(a);
    r.at(2, 2) = std::cos(a);
    return r;
}

inline Mat4 rotationY(float a)
{
    Mat4 r = Mat4::identity();
    r.at(0, 0) = std::cos(a);
    r.at(0, 2) = std::sin(a);
    r.at(2, 0) = -std::sin(a);
    r.at(2, 2) = std::cos(a);
    return r;
}

inline Mat4 rotationZ(float a)
{
    Mat4 r = Mat4::identity();
    r.at(0, 0) = std::cos(a);
    r.at(0, 1) = -std::sin(a);
    r.at(1, 0) = std::sin(a);
    r.at(1, 1) = std::cos(a);
    return r;
}

// Rotation from Euler angles in radians (x, y, z), applied in the order x, then y, then z.
inline Mat4 rotationEuler(Vec3 angles)
{
    return rotationZ(angles.z) * rotationY(angles.y) * rotationX(angles.x);
}

// Orthographic projection for Vulkan clip space (y down, depth 0..1) over [left,right] x [bottom,top]: scene
// y = bottom ends up at the last image row (clip y = +1), so a y-up scene comes out upright.
inline Mat4 orthographicVulkan(float left, float right, float bottom, float top, float zNear, float zFar)
{
    Mat4 r;
    r.at(0, 0) = 2.0f / (right - left);
    r.at(0, 3) = -(right + left) / (right - left);
    r.at(1, 1) = -2.0f / (top - bottom);
    r.at(1, 3) = (top + bottom) / (top - bottom);
    r.at(2, 2) = 1.0f / (zFar - zNear);
    r.at(2, 3) = -zNear / (zFar - zNear);
    r.at(3, 3) = 1.0f;
    return r;
}

// Orthographic projection in OpenGL clip space (y up, depth -1..1), the space Wallpaper Engine's shaders are written
// for: scene y = top lands at clip +1. The translated vertex shaders flip y and remap depth to Vulkan's clip space.
inline Mat4 orthographicGl(float left, float right, float bottom, float top, float zNear, float zFar)
{
    Mat4 r;
    r.at(0, 0) = 2.0f / (right - left);
    r.at(0, 3) = -(right + left) / (right - left);
    r.at(1, 1) = 2.0f / (top - bottom);
    r.at(1, 3) = -(top + bottom) / (top - bottom);
    r.at(2, 2) = -2.0f / (zFar - zNear);
    r.at(2, 3) = -(zFar + zNear) / (zFar - zNear);
    r.at(3, 3) = 1.0f;
    return r;
}

// The range a vertical field of view is kept in, in degrees (scene fov keys, camera objects, paths, scripts); a value that is not a
// number is the editor's default, 50.
constexpr float kMinFovDegrees = 1.0f;
constexpr float kMaxFovDegrees = 170.0f;
inline float clampFov(float degrees)
{
    return std::isfinite(degrees) ? (degrees < kMinFovDegrees ? kMinFovDegrees : degrees > kMaxFovDegrees ? kMaxFovDegrees : degrees) : 50.0f;
}

// Perspective projection in OpenGL clip space (vertical field of view in radians), looking down -z.
inline Mat4 perspectiveGl(float fovY, float aspect, float zNear, float zFar)
{
    const float f = 1.0f / std::tan(fovY / 2.0f);
    Mat4 r;
    r.at(0, 0) = f / aspect;
    r.at(1, 1) = f;
    r.at(2, 2) = -(zFar + zNear) / (zFar - zNear);
    r.at(2, 3) = -(2.0f * zFar * zNear) / (zFar - zNear);
    r.at(3, 2) = -1.0f;
    return r;
}

// General 4x4 inverse (cofactors). Returns the identity for a singular matrix.
inline Mat4 inverse(const Mat4 &mat)
{
    const auto &m = mat.m;
    std::array<float, 16> inv{};
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (std::fabs(det) < 1e-20f) {
        return Mat4::identity();
    }
    Mat4 out;
    for (size_t i = 0; i < 16; ++i) {
        out.m[i] = inv[i] / det;
    }
    return out;
}

// Reads up to `count` whitespace separated numbers ("0.5 1 -0.000") into `out`. Returns how many were read; the
// rest of `out` is left untouched.
inline int parseFloats(std::string_view text, float *out, int count)
{
    const std::string copy(text); // strtof needs a terminated string
    const char *p = copy.c_str();
    int n = 0;
    while (n < count) {
        char *end = nullptr;
        const float v = std::strtof(p, &end);
        if (end == p) {
            break;
        }
        out[n++] = v;
        p = end;
    }
    return n;
}

} // namespace murasaki::scene
