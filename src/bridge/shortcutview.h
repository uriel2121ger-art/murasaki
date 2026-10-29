#pragma once

// Murasaki's extension of the bridge protocol: user shortcuts, version 5 (see docs/design/specs/2026-10-21-user-shortcuts-design.md).
// Like SpanView and the media messages it stays out of protocol.h: a producer of the base protocol announces version 2, ignores the unknown
// message types, and a consumer never sends these to a producer that announced less than kShortcutVersion. Version 5 includes 4's messages.
//
//   ShortcutIcon (consumer -> producer): the icon of the application the user chose for a `usershortcut` property, scaled by the host.
//     One sealed memfd of exactly width * height * 4 bytes of RGBA8 (straight alpha), each side 1..kMaxShortcutIconSide. Width and height
//     0 and no fd: the slot has no icon (the user cleared it). `name` is the property key the icon belongs to.
//   ShortcutLaunch (producer -> consumer): a script asked to start the application of the property `name`
//     (engine.openUserShortcut). The consumer decides whether anything starts (docs: the host checks a real click, its own assignment, the
//     user's switch and rate limits); the producer learns nothing back.
//
// The types of Murasaki's extension, so no number is used twice: 43 SpanView (spanview.h), 44 Media, 45 MediaThumbnail, 46 MediaWants
// (mediaview.h), 47 ShortcutIcon, 48 ShortcutLaunch (this file).

#include "channel.h"
#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <type_traits>

namespace murasaki::bridge {

inline constexpr uint32_t kShortcutVersion = 5;
inline constexpr MsgType kShortcutIconType = static_cast<MsgType>(47);
inline constexpr MsgType kShortcutLaunchType = static_cast<MsgType>(48);
inline constexpr uint32_t kMaxShortcutIconSide = 256;
inline constexpr size_t kShortcutNameBytes = 64; // the field; a name is 1 to 63 bytes and a NUL

// A property key as the messages carry it: 1 to 63 bytes of well formed UTF-8 with no control characters, then NUL padding to the end of
// the field (nothing but zeros after the first NUL).
inline bool validShortcutName(const char (&name)[kShortcutNameBytes])
{
    size_t length = 0;
    while (length < kShortcutNameBytes && name[length] != '\0') {
        ++length;
    }
    if (length == 0 || length >= kShortcutNameBytes) {
        return false;
    }
    for (size_t i = length; i < kShortcutNameBytes; ++i) {
        if (name[i] != '\0') {
            return false;
        }
    }
    // UTF-8: no overlong forms, surrogates or values above U+10FFFF; controls (below 0x20, 0x7f) refused
    for (size_t i = 0; i < length;) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7f) {
            return false;
        }
        size_t more = 0;
        uint32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if (c >= 0xC2 && c <= 0xDF) {
            more = 1;
            cp = c & 0x1F;
        } else if (c >= 0xE0 && c <= 0xEF) {
            more = 2;
            cp = c & 0x0F;
        } else if (c >= 0xF0 && c <= 0xF4) {
            more = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + more >= length) {
            return false; // cut short
        }
        for (size_t k = 1; k <= more; ++k) {
            const auto cc = static_cast<unsigned char>(name[i + k]);
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if ((more == 2 && cp < 0x800) || (more == 3 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

namespace msg {

struct ShortcutIcon {
    static constexpr MsgType kType = kShortcutIconType;
    uint32_t width;
    uint32_t height;
    uint32_t generation; // the host's counter for this slot: a repeated one is the same picture
    uint32_t reserved;   // zero
    char name[kShortcutNameBytes];
};
static_assert(sizeof(ShortcutIcon) == 80 && std::is_trivially_copyable_v<ShortcutIcon>);
static_assert(std::has_unique_object_representations_v<ShortcutIcon>, "ShortcutIcon must have no padding");

inline bool validate(const ShortcutIcon &m, size_t fdCount)
{
    if (m.reserved != 0 || !validShortcutName(m.name)) {
        return false;
    }
    if (m.width == 0 && m.height == 0) {
        return fdCount == 0;
    }
    return fdCount == 1 && m.width >= 1 && m.width <= kMaxShortcutIconSide && m.height >= 1 && m.height <= kMaxShortcutIconSide;
}

struct ShortcutLaunch {
    static constexpr MsgType kType = kShortcutLaunchType;
    uint32_t serial;   // the producer's counter, for the logs
    uint32_t reserved; // zero
    char name[kShortcutNameBytes];
};
static_assert(sizeof(ShortcutLaunch) == 72 && std::is_trivially_copyable_v<ShortcutLaunch>);
static_assert(std::has_unique_object_representations_v<ShortcutLaunch>, "ShortcutLaunch must have no padding");

inline bool validate(const ShortcutLaunch &m, size_t fdCount)
{
    return fdCount == 0 && m.reserved == 0 && validShortcutName(m.name);
}

// A message with `key` in its name field (the key must be 1 to 63 bytes: the caller checked); zero padded.
template <class T>
inline void setShortcutName(T &m, std::string_view key)
{
    for (char &c : m.name) {
        c = '\0';
    }
    for (size_t i = 0; i < key.size() && i < kShortcutNameBytes - 1; ++i) {
        m.name[i] = key[i];
    }
}

} // namespace msg
} // namespace murasaki::bridge
