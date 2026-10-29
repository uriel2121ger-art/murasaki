#include "puppetmask.h"

#include <algorithm>

namespace murasaki {

std::shared_ptr<const MaskSet> maskSetFromHitMask(int cols, int rows, int cell, const QByteArray &bits)
{
    if (cols < 1 || cols > 256 || rows < 1 || rows > 256 || cell < 1 || cell > 64)
        return nullptr;
    const qsizetype cells = qsizetype(cols) * rows;
    if (bits.size() != (cells + 7) / 8)
        return nullptr;
    const auto set = [&](qsizetype i) { return (static_cast<unsigned char>(bits.at(i / 8)) >> (i % 8)) & 1u; };
    for (qsizetype i = cells; i < bits.size() * 8; ++i) {
        if (set(i))
            return nullptr; // padding must be zero
    }
    const int spaceW = cols * cell, spaceH = rows * cell;
    AlphaMask mask(spaceW, spaceH, kMaxMaskGrid);
    const int target = std::max(1, mask.cellPx());
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            if (!set(qsizetype(r) * cols + c))
                continue;
            const int x0 = c * cell, y0 = r * cell;
            for (int ty = y0 / target; ty <= (y0 + cell - 1) / target; ++ty)
                for (int tx = x0 / target; tx <= (x0 + cell - 1) / target; ++tx)
                    mask.setCell(tx, ty);
        }
    }
    return std::make_shared<const MaskSet>(MaskSet::assemble(MaskQuality::Silhouette, std::move(mask), {}));
}

} // namespace murasaki
