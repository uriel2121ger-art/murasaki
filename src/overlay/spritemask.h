#pragma once

// Hit masks for the pets of the overlay, from the decoder's side (task 3 of the phase 2 plan):
//   - a pet that plays from the frame cache gets its masks while its frames are decoded: pass a SpriteDecoder::MaskRequest to
//     SpriteDecoder::decode() and read Result::masks (nothing is decoded twice);
//   - a pet that plays through libmpv has no pixels in the GUI: loadMaskPass() decodes its file once (streamed, at its own size), keeps
//     only the masks, and caches them on disk, so the next start reads a few KiB instead of decoding.
// Both give a MaskSet (alphamask.h). Run them on the low-priority decode worker; both take a cancel flag.

#include "alphamask.h"

#include <QSize>
#include <QString>

#include <atomic>

namespace murasaki {

struct MaskLoad {
    bool ok = false;
    bool cancelled = false;
    bool fromCache = false;
    QString error; // when !ok and !cancelled
    MaskSet masks;
    qint64 frames = 0; // frames the masks were made from (0 for a union-only set read from the cache)
    QSize size;        // the picture the masks describe (the file's size, or SpriteDecoder::maskPassSize of it)
};

// $XDG_CACHE_HOME (else ~/.cache), the root under which `murasaki/pets/<item>-<h>/<variant>-<h>-<key>.mask` lives.
QString defaultMaskCacheRoot();
// A name made safe to be one path component: [A-Za-z0-9._-] kept, anything else and a leading dot become '_', at most 80 characters.
// Different names can give the same result, so the directory and file names add maskCacheHash().
QString maskCacheSafeName(const QString &name);
// Eight hex digits of the SHA-256 of the raw name: what tells `日本` from `中国` and `anima:1` from `anima_1` once both are made safe.
QString maskCacheHash(const QString &name);
// The key of a variant's file: a hash of its canonical path, size and modification time, so that a file that is replaced or changed (a
// new transcode) has another key. Empty when the file cannot be read.
QString maskCacheKey(const QString &file);
// `<cacheRoot>/murasaki/pets/<safe item>-<hash of item>/<safe variant>-<hash of variant>-<key>.mask`.
QString maskCachePath(const QString &cacheRoot, const QString &item, const QString &variant, const QString &key);

// The masks of `file` for the pet's item and variant: from the cache when the variant's file for this key is there and valid for the
// file's picture size, else by the mask pass (SpriteDecoder::decodeMasks), after which the cache file is written and the variant's files
// of other keys (and only its own: the match is exact) are removed. A cache that can not be written is not an error. A cache hit is
// returned as it was made, whatever `budgetBytes` is now: a set made under a small budget stays coarse or silhouette-only until the file
// changes. `file` should be the file the pet plays (the transcoded copy), so that frame i of the masks is frame i on screen. An empty
// `cacheRoot` is an error. A failed pass is logged and not remembered: the next call tries again (a file over the frame limit is
// decoded again each time, so the caller should not ask twice for a pet whose pass failed in this session).
MaskLoad loadMaskPass(const QString &cacheRoot, const QString &item, const QString &variant, const QString &file,
                      const std::atomic_bool *cancel = nullptr, std::size_t budgetBytes = kMaskBudgetBytes);

} // namespace murasaki
