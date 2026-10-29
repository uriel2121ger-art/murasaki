#pragma once

// Reading a .tex from a Vfs without holding all of it when it is a video: a video texture carries an MP4 stream of tens to hundreds of
// megabytes, and the stream is played from where it lies (a region of the package), not from a copy in memory.

#include "assets/tex.h"
#include "assets/vfs.h"

#include <optional>
#include <string>
#include <string_view>

namespace murasaki::scene {

// Files over this are looked at through their first bytes before they are read whole.
inline constexpr uint64_t kBigTexBytes = 16ull << 20;

// A big video texture, looked at in place: its Tex has `videoFile`, `videoFileOffset` and `videoSize`, and no bytes of the stream. Nullopt for
// anything else (a small file, a picture), which the caller then reads whole.
std::optional<Tex> peekVideoTexFile(const Vfs &vfs, std::string_view path, const TexReadOptions &options = {});

// The same as parseTex() over the file's bytes, except that a big video texture is neither read nor copied: its Tex has `videoFile` and
// `videoFileOffset` (the stream's place on disk) and `videoSize`, and `video` stays empty. Other files are read whole.
std::optional<Tex> readTexFile(const Vfs &vfs, std::string_view path, const TexReadOptions &options = {}, std::string *error = nullptr);

} // namespace murasaki::scene
