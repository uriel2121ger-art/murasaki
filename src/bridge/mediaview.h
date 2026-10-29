#pragma once

// Murasaki's extension of the bridge protocol: media, version 4 (see docs/design/specs/2026-10-21-media-thumbnail-design.md).
// Like SpanView it stays out of protocol.h: a producer of the base protocol announces version 2, ignores the unknown message
// types, and a consumer never sends these to a producer that announced less than kMediaVersion.
//
// The types of Murasaki's extension (no number is used twice): 43 SpanView (spanview.h), 44 Media, 45 MediaThumbnail, 46 MediaWants (here),
// 47 ShortcutIcon and 48 ShortcutLaunch (shortcutview.h, version 5).
//
//   Media (consumer -> producer): the state of the player the host follows, as JSON in the Properties encoding
//     (u32 length + UTF-8, length <= kMaxMediaJson): {"enabled":bool,"state":"playing|paused|stopped","title","artist",
//     "albumTitle","albumArtist","genres":[..],"position":s,"positionAt":ms,"rate":x,"duration":s}. The host has cut and
//     cleaned the strings; the engine still treats every field as untrusted.
//   MediaThumbnail (consumer -> producer): the cover of the track, scaled by the host. One sealed memfd of exactly
//     width * height * 4 bytes of RGBA8 (straight alpha), each side 1..kMaxThumbnailSide. Width and height 0 and no fd:
//     the track has no cover.

#include "protocol.h"
#include "channel.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace murasaki::bridge {

inline constexpr uint32_t kMediaVersion = 4;
inline constexpr MsgType kMediaType = static_cast<MsgType>(44);
inline constexpr MsgType kMediaThumbnailType = static_cast<MsgType>(45);
// Producer to consumer: whether the scene shows the cover or has scripts that listen to the media events. Sent once, after the scene
// is loaded; until the consumer has it, it sends the engine no media at all (the host keeps what plays to itself).
inline constexpr MsgType kMediaWantsType = static_cast<MsgType>(46);
inline constexpr uint32_t kMaxMediaJson = 16 * 1024;
inline constexpr uint32_t kMaxThumbnailSide = 512;

namespace msg {

struct MediaThumbnail {
    static constexpr MsgType kType = kMediaThumbnailType;
    uint32_t width;
    uint32_t height;
    uint32_t generation; // the host's counter of covers: a repeated one is the same picture
    uint32_t reserved;   // zero
};
static_assert(sizeof(MediaThumbnail) == 16 && std::is_trivially_copyable_v<MediaThumbnail>);
static_assert(std::has_unique_object_representations_v<MediaThumbnail>, "MediaThumbnail must have no padding");

inline bool validate(const MediaThumbnail &m, size_t fdCount)
{
    if (m.width == 0 && m.height == 0) {
        return fdCount == 0;
    }
    return fdCount == 1 && m.width >= 1 && m.width <= kMaxThumbnailSide && m.height >= 1 && m.height <= kMaxThumbnailSide;
}

struct MediaWants {
    static constexpr MsgType kType = kMediaWantsType;
    uint32_t wants; // 0 or 1
};
static_assert(sizeof(MediaWants) == 4 && std::is_trivially_copyable_v<MediaWants>);

inline bool validate(const MediaWants &m, size_t fdCount)
{
    return fdCount == 0 && m.wants <= 1;
}

} // namespace msg

// A sealed memfd holding `rgba` (shrink, grow, write and seal seals: the receiver can rely on its size and contents), or an
// invalid fd when the kernel refuses.
UniqueFd sealedPixelsFd(std::span<const uint8_t> rgba);

// The pixels of a received thumbnail fd: it must be a regular file of exactly width * height * 4 bytes carrying the shrink, grow
// and write seals. Copied out, so the fd can close. Nullopt for anything else.
std::optional<std::vector<uint8_t>> readSealedPixels(UniqueFd fd, uint32_t width, uint32_t height);

} // namespace murasaki::bridge
