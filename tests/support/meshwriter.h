#pragma once

// A writer of 3D mesh models (`"model": "x.mdl"` objects) for the tests, laid out as docs/scene-vk/formats.md (meshes) describes: a
// header, then per part the material path, the index width, the bounds (version 19 on), the vertex format (16 on), the vertices
// (48 bytes, 32 without tangents, 80 skinned) and the indices, parts separated by zero bytes, zero bytes at the end, then for a
// skinned mesh the puppet's skeleton and animations (tests/support/mdlwriter.h). Nothing here comes from a Workshop item.

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "mdlwriter.h"

namespace meshgen {

struct Vertex {
    std::array<float, 3> position{0, 0, 0};
    std::array<float, 3> normal{0, 0, 1};
    std::array<float, 4> tangent{1, 0, 0, 1};
    std::array<float, 2> uv{0, 0};
    std::array<uint32_t, 4> bones{0, 0, 0, 0};  // a skinned mesh's
    std::array<float, 4> weights{1, 0, 0, 0};
};

struct Part {
    std::string material = "materials/models/test/part.json";
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    bool wideIndices = false; // u32 indices (the flag after the material is 1)
};

struct Model {
    std::vector<Part> parts;
    const char *version = "MDLV0023";
    uint32_t flags = 0x0f;          // 0x0b: no tangents (32 byte vertices); set to 0x0180000f by `skinned`
    bool skinned = false;           // the puppet vertex and, after the parts, `bones` and `animations`
    std::vector<mdlgen::Bone> bones;
    std::vector<mdlgen::Animation> animations;
    std::vector<uint8_t> trailer;   // what follows the last part (a section such as "MDMP0001")
};

inline std::vector<uint8_t> write(const Model &model)
{
    std::vector<uint8_t> d;
    const auto raw = [&](const void *p, size_t n) { d.insert(d.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); };
    const auto u32 = [&](uint32_t v) { raw(&v, 4); };
    const auto f32 = [&](float v) { raw(&v, 4); };
    const int version = std::atoi(model.version + 4);
    const uint32_t flags = model.skinned ? 0x0180000fu : model.flags;
    raw(model.version, 8);
    d.push_back(0);
    u32(flags);
    u32(1);
    u32(static_cast<uint32_t>(model.parts.size()));
    for (size_t p = 0; p < model.parts.size(); ++p) {
        const Part &part = model.parts[p];
        if (p > 0) {
            d.insert(d.end(), version == 21 ? 2 : version == 23 ? 6 : 0, 0); // the zero bytes between two parts
        }
        raw(part.material.c_str(), part.material.size() + 1);
        u32(part.wideIndices ? 1 : 0);
        if (version >= 19) {
            std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const Vertex &v : part.vertices) {
                for (size_t k = 0; k < 3; ++k) {
                    lo[k] = std::min(lo[k], v.position[k]);
                    hi[k] = std::max(hi[k], v.position[k]);
                }
            }
            for (float f : lo)
                f32(f);
            for (float f : hi)
                f32(f);
        }
        if (version >= 16) {
            u32(flags);
        }
        const size_t stride = model.skinned ? 80 : (flags & 4u) ? 48 : 32;
        u32(static_cast<uint32_t>(part.vertices.size() * stride));
        for (const Vertex &v : part.vertices) {
            for (float f : v.position)
                f32(f);
            for (float f : v.normal)
                f32(f);
            if (flags & 4u) {
                for (float f : v.tangent)
                    f32(f);
            }
            if (model.skinned) {
                for (uint32_t b : v.bones)
                    u32(b);
                for (float f : v.weights)
                    f32(f);
            }
            for (float f : v.uv)
                f32(f);
        }
        u32(static_cast<uint32_t>(part.indices.size() * (part.wideIndices ? 4 : 2)));
        for (uint32_t i : part.indices) {
            if (part.wideIndices) {
                u32(i);
            } else {
                const uint16_t s = static_cast<uint16_t>(i);
                raw(&s, 2);
            }
        }
    }
    if (model.skinned) {
        d.insert(d.end(), version == 23 ? 6 : 0, 0);
        mdlgen::appendRig(d, model.bones, model.animations);
    } else {
        d.insert(d.end(), version == 21 ? 3 : version == 23 ? 7 : version == 16 ? 1 : 0, 0);
    }
    d.insert(d.end(), model.trailer.begin(), model.trailer.end());
    return d;
}

// An axis-aligned box from `lo` to `hi`, its faces wound counter-clockwise seen from outside (as the library's models are), with
// outward normals; every vertex of the face whose normal is +z has uv (0.75, 0.5), every other one (0.25, 0.5).
inline Part box(std::array<float, 3> lo, std::array<float, 3> hi, const std::string &material = "materials/models/test/part.json")
{
    Part part;
    part.material = material;
    // per face: the normal axis and sign; the four corners counter-clockwise seen from that side
    struct Face {
        int axis;
        float sign;
    };
    const Face faces[6] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {2, 1}, {2, -1}};
    for (const Face &f : faces) {
        const int u = (f.axis + 1) % 3, v = (f.axis + 2) % 3;
        // (u, v) runs counter-clockwise around +axis; seen from -axis that is clockwise, so the order flips
        const float corners[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        const size_t base = part.vertices.size();
        for (int c = 0; c < 4; ++c) {
            const int k = f.sign > 0 ? c : 3 - c;
            Vertex vx;
            vx.position[static_cast<size_t>(f.axis)] = f.sign > 0 ? hi[static_cast<size_t>(f.axis)] : lo[static_cast<size_t>(f.axis)];
            vx.position[static_cast<size_t>(u)] = corners[k][0] > 0 ? hi[static_cast<size_t>(u)] : lo[static_cast<size_t>(u)];
            vx.position[static_cast<size_t>(v)] = corners[k][1] > 0 ? hi[static_cast<size_t>(v)] : lo[static_cast<size_t>(v)];
            vx.normal = {0, 0, 0};
            vx.normal[static_cast<size_t>(f.axis)] = f.sign;
            vx.uv = {f.axis == 2 && f.sign > 0 ? 0.75f : 0.25f, 0.5f};
            part.vertices.push_back(vx);
        }
        for (uint32_t i : {0u, 1u, 2u, 0u, 2u, 3u}) {
            part.indices.push_back(static_cast<uint32_t>(base) + i);
        }
    }
    return part;
}

} // namespace meshgen
