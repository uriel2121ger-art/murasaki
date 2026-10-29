#pragma once

// Murasaki's extension of the bridge protocol for puppets, version 6 (docs/pets/puppets.md, section 19). Like the span, media and shortcut messages it stays out of protocol.h: a
// producer of the base protocol announces version 2 and ignores unknown message types, and only a puppet engine sends this one.
//
//   HitMask (producer -> consumer): the coarse footprint of the puppet on the picture it just drew, so the overlay can set the window's input region from it (the pet's existing
//     alpha-mask code works on a bit grid). Payload: HitMaskHeader, then cols x rows bits, row 0 first, bit 0 of a byte first, rows padded to nothing (the bits run on), the
//     last byte padded with zeros. `cell` is the side of a cell in picture pixels. `generation` is the bridge buffer-set generation the mask was made for (a Resize makes a
//     new one); the consumer drops a mask of an older generation. No fds.
//
// Type number 49 follows 43 SpanView, 44 Media, 45 MediaThumbnail, 46 MediaWants, 47 ShortcutIcon, 48 ShortcutLaunch.

#include "protocol.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <vector>

namespace murasaki::bridge {

inline constexpr uint32_t kPuppetVersion = 6;
inline constexpr MsgType kHitMaskType = static_cast<MsgType>(49);
inline constexpr uint32_t kMaxHitMaskSide = 256; // cells; 2048 pixels of picture at the default cell of 8
inline constexpr uint32_t kMaxHitMaskCell = 64;

struct HitMaskHeader {
    uint32_t cols;
    uint32_t rows;
    uint32_t cell;
    uint32_t generation;
};
static_assert(sizeof(HitMaskHeader) == 16 && std::is_trivially_copyable_v<HitMaskHeader>);
static_assert(std::has_unique_object_representations_v<HitMaskHeader>, "HitMaskHeader must have no padding");

inline constexpr size_t hitMaskBytes(uint32_t cols, uint32_t rows)
{
    return (static_cast<size_t>(cols) * rows + 7) / 8;
}

inline bool validHitMask(const HitMaskHeader &h)
{
    return h.cols >= 1 && h.cols <= kMaxHitMaskSide && h.rows >= 1 && h.rows <= kMaxHitMaskSide && h.cell >= 1 && h.cell <= kMaxHitMaskCell;
}

// The payload of a HitMask message; nullopt if the header is invalid or `bits` is not exactly the size the grid needs.
inline std::optional<std::vector<uint8_t>> encodeHitMask(const HitMaskHeader &header, std::span<const uint8_t> bits)
{
    if (!validHitMask(header) || bits.size() != hitMaskBytes(header.cols, header.rows)) {
        return std::nullopt;
    }
    std::vector<uint8_t> out(sizeof(header) + bits.size());
    std::memcpy(out.data(), &header, sizeof(header));
    if (!bits.empty()) {
        std::memcpy(out.data() + sizeof(header), bits.data(), bits.size());
    }
    return out;
}

struct HitMaskView {
    HitMaskHeader header;
    std::span<const uint8_t> bits; // aliases the payload
};

// Fails on a short or oversize payload, an invalid grid, or a length that does not match the grid exactly (the unused bits of the last byte must be zero too).
inline std::optional<HitMaskView> decodeHitMask(std::span<const uint8_t> payload)
{
    if (payload.size() < sizeof(HitMaskHeader)) {
        return std::nullopt;
    }
    HitMaskView view{};
    std::memcpy(&view.header, payload.data(), sizeof(HitMaskHeader));
    if (!validHitMask(view.header) || payload.size() - sizeof(HitMaskHeader) != hitMaskBytes(view.header.cols, view.header.rows)) {
        return std::nullopt;
    }
    view.bits = payload.subspan(sizeof(HitMaskHeader));
    const size_t used = static_cast<size_t>(view.header.cols) * view.header.rows;
    if (used % 8 != 0 && (view.bits.back() >> (used % 8)) != 0) {
        return std::nullopt;
    }
    return view;
}

} // namespace murasaki::bridge
