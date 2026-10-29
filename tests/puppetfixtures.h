#pragma once

// MDL puppet models written by the tests (laid out as docs/scene-vk/puppets.md describes): a quad of four vertices, a
// skeleton and animations. Nothing here comes from a Workshop item.

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace puppetfix {

struct Writer {
    std::vector<uint8_t> d;
    void u32(uint32_t v) { raw(&v, 4); }
    void f32(float v) { raw(&v, 4); }
    void u16(uint16_t v) { raw(&v, 2); }
    void raw(const void *p, size_t n) { d.insert(d.end(), static_cast<const uint8_t *>(p), static_cast<const uint8_t *>(p) + n); }
    void str(const std::string &s) { raw(s.c_str(), s.size() + 1); }
};

struct BoneSpec {
    std::string name;
    int parent;
    float tx, ty; // local bind translation
    std::string note = ""; // the string after the matrix (the editor's constraint JSON in real files)
};

// An attachment point (MDAT0001): a name, the bone it sits on and its local translation and angle about z
struct AttachSpec {
    std::string name;
    uint16_t bone;
    float tx, ty;
    float angleZ = 0.0f;
};

struct TrackSpec {
    std::vector<std::array<float, 9>> frames;
};

struct AnimSpec {
    uint32_t id;
    std::string name, mode;
    float fps;
    uint32_t length;
    std::vector<TrackSpec> tracks;
    int gap = 36;         // zero bytes after the record
    int extraTracks = 0;  // track-shaped records beyond the declared ones
};

// A model with a quad of four vertices skinned to bone 0 (or split between bones 0 and 1 by `halfAndHalf`).
std::vector<uint8_t> makeMdl(const std::vector<BoneSpec> &bones, const std::vector<AnimSpec> &anims, bool halfAndHalf = false,
                             const char *version = "MDLV0023", const char *skeletonVersion = "MDLS0004",
                             const char *animationVersion = "MDLA0006", bool shortVertices = false, bool coveringQuad = false, float depth = 0.0f,
                             const std::vector<AttachSpec> &attachments = {}, int attachmentEndError = 0)
{
    Writer w;
    w.raw(version, 8);
    w.d.push_back(0);
    w.u32(0x01800009);
    w.u32(1);
    w.u32(1);
    w.str("materials/test.json");
    for (int i = 0; i < 40; ++i)
        w.d.push_back(0); // padding
    // mesh block
    const float quad[4][2] = {{-10, -10}, {10, -10}, {10, 10}, {-10, 10}};
    w.u32(0x0180000f);
    // `coveringQuad`: a second quad over the first, drawn after it, whose uv is u = 1 all over (the first then has u = 0 all over)
    const int quads = coveringQuad ? 2 : 1;
    w.u32(static_cast<uint32_t>(4 * quads * (shortVertices ? 52 : 80)));
    for (int vertex = 0; vertex < 4 * quads; ++vertex) {
        const int v = vertex % 4;
        const bool covering = vertex >= 4;
        w.f32(quad[v][0]);
        w.f32(quad[v][1]);
        w.f32(depth);                   // position
        if (!shortVertices) {           // the older writers' vertex has neither
            w.f32(0); w.f32(0); w.f32(1);   // normal
            w.f32(1); w.f32(0); w.f32(0); w.f32(1); // tangent
        }
        const uint32_t bone = halfAndHalf && quad[v][0] > 0 ? 1 : 0;
        w.u32(bone); w.u32(0); w.u32(0); w.u32(0);
        w.f32(1); w.f32(0); w.f32(0); w.f32(0); // weights
        w.f32(covering ? 1.0f : coveringQuad ? 0.0f : quad[v][0] > 0 ? 1.0f : 0.0f);
        w.f32(quad[v][1] > 0 ? 0.0f : 1.0f); // uv
    }
    w.u32(static_cast<uint32_t>(12 * quads));
    for (int q = 0; q < quads; ++q)
        for (uint16_t i : {0, 1, 2, 2, 3, 0})
            w.u16(static_cast<uint16_t>(i + 4 * q));
    // skeleton
    const size_t skeletonAt = w.d.size();
    w.raw(skeletonVersion, 8);
    w.d.push_back(0);
    const size_t endAt = w.d.size();
    w.u32(0); // end, patched
    w.u32(static_cast<uint32_t>(bones.size()));
    for (const BoneSpec &b : bones) {
        w.str(b.name);
        w.u32(1);
        w.u32(static_cast<uint32_t>(b.parent));
        w.u32(64);
        const float rows[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, b.tx, b.ty, 0, 1};
        w.raw(rows, 64);
        w.str(b.note);
    }
    for (int i = 0; i < 24; ++i)
        w.d.push_back(0x77); // data the reader does not use
    const uint32_t end = static_cast<uint32_t>(w.d.size());
    std::memcpy(w.d.data() + endAt, &end, 4);
    (void)skeletonAt;
    if (!attachments.empty()) {
        // the section the skeleton's `end` points at: "MDAT0001", NUL, u32 end, u16 count, per point u16 bone, NUL name, 16f (rows)
        w.raw("MDAT0001", 8);
        w.d.push_back(0);
        const size_t mdatEndAt = w.d.size();
        w.u32(0);
        w.u16(static_cast<uint16_t>(attachments.size()));
        for (const AttachSpec &a : attachments) {
            w.u16(a.bone);
            w.str(a.name);
            const float c = std::cos(a.angleZ), s = std::sin(a.angleZ);
            const float rows[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, 1, 0, a.tx, a.ty, 0, 1};
            w.raw(rows, 64);
        }
        const uint32_t mdatEnd = static_cast<uint32_t>(w.d.size()) + static_cast<uint32_t>(attachmentEndError);
        std::memcpy(w.d.data() + mdatEndAt, &mdatEnd, 4);
    }
    // animations
    w.raw(animationVersion, 8);
    w.d.push_back(0);
    w.u32(0);
    w.u32(static_cast<uint32_t>(anims.size()));
    for (const AnimSpec &a : anims) {
        w.u32(a.id);
        w.u32(0);
        w.str(a.name);
        w.str(a.mode);
        w.f32(a.fps);
        w.u32(a.length);
        w.u32(0);
        w.u32(static_cast<uint32_t>(a.tracks.size()));
        for (const TrackSpec &t : a.tracks) {
            w.u32(0);
            w.u32(static_cast<uint32_t>(t.frames.size() * 36));
            for (const auto &f : t.frames)
                for (float x : f)
                    w.f32(x);
        }
        for (int e = 0; e < a.extraTracks; ++e) {
            w.u32(0);
            w.u32(3 * 36);
            for (int i = 0; i < 27; ++i)
                w.f32(0.5f);
        }
        for (int i = 0; i < a.gap; ++i)
            w.d.push_back(0);
    }
    return w.d;
}

std::array<float, 9> frame(float x, float y, float angleZ = 0.0f, float scale = 1.0f)
{
    return {x, y, 0, 0, 0, angleZ, scale, scale, 1};
}


} // namespace puppetfix
