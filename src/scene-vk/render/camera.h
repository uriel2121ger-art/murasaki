#pragma once

// The scene camera's math: a look-at basis (eye, where it looks, which way is up), the view matrix it makes, the perspective
// projection of the part of the scene an output shows, and the queue that plays a camera object's paths. Pure arithmetic, no
// Vulkan state: tst_scenevk_camera checks it directly.
//
// Conventions (docs/scene-vk/formats.md, "Camera"): world y is up; a camera looks down its own -z with its own +y up, so a camera
// object with angles 0 looks down the world's -z, and its `angles` turn it the way they turn any object (rotationEuler). The
// field of view is vertical, in degrees. Clip space is OpenGL's (y up, depth -1..1): the translated shaders take it to Vulkan's.

#include "util/math.h"

#include <cstddef>
#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace murasaki::scene {

// Small vector helpers of the camera math (a namespace of their own, so they meet no other code's).
namespace camvec {
inline Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 mul(Vec3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(Vec3 a) { return std::sqrt(dot(a, a)); }
} // namespace camvec

// Where the camera is and which way its axes point (unit vectors, right-handed: right x up = -forward).
struct CameraBasis {
    Vec3 eye{0.0f, 0.0f, 0.0f};
    Vec3 forward{0.0f, 0.0f, -1.0f};
    Vec3 right{1.0f, 0.0f, 0.0f};
    Vec3 up{0.0f, 1.0f, 0.0f};
};

// The basis of a camera at `eye` looking at `center`, `up` being the world direction that comes out upwards. Any orientation:
// an `up` along the line of sight (or zero) is replaced by the world's y (or z when looking straight up or down); an eye on the
// centre looks down -z.
CameraBasis lookAtBasis(Vec3 eye, Vec3 center, Vec3 up);
// The basis of a camera object from its world matrix (scale taken out): it looks down its own -z, its +y up.
CameraBasis basisFromWorld(const Mat4 &world);
// The rotation that turns a camera object onto this basis (its columns right, up and -forward).
Mat4 rotationOf(const CameraBasis &basis);
// World to camera space (the camera at the origin looking down -z, y up), what g_ViewProjectionMatrix is made with.
Mat4 viewMatrix(const CameraBasis &basis);
// The Euler angles (rotationEuler's order: x, then y, then z) that turn a camera object onto this basis: -z onto `forward` and
// +y onto `up`. Used to move a camera object along its path, so the objects that hang on it follow.
Vec3 eulerFromBasis(const CameraBasis &basis);

// The perspective projection of a window of the canvas. The camera's picture is the canvas (`canvas`, the scene's nominal size
// in scene units, whose aspect the projection takes) under a vertical field of view of `fovYDegrees`; `windowLow` and
// `windowSize` are the part of it that is drawn (the scaling mode's crop, the zoom and the shake), in the same units with y up.
// The window covering the whole canvas is the plain projection.
Mat4 perspectiveWindow(float fovYDegrees, Vec2 canvas, Vec2 windowLow, Vec2 windowSize, float zNear, float zFar);

// The order a camera object's paths are taken in (`queuemode`): one after the other, each played from its start for its own
// length, then the next; "random" picks the next one at random (another one than the path that just ended when there are two
// or more; the generator's output modulo the choices, so a seed gives the same order everywhere), any other mode goes in file
// order and starts over. Paths that cannot play (hidden, or no length) are left out. The
// random order is seeded, so a given scene always plays its paths in the same order.
class CameraPathQueue
{
public:
    // `seconds[i]`: the length of path i, 0 for a path that cannot play.
    void reset(std::vector<double> seconds, bool random, uint32_t seed = 0x6d757261u);
    // The path playing at `time` (seconds of the scene clock, never going back) and the seconds since it started; -1 when no
    // path can play.
    std::pair<int, double> at(double time);

private:
    int next();
    std::vector<double> m_seconds;
    std::vector<size_t> m_playable;
    bool m_random = false;
    std::mt19937 m_rng;
    int m_index = -1;      // into m_seconds
    double m_start = 0.0;  // when it started
    double m_total = 0.0;  // all the playable paths, one after the other
};

} // namespace murasaki::scene
