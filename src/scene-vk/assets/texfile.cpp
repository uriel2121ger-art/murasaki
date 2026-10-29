#include "assets/texfile.h"

namespace murasaki::scene {

namespace {
constexpr size_t kHeaderBytes = 1 << 20; // a texture's header and its first mip's header are a few dozen bytes
}

std::optional<Tex> peekVideoTexFile(const Vfs &vfs, std::string_view path, const TexReadOptions &options)
{
    const auto where = vfs.locate(path);
    if (!where || where->length <= kBigTexBytes) {
        return std::nullopt;
    }
    const auto head = vfs.readPrefix(path, kHeaderBytes);
    if (!head) {
        return std::nullopt;
    }
    TexReadOptions peek = options;
    peek.videoInPlace = true;
    std::string ignored;
    auto tex = parseTex(std::span<const uint8_t>(head->data(), head->size()), peek, &ignored);
    if (!tex || !tex->hasVideo() || tex->videoSize == 0 || tex->videoOffset + tex->videoSize > where->length) {
        return std::nullopt;
    }
    tex->videoFile = where->file;
    tex->videoFileOffset = where->offset + tex->videoOffset;
    return tex;
}

std::optional<Tex> readTexFile(const Vfs &vfs, std::string_view path, const TexReadOptions &options, std::string *error)
{
    if (auto tex = peekVideoTexFile(vfs, path, options)) {
        return tex;
    }
    const auto bytes = vfs.read(path);
    if (!bytes) {
        if (error) {
            *error = "not found";
        }
        return std::nullopt;
    }
    return parseTex(std::span<const uint8_t>(bytes->data(), bytes->size()), options, error);
}

} // namespace murasaki::scene
