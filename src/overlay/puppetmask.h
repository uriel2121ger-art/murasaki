#pragma once

#include "alphamask.h"

#include <QByteArray>

#include <memory>

namespace murasaki {

// The footprint a puppet engine sends (bridge HitMask: cols x rows bits of `cell` pixel cells, row 0 first, bit 0 of a byte first) as the pet's hit mask: a silhouette (one mask for every frame, since the engine
// sends a new footprint when the pose changes and the window takes the newest). The grid is lowered to the pet masks' 128 cells; a cell of the engine's that overlaps a cell of the result sets it (never smaller than
// what is drawn). Null for a grid or a payload that does not fit (cols and rows 1..256, cell 1..64, bits exactly ceil(cols * rows / 8) bytes, no bit set beyond the last cell).
std::shared_ptr<const MaskSet> maskSetFromHitMask(int cols, int rows, int cell, const QByteArray &bits);

} // namespace murasaki
