#pragma once

#include <array>
#include <string_view>

namespace murasaki::puppet {

// The names a puppet reports (docs/pets/puppets.md, section 10): a reason of `murasaki-puppet --check` (the item is refused), a degraded entry of the check (the item
// is accepted, with a gap), or a degrade of the overlay's Windows() while the engine runs. An entry printed is a name below, alone or followed by ": " and the file, limit or value it
// is about; a caller matches the part before the first ": ". The table is the one list: the document lists the same rows and a test keeps them equal.
enum class NameKind { Refused, Degraded, Runtime };

struct Name {
    NameKind kind;
    std::string_view text;
    std::string_view meaning;
};

inline constexpr std::string_view kNotAPuppet = "puppet: not a puppet";
inline constexpr std::string_view kUnsafePath = "puppet: unsafe path";
inline constexpr std::string_view kFileMissing = "puppet: file missing";
inline constexpr std::string_view kTooLarge = "puppet: too large";
inline constexpr std::string_view kManifestInvalid = "puppet: manifest invalid";
inline constexpr std::string_view kUnsupportedManifestVersion = "puppet: unsupported manifest version";
inline constexpr std::string_view kUnsupportedFormat = "puppet: unsupported format";
inline constexpr std::string_view kUnsupportedMocVersion = "puppet: unsupported moc version";
inline constexpr std::string_view kPayloadNotRead = "puppet: payload not read";
inline constexpr std::string_view kNeedsCubismRuntime = "puppet: needs the Cubism runtime";
inline constexpr std::string_view kCoreUnavailable = "puppet: core unavailable";
inline constexpr std::string_view kPhysicsRigSkipped = "puppet: physics rig skipped";
inline constexpr std::string_view kMotionSkipped = "puppet: motion skipped";
inline constexpr std::string_view kResolutionReduced = "puppet: resolution reduced";
inline constexpr std::string_view kEngineRestarted = "puppet: engine restarted";
inline constexpr std::string_view kBlockedAfterCrashes = "puppet: blocked after crashes";
inline constexpr std::string_view kNoGpu = "puppet: no GPU";

inline constexpr std::array<Name, 17> kNames = {{
    {NameKind::Refused, kNotAPuppet, "the folder holds no model3.json and no .inp file, or holds both, or several of one"},
    {NameKind::Refused, kUnsafePath,
     "the folder holds a symbolic link or a file that is not a regular file, or a path the manifest names is absolute, leaves the folder or breaks the path rules"},
    {NameKind::Refused, kFileMissing, "a file the manifest names is not in the folder"},
    {NameKind::Refused, kTooLarge, "a count or a size is over a limit of the limits table (the entry says which)"},
    {NameKind::Refused, kManifestInvalid,
     "the manifest is not strict JSON, repeats a key, nests too deep, lacks a required key or has a value of the wrong type"},
    {NameKind::Refused, kUnsupportedManifestVersion, "the manifest's Version is not one Murasaki reads (3)"},
    {NameKind::Refused, kUnsupportedFormat, "a named file has an extension its role does not allow, or a container does not start with a known signature"},
    {NameKind::Refused, kUnsupportedMocVersion, "the Cubism runtime reports a .moc3 version it does not support (reserved: the check cannot tell before the runtime is loaded)"},
    {NameKind::Degraded, kPayloadNotRead,
     "the check looked at the container's signature and size only (loadManifest); the puppet inside was not read, so it is not known to be drawable (checkItem reads it)"},
    {NameKind::Degraded, kNeedsCubismRuntime,
     "the model is a Cubism model, which draws only through a Core library the user installs (Murasaki ships none); without it the item cannot be shown"},
    {NameKind::Runtime, kCoreUnavailable, "the Cubism runtime could not be loaded: no path set, a path that fails the checks, or a library that does not load"},
    {NameKind::Runtime, kPhysicsRigSkipped, "a physics rig whose input or output parameter the model does not have is not simulated"},
    {NameKind::Runtime, kMotionSkipped, "a motion that cannot be read or names nothing the model has is not played"},
    {NameKind::Runtime, kResolutionReduced, "the picture is drawn smaller than the pet's size asks for, because of the pixel limit"},
    {NameKind::Runtime, kEngineRestarted, "the puppet engine stopped and was started again"},
    {NameKind::Runtime, kBlockedAfterCrashes, "the model made the engine crash three times and is not started any more"},
    {NameKind::Runtime, kNoGpu, "no usable Vulkan device for the engine: puppets are not shown"},
}};

} // namespace murasaki::puppet
