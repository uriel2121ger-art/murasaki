#pragma once

// A persistent cache of decoded embedded pictures: decoding a 4000 x 6000 PNG takes most of a second, reading its RGBA back
// from a LZ4 file takes a tenth. The key is the SHA-256 of the picture's source bytes plus the decoder version, so a file can
// never answer for different input; entries are validated when read (magic, sizes, exact decompressed length), and the
// directory ($XDG_CACHE_HOME/murasaki/texture-cache) is trimmed to 1 GiB, least recently used first. MURASAKI_TEXTURE_CACHE=0
// turns it off. Thread safe (files are written to a temporary name and renamed).

#include "assets/imagedecode.h"

#include <span>

namespace murasaki::scene {

// Bump when decoding changes in a way that changes the pixels.
inline constexpr int kTextureDecoderVersion = 1;
// Pictures with fewer pixels than this are decoded every time: a cache file would not save anything.
inline constexpr uint64_t kTextureCacheMinPixels = 512 * 512;

// True and `out` filled when the picture is cached.
bool loadCachedImage(std::span<const uint8_t> source, uint32_t maxDimension, uint64_t maxBytes, DecodedImage &out);
// Remembers a decoded picture (no error when the cache cannot be written). The call only hashes the source and copies the
// pixels: compressing and writing happen on a background thread, so a first load does not wait for the cache.
void storeCachedImage(std::span<const uint8_t> source, const DecodedImage &image);
// Waits until every store queued so far is on disk (tests, and anything that must see the files).
void flushCachedImageStores();

} // namespace murasaki::scene
