#pragma once

// Cover pictures: read a file or decode downloaded bytes into the RGBA8 thumbnail the engine gets. The picture is untrusted: its
// size is checked from the header before any pixel is decoded, the formats are the ones Qt reads as plain rasters (no SVG), and
// the result is scaled to kThumbnailSide.

#include "media/mediapolicy.h"

#include <QByteArray>
#include <QString>

#include <optional>

namespace murasaki::media {

struct Cover {
    uint32_t width = 0;
    uint32_t height = 0;
    QByteArray rgba; // width * height * 4, straight alpha
};

// nullopt for anything that is not a picture within the limits (more than kMaxArtSide on a side or kMaxArtPixels in all, a
// vector format, a truncated file...).
std::optional<Cover> decodeArt(const QByteArray &bytes);

// Residual risk, accepted: any picture the user can read may be named by a player and so be shown to the scenes (and their scripts).
// A cover file: the path is resolved first; /proc, /sys and /dev are refused, and so is anything that is not a regular file
// (opened with O_NOFOLLOW | O_NONBLOCK and judged by fstat on the open descriptor: no FIFO or device can stall or fool the read)
// or is over kMaxArtBytes.
std::optional<Cover> loadArtFile(const QString &path);

} // namespace murasaki::media
