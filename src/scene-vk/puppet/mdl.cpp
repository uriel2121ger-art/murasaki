#include "puppet/mdl.h"

#include "puppet/bonephysics.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace murasaki::scene {

namespace {

class Reader
{
public:
    explicit Reader(std::span<const uint8_t> data, size_t pos = 0) : m_data(data), m_pos(pos) {}
    bool ok() const { return m_ok; }
    size_t pos() const { return m_pos; }
    size_t remaining() const { return m_pos <= m_data.size() ? m_data.size() - m_pos : 0; }
    void seek(size_t pos)
    {
        if (pos > m_data.size()) {
            m_ok = false;
        } else {
            m_pos = pos;
        }
    }
    uint32_t u32()
    {
        uint32_t v = 0;
        read(&v, 4);
        return v;
    }
    uint16_t u16()
    {
        uint16_t v = 0;
        read(&v, 2);
        return v;
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    float f32()
    {
        float v = 0;
        read(&v, 4);
        return std::isfinite(v) ? v : 0.0f;
    }
    float f32Raw() // as stored, NaN and infinity included (a caller that wants to refuse them)
    {
        float v = 0;
        read(&v, 4);
        return v;
    }
    // A NUL terminated string of at most `limit` bytes.
    std::string cstring(size_t limit = 256)
    {
        std::string out;
        for (size_t i = 0; i <= limit; ++i) {
            uint8_t c = 0;
            read(&c, 1);
            if (!m_ok || c == 0) {
                return out;
            }
            out.push_back(static_cast<char>(c));
        }
        m_ok = false;
        return out;
    }
    void skip(size_t n)
    {
        if (n > remaining()) {
            m_ok = false;
        } else {
            m_pos += n;
        }
    }
    const uint8_t *here() const { return m_data.data() + m_pos; }

private:
    void read(void *dst, size_t n)
    {
        if (!m_ok || n > remaining()) {
            m_ok = false;
            std::memset(dst, 0, n);
            return;
        }
        std::memcpy(dst, m_data.data() + m_pos, n);
        m_pos += n;
    }
    std::span<const uint8_t> m_data;
    size_t m_pos = 0;
    bool m_ok = true;
};

constexpr size_t kMaxBoneNoteBytes = 1u << 16;
// The zero bytes a mesh model may have between two parts and after the last one: at most 7 in the library (6 and 7 in version
// 23, 2 and 3 in 21, 1 in 16, 6 before a skeleton); skipping is capped so a file of zeros is not walked to its end.
constexpr size_t kMaxMeshZeroGap = 16;
constexpr size_t kMaxAnimationGap = 256;
constexpr size_t kMaxExtraTracks = 4096;

uint32_t u32At(std::span<const uint8_t> d, size_t at)
{
    uint32_t v = 0;
    if (at + 4 <= d.size()) {
        std::memcpy(&v, d.data() + at, 4);
    }
    return v;
}

// The animation section: "MDLA000" and a version digit 1 to 6, then its NUL (what a fake inside other data, like a name, rarely is).
size_t findAnimationTag(std::span<const uint8_t> d, size_t from)
{
    for (size_t i = from; i + 9 <= d.size(); ++i) {
        if (std::memcmp(d.data() + i, "MDLA000", 7) == 0 && d[i + 7] >= '1' && d[i + 7] <= '6' && d[i + 8] == 0) {
            return i;
        }
    }
    return std::string::npos;
}

// Finds a section tag ("MDLS" followed by a digit). The callers read the whole 8 character version and its NUL after it,
// so a candidate must leave nine bytes.
size_t findTag(std::span<const uint8_t> d, const char *tag, size_t from)
{
    const size_t n = std::strlen(tag);
    for (size_t i = from; i + 9 <= d.size(); ++i) {
        if (std::memcmp(d.data() + i, tag, n) == 0 && i + n < d.size() && d[i + n] >= '0' && d[i + n] <= '9') {
            return i;
        }
    }
    return std::string::npos;
}

// Where the next animation record starts, looking over at most kMaxAnimationGap bytes: id u32, u32 0, a name and a mode that is
// one of the known three, or an empty mode after a name (the clips an FBX import adds: 3164411828); then an fps, a length and a
// track count in range, and a first track whose size is whole frames. npos when nothing matches.
size_t nextAnimationRecord(std::span<const uint8_t> d, size_t from)
{
    for (size_t gap = 0; gap <= kMaxAnimationGap && from + gap + 10 <= d.size(); ++gap) {
        const size_t at = from + gap;
        if (u32At(d, at + 4) != 0) {
            continue;
        }
        size_t p = at + 8;
        size_t nameEnd = p;
        while (nameEnd < d.size() && nameEnd - p <= 256 && d[nameEnd] >= 0x20) {
            ++nameEnd;
        }
        if (nameEnd >= d.size() || d[nameEnd] != 0) {
            continue;
        }
        const bool named = nameEnd > p;
        p = nameEnd + 1;
        size_t modeEnd = std::string::npos;
        for (const char *mode : {"loop", "mirror", "single", ""}) {
            const size_t n = std::strlen(mode);
            if (p + n < d.size() && std::memcmp(d.data() + p, mode, n) == 0 && d[p + n] == 0 && (n > 0 || named)) {
                modeEnd = p + n + 1;
                break;
            }
        }
        if (modeEnd == std::string::npos || modeEnd + 24 > d.size()) {
            continue;
        }
        float fps = 0.0f;
        std::memcpy(&fps, d.data() + modeEnd, 4);
        const uint32_t length = u32At(d, modeEnd + 4), tracks = u32At(d, modeEnd + 12);
        const bool trackHeader = tracks == 0 || u32At(d, modeEnd + 20) % 36 == 0; // (the u32 before the size is not always 0)
        if (std::isfinite(fps) && fps > 0.0f && fps <= 1000.0f && length <= kMaxPuppetFrames && tracks <= kMaxPuppetBones && trackHeader) {
            return at;
        }
    }
    return std::string::npos;
}

Mat4 matrixFromRows(const float *f)
{
    // The file stores a row vector matrix (translation in the last row, 16 floats row by row); Mat4 is column vector and
    // stores m[column * 4 + row]: the transpose of one is the memory image of the other, so the floats go over as they are.
    Mat4 r;
    for (size_t i = 0; i < 16; ++i) {
        r.m[i] = f[i];
    }
    return r;
}

Mat4 composeLocal(const PuppetFrame &f)
{
    return translation(f.position) * rotationEuler(f.angles) * scaling(f.scale);
}

// The skeleton section at `at` ("MDLS...") and the animation section that follows it into `rig`: its bones, their parent-first
// order and its animations. An empty string, or why the sections cannot be read.
std::string readRig(std::span<const uint8_t> bytes, size_t at, Puppet &rig)
{
    // Skeleton.
    Reader r(bytes, at);
    const std::string skeletonTag(reinterpret_cast<const char *>(bytes.data() + at), 8);
    if (skeletonTag != "MDLS0001" && skeletonTag != "MDLS0002" && skeletonTag != "MDLS0003" && skeletonTag != "MDLS0004") {
        return "puppet skeleton version " + skeletonTag + " is not supported";
    }
    r.skip(9);
    const uint32_t skeletonEnd = r.u32();
    const uint32_t boneCount = r.u32();
    if (!r.ok() || boneCount > kMaxPuppetBones || skeletonEnd > bytes.size() || skeletonEnd < r.pos()) {
        return "bad skeleton header";
    }
    for (uint32_t b = 0; b < boneCount; ++b) {
        PuppetBone bone;
        bone.name = r.cstring();
        r.u32();
        bone.parent = r.i32();
        const uint32_t matrixBytes = r.u32();
        if (!r.ok() || matrixBytes != 64 || r.remaining() < 65) {
            return "bad bone record";
        }
        float f[16];
        for (float &x : f) {
            x = r.f32();
        }
        // the editor's constraint settings as JSON (a few hundred bytes): bone physics reads them (puppet/bonephysics.h)
        const std::string note = r.cstring(kMaxBoneNoteBytes);
        if (!r.ok() || bone.parent < -1 || bone.parent >= static_cast<int>(boneCount) || bone.parent == static_cast<int>(b)) {
            return "a bone's parent is not a bone of the model";
        }
        bone.bindLocal = matrixFromRows(f);
        if (const auto constraint = parseBoneConstraint(note)) {
            bone.constraint = *constraint;
            rig.simulatedBones += bone.constraint.simulated() ? 1 : 0;
            rig.ikBones += bone.constraint.ik ? 1 : 0;
            rig.ikGoalBones += bone.constraint.ikControlPoint ? 1 : 0;
        }
        rig.bones.push_back(std::move(bone));
    }
    // a bone's lever is the distance to its first child (the default stays for a bone without one)
    {
        std::vector<uint8_t> measured(rig.bones.size(), 0);
        for (const PuppetBone &child : rig.bones) {
            if (child.parent >= 0 && !measured[static_cast<size_t>(child.parent)]) {
                const float len = std::sqrt(child.bindLocal.m[12] * child.bindLocal.m[12] + child.bindLocal.m[13] * child.bindLocal.m[13]
                                            + child.bindLocal.m[14] * child.bindLocal.m[14]);
                if (len > 1e-3f) {
                    rig.bones[static_cast<size_t>(child.parent)].constraint.length = len;
                    measured[static_cast<size_t>(child.parent)] = 1;
                }
            }
        }
    }
    // parents may come after their children in the file; the order the matrices are built in puts every parent first, and a
    // loop of parents is refused
    {
        const size_t n = rig.bones.size();
        std::vector<uint8_t> placed(n, 0);
        rig.order.reserve(n);
        while (rig.order.size() < n) {
            const size_t before = rig.order.size();
            for (size_t i = 0; i < n; ++i) {
                const int parent = rig.bones[i].parent;
                if (!placed[i] && (parent < 0 || placed[static_cast<size_t>(parent)])) {
                    placed[i] = 1;
                    rig.order.push_back(static_cast<uint32_t>(i));
                }
            }
            if (rig.order.size() == before) {
                return "the bones' parents form a loop";
            }
        }
    }
    // Attachment points: a section of its own right after the skeleton ("MDAT0001", NUL, u32 end, u16 count, then per point u16 bone,
    // NUL name, 16f matrix with the translation in the last row); its `end` is where the animations start. A section that does not
    // line up to its `end` is not used (the objects attached to it are then reported) and the animations are looked for by their tag.
    size_t animationsAt = skeletonEnd;
    if (skeletonEnd + 9 <= bytes.size() && std::memcmp(bytes.data() + skeletonEnd, "MDAT0001", 8) == 0 && bytes[skeletonEnd + 8] == 0) {
        Reader a(bytes, skeletonEnd + 9);
        const uint32_t end = a.u32();
        const uint32_t count = a.u16();
        std::vector<PuppetAttachment> points;
        uint32_t read = 0;
        bool broken = count > kMaxPuppetAttachments;
        for (uint32_t k = 0; k < count && a.ok() && !broken; ++k, ++read) {
            PuppetAttachment point;
            point.bone = a.u16();
            point.name = a.cstring();
            float f[16];
            bool finite = true;
            for (float &x : f) {
                x = a.f32Raw();
                finite = finite && std::isfinite(x);
            }
            if (!a.ok() || point.bone >= rig.bones.size()) {
                broken = true;
                break;
            }
            if (!finite) {
                continue; // a point with a matrix that is not a number is left out (an object on it is reported)
            }
            point.local = matrixFromRows(f);
            points.push_back(std::move(point));
        }
        const bool lined = !broken && a.ok() && read == count && a.pos() == end;
        if (lined) {
            rig.attachments = std::move(points);
        }
        // the animations follow the section; if they are not exactly there they are looked for by their tag, after the section
        // (not inside its names), and a model where that fails keeps its skeleton and points and has no animations
        animationsAt = lined && end + 9 <= bytes.size() && std::memcmp(bytes.data() + end, "MDLA000", 7) == 0 && bytes[end + 7] >= '1' && bytes[end + 7] <= '6' && bytes[end + 8] == 0
            ? end
            : findAnimationTag(bytes, lined ? a.pos() : skeletonEnd + 9);
    }
    // Animations.
    const size_t mdla = animationsAt <= bytes.size() ? findTag(bytes, "MDLA", animationsAt) : std::string::npos;
    if (mdla == animationsAt && mdla != std::string::npos) {
        Reader a(bytes, mdla);
        const std::string animationTag(reinterpret_cast<const char *>(bytes.data() + mdla), 8);
        if (animationTag < "MDLA0001" || animationTag > "MDLA0006" || animationTag.size() != 8) {
            return "puppet animation version " + animationTag + " is not supported";
        }
        a.skip(9);
        a.u32(); // end of the section
        const uint32_t count = a.u32();
        if (!a.ok() || count > kMaxPuppetAnimations) {
            return "bad animation header";
        }
        size_t keyframes = 0;
        for (uint32_t i = 0; i < count; ++i) {
            // a gap of zeros (35 bytes in one model) may sit between animations: the record starts at the first offset that
            // reads like one
            if (i > 0) {
                const size_t next = nextAnimationRecord(bytes, a.pos());
                if (next == std::string::npos) {
                    break; // what is left does not read like an animation: keep the ones read
                }
                a.seek(next);
            }
            PuppetAnimation anim;
            anim.id = a.u32();
            a.u32();
            anim.name = a.cstring();
            const std::string mode = a.cstring(32);
            anim.loop = mode == "mirror" ? PuppetLoop::Mirror : mode == "single" ? PuppetLoop::Single : PuppetLoop::Loop;
            anim.fps = std::clamp(a.f32(), 1.0f, 240.0f);
            anim.length = a.u32();
            a.u32();
            const uint32_t tracks = a.u32();
            if (!a.ok() || tracks > kMaxPuppetBones || anim.length > kMaxPuppetFrames) {
                return "bad animation record";
            }
            anim.tracks.resize(tracks);
            for (uint32_t t = 0; t < tracks; ++t) {
                a.u32();
                const uint32_t byteSize = a.u32();
                if (!a.ok() || byteSize % 36 != 0 || byteSize / 36 > kMaxPuppetFrames || byteSize > a.remaining()) {
                    return "bad animation track";
                }
                const size_t frames = byteSize / 36;
                keyframes += frames;
                if (keyframes > kMaxPuppetKeyframes) {
                    return "too many animation keyframes";
                }
                anim.tracks[t].resize(frames);
                for (PuppetFrame &f : anim.tracks[t]) {
                    f.position = {a.f32(), a.f32(), a.f32()};
                    f.angles = {a.f32(), a.f32(), a.f32()};
                    f.scale = {a.f32(), a.f32(), a.f32()};
                }
            }
            // Some models carry more track-shaped records after the declared ones (twenty tracks for eighteen bones, then empty
            // ones): not bone tracks as far as anything shows, so they are walked over, counted against the keyframe cap,
            // and not kept.
            for (size_t extra = 0; extra < kMaxExtraTracks && a.ok() && a.remaining() >= 8; ++extra) {
                const uint32_t zero = u32At(bytes, a.pos());
                const uint32_t byteSize = u32At(bytes, a.pos() + 4);
                // a real track has frames: an empty record is the zeros before the next animation (whose id may be 0 itself)
                if (zero != 0 || byteSize == 0 || byteSize % 36 != 0 || byteSize / 36 > kMaxPuppetFrames || byteSize > a.remaining() - 8) {
                    break;
                }
                keyframes += byteSize / 36;
                if (keyframes > kMaxPuppetKeyframes) {
                    return "too many animation keyframes";
                }
                a.skip(8 + byteSize);
            }
            if (!a.ok()) {
                return "truncated animation";
            }
            rig.animations.push_back(std::move(anim));
        }
    }
    preparePuppetBind(rig);
    return {};
}

// Every bone index of `count` puppet-layout vertices (bone indices at byte 40, weights at 56) must name a bone of `rig`; the
// largest index any vertex weights is recorded as rig.usedBones, the largest it names at all as rig.boneSlots (the BONECOUNT the
// shader needs). An empty string or why not.
std::string checkBoneIndices(const uint8_t *vertices, size_t count, size_t stride, Puppet &rig)
{
    if (rig.bones.empty()) {
        return {};
    }
    for (size_t v = 0; v < count; ++v) {
        uint32_t idx[4];
        float w[4];
        std::memcpy(idx, vertices + v * stride + 40, 16);
        std::memcpy(w, vertices + v * stride + 56, 16);
        for (int k = 0; k < 4; ++k) {
            // every index, whatever its weight (zero, negative or NaN weights are still read by the shader)
            if (idx[k] >= rig.bones.size()) {
                return "a puppet vertex names a bone that does not exist";
            }
            rig.boneSlots = std::max<size_t>(rig.boneSlots, idx[k] + 1);
            if (w[k] > 0.0f) {
                rig.usedBones = std::max<size_t>(rig.usedBones, idx[k] + 1);
            }
        }
    }
    return {};
}

} // namespace

std::optional<Puppet> parsePuppet(std::span<const uint8_t> bytes, std::string *error)
{
    const auto fail = [&](const std::string &why) -> std::optional<Puppet> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (bytes.size() < 64 || std::memcmp(bytes.data(), "MDLV", 4) != 0) {
        return fail("not a puppet model");
    }
    const std::string version(reinterpret_cast<const char *>(bytes.data()), 8);
    // The versions the library holds: 13 to 17 are older writers, 19 to 23 the newer ones. The vertex of 13 to 16 has no normal or
    // tangent (52 bytes: position, four bone indices, four weights, uv); from 17 on it has both (80 bytes).
    static const char *const kVersions[] = {"MDLV0013", "MDLV0014", "MDLV0016", "MDLV0017", "MDLV0019", "MDLV0021", "MDLV0023"};
    if (std::none_of(std::begin(kVersions), std::end(kVersions), [&](const char *v) { return version == v; })) {
        return fail("puppet model version " + version + " is not supported");
    }
    const size_t inStride = version <= "MDLV0016" ? kShortVertexStride : kPuppetVertexStride;
    Puppet puppet;
    // A puppet without a skeleton section is a plain mesh (an editor puppet nobody rigged): its vertices have no bone indices or
    // weights (48 bytes: position, normal, tangent, uv) and the mesh block runs to the end of the file. It is drawn unskinned.
    const size_t skeletonAt = findTag(bytes, "MDLS", 9);
    const bool skeleton = skeletonAt != std::string::npos;
    const size_t mdls = skeleton ? skeletonAt : bytes.size();

    // The material path, for information.
    {
        Reader r(bytes, 9 + 12);
        puppet.material = r.cstring();
    }

    // The mesh block: u32 flags, u32 vertexBytes, vertices, u32 indexBytes, indices, entirely before the skeleton section (or the end).
    const auto findMesh = [&](size_t stride) -> bool {
        for (size_t at = 9; at + 8 + 4 < mdls; ++at) {
            const uint32_t vertexBytes = u32At(bytes, at + 4);
            if (vertexBytes == 0 || vertexBytes % stride != 0 || vertexBytes / stride > kMaxPuppetVertices) {
                continue;
            }
            const size_t indexAt = at + 8 + vertexBytes;
            if (indexAt + 4 > mdls) {
                continue;
            }
            const uint32_t indexBytes = u32At(bytes, indexAt);
            if (indexBytes == 0 || indexBytes % 6 != 0 || indexBytes / 2 > kMaxPuppetIndices || indexAt + 4 + indexBytes > mdls) {
                continue;
            }
            const size_t vertexCount = vertexBytes / stride;
            // validate in place: a candidate offset costs no allocation until it has proved to be the mesh
            const size_t indexCount = indexBytes / 2;
            bool valid = true;
            for (size_t i = 0; i < indexCount && valid; ++i) {
                uint16_t v;
                std::memcpy(&v, bytes.data() + indexAt + 4 + i * 2, 2);
                valid = v < vertexCount;
            }
            if (!valid) {
                continue;
            }
            std::vector<uint32_t> indices(indexCount);
            for (size_t i = 0; i < indexCount; ++i) {
                uint16_t v;
                std::memcpy(&v, bytes.data() + indexAt + 4 + i * 2, 2);
                indices[i] = v;
            }
            if (stride == kExtendedVertexStride) {
                // a float after the position (always 0 in the library's files) that the long vertex does not have: dropped
                puppet.vertices.assign(vertexCount * kPuppetVertexStride, 0);
                for (size_t v = 0; v < vertexCount; ++v) {
                    const uint8_t *in = bytes.data() + at + 8 + v * stride;
                    uint8_t *out = puppet.vertices.data() + v * kPuppetVertexStride;
                    std::memcpy(out, in, 12);
                    std::memcpy(out + 12, in + 16, kPuppetVertexStride - 12);
                }
            } else if (stride == kPuppetVertexStride) {
                puppet.vertices.assign(bytes.begin() + static_cast<std::ptrdiff_t>(at + 8), bytes.begin() + static_cast<std::ptrdiff_t>(at + 8 + vertexBytes));
            } else if (stride == kStaticVertexStride) {
                // the unrigged vertex into the long one: bone 0 with all the weight, which nothing reads without a skeleton
                puppet.vertices.assign(vertexCount * kPuppetVertexStride, 0);
                for (size_t v = 0; v < vertexCount; ++v) {
                    const uint8_t *in = bytes.data() + at + 8 + v * stride;
                    uint8_t *out = puppet.vertices.data() + v * kPuppetVertexStride;
                    const float weights[4] = {1.0f, 0.0f, 0.0f, 0.0f};
                    std::memcpy(out, in, 40);          // position, normal, tangent
                    std::memcpy(out + 56, weights, 16);
                    std::memcpy(out + 72, in + 40, 8); // uv
                }
            } else {
                // the short vertex into the long one: a normal toward the viewer and a tangent along x, which the shaders read and the
                // old models never had
                puppet.vertices.assign(vertexCount * kPuppetVertexStride, 0);
                for (size_t v = 0; v < vertexCount; ++v) {
                    const uint8_t *in = bytes.data() + at + 8 + v * stride;
                    uint8_t *out = puppet.vertices.data() + v * kPuppetVertexStride;
                    const float normal[3] = {0.0f, 0.0f, 1.0f}, tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f};
                    std::memcpy(out, in, 12);                  // position
                    std::memcpy(out + 12, normal, 12);
                    std::memcpy(out + 24, tangent, 16);
                    std::memcpy(out + 40, in + 12, 16 + 16);   // bone indices and weights
                    std::memcpy(out + 72, in + 44, 8);         // uv
                }
            }
            puppet.indices = std::move(indices);
            return true;
        }
        return false;
    };
    // (the longer vertices carry bone indices, which only a skeleton gives a meaning: a file without one has the static vertex)
    // (a rigged mesh whose block flags are 0x181000e rather than 0x180000f has the 84 byte vertex: 3810943704, 3577990983)
    const bool found = skeleton ? (findMesh(inStride) || (inStride == kPuppetVertexStride && findMesh(kExtendedVertexStride)))
                                : findMesh(kStaticVertexStride);
    if (!found) {
        return fail("no usable mesh block in the puppet");
    }
    // finite positions and uvs only: the rest of a vertex is read by the shader as it is
    for (size_t v = 0; v < puppet.vertexCount(); ++v) {
        float f[3];
        std::memcpy(f, puppet.vertices.data() + v * kPuppetVertexStride, 12);
        if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) {
            return fail("a puppet vertex has a position that is not a number");
        }
    }

    if (!skeleton) {
        return puppet;
    }

    if (const std::string why = readRig(bytes, mdls, puppet); !why.empty()) {
        return fail(why);
    }
    if (const std::string why = checkBoneIndices(puppet.vertices.data(), puppet.vertexCount(), kPuppetVertexStride, puppet); !why.empty()) {
        return fail(why);
    }
    return puppet;
}

namespace {

// What a bind matrix says in the terms of a frame: translation, the Euler angles of rotationEuler (z * y * x) and the scale along
// each axis (a 2D puppet's bind turns about z, or mirrors a bone with a half turn about x; a 3D mesh's turns about any axis).
PuppetFrame bindFrameOf(const Mat4 &m)
{
    const auto colLen = [&](size_t c) { return std::sqrt(m.m[c * 4] * m.m[c * 4] + m.m[c * 4 + 1] * m.m[c * 4 + 1] + m.m[c * 4 + 2] * m.m[c * 4 + 2]); };
    const Vec3 scale{colLen(0), colLen(1), colLen(2)};
    // the rotation's element (row, col): the column divided by its length
    const auto rot = [&](size_t row, size_t col) {
        const float len = col == 0 ? scale.x : col == 1 ? scale.y : scale.z;
        return len > 0.0f ? m.m[col * 4 + row] / len : 0.0f;
    };
    Vec3 angles;
    const float sy = std::clamp(-rot(2, 0), -1.0f, 1.0f);
    angles.y = std::asin(sy);
    if (std::fabs(sy) < 0.99999f) {
        angles.x = std::atan2(rot(2, 1), rot(2, 2));
        angles.z = std::atan2(rot(1, 0), rot(0, 0));
    } else {
        // gimbal lock: x and z turn about the same axis; all of it goes to z
        angles.x = 0.0f;
        angles.z = std::atan2(-rot(0, 1), rot(1, 1));
    }
    return PuppetFrame{{m.m[12], m.m[13], m.m[14]}, angles, scale};
}

constexpr float kTwoPi = 6.28318530717958647692f;

// `angle` moved by whole turns to the one nearest `near`: atan2 gives a half turn as -pi where a frame may store +pi, and
// blending -pi with +pi would undo the turn.
float nearestTurn(float angle, float near)
{
    const float turns = std::round((near - angle) / kTwoPi);
    return std::isfinite(turns) ? angle + turns * kTwoPi : angle;
}

// Per-thread working space of puppetBoneMatrices: it grows to the largest skeleton and is reused.
struct BoneScratch {
    std::vector<Mat4> animWorld;
    std::vector<PuppetFrame> pose, delta, bindFrames;
    std::vector<Vec3> firstAngles; // per bone, the blend-weighted angles of the first frames of the tracks that pull it
    std::vector<Mat4> inverseBindWorld;
    std::vector<float> weight;
    std::vector<uint8_t> additiveTouched;
};

void computeBind(const Puppet &puppet, std::vector<PuppetFrame> &frames, std::vector<Mat4> &inverseWorld)
{
    const size_t n = puppet.bones.size();
    std::vector<Mat4> world(n);
    frames.resize(n);
    inverseWorld.resize(n);
    for (size_t k = 0; k < n; ++k) {
        const size_t i = puppet.order.size() == n ? static_cast<size_t>(puppet.order[k]) : k;
        const PuppetBone &b = puppet.bones[i];
        world[i] = b.parent >= 0 ? world[static_cast<size_t>(b.parent)] * b.bindLocal : b.bindLocal;
    }
    for (size_t i = 0; i < n; ++i) {
        frames[i] = bindFrameOf(puppet.bones[i].bindLocal);
        inverseWorld[i] = inverse(world[i]);
    }
}

} // namespace

void preparePuppetBind(Puppet &puppet)
{
    computeBind(puppet, puppet.bindFrames, puppet.inverseBindWorld);
}

std::vector<float> puppetBoneMatrices(const Puppet &puppet, const std::vector<PuppetLayerState> &layers, double time)
{
    std::vector<float> out;
    puppetBoneMatrices(puppet, layers, time, out);
    return out;
}

void puppetBoneMatrices(const Puppet &puppet, const std::vector<PuppetLayerState> &layers, double time, std::vector<float> &out,
                        BonePhysicsState *physics, const BoneWorld *world, std::vector<Mat4> *worlds)
{
    thread_local BoneScratch scratch;
    const size_t n = puppet.bones.size();
    if (physics && (physics->size() != n || puppet.simulatedBones == 0)) {
        physics = nullptr; // a state made for another skeleton, or nothing to simulate
    }
    if (physics) {
        physics->beginFrame(time);
    }
    const BoneWorld defaultWorld;
    out.assign(n * 16, 0.0f);
    // the bind pose: kept with the puppet by the reader, else worked out here
    const bool prepared = puppet.bindFrames.size() == n && puppet.inverseBindWorld.size() == n;
    if (!prepared) {
        computeBind(puppet, scratch.bindFrames, scratch.inverseBindWorld);
    }
    const std::vector<PuppetFrame> &bindFrame = prepared ? puppet.bindFrames : scratch.bindFrames;
    const std::vector<Mat4> &inverseBindWorld = prepared ? puppet.inverseBindWorld : scratch.inverseBindWorld;
    const auto inOrder = [&](size_t k) { return puppet.order.size() == n ? static_cast<size_t>(puppet.order[k]) : k; };
    // the weighted pose: layers pull a bone's local transform towards their frame, in proportion to their blend; what the
    // layers leave over (their blends sum to less than one) stays the bind pose
    std::vector<PuppetFrame> &pose = scratch.pose;
    std::vector<float> &weight = scratch.weight;
    // additive layers add their difference from the bind pose, times their blend, on top of whatever the others made
    std::vector<PuppetFrame> &delta = scratch.delta;
    std::vector<uint8_t> &additiveTouched = scratch.additiveTouched;
    std::vector<Mat4> &animWorld = scratch.animWorld;
    std::vector<Vec3> &firstAngles = scratch.firstAngles;
    firstAngles.assign(n, Vec3{0, 0, 0});
    pose.assign(n, PuppetFrame{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}});
    delta.assign(n, PuppetFrame{{0, 0, 0}, {0, 0, 0}, {0, 0, 0}});
    weight.assign(n, 0.0f);
    additiveTouched.assign(n, 0);
    animWorld.resize(n);
    float total = 0.0f;
    for (const PuppetLayerState &l : layers) {
        if (!l.additive) {
            total += std::max(0.0f, l.blend);
        }
    }
    const float norm = total > 1.0f ? 1.0f / total : 1.0f;
    for (const PuppetLayerState &l : layers) {
        const PuppetAnimation *anim = nullptr;
        for (const PuppetAnimation &a : puppet.animations) {
            if (a.id == l.animation) {
                anim = &a;
            }
        }
        const float w = std::max(0.0f, l.blend) * (l.additive ? 1.0f : norm);
        if (!anim || w <= 0.0f) {
            continue;
        }
        const double frameTime = time * static_cast<double>(l.rate) * static_cast<double>(anim->fps) + static_cast<double>(l.frameOffset);
        for (size_t b = 0; b < n && b < anim->tracks.size(); ++b) {
            const auto &track = anim->tracks[b];
            if (track.empty()) {
                continue;
            }
            // the playhead in frames: loop wraps, mirror runs there and back, single holds the last frame
            const double span = std::max<double>(1.0, anim->length > 0 ? anim->length : static_cast<double>(track.size() - 1));
            double head = frameTime;
            switch (anim->loop) {
            case PuppetLoop::Loop:
                head = std::fmod(std::max(0.0, head), span);
                break;
            case PuppetLoop::Mirror: {
                const double m = std::fmod(std::max(0.0, head), 2.0 * span);
                head = m <= span ? m : 2.0 * span - m;
                break;
            }
            case PuppetLoop::Single:
                head = std::clamp(head, 0.0, span);
                break;
            }
            if (!std::isfinite(head)) {
                head = 0.0; // a rate or fps that overflowed: the first frame, never an undefined cast
            }
            const size_t k0 = std::min<size_t>(static_cast<size_t>(head), track.size() - 1);
            const size_t k1 = std::min(k0 + 1, track.size() - 1);
            const float f = static_cast<float>(head - std::floor(head));
            const PuppetFrame &p0 = track[k0], &p1 = track[k1];
            const auto lerp = [&](Vec3 a, Vec3 c) { return Vec3{a.x + (c.x - a.x) * f, a.y + (c.y - a.y) * f, a.z + (c.z - a.z) * f}; };
            const Vec3 pos = lerp(p0.position, p1.position), ang = lerp(p0.angles, p1.angles), sc = lerp(p0.scale, p1.scale);
            if (l.additive) {
                // the turn from the bind pose, with the bind's angles taken the whole turns nearest the track's first frame (a bind
                // read as -pi and frames stored as +pi are the same pose; a track that spins on past a half turn still adds smoothly)
                const PuppetFrame &bf = bindFrame[b];
                const Vec3 &first = track.front().angles;
                const Vec3 bindAngles{nearestTurn(bf.angles.x, first.x), nearestTurn(bf.angles.y, first.y), nearestTurn(bf.angles.z, first.z)};
                delta[b].position = {delta[b].position.x + (pos.x - bf.position.x) * w, delta[b].position.y + (pos.y - bf.position.y) * w, delta[b].position.z + (pos.z - bf.position.z) * w};
                delta[b].angles = {delta[b].angles.x + (ang.x - bindAngles.x) * w, delta[b].angles.y + (ang.y - bindAngles.y) * w,
                                   delta[b].angles.z + (ang.z - bindAngles.z) * w};
                delta[b].scale = {delta[b].scale.x + (sc.x - bf.scale.x) * w, delta[b].scale.y + (sc.y - bf.scale.y) * w, delta[b].scale.z + (sc.z - bf.scale.z) * w};
                additiveTouched[b] = 1;
                continue;
            }
            pose[b].position = {pose[b].position.x + pos.x * w, pose[b].position.y + pos.y * w, pose[b].position.z + pos.z * w};
            pose[b].angles = {pose[b].angles.x + ang.x * w, pose[b].angles.y + ang.y * w, pose[b].angles.z + ang.z * w};
            const Vec3 &first = track.front().angles;
            firstAngles[b] = {firstAngles[b].x + first.x * w, firstAngles[b].y + first.y * w, firstAngles[b].z + first.z * w};
            pose[b].scale = {pose[b].scale.x + sc.x * w, pose[b].scale.y + sc.y * w, pose[b].scale.z + sc.z * w};
            weight[b] += w;
        }
    }
    for (size_t k = 0; k < n; ++k) {
        const size_t i = inOrder(k);
        const PuppetBone &b = puppet.bones[i];
        Mat4 local = b.bindLocal;
        PuppetFrame animated = bindFrame[i]; // the pose the animations ask for, which the simulated bones follow
        if (weight[i] > 0.0f || additiveTouched[i]) {
            // the animated part by its weight, the bind pose by the rest: positions and angles blend linearly, so does scale
            // (a bone no layer touches stays the bind matrix). The bind's angles are taken the whole turns nearest the first frames
            // of the tracks that pull the bone (their weighted mean): a half turn stored as +pi in the frames and read as -pi from the
            // bind stays a half turn, and the choice does not change while the tracks play (no jump when a track spins round).
            const float rest = weight[i] > 0.0f ? std::max(0.0f, 1.0f - weight[i]) : 1.0f;
            const PuppetFrame &bf = bindFrame[i];
            Vec3 bindAngles = bf.angles;
            if (weight[i] > 0.0f) {
                const Vec3 mean{firstAngles[i].x / weight[i], firstAngles[i].y / weight[i], firstAngles[i].z / weight[i]};
                bindAngles = {nearestTurn(bf.angles.x, mean.x), nearestTurn(bf.angles.y, mean.y), nearestTurn(bf.angles.z, mean.z)};
            }
            const Vec3 &dp = delta[i].position, &da = delta[i].angles, &ds = delta[i].scale;
            PuppetFrame f;
            f.position = {pose[i].position.x + bf.position.x * rest + dp.x, pose[i].position.y + bf.position.y * rest + dp.y, pose[i].position.z + bf.position.z * rest + dp.z};
            f.angles = {pose[i].angles.x + bindAngles.x * rest + da.x, pose[i].angles.y + bindAngles.y * rest + da.y, pose[i].angles.z + bindAngles.z * rest + da.z};
            f.scale = {pose[i].scale.x + bf.scale.x * rest + ds.x, pose[i].scale.y + bf.scale.y * rest + ds.y, pose[i].scale.z + bf.scale.z * rest + ds.z};
            local = composeLocal(f);
            animated = f;
        }
        if (physics && b.constraint.simulated()) {
            local = composeLocal(physics->step(i, b.constraint, animated, b.parent >= 0 ? animWorld[static_cast<size_t>(b.parent)] : Mat4::identity(),
                                               world ? *world : defaultWorld));
        }
        animWorld[i] = b.parent >= 0 ? animWorld[static_cast<size_t>(b.parent)] * local : local;
    }
    if (worlds) {
        worlds->assign(animWorld.begin(), animWorld.begin() + static_cast<std::ptrdiff_t>(n));
    }
    for (size_t i = 0; i < n; ++i) {
        Mat4 skin = animWorld[i] * inverseBindWorld[i];
        if (!std::all_of(skin.m.begin(), skin.m.end(), [](float x) { return std::isfinite(x); })) {
            skin = Mat4::identity(); // a degenerate bind or pose: this bone leaves its vertices where the mesh has them
        }
        // g_Bones[i] is a mat4x3 (std140: four columns of three floats, each padded to four): the upper three rows of `skin`
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 3; ++row) {
                out[i * 16 + static_cast<size_t>(col * 4 + row)] = skin.m[static_cast<size_t>(col * 4 + row)];
            }
        }
    }
}

void puppetAttachmentPoses(const Puppet &puppet, const std::vector<PuppetLayerState> &layers, double time, std::vector<Mat4> &out,
                           BonePhysicsState *physics, const BoneWorld *world)
{
    thread_local std::vector<float> bones;
    thread_local std::vector<Mat4> worlds;
    puppetBoneMatrices(puppet, layers, time, bones, physics, world, &worlds);
    out.resize(puppet.attachments.size());
    for (size_t i = 0; i < out.size(); ++i) {
        const size_t bone = puppet.attachments[i].bone;
        Mat4 pose = bone < worlds.size() ? worlds[bone] * puppet.attachments[i].local : puppet.attachments[i].local;
        const auto finite = [](const Mat4 &m) { return std::all_of(m.m.begin(), m.m.end(), [](float x) { return std::isfinite(x); }); };
        if (!finite(pose)) {
            pose = finite(puppet.attachments[i].local) ? puppet.attachments[i].local : Mat4::identity(); // as a bone's skin matrix does
        }
        out[i] = pose;
    }
}

uint32_t MeshPart::index(size_t i) const
{
    uint32_t v = 0;
    const size_t size = wideIndices ? 4 : 2;
    if ((i + 1) * size <= indices.size()) {
        std::memcpy(&v, indices.data() + i * size, size);
    }
    return v;
}

std::optional<MeshModel> parseMeshModel(std::span<const uint8_t> bytes, std::string *error)
{
    const auto fail = [&](const std::string &why) -> std::optional<MeshModel> {
        if (error) {
            *error = why;
        }
        return std::nullopt;
    };
    if (bytes.size() < 21 || std::memcmp(bytes.data(), "MDLV", 4) != 0 || bytes[8] != 0) {
        return fail("not a mesh model");
    }
    // The versions the library's mesh objects hold. 7 has no bounds and no vertex format of its own in a part (the header's
    // applies), 16 adds the part's vertex format, 19 on the bounds before it.
    const std::string version(reinterpret_cast<const char *>(bytes.data()), 8);
    static const char *const kVersions[] = {"MDLV0007", "MDLV0016", "MDLV0019", "MDLV0021", "MDLV0023"};
    if (std::none_of(std::begin(kVersions), std::end(kVersions), [&](const char *v) { return version == v; })) {
        return fail("mesh model version " + version + " is not supported");
    }
    const bool partFlags = version >= "MDLV0016";
    const bool partBounds = version >= "MDLV0019";
    MeshModel model;
    Reader r(bytes, 9);
    const uint32_t headerFlags = r.u32();
    const uint32_t one = r.u32();
    const uint32_t partCount = r.u32();
    if (!r.ok() || one != 1 || partCount == 0 || partCount > kMaxMeshParts) {
        return fail("bad mesh header");
    }
    // the skinning bits: the parts have the puppet's vertex and the skeleton follows them
    constexpr uint32_t kSkinBits = 0x01800000u;
    model.skinned = (headerFlags & kSkinBits) != 0;
    size_t vertexTotal = 0, indexTotal = 0;
    for (uint32_t p = 0; p < partCount; ++p) {
        if (p > 0) {
            // the zero bytes between two parts (their meaning is not known; never more than six in the library)
            for (size_t gap = 0; gap < kMaxMeshZeroGap && r.remaining() > 0 && *r.here() == 0; ++gap) {
                r.skip(1);
            }
        }
        MeshPart part;
        part.material = r.cstring(512);
        const uint32_t wide = r.u32();
        if (partBounds) {
            part.boundsMin = {r.f32(), r.f32(), r.f32()};
            part.boundsMax = {r.f32(), r.f32(), r.f32()};
        }
        const uint32_t format = partFlags ? r.u32() : headerFlags;
        const uint32_t vertexBytes = r.u32();
        if (!r.ok() || part.material.empty() || wide > 1) {
            return fail("bad mesh part");
        }
        // the vertex: bit 1 position, 2 normal, 4 tangent, 8 uv (a static part); the skinning bits with all four, the puppet's
        size_t inStride = 0;
        if ((format & kSkinBits) == kSkinBits && (format & ~kSkinBits) == 0x0f) {
            inStride = kPuppetVertexStride;
        } else if ((format & ~0x0fu) == 0 && (format & 0x01u) != 0) {
            inStride = 12 + ((format & 2u) ? 12 : 0) + ((format & 4u) ? 16 : 0) + ((format & 8u) ? 8 : 0);
        } else {
            return fail(std::format("mesh vertex format 0x{:x} is not supported", format));
        }
        if ((inStride == kPuppetVertexStride) != model.skinned) {
            return fail("a mesh part's vertex does not match the model's skinning");
        }
        if (vertexBytes == 0 || vertexBytes % inStride != 0 || vertexBytes > r.remaining()) {
            return fail("bad mesh vertex block");
        }
        part.vertexCount = vertexBytes / inStride;
        vertexTotal += part.vertexCount;
        if (vertexTotal > kMaxMeshVertices) {
            return fail("too many mesh vertices");
        }
        part.stride = inStride == kPuppetVertexStride ? kPuppetVertexStride : kMeshVertexStride;
        if (inStride == part.stride) {
            part.vertices.assign(r.here(), r.here() + vertexBytes);
        } else {
            // a vertex without some of the attributes widened to the 48 byte one: a normal along +z, a tangent along +x, uv 0
            part.vertices.resize(part.vertexCount * kMeshVertexStride);
            const float defaults[12] = {0, 0, 0, 0, 0, 1, 1, 0, 0, 1, 0, 0};
            for (size_t v = 0; v < part.vertexCount; ++v) {
                const uint8_t *in = r.here() + v * inStride;
                uint8_t *out = part.vertices.data() + v * kMeshVertexStride;
                std::memcpy(out, defaults, sizeof(defaults));
                size_t at = 0;
                struct Field {
                    uint32_t bit;
                    size_t offset, size;
                };
                static constexpr Field kFields[] = {{1u, 0, 12}, {2u, 12, 12}, {4u, 24, 16}, {8u, 40, 8}};
                for (const Field &f : kFields) {
                    if (format & f.bit) {
                        std::memcpy(out + f.offset, in + at, f.size);
                        at += f.size;
                    }
                }
            }
        }
        r.skip(vertexBytes);
        const uint32_t indexBytes = r.u32();
        const size_t indexSize = wide ? 4 : 2;
        if (!r.ok() || indexBytes == 0 || indexBytes % (3 * indexSize) != 0 || indexBytes > r.remaining()) {
            return fail("bad mesh index block");
        }
        part.wideIndices = wide != 0;
        part.indexCount = indexBytes / indexSize;
        indexTotal += part.indexCount;
        if (indexTotal > kMaxMeshIndices) {
            return fail("too many mesh indices");
        }
        part.indices.assign(r.here(), r.here() + indexBytes);
        r.skip(indexBytes);
        for (size_t i = 0; i < part.indexCount; ++i) {
            if (part.index(i) >= part.vertexCount) {
                return fail("a mesh index names a vertex that does not exist");
            }
        }
        for (size_t v = 0; v < part.vertexCount; ++v) {
            float f[3];
            std::memcpy(f, part.vertices.data() + v * part.stride, 12);
            if (!std::isfinite(f[0]) || !std::isfinite(f[1]) || !std::isfinite(f[2])) {
                return fail("a mesh vertex has a position that is not a number");
            }
            if (!partBounds) {
                // (the older versions do not store the bounds)
                const Vec3 pos{f[0], f[1], f[2]};
                part.boundsMin = v == 0 ? pos : Vec3{std::min(part.boundsMin.x, pos.x), std::min(part.boundsMin.y, pos.y), std::min(part.boundsMin.z, pos.z)};
                part.boundsMax = v == 0 ? pos : Vec3{std::max(part.boundsMax.x, pos.x), std::max(part.boundsMax.y, pos.y), std::max(part.boundsMax.z, pos.z)};
            }
        }
        model.parts.push_back(std::move(part));
    }
    // what follows the parts: zero bytes (none to seven in the library), then for a skinned mesh its skeleton and animations
    for (size_t gap = 0; gap < kMaxMeshZeroGap && r.remaining() > 0 && *r.here() == 0; ++gap) {
        r.skip(1);
    }
    const bool skeletonNext = r.remaining() >= 9 && std::memcmp(r.here(), "MDLS", 4) == 0;
    if (model.skinned) {
        if (!skeletonNext) {
            return fail("a skinned mesh without its skeleton");
        }
        if (const std::string why = readRig(bytes, r.pos(), model.rig); !why.empty()) {
            return fail(why);
        }
        if (model.rig.bones.empty()) {
            return fail("a skinned mesh without bones");
        }
        for (const MeshPart &part : model.parts) {
            if (const std::string why = checkBoneIndices(part.vertices.data(), part.vertexCount, part.stride, model.rig); !why.empty()) {
                return fail(why);
            }
        }
    } else if (skeletonNext) {
        return fail("a skeleton on a mesh whose vertices have no bones");
    }
    // sections the draw does not read
    model.morphTargets = findTag(bytes, "MDMP", r.pos()) != std::string::npos;
    model.attachments = findTag(bytes, "MDAT", r.pos()) != std::string::npos;
    return model;
}

} // namespace murasaki::scene
