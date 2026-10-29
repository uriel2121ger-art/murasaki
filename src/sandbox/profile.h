#pragma once

#include "landlock.h"

#include <string>
#include <utility>
#include <vector>

namespace murasaki::sandbox {

// The environment a profile reads, passed in so it can be tested.
struct ProfileEnvironment {
    std::string home;
    std::string xdgCacheHome;   // empty: $home/.cache
    std::string xdgConfigHome;  // empty: $home/.config
    std::string xdgRuntimeDir;  // empty: none (no per-user runtime directory)
};

struct SceneProfileInput {
    std::string itemDir;   // the Workshop item the engine renders (read only)
    std::string assetsDir; // Wallpaper Engine's assets directory (read only), may be empty
    std::string program;   // the engine's executable, as given to the helper (looked up in PATH when it has no '/')
};

// What the scene engines (murasaki-scene-vk) need and nothing more: the item and
// the assets to read; the system's libraries, fonts, Vulkan and GPU description to read; the GPU device nodes and the
// shader caches to write; no TCP. Their sockets (PipeWire, the bridge) are not files this policy has to open: a
// socket that is connected to is not a filesystem right, and the bridge descriptor is inherited.
Policy sceneProfile(const SceneProfileInput &input, const ProfileEnvironment &environment);

struct ImportProfileInput {
    std::string program;                // murasaki-petimport, as given to the helper
    std::vector<std::string> sources;   // the files or folders the job reads (the single source file, or the image folder)
    std::string outDir;                 // the one directory the job writes (its output root)
};

// What the pet import helper may see and nothing more: the system's libraries and Qt's plugins (read), the helper's own libraries, the named sources (read), the
// output directory (read and write), a few devices and its own /proc entry; no home, no GPU, no other path, no TCP. The caller (main.cpp) also runs it in a mount namespace with no runtime directory, which is what hides the session bus and the display sockets: Landlock alone cannot. The caller
// (murasaki-sandbox --profile import) also sets the limits and the environment below.
Policy importProfile(const ImportProfileInput &input);

// The whole environment of the import helper (the caller's is dropped: no QT_PLUGIN_PATH, QT_IMAGEIO_MAXALLOC, LD_PRELOAD, LD_LIBRARY_PATH, no session bus address).
std::vector<std::pair<std::string, std::string>> importProfileEnvironment();

struct ScreenmatchProfileInput {
    std::string program; // murasaki-screenmatch, as given to the helper
};

// The screen matching helper reads live pixels of the whole screen, so it gets the least of everything: the system's libraries and PipeWire's own configuration and plugins (read), its own
// binary, a few devices and its own /proc entry. No user path of any kind, nothing writable at all, no TCP. (Landlock is only part of it: the helper also loads a seccomp filter at its start that
// refuses socket, connect, bind, listen, accept and exec, so that the one PipeWire descriptor and the one daemon channel it inherits are all it can ever talk to.)
Policy screenmatchProfile(const ScreenmatchProfileInput &input);

// The whole environment of the helper (the caller's is dropped).
std::vector<std::pair<std::string, std::string>> screenmatchProfileEnvironment();

struct PuppetProfileInput {
    std::string itemDir;   // the puppet folder the engine reads (read only)
    std::string corePath;  // the one extra read-only file of the optional Cubism path (plan 4.4): a Core library the user installed; empty: none
    std::string program;   // murasaki-puppet, as given to the helper
};

// The puppet engine (murasaki-puppet --engine) draws a model that is hostile input, on the GPU, and talks to the overlay over the inherited bridge socket. So: the model folder (read), the
// system's libraries and the Vulkan and Mesa description (read), the GPU render nodes and its own shader caches (write), the process's own /proc entries, and nothing else - no home, no fonts, no audio, no
// session or display (the mount namespace hides the sockets), no TCP. The Core file, when there is one, is the only other path. A variation of the scene profile with everything a scene needs and a puppet does not removed.
Policy puppetProfile(const PuppetProfileInput &input, const ProfileEnvironment &environment);

// Environment variables the profile sets for the program (when the user has not set them), so the GPU drivers keep
// their shader caches in the one directory the policy lets them write. Name, value.
std::vector<std::pair<std::string, std::string>> sceneProfileEnvironment(const ProfileEnvironment &environment);

// The same variables for the puppet engine. Its caches are its own (puppet-shader-cache, puppet-gpu-cache): a model is hostile input, and a cache a scene engine reads later must not be
// something it could have planted.
std::vector<std::pair<std::string, std::string>> puppetProfileEnvironment(const ProfileEnvironment &environment);

// Creates the puppet engine's writable cache directories.
void preparePuppetProfile(const ProfileEnvironment &environment);

// Creates the writable cache directories of the profile (the program can not create them in a parent it may not write).
void prepareSceneProfile(const ProfileEnvironment &environment);

} // namespace murasaki::sandbox
