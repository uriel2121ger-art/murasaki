#pragma once

// GLSL 450 -> SPIR-V through glslang, for Vulkan 1.3 / SPIR-V 1.6. In milestone 1 it compiles the engine's own
// shaders (the image-layer quad and the final scaling pass); milestone 2 puts the Wallpaper Engine dialect
// translator in front of it.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace murasaki::scene {

enum class ShaderStage { Vertex, Fragment };

// Empty result on failure, with the compiler's message in `error`. Thread safe.
//
// Results are cached on disk in $XDG_CACHE_HOME/murasaki/shader-cache (default ~/.cache/murasaki/shader-cache), one
// file per shader keyed by a hash of the stage, the source and the compiler setup. A hit never starts glslang, whose
// start-up costs more than compiling a small shader. MURASAKI_SHADER_CACHE=0 turns the cache off. Failures are not
// cached (they are bugs in the shader source, found the first time it is compiled).
std::vector<uint32_t> compileGlsl(ShaderStage stage, std::string_view source, std::string &error);

// Runs only the preprocessor (macros expanded, `#if` resolved) over text that has no `#version` line. Used by the
// dialect translator to learn which declarations are live. Empty result on failure, with the message in `error`.
std::string preprocessGlsl(ShaderStage stage, std::string_view source, std::string &error);

// How many times this process started the compiler (preprocessing or compiling; answers from a cache are not counted).
// Every source is compiled once per process: the answer, a failure included, is kept by a hash of the text. For the tests.
uint64_t glslCompilerRuns();

// The glslang version this build compiles with, for cache keys.
std::string compilerVersion();

// The directory the cache uses (empty when it is disabled or no home directory is known).
std::string shaderCacheDirectory();

} // namespace murasaki::scene
