#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace murasaki::puppet {

// Every bound on an untrusted puppet model folder (docs/pets/puppets.md, section 8): the model formats state no limits of their own, so these are ours. A reader checks a
// count or a size against its limit before it allocates, and a value over a limit is refused, never clamped. Qt-free: murasaki-puppet, the process that reads hostile models,
// includes this header. docs/pets/puppets.md lists the same rows (a test keeps them equal).

inline constexpr std::int64_t kMaxManifestBytes = 1048576; // the manifest (`*.model3.json`)
inline constexpr std::int64_t kMaxJsonDepth = 16; // nesting of arrays and objects in any JSON file
inline constexpr std::int64_t kMaxJsonValues = 65536; // values (including keys' values) in one JSON file
inline constexpr std::int64_t kMaxJsonStringBytes = 4096; // one string, after unescaping
inline constexpr std::int64_t kMaxIdBytes = 128; // a parameter, part, group, hit-area or motion-group id or name
inline constexpr std::int64_t kMaxPathBytes = 1024; // a path named by a manifest
inline constexpr std::int64_t kMaxPathComponentBytes = 255; // one component of such a path
inline constexpr std::int64_t kMaxItemFiles = 1024; // entries (files and folders) in the item, all levels
inline constexpr std::int64_t kMaxItemDirDepth = 8; // folders below the item folder
inline constexpr std::int64_t kMaxItemBytes = 1073741824; // the sum of the sizes of the files of the item
inline constexpr std::int64_t kMaxMocBytes = 67108864; // the `.moc3` file
inline constexpr std::int64_t kMaxInpBytes = 268435456; // an Inochi2D `.inp` container
inline constexpr std::int64_t kMaxPuppetJsonValues = 1572864; // values (numbers and lists included) in the JSON payload of an `.inp`
inline constexpr std::int64_t kMaxWarpCandidates = 256; // triangles tried for one point of a mesh group
inline constexpr std::int64_t kMaxWarpCellEntries = 1048576; // grid entries of all the mesh groups of a puppet together
inline constexpr std::int64_t kMaxWarpTriangles = 16384; // distinct triangles of one mesh group (repeated and flat ones do not count)
inline constexpr std::int64_t kMaxCoordinate = 10000000; // the largest magnitude of a coordinate, offset, scale, zsort, binding value, parameter bound or physics number in a puppet or a physics file
inline constexpr std::int64_t kMaxTotalTextureBytes = 134217728; // the encoded textures of one `.inp` together
inline constexpr std::int64_t kMaxInpJsonBytes = 16777216; // the JSON payload of an Inochi2D `.inp`
inline constexpr std::int64_t kMaxTextures = 16; // texture files of one model
inline constexpr std::int64_t kMaxTextureFileBytes = 33554432; // one texture file
inline constexpr std::int64_t kMaxTextureSide = 8192; // width or height of a texture, pixels
inline constexpr std::int64_t kMaxTexturePixels = 33554432; // pixels of one texture
inline constexpr std::int64_t kMaxTotalTexturePixels = 67108864; // pixels of all the textures of a model
inline constexpr std::int64_t kMaxMotionGroups = 64; // groups named in `Motions`
inline constexpr std::int64_t kMaxMotionsPerGroup = 64; // motions in one group
inline constexpr std::int64_t kMaxMotions = 512; // motions in all groups
inline constexpr std::int64_t kMaxFadingMotions = 16; // motions fading out beside the current one
inline constexpr std::int64_t kMaxMotionSeconds = 3600; // the duration of a motion, and the time of any of its points, seconds
inline constexpr std::int64_t kMaxMotionQueue = 8; // motions waiting behind the one playing
inline constexpr std::int64_t kMaxFadeSeconds = 3600; // a fade time named by the manifest, seconds
inline constexpr std::int64_t kMaxExpressions = 128; // expressions
inline constexpr std::int64_t kMaxHitAreas = 128; // hit areas
inline constexpr std::int64_t kMaxParameterGroups = 16; // entries of `Groups`
inline constexpr std::int64_t kMaxIdsPerGroup = 64; // ids in one group
inline constexpr std::int64_t kMaxMotionFileBytes = 4194304; // one motion file
inline constexpr std::int64_t kMaxMotionJsonValues = 1048576; // values (numbers included) in one motion file
inline constexpr std::int64_t kMaxExpressionFileBytes = 262144; // one expression file
inline constexpr std::int64_t kMaxPhysicsFileBytes = 1048576; // the physics file
inline constexpr std::int64_t kMaxPoseFileBytes = 65536; // the pose file
inline constexpr std::int64_t kMaxDisplayInfoFileBytes = 1048576; // the display-info file
inline constexpr std::int64_t kMaxUserDataFileBytes = 1048576; // the user-data file (read by no one yet; sized for the check)
inline constexpr std::int64_t kMaxMotionSyncFileBytes = 1048576; // the motion-sync file
inline constexpr std::int64_t kMaxParameters = 512; // parameters of a model
inline constexpr std::int64_t kMaxParts = 512; // parts of a model
inline constexpr std::int64_t kMaxDrawables = 2048; // drawables (art meshes) of a model
inline constexpr std::int64_t kMaxVerticesPerMesh = 32768; // vertices of one mesh
inline constexpr std::int64_t kMaxVerticesTotal = 524288; // vertices of all meshes
inline constexpr std::int64_t kMaxIndicesPerMesh = 196608; // triangle indices of one mesh
inline constexpr std::int64_t kMaxIndicesTotal = 1572864; // triangle indices of all meshes
inline constexpr std::int64_t kMaxDeformerDepth = 64; // depth of the chain of deformer parents
inline constexpr std::int64_t kMaxMasksPerPart = 16; // masks of one part
inline constexpr std::int64_t kMaxAxisPoints = 64; // keypoints on one axis of a parameter
inline constexpr std::int64_t kMaxBindings = 16384; // parameter bindings of an Inochi2D puppet
inline constexpr std::int64_t kMaxNodes = 4096; // nodes of an Inochi2D puppet
inline constexpr std::int64_t kMaxCurvesPerMotion = 1024; // curves of one motion
inline constexpr std::int64_t kMaxSegmentsPerMotion = 262144; // segments of one motion
inline constexpr std::int64_t kMaxPointsPerMotion = 1048576; // points of one motion
inline constexpr std::int64_t kMaxPhysicsRigs = 128; // physics settings
inline constexpr std::int64_t kMaxPhysicsInputs = 1024; // inputs, all rigs
inline constexpr std::int64_t kMaxPhysicsOutputs = 1024; // outputs, all rigs
inline constexpr std::int64_t kMaxPhysicsVertices = 2048; // pendulum vertices, all rigs
inline constexpr std::int64_t kMaxPhysicsStepsPerUpdate = 8; // fixed steps one update simulates at most
inline constexpr std::int64_t kMaxPhysicsIoPerRig = 32; // inputs, and outputs, of one physics setting
inline constexpr std::int64_t kMaxPhysicsVerticesPerRig = 32; // pendulum vertices of one physics setting
inline constexpr std::int64_t kMaxPoseGroups = 64; // groups of the pose file
inline constexpr std::int64_t kMaxPoseNodes = 512; // nodes of the pose file
inline constexpr std::int64_t kMaxPuppetPixels = 2048; // the long side of a frame the engine renders, pixels (plan 4.2)
inline constexpr std::int64_t kMinPuppetMaxPixels = 1000000; // the lowest `puppetMaxPixels` setting
inline constexpr std::int64_t kMaxPuppetMaxPixels = 16000000; // the highest `puppetMaxPixels` setting
inline constexpr std::int64_t kDefaultPuppetMaxPixels = 4194304; // the default `puppetMaxPixels`
inline constexpr std::int64_t kMaxTotalAssetBytes = 33554432; // the motion, expression, physics and pose files an item names, read together by loadSessionAssets (a file named twice is read once); what is over is left out and named
inline constexpr std::int64_t kSlowRecoveryFrames = 300; // fast frames in a row before the engine doubles its own reduced frame rate again
inline constexpr std::int64_t kStencilClearShare = 16; // a stencil clear costs 1/this of a picture in the estimate (a fast clear, not a drawn picture)
inline constexpr std::int64_t kMaxFrameCostWork = 4194304; // triangles the cost estimate of one frame may look at; the items left are each counted as a whole picture
inline constexpr std::int64_t kMinSlowFps = 5; // the lowest frame rate the engine falls back to on its own when its GPU work is slow
inline constexpr std::int64_t kMaxOutputCoordinate = 1000000000000; // the largest magnitude of a vertex position the model hands the renderer (model units); a computed position beyond it, unlike a number read from a file, is clamped to it, so no float conversion downstream is out of range
inline constexpr std::int64_t kCrashForgetHours = 24; // a crash this long after the previous one starts the count again
inline constexpr std::int64_t kEngineLongRunSeconds = 1800; // an engine that ran this long (30 minutes) before it crashed was not broken by its model: the count starts again at one. A model that survives a minute and dies is still the model's
inline constexpr std::int64_t kMaxPositionVertices = 8388608; // vertex positions one frame may write for meshes and mask sources together
inline constexpr std::int64_t kMaxHitMaskWork = 4194304; // work units (about 10 ns each) one hit mask may cost, some 40 ms; the rest of the triangles of an item are covered by the one box that holds them
inline constexpr std::int64_t kMaxSlowFrames = 3; // slow frames in a row before the engine halves its own frame rate (to a floor of kMinSlowFps); it never gives the model up for being slow
inline constexpr std::int64_t kMaxSlowFrameMs = 250; // a frame whose GPU work takes longer than this (milliseconds) counts as slow
inline constexpr std::int64_t kMaxHeavyFrames = 30; // frames in a row over kMaxOverdrawFactor before the engine gives up on the model (puppet: too heavy)
inline constexpr std::int64_t kMaxOverdrawFactor = 64; // the area the triangles of one frame cover (an upper bound worked out from their boxes), mask sources drawn again and group images included, as a multiple of the picture's area; a frame over it is not drawn

struct Limit {
    std::string_view name;
    std::int64_t value;
};

inline constexpr std::array<Limit, 85> kLimits = {{
    {"kMaxManifestBytes", kMaxManifestBytes},
    {"kMaxJsonDepth", kMaxJsonDepth},
    {"kMaxJsonValues", kMaxJsonValues},
    {"kMaxJsonStringBytes", kMaxJsonStringBytes},
    {"kMaxIdBytes", kMaxIdBytes},
    {"kMaxPathBytes", kMaxPathBytes},
    {"kMaxPathComponentBytes", kMaxPathComponentBytes},
    {"kMaxItemFiles", kMaxItemFiles},
    {"kMaxItemDirDepth", kMaxItemDirDepth},
    {"kMaxItemBytes", kMaxItemBytes},
    {"kMaxMocBytes", kMaxMocBytes},
    {"kMaxInpBytes", kMaxInpBytes},
    {"kMaxPuppetJsonValues", kMaxPuppetJsonValues},
    {"kMaxWarpCandidates", kMaxWarpCandidates},
    {"kMaxWarpCellEntries", kMaxWarpCellEntries},
    {"kMaxWarpTriangles", kMaxWarpTriangles},
    {"kMaxCoordinate", kMaxCoordinate},
    {"kMaxTotalTextureBytes", kMaxTotalTextureBytes},
    {"kMaxInpJsonBytes", kMaxInpJsonBytes},
    {"kMaxTextures", kMaxTextures},
    {"kMaxTextureFileBytes", kMaxTextureFileBytes},
    {"kMaxTextureSide", kMaxTextureSide},
    {"kMaxTexturePixels", kMaxTexturePixels},
    {"kMaxTotalTexturePixels", kMaxTotalTexturePixels},
    {"kMaxMotionGroups", kMaxMotionGroups},
    {"kMaxMotionsPerGroup", kMaxMotionsPerGroup},
    {"kMaxMotions", kMaxMotions},
    {"kMaxFadingMotions", kMaxFadingMotions},
    {"kMaxMotionSeconds", kMaxMotionSeconds},
    {"kMaxMotionQueue", kMaxMotionQueue},
    {"kMaxFadeSeconds", kMaxFadeSeconds},
    {"kMaxExpressions", kMaxExpressions},
    {"kMaxHitAreas", kMaxHitAreas},
    {"kMaxParameterGroups", kMaxParameterGroups},
    {"kMaxIdsPerGroup", kMaxIdsPerGroup},
    {"kMaxMotionFileBytes", kMaxMotionFileBytes},
    {"kMaxMotionJsonValues", kMaxMotionJsonValues},
    {"kMaxExpressionFileBytes", kMaxExpressionFileBytes},
    {"kMaxPhysicsFileBytes", kMaxPhysicsFileBytes},
    {"kMaxPoseFileBytes", kMaxPoseFileBytes},
    {"kMaxDisplayInfoFileBytes", kMaxDisplayInfoFileBytes},
    {"kMaxUserDataFileBytes", kMaxUserDataFileBytes},
    {"kMaxMotionSyncFileBytes", kMaxMotionSyncFileBytes},
    {"kMaxParameters", kMaxParameters},
    {"kMaxParts", kMaxParts},
    {"kMaxDrawables", kMaxDrawables},
    {"kMaxVerticesPerMesh", kMaxVerticesPerMesh},
    {"kMaxVerticesTotal", kMaxVerticesTotal},
    {"kMaxIndicesPerMesh", kMaxIndicesPerMesh},
    {"kMaxIndicesTotal", kMaxIndicesTotal},
    {"kMaxDeformerDepth", kMaxDeformerDepth},
    {"kMaxMasksPerPart", kMaxMasksPerPart},
    {"kMaxAxisPoints", kMaxAxisPoints},
    {"kMaxBindings", kMaxBindings},
    {"kMaxNodes", kMaxNodes},
    {"kMaxCurvesPerMotion", kMaxCurvesPerMotion},
    {"kMaxSegmentsPerMotion", kMaxSegmentsPerMotion},
    {"kMaxPointsPerMotion", kMaxPointsPerMotion},
    {"kMaxPhysicsRigs", kMaxPhysicsRigs},
    {"kMaxPhysicsInputs", kMaxPhysicsInputs},
    {"kMaxPhysicsOutputs", kMaxPhysicsOutputs},
    {"kMaxPhysicsVertices", kMaxPhysicsVertices},
    {"kMaxPhysicsStepsPerUpdate", kMaxPhysicsStepsPerUpdate},
    {"kMaxPhysicsIoPerRig", kMaxPhysicsIoPerRig},
    {"kMaxPhysicsVerticesPerRig", kMaxPhysicsVerticesPerRig},
    {"kMaxPoseGroups", kMaxPoseGroups},
    {"kMaxPoseNodes", kMaxPoseNodes},
    {"kMaxPuppetPixels", kMaxPuppetPixels},
    {"kMinPuppetMaxPixels", kMinPuppetMaxPixels},
    {"kMaxPuppetMaxPixels", kMaxPuppetMaxPixels},
    {"kDefaultPuppetMaxPixels", kDefaultPuppetMaxPixels},
    {"kMaxTotalAssetBytes", kMaxTotalAssetBytes},
    {"kSlowRecoveryFrames", kSlowRecoveryFrames},
    {"kStencilClearShare", kStencilClearShare},
    {"kMaxFrameCostWork", kMaxFrameCostWork},
    {"kMinSlowFps", kMinSlowFps},
    {"kMaxOutputCoordinate", kMaxOutputCoordinate},
    {"kCrashForgetHours", kCrashForgetHours},
    {"kEngineLongRunSeconds", kEngineLongRunSeconds},
    {"kMaxPositionVertices", kMaxPositionVertices},
    {"kMaxHitMaskWork", kMaxHitMaskWork},
    {"kMaxSlowFrames", kMaxSlowFrames},
    {"kMaxSlowFrameMs", kMaxSlowFrameMs},
    {"kMaxHeavyFrames", kMaxHeavyFrames},
    {"kMaxOverdrawFactor", kMaxOverdrawFactor},
}};

} // namespace murasaki::puppet
