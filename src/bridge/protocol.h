#pragma once

// Murasaki <-> scene engine bridge protocol, version 2.
//
// This file is shared verbatim with the bridge producers: plain C++20 and POSIX only, no Qt and no
// Murasaki headers.
//
// Wire format: every message is one SOCK_SEQPACKET datagram made of
//     MsgHeader { u32 type; u32 size; }   // size = number of payload bytes after the header
//     payload   (size bytes)
// All integers are little-endian (enforced by a static_assert) and every payload struct below is a
// trivially copyable POD with explicit padding. File descriptors travel as SCM_RIGHTS ancillary data of the same
// datagram, in the order documented per message.
//
// Naming: message structs live in murasaki::bridge::msg and transport names avoid X11 macros (Status, None,
// Bool, Success, True, False), so this header can be included next to Xlib.h and the producers' own types.
//
// Decoding accepts a payload that is longer than the struct (newer peers may append fields) but never reads
// past the declared payload size, and rejects one that is shorter. Unknown message types are ignored by
// callers (see channel.h).
//
// Properties carries variable-length JSON and therefore has its own encoding:
//     payload = u32 json_length (LE) followed by exactly json_length bytes of UTF-8 JSON (no NUL terminator).
//     json_length <= kMaxPropertiesJson (64 KiB) and payload size must equal 4 + json_length exactly.

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <concepts>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace murasaki::bridge {

static_assert(std::endian::native == std::endian::little, "the bridge protocol is little-endian only");

inline constexpr uint32_t kBridgeVersion = 2;

inline constexpr uint32_t kMaxPropertiesJson = 64 * 1024;
inline constexpr uint32_t kMaxDimension = 16384;
// Largest shared-memory ring: 3 frames of 16384x16384 RGBA8 (about 3.2 GiB). Anything bigger is a bug or an attack.
inline constexpr uint64_t kMaxRingBytes = 3ull * kMaxDimension * kMaxDimension * 4;
inline constexpr uint32_t kMaxBuffers = 3;
inline constexpr uint32_t kMaxPlanes = 4;
inline constexpr size_t kViewportNameSize = 64;
inline constexpr size_t kErrorTextSize = 200;

enum class MsgType : uint32_t {
    // producer (engine) -> consumer
    Hello = 1,
    Viewport = 2,
    DmabufBuffers = 3,
    ShmBuffers = 4,
    Frame = 5,
    Error = 6,
    // consumer -> producer
    Release = 32,
    RequestLinear = 33,
    RequestShm = 34,
    Pause = 35,
    Properties = 36,
    Volume = 37,
    Pointer = 38,
    Resize = 39,
    Scaling = 40,
    Fps = 41,
    Quit = 42,
};

struct MsgHeader {
    uint32_t type;
    uint32_t size; // payload bytes following the header
};
static_assert(sizeof(MsgHeader) == 8);

// ---- producer -> consumer ------------------------------------------------------------------------------

inline constexpr uint32_t kFormatRgba8 = 1;

namespace msg {

struct Hello {
    static constexpr MsgType kType = MsgType::Hello;
    uint32_t version;
    uint32_t pid;
};

struct Viewport {
    static constexpr MsgType kType = MsgType::Viewport;
    uint32_t id;
    uint32_t width;
    uint32_t height;
    char name[kViewportNameSize]; // NUL-terminated
};

// Attached fds: count * planes, buffer-major then plane-minor (fd of buffer b plane p is index b * planes + p).
struct DmabufBuffers {
    static constexpr MsgType kType = MsgType::DmabufBuffers;
    struct Plane {
        uint32_t offset;
        uint32_t stride;
    };
    uint32_t viewport;
    uint32_t count;  // 1..kMaxBuffers
    uint32_t planes; // 1..kMaxPlanes, same for every buffer
    uint32_t fourcc; // DRM fourcc
    uint32_t width;
    uint32_t height;
    uint32_t generation; // buffer-set generation, see ShmBuffers
    uint32_t reserved;   // keeps the u64 below aligned; zero
    uint64_t modifier;
    Plane plane[kMaxBuffers][kMaxPlanes]; // [buffer][plane]; entries beyond count/planes are zero
};

// Attached fds: exactly one memfd holding `count` frames of stride * height bytes each, back to back.
struct ShmBuffers {
    static constexpr MsgType kType = MsgType::ShmBuffers;
    uint32_t viewport;
    uint32_t count; // 1..kMaxBuffers
    uint32_t width;
    uint32_t height;
    uint32_t stride; // bytes per row, >= width * 4
    uint32_t format; // kFormatRgba8
    // Buffer-set generation of this viewport. The producer increments it on every buffer-set change (Resize,
    // switch between DMA-BUF and SHM, re-export). Frame and Release carry it so stale ones can be dropped.
    uint32_t generation;
};

struct Frame {
    static constexpr MsgType kType = MsgType::Frame;
    uint32_t viewport;
    uint32_t buffer;
    uint64_t seq;
    uint32_t flipY; // non-zero: image rows are bottom-up
    uint32_t generation; // generation of the buffer set that `buffer` indexes
};

struct Error {
    static constexpr MsgType kType = MsgType::Error;
    uint32_t code;
    char text[kErrorTextSize]; // NUL-terminated
};

// ---- consumer -> producer ------------------------------------------------------------------------------

struct Release {
    static constexpr MsgType kType = MsgType::Release;
    uint32_t viewport;
    uint32_t buffer;
    uint32_t generation; // echo of the generation of the Frame being released
};

struct RequestLinear {
    static constexpr MsgType kType = MsgType::RequestLinear;
    uint32_t viewport;
};

struct RequestShm {
    static constexpr MsgType kType = MsgType::RequestShm;
    uint32_t viewport;
};

struct Pause {
    static constexpr MsgType kType = MsgType::Pause;
    uint32_t paused; // 0 or 1
};

struct Volume {
    static constexpr MsgType kType = MsgType::Volume;
    uint32_t volume; // 0..128
};

struct Pointer {
    static constexpr MsgType kType = MsgType::Pointer;
    uint32_t viewport;
    int32_t x; // pixels, top-left origin
    int32_t y;
    uint32_t buttons; // bit 0 left, bit 1 right, bit 2 middle
};

struct Resize {
    static constexpr MsgType kType = MsgType::Resize;
    uint32_t viewport;
    uint32_t width;
    uint32_t height;
};

enum class ScalingMode : uint32_t { Fill = 0, Fit = 1, Stretch = 2 };

struct Scaling {
    static constexpr MsgType kType = MsgType::Scaling;
    uint32_t mode; // ScalingMode
};

struct Fps {
    static constexpr MsgType kType = MsgType::Fps;
    uint32_t fps;
};

struct Quit {
    static constexpr MsgType kType = MsgType::Quit;
    uint32_t reserved;
};

} // namespace msg

// Properties has no fixed struct: see the encoding at the top of this file.
inline constexpr uint32_t kPropertiesPrefix = sizeof(uint32_t);

// ---- layout checks -------------------------------------------------------------------------------------

#define MURASAKI_BRIDGE_CHECK(T, N)                                                                            \
    static_assert(sizeof(msg::T) == (N), #T " has an unexpected wire size");                                    \
    static_assert(std::is_trivially_copyable_v<msg::T> && std::is_standard_layout_v<msg::T>, #T " must be POD"); \
    static_assert(std::has_unique_object_representations_v<msg::T>, #T " must have no padding")

MURASAKI_BRIDGE_CHECK(Hello, 8);
MURASAKI_BRIDGE_CHECK(Viewport, 76);
MURASAKI_BRIDGE_CHECK(DmabufBuffers, 136);
MURASAKI_BRIDGE_CHECK(ShmBuffers, 28);
MURASAKI_BRIDGE_CHECK(Frame, 24);
MURASAKI_BRIDGE_CHECK(Error, 204);
MURASAKI_BRIDGE_CHECK(Release, 12);
MURASAKI_BRIDGE_CHECK(RequestLinear, 4);
MURASAKI_BRIDGE_CHECK(RequestShm, 4);
MURASAKI_BRIDGE_CHECK(Pause, 4);
MURASAKI_BRIDGE_CHECK(Volume, 4);
MURASAKI_BRIDGE_CHECK(Pointer, 16);
MURASAKI_BRIDGE_CHECK(Resize, 12);
MURASAKI_BRIDGE_CHECK(Scaling, 4);
MURASAKI_BRIDGE_CHECK(Fps, 4);
MURASAKI_BRIDGE_CHECK(Quit, 4);
#undef MURASAKI_BRIDGE_CHECK

// ---- strings -------------------------------------------------------------------------------------------

// Copies `src` into the fixed buffer, truncating if needed; the buffer is always NUL-terminated and the
// unused tail is zeroed.
template <size_t N>
inline void setString(char (&dst)[N], std::string_view src)
{
    static_assert(N >= 1);
    const size_t n = std::min(src.size(), N - 1);
    std::memset(dst, 0, N);
    if (n > 0) {
        std::memcpy(dst, src.data(), n);
    }
}

// Reads a fixed buffer up to the first NUL (or its end if a hostile peer left none).
template <size_t N>
inline std::string_view getString(const char (&src)[N])
{
    size_t n = 0;
    while (n < N && src[n] != '\0') {
        ++n;
    }
    return {src, n};
}

// ---- encode / decode -----------------------------------------------------------------------------------

template <class T>
concept Wire = requires { { T::kType } -> std::convertible_to<MsgType>; } && std::is_trivially_copyable_v<T>;

// Payload bytes of a message (without the header).
template <Wire T>
inline std::vector<uint8_t> encode(const T &m)
{
    std::vector<uint8_t> out(sizeof(T));
    std::memcpy(out.data(), &m, sizeof(T));
    return out;
}

// Zero-copy view of a message's wire bytes (the structs are their own wire format).
template <Wire T>
inline std::span<const uint8_t> asBytes(const T &m)
{
    return {reinterpret_cast<const uint8_t *>(&m), sizeof(T)};
}

// Decodes a payload; fails if it is shorter than the struct. Trailing bytes are ignored. Fixed strings are
// re-terminated so getString() is always bounded, and unused DmabufBuffers plane entries are zeroed. Decoding
// does not judge values: call validate() on the result.
template <Wire T>
inline bool decode(std::span<const uint8_t> payload, T &out)
{
    if (payload.size() < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, payload.data(), sizeof(T));
    if constexpr (std::is_same_v<T, msg::Viewport>) {
        out.name[kViewportNameSize - 1] = '\0';
    } else if constexpr (std::is_same_v<T, msg::Error>) {
        out.text[kErrorTextSize - 1] = '\0';
    } else if constexpr (std::is_same_v<T, msg::DmabufBuffers>) {
        const uint32_t count = std::min(out.count, kMaxBuffers);
        const uint32_t planes = std::min(out.planes, kMaxPlanes);
        for (uint32_t b = 0; b < kMaxBuffers; ++b) {
            for (uint32_t p = 0; p < kMaxPlanes; ++p) {
                if (b >= count || p >= planes) {
                    out.plane[b][p] = {};
                }
            }
        }
    }
    return true;
}

// Properties payload: u32 length + JSON bytes. Returns nullopt if the JSON exceeds kMaxPropertiesJson.
inline std::optional<std::vector<uint8_t>> encodeProperties(std::string_view json)
{
    if (json.size() > kMaxPropertiesJson) {
        return std::nullopt;
    }
    std::vector<uint8_t> out(kPropertiesPrefix + json.size());
    const auto len = static_cast<uint32_t>(json.size());
    std::memcpy(out.data(), &len, sizeof(len));
    if (!json.empty()) {
        std::memcpy(out.data() + kPropertiesPrefix, json.data(), json.size());
    }
    return out;
}

// The returned view aliases `payload`. Fails on a short prefix, an oversize length, or a length that does
// not match the payload size exactly.
inline std::optional<std::string_view> decodeProperties(std::span<const uint8_t> payload)
{
    if (payload.size() < kPropertiesPrefix) {
        return std::nullopt;
    }
    uint32_t len = 0;
    std::memcpy(&len, payload.data(), sizeof(len));
    if (len > kMaxPropertiesJson || payload.size() - kPropertiesPrefix != len) {
        return std::nullopt;
    }
    return std::string_view(reinterpret_cast<const char *>(payload.data()) + kPropertiesPrefix, len);
}

// ---- shared-memory geometry ----------------------------------------------------------------------------

struct ShmGeometry {
    uint32_t count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0; // bytes per row
};

// Bytes of the whole ring, or 0 for invalid geometry: count 1..kMaxBuffers, width/height 1..kMaxDimension,
// stride a multiple of 4 and >= width * 4, total <= kMaxRingBytes. The arithmetic cannot overflow 64 bits
// (stride < 2^32, height <= 2^14, count <= 3).
inline constexpr uint64_t ringBytes(const ShmGeometry &g)
{
    if (g.count < 1 || g.count > kMaxBuffers || g.width < 1 || g.height < 1 || g.width > kMaxDimension
        || g.height > kMaxDimension || g.stride % 4 != 0 || static_cast<uint64_t>(g.stride) < uint64_t{g.width} * 4) {
        return 0;
    }
    const uint64_t total = uint64_t{g.count} * g.stride * g.height;
    return total <= kMaxRingBytes ? total : 0;
}

inline constexpr bool validGeometry(const ShmGeometry &g)
{
    return ringBytes(g) != 0;
}

// ---- semantic validation -------------------------------------------------------------------------------
//
// Call validate(decoded, receivedFdCount) on every decoded, untrusted message before acting on it. The generic
// overload accepts messages that must carry no fds and have no value constraints; the overloads below add the
// per-message rules. Frame::buffer and every `viewport` id are NOT checked here: only the caller knows how many
// buffers and viewports exist, so it must check `buffer < count` itself.

namespace msg {

template <Wire T>
inline bool validate(const T &, size_t fdCount)
{
    return fdCount == 0;
}

inline bool validate(const DmabufBuffers &m, size_t fdCount)
{
    if (m.count < 1 || m.count > kMaxBuffers || m.planes < 1 || m.planes > kMaxPlanes || m.fourcc == 0
        || m.width < 1 || m.width > kMaxDimension || m.height < 1 || m.height > kMaxDimension
        || fdCount != size_t{m.count} * m.planes) {
        return false;
    }
    for (uint32_t b = 0; b < m.count; ++b) {
        for (uint32_t p = 0; p < m.planes; ++p) {
            const auto &pl = m.plane[b][p];
            // 64-bit, cannot overflow: offset < 2^32, stride < 2^32, height <= 2^14.
            if (pl.stride == 0 || uint64_t{pl.offset} + uint64_t{pl.stride} * m.height > kMaxRingBytes) {
                return false;
            }
        }
    }
    return true;
}

inline bool validate(const ShmBuffers &m, size_t fdCount)
{
    return fdCount == 1 && m.format == kFormatRgba8 && validGeometry({m.count, m.width, m.height, m.stride});
}

inline bool validate(const Resize &m, size_t fdCount)
{
    return fdCount == 0 && m.width >= 1 && m.width <= kMaxDimension && m.height >= 1 && m.height <= kMaxDimension;
}

inline bool validate(const Volume &m, size_t fdCount)
{
    return fdCount == 0 && m.volume <= 128;
}

inline bool validate(const Scaling &m, size_t fdCount)
{
    return fdCount == 0 && m.mode <= static_cast<uint32_t>(ScalingMode::Stretch);
}

// Pointer coordinates may lie slightly outside the item (drags) but are bounded to +-kMaxDimension.
inline bool validate(const Pointer &m, size_t fdCount)
{
    constexpr int32_t lim = static_cast<int32_t>(kMaxDimension);
    return fdCount == 0 && m.x >= -lim && m.x <= lim && m.y >= -lim && m.y <= lim && (m.buttons & ~7u) == 0;
}

inline bool validate(const Frame &m, size_t fdCount)
{
    return fdCount == 0 && m.flipY <= 1;
}

} // namespace msg

} // namespace murasaki::bridge
