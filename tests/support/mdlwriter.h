#pragma once

// A general writer of MDL puppet models for the tests (the layout is docs/scene-vk/puppets.md's): any number of vertices skinned
// to any bones, bones in any order (a parent may come after its child), arbitrary bind matrices, animations of any length and mode
// with a pose per frame and bone, and the unused data the reader skips. Nothing here comes from a Workshop item. The older
// tests/puppetfixtures.h writes the small quad models of the render tests.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace mdlgen {

struct Pose {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> angles{0, 0, 0}; // radians, applied x, then y, then z
    std::array<float, 3> scale{1, 1, 1};
};

struct Bone {
    std::string name;
    int parent = -1;
    Pose bind; // the bind pose, local to the parent
};

struct Vertex {
    std::array<float, 3> position{0, 0, 0};
    std::array<uint32_t, 4> bones{0, 0, 0, 0};
    std::array<float, 4> weights{1, 0, 0, 0};
};

struct Animation {
    uint32_t id = 1;
    std::string name = "anim";
    std::string mode = "loop"; // loop, mirror, single
    float fps = 30.0f;
    uint32_t length = 0;                    // frames; a track holds length + 1 poses
    std::vector<std::vector<Pose>> tracks;  // [bone][frame]; an empty track: the bone has none
};

struct Model {
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;
    std::vector<Bone> bones;
    std::vector<Animation> animations;
    const char *version = "MDLV0023";
};

namespace detail {

struct Writer {
    std::vector<uint8_t> d;
    void raw(const void *p, size_t n) { d.insert(d.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); }
    void u32(uint32_t v) { raw(&v, 4); }
    void i32(int32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void u16(uint16_t v) { raw(&v, 2); }
    void str(const std::string &s) { raw(s.c_str(), s.size() + 1); }
};

// The row-vector bind matrix of a pose: scale, then rotation (x, y, z), then translation in the last row.
inline std::array<float, 16> rowMatrix(const Pose &p)
{
    const double cx = std::cos(p.angles[0]), sx = std::sin(p.angles[0]);
    const double cy = std::cos(p.angles[1]), sy = std::sin(p.angles[1]);
    const double cz = std::cos(p.angles[2]), sz = std::sin(p.angles[2]);
    // column-vector rotation R = Rz * Ry * Rx
    const double r[3][3] = {{cz * cy, cz * sy * sx - sz * cx, cz * sy * cx + sz * sx},
                            {sz * cy, sz * sy * sx + cz * cx, sz * sy * cx - cz * sx},
                            {-sy, cy * sx, cy * cx}};
    // column-vector local = T * R * S: element (row, col) = r[row][col] * scale[col]; the file stores its transpose
    std::array<float, 16> m{};
    for (int row = 0; row < 3; ++row)
        for (int col = 0; col < 3; ++col)
            m[static_cast<size_t>(col * 4 + row)] = static_cast<float>(r[row][col] * p.scale[static_cast<size_t>(col)]);
    m[12] = p.position[0];
    m[13] = p.position[1];
    m[14] = p.position[2];
    m[15] = 1.0f;
    return m;
}

} // namespace detail

// The skeleton (MDLS0004) and animation (MDLA0006) sections, appended to `d` (their end offsets are offsets in `d`): what follows a
// puppet's mesh block, and a skinned 3D mesh's parts.
inline void appendRig(std::vector<uint8_t> &d, const std::vector<Bone> &bones, const std::vector<Animation> &animations)
{
    detail::Writer w;
    w.d = std::move(d);
    // skeleton
    w.raw("MDLS0004", 8);
    w.d.push_back(0);
    const size_t endAt = w.d.size();
    w.u32(0); // end, patched
    w.u32(static_cast<uint32_t>(bones.size()));
    for (const Bone &b : bones) {
        w.str(b.name);
        w.u32(1);
        w.i32(b.parent);
        w.u32(64);
        const auto rows = detail::rowMatrix(b.bind);
        w.raw(rows.data(), 64);
        w.str("");
    }
    for (int i = 0; i < 24; ++i)
        w.d.push_back(0x77); // data the reader does not use
    const uint32_t end = static_cast<uint32_t>(w.d.size());
    std::memcpy(w.d.data() + endAt, &end, 4);
    // animations
    w.raw("MDLA0006", 8);
    w.d.push_back(0);
    w.u32(0);
    w.u32(static_cast<uint32_t>(animations.size()));
    for (const Animation &a : animations) {
        w.u32(a.id);
        w.u32(0);
        w.str(a.name);
        w.str(a.mode);
        w.f32(a.fps);
        w.u32(a.length);
        w.u32(0);
        w.u32(static_cast<uint32_t>(a.tracks.size()));
        for (const auto &track : a.tracks) {
            w.u32(0);
            w.u32(static_cast<uint32_t>(track.size() * 36));
            for (const Pose &p : track) {
                for (float f : p.position)
                    w.f32(f);
                for (float f : p.angles)
                    w.f32(f);
                for (float f : p.scale)
                    w.f32(f);
            }
        }
        for (int i = 0; i < 36; ++i)
            w.d.push_back(0);
    }
    d = std::move(w.d);
}

inline std::vector<uint8_t> write(const Model &model)
{
    detail::Writer w;
    w.raw(model.version, 8);
    w.d.push_back(0);
    w.u32(0x01800009);
    w.u32(1);
    w.u32(1);
    w.str("materials/test.json");
    for (int i = 0; i < 40; ++i)
        w.d.push_back(0);
    w.u32(0x0180000f);
    w.u32(static_cast<uint32_t>(model.vertices.size() * 80));
    for (const Vertex &v : model.vertices) {
        for (float f : v.position)
            w.f32(f);
        for (float f : {0.0f, 0.0f, 1.0f})
            w.f32(f); // normal
        for (float f : {1.0f, 0.0f, 0.0f, 1.0f})
            w.f32(f); // tangent
        for (uint32_t b : v.bones)
            w.u32(b);
        for (float f : v.weights)
            w.f32(f);
        w.f32(0.5f);
        w.f32(0.5f); // uv
    }
    w.u32(static_cast<uint32_t>(model.indices.size() * 2));
    for (uint16_t i : model.indices)
        w.u16(i);
    appendRig(w.d, model.bones, model.animations);
    return w.d;
}

} // namespace mdlgen
