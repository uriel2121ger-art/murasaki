#pragma once

// Wallpaper Engine puppet models (.mdl): a skinned mesh, its skeleton and its animations. The layout below was worked out
// from the files of the author's library (the mesh block is found with explicit checks); docs/scene-vk/puppets.md describes it and what is assumed. All little-endian.
//
//   "MDLV0013|14|16|17|19|21|23\0"          9 bytes
//   ...                        three u32 (flags, 1, 1), the material path as a NUL terminated string, padding of zeros
//   u32 flags, u32 vertexBytes the mesh block: vertexBytes is a multiple of 80, the size of a vertex:
//   vertices                     pos 3f, normal 3f, tangent 4f, blend indices 4 x u32, blend weights 4f, uv 2f
//   u32 indexBytes, indices      u16 triples
//   "MDLS0001..0004\0" u32 end         the skeleton section (absent from a puppet nobody rigged: its 48 byte vertex has no bone
//                                 indices or weights, and it is drawn as a static mesh); `end` is the file offset of the next one
//   u32 boneCount, then per bone: NUL string name, u32 (1), i32 parent (-1 for a root), u32 64, 16f local bind matrix
//                                 (row vectors, translation in the last row), NUL string (empty)
//                                 ... (data the renderer does not use) up to `end`
//   "MDLA0001..0006\0" u32 end, u32 animationCount, per animation: u32 id, u32 0, NUL string name, NUL string mode
//                                 ("loop", "mirror" or "single"), f32 fps, u32 length (frames), u32 0, u32 trackCount,
//                                 per track (one per bone): u32 0, u32 byteSize, then byteSize / 36 frames of
//                                 pos 3f, euler angles 3f (radians), scale 3f
//
// Every count and size is checked against the data and against the limits below before anything is allocated.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "util/math.h"

namespace murasaki::scene {

// Hostile input limits.
inline constexpr size_t kMaxPuppetVertices = 200000;
inline constexpr size_t kMaxPuppetIndices = 3 * 400000;
inline constexpr size_t kMaxPuppetBones = 128;       // g_Bones[] must fit the 16 KiB uniform window at 64 bytes a bone
inline constexpr size_t kMaxPuppetAnimations = 64;
inline constexpr size_t kMaxPuppetFrames = 65536;     // per track
inline constexpr size_t kMaxPuppetKeyframes = 2000000; // over all tracks of a puppet
inline constexpr size_t kMaxPuppetAttachments = 256;
inline constexpr size_t kPuppetVertexStride = 80;
inline constexpr size_t kPuppetTexCoordOffset = 72; // the uv (2f) sits last in the 80 byte vertex
inline constexpr size_t kShortVertexStride = 52; // the older writers' vertex, widened to 80 on reading
inline constexpr size_t kExtendedVertexStride = 84; // the long vertex with a float after the position (mesh flags 0x181000e)
inline constexpr size_t kStaticVertexStride = 48; // a puppet without a skeleton: no bone indices or weights, widened to 80

struct PuppetVertexView; // the raw bytes are kept as they are: the GPU reads this very layout

// The editor's constraint JSON after a bone's matrix, as far as the library's models show (docs/scene-vk/puppets.md, "Bone
// physics"): which keys exist is measured, what they mean is assumed from their names and effects. A bone simulates when its
// rotation (`r`) or translation (`t`) flag is set.
struct BoneConstraint {
    bool rotation = false;    // `r`: the bone's angles follow the animated ones through a spring
    bool translation = false; // `t`: the same for its position
    float rotationStiffness = 200.0f, rotationDamping = 20.0f;       // `rs`, `rf`
    float translationStiffness = 200.0f, translationDamping = 20.0f; // `ts`, `tf`
    bool rotationAxes[3] = {false, false, true};     // `rax`, `ray`, `raz`: absent means about z only (a 2D puppet)
    bool translationAxes[3] = {true, true, false};   // `tax`, `tay`, `taz`: absent means in the plane only
    bool gravity = false;                            // `ge`
    Vec3 gravityDirection{0, -1, 0};                 // `gd`
    Vec3 axis{1, 0, 0};                              // `a`: the bone's own direction, the lever gravity and wind turn
    bool limitAngles = false;                        // `la`: `lamin`..`lamax` bound the angles' offset from the animated pose
    Vec3 angleMin{-3.14159265f, -3.14159265f, -3.14159265f}, angleMax{3.14159265f, 3.14159265f, 3.14159265f};
    bool limitTranslation = false;                   // `lt`: `ltmax` bounds the position's offset from the animated pose
    float translationLimit = 100.0f;
    bool ik = false;                                 // `ik`: the end of an IK chain `ikd` bones long (not evaluated: no goal in the data)
    bool ikControlPoint = false;                     // `ikcp`: the goal would be control point `ikcpn` (false in every bone of the library)
    int ikDepth = 0;                                 // `ikd`: ancestors in the chain (2 in the library: the chain's bones carry sim type 3)
    float length = 100.0f;                           // not in the JSON: the distance to the bone's first child, the lever of a force
    bool simulated() const { return rotation || translation; }
};

// A named point of the rig that objects can be attached to (an `MDAT0001` section): the bone it sits on and its matrix, which is
// local to that bone (docs/scene-vk/puppets.md, "Attachments").
struct PuppetAttachment {
    std::string name;
    uint32_t bone = 0;
    Mat4 local = Mat4::identity(); // column vector convention (the file's matrix transposed)
};

struct PuppetBone {
    std::string name;
    int parent = -1;
    Mat4 bindLocal = Mat4::identity(); // column vector convention (the file's matrix transposed)
    BoneConstraint constraint;
};

struct PuppetFrame {
    Vec3 position;
    Vec3 angles;
    Vec3 scale{1, 1, 1};
};

enum class PuppetLoop { Loop, Mirror, Single };

struct PuppetAnimation {
    uint32_t id = 0;
    std::string name;
    PuppetLoop loop = PuppetLoop::Loop;
    float fps = 30.0f;
    uint32_t length = 0;                         // frames
    std::vector<std::vector<PuppetFrame>> tracks; // [bone][frame]; a bone without a track is empty
};

struct Puppet {
    std::string material;            // "materials/sun.json" (informative: the model JSON names the real one)
    std::vector<uint8_t> vertices;   // vertexCount * 80 bytes, as in the file
    std::vector<uint32_t> indices;
    std::vector<PuppetBone> bones;
    std::vector<PuppetAnimation> animations;
    std::vector<uint32_t> order;     // bone indices, every parent before its children (parents may come later in the file)
    size_t vertexCount() const { return vertices.size() / kPuppetVertexStride; }
    // Largest bone index a vertex weights, plus one (0 without a skeleton): whether the mesh moves with its bones at all.
    size_t usedBones = 0;
    // Largest bone index any vertex names, weighted or not, plus one: the BONECOUNT the shader needs (it reads all four of a
    // vertex's bones, whatever their weights), so g_Bones holds the first boneSlots matrices.
    size_t boneSlots = 0;
    // Filled by the reader (or on first use): what the bind pose says, for puppetBoneMatrices. Per bone, its bind matrix taken
    // apart into a frame (translation, Euler angles of Rz * Ry * Rx, scale) and the inverse of its bind world matrix.
    std::vector<PuppetFrame> bindFrames;
    std::vector<Mat4> inverseBindWorld;
    std::vector<PuppetAttachment> attachments;
    size_t simulatedBones = 0; // bones whose constraint asks for a rotation or translation spring (the reader counts them)
    size_t ikBones = 0;        // bones whose constraint marks them as an IK chain end: not evaluated
    size_t ikGoalBones = 0;    // of those, the ones that say their goal is a control point
};

struct BonePhysicsState; // puppet/bonephysics.h
struct BoneWorld;

// Parses a puppet; nullopt with a reason in `error` for anything it does not understand or that is over the limits.
std::optional<Puppet> parsePuppet(std::span<const uint8_t> bytes, std::string *error = nullptr);

// Bone matrices for `layers` at `time` seconds: for every bone the skinning matrix (animated world * inverse bind world) in
// the layout of g_Bones (mat4x3 std140: four columns of three floats, each padded to four: 16 floats a bone).
struct PuppetLayerState {
    uint32_t animation = 0; // id
    float blend = 1.0f;
    float rate = 1.0f;
    bool additive = false; // adds (frame - bind pose) * blend on top of the other layers instead of taking part in their blend
    float frameOffset = 0.0f; // frames added to the clock's (a script's setFrame)
};
std::vector<float> puppetBoneMatrices(const Puppet &puppet, const std::vector<PuppetLayerState> &layers, double time);
// The same into `out` (resized to 16 floats a bone), with no allocation once `out` and the thread's scratch have grown: what the
// renderer calls every frame. With `physics` (and the scene's `world` forces) the bones that declare a spring are simulated on top
// of the animated pose before the matrices are worked out (puppet/bonephysics.h); without it the pose is the animations' alone.
// `worlds`, when given, receives every bone's animated (and simulated) world matrix.
void puppetBoneMatrices(const Puppet &puppet, const std::vector<PuppetLayerState> &layers, double time, std::vector<float> &out,
                        BonePhysicsState *physics = nullptr, const BoneWorld *world = nullptr, std::vector<Mat4> *worlds = nullptr);
// The matrix of every attachment point of the puppet in its mesh space at `time`: its bone's animated world matrix (physics
// included) times the point's own. `out` is resized to puppet.attachments.
void puppetAttachmentPoses(const Puppet &puppet, const std::vector<PuppetLayerState> &layers, double time, std::vector<Mat4> &out,
                           BonePhysicsState *physics = nullptr, const BoneWorld *world = nullptr);
// Fills puppet.bindFrames and puppet.inverseBindWorld from its bones (the reader does it; a puppet built by hand may call it).
void preparePuppetBind(Puppet &puppet);

// 3D mesh objects (`"model": "models/x/x.mdl"` in scene.json): the same container with a mesh in one or more parts, each drawn
// with its own material, and for a skinned mesh the puppet's skeleton and animations after the parts. docs/scene-vk/formats.md
// (meshes) describes the layout as read from the library's models:
//
//   "MDLV0007|16|19|21|23\0"  9 bytes
//   u32 flags, u32 1, u32 partCount   flags 0x0f (0x0b: no tangents); 0x0180000f a skinned mesh
//   per part, after zero bytes between parts (6 in version 23, 2 in 21):
//     NUL string material      "materials/models/x/name.json"
//     u32 wideIndices          0: u16 indices, 1: u32 indices (the big models)
//     6f bounds                min xyz, max xyz (from version 19 on)
//     u32 flags                the vertex format (from version 16 on; version 7 has the header's)
//     u32 vertexBytes, vertices  0x0f: position 3f, normal 3f, tangent 4f, uv 2f (48 bytes); 0x0b: no tangent (32 bytes);
//                              0x0180000f: the puppet vertex, bone indices 4 x u32 and weights 4f before the uv (80 bytes)
//     u32 indexBytes, indices  triangles, counter-clockwise seen from outside
//   zero bytes, then for a skinned mesh the puppet's skeleton (MDLS) and animations (MDLA), and sections the draw does not
//   read (MDMP: morph targets, MDAT: attachments, MDLE)
inline constexpr size_t kMeshVertexStride = 48;
inline constexpr size_t kMaxMeshParts = 256;
inline constexpr size_t kMaxMeshVertices = 4000000; // over all parts (192 MB of vertices; the largest model of the library has 3.1 million)
inline constexpr size_t kMaxMeshIndices = 16000000;

struct MeshPart {
    std::string material;          // the material path the part names
    std::vector<uint8_t> vertices; // vertexCount * stride bytes, as in the file (or widened to 48), dropped once on the GPU
    std::vector<uint8_t> indices;  // indexCount indices as in the file: u16, or u32 with wideIndices (dropped likewise)
    bool wideIndices = false;
    size_t stride = kMeshVertexStride; // 48, or kPuppetVertexStride (80) for a skinned mesh
    size_t vertexCount = 0;
    size_t indexCount = 0;
    Vec3 boundsMin, boundsMax;
    uint32_t index(size_t i) const; // the i-th index (while the bytes are held)
};

struct MeshModel {
    std::vector<MeshPart> parts;
    bool skinned = false;      // the parts have the puppet vertex and `rig` holds the skeleton and animations
    Puppet rig;                // a skinned mesh's bones, animations, usedBones and boneSlots (no vertices: the parts hold them)
    bool morphTargets = false; // an MDMP section: blend shapes the draw leaves at rest
    bool attachments = false;  // an MDAT section
};

// Parses a mesh model; nullopt with a reason in `error` for anything it does not understand or that is over the limits.
std::optional<MeshModel> parseMeshModel(std::span<const uint8_t> bytes, std::string *error = nullptr);

} // namespace murasaki::scene
