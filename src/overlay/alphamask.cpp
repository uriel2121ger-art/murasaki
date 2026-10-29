#include "alphamask.h"

#include <QFile>
#include <QSaveFile>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace murasaki {

namespace {

int ceilDiv(int a, int b)
{
    return (a + b - 1) / b;
}

bool sameShape(const AlphaMask &a, const AlphaMask &b)
{
    return a.spaceWidth() == b.spaceWidth() && a.spaceHeight() == b.spaceHeight() && a.cols() == b.cols() && a.rows() == b.rows()
        && a.cellPx() == b.cellPx();
}

} // namespace

AlphaMask::AlphaMask(int spaceW, int spaceH, int level)
{
    if (spaceW < 1 || spaceH < 1 || spaceW > kMaxMaskSpace || spaceH > kMaxMaskSpace)
        return;
    level = std::clamp(level, 1, kMaxMaskGrid);
    m_spaceW = spaceW;
    m_spaceH = spaceH;
    m_cell = ceilDiv(std::max(spaceW, spaceH), level);
    m_cols = ceilDiv(spaceW, m_cell);
    m_rows = ceilDiv(spaceH, m_cell);
    m_words.assign(wordsFor(m_cols, m_rows), 0);
}

AlphaMask AlphaMask::fromAlpha(const std::uint8_t *alpha, int stride, int pixelStep, int w, int h, int level)
{
    AlphaMask mask(w, h, level);
    if (mask.isNull() || !alpha || pixelStep < 1 || stride < 0 || std::size_t(w) * std::size_t(pixelStep) > std::size_t(stride))
        return mask;
    // One pass over the pixels with running cell counters: no division per pixel, no bounds check per cell.
    const std::size_t step = std::size_t(pixelStep);
    const std::size_t perRow = std::size_t(mask.wordsPerRow());
    int cellRow = 0;
    int inRow = 0;
    for (int y = 0; y < h; ++y) {
        const std::uint8_t *p = alpha + std::size_t(y) * std::size_t(stride);
        std::uint64_t *words = mask.m_words.data() + std::size_t(cellRow) * perRow;
        int col = 0;
        int inCol = 0;
        for (int x = 0; x < w; ++x, p += step) {
            if (*p >= kMaskAlphaThreshold)
                words[col >> 6] |= std::uint64_t(1) << (col & 63);
            if (++inCol == mask.m_cell) {
                inCol = 0;
                ++col;
            }
        }
        if (++inRow == mask.m_cell) {
            inRow = 0;
            ++cellRow;
        }
    }
    return mask;
}

bool AlphaMask::cell(int col, int row) const
{
    if (col < 0 || row < 0 || col >= m_cols || row >= m_rows)
        return false;
    return (m_words[std::size_t(row) * std::size_t(wordsPerRow()) + std::size_t(col / 64)] >> (col % 64)) & 1u;
}

void AlphaMask::setCell(int col, int row, bool on)
{
    if (col < 0 || row < 0 || col >= m_cols || row >= m_rows)
        return;
    std::uint64_t &word = m_words[std::size_t(row) * std::size_t(wordsPerRow()) + std::size_t(col / 64)];
    const std::uint64_t bit = std::uint64_t(1) << (col % 64);
    word = on ? (word | bit) : (word & ~bit);
}

bool AlphaMask::contains(double x, double y) const
{
    if (isNull() || !std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 || x >= m_spaceW || y >= m_spaceH)
        return false;
    return cell(int(x) / m_cell, int(y) / m_cell);
}

int AlphaMask::setCells() const
{
    int n = 0;
    for (const std::uint64_t w : m_words)
        n += std::popcount(w);
    return n;
}

AlphaMask AlphaMask::united(const AlphaMask &other) const
{
    if (!sameShape(*this, other))
        return *this;
    AlphaMask out = *this;
    for (std::size_t i = 0; i < out.m_words.size(); ++i)
        out.m_words[i] |= other.m_words[i];
    return out;
}

bool AlphaMask::isSubsetOf(const AlphaMask &other) const
{
    if (!sameShape(*this, other))
        return false;
    for (std::size_t i = 0; i < m_words.size(); ++i) {
        if (m_words[i] & ~other.m_words[i])
            return false;
    }
    return true;
}

namespace {

// The 32 even-numbered bits of x, packed (a software pext with the mask 0x5555...).
std::uint64_t compactEvenBits(std::uint64_t x)
{
    x &= 0x5555555555555555ull;
    x = (x | (x >> 1)) & 0x3333333333333333ull;
    x = (x | (x >> 2)) & 0x0F0F0F0F0F0F0F0Full;
    x = (x | (x >> 4)) & 0x00FF00FF00FF00FFull;
    x = (x | (x >> 8)) & 0x0000FFFF0000FFFFull;
    x = (x | (x >> 16)) & 0x00000000FFFFFFFFull;
    return x;
}

} // namespace

AlphaMask AlphaMask::coarsened() const
{
    if (isNull() || !canCoarsen())
        return *this;
    AlphaMask out;
    out.m_spaceW = m_spaceW;
    out.m_spaceH = m_spaceH;
    out.m_cell = m_cell * 2;
    out.m_cols = (m_cols + 1) / 2;
    out.m_rows = (m_rows + 1) / 2;
    out.m_words.assign(wordsFor(out.m_cols, out.m_rows), 0);
    const int inPerRow = wordsPerRow();
    const int outPerRow = out.wordsPerRow();
    for (int r = 0; r < out.m_rows; ++r) {
        for (int k = 0; k < outPerRow; ++k) {
            std::uint64_t bits = 0;
            for (int half = 0; half < 2; ++half) {
                const int w = 2 * k + half;
                if (w >= inPerRow)
                    break;
                std::uint64_t v = m_words[std::size_t(2 * r) * std::size_t(inPerRow) + std::size_t(w)];
                if (2 * r + 1 < m_rows)
                    v |= m_words[std::size_t(2 * r + 1) * std::size_t(inPerRow) + std::size_t(w)];
                // Neighbour columns (2i, 2i+1) are one destination cell: OR the pair, then keep one bit of it.
                bits |= compactEvenBits(v | (v >> 1)) << (32 * half);
            }
            out.m_words[std::size_t(r) * std::size_t(outPerRow) + std::size_t(k)] = bits;
        }
    }
    return out;
}

AlphaMask AlphaMask::transformed(double degrees, bool flipH, bool flipV) const
{
    if (isNull())
        return *this;
    // A pet picture is at most kMaxPetCanvasSide (8192) a side, so its footprint (<= 11586) fits the bound of a mask space.
    const PetSize fp = petFootprint({double(m_spaceW), double(m_spaceH)}, degrees);
    const int ow = std::min(petCeilPx(fp.w), kMaxMaskSpace);
    const int oh = std::min(petCeilPx(fp.h), kMaxMaskSpace);
    AlphaMask out(ow, oh, std::max(m_cols, m_rows));
    if (empty())
        return out;
    // Forward mapping: each set source cell becomes the bounding box of its four corners, flipped in the picture's own frame
    // and turned clockwise about the centre, and every destination cell that box overlaps is set. A pixel drawn inside a set
    // cell lands inside that box, so the result is a superset of what is drawn; at quarter turns, with cells that tile the
    // picture, the boxes are exactly destination cells.
    const double r = normalizeRotation(degrees);
    double cs = std::cos(r * kPetPi / 180.0);
    double sn = std::sin(r * kPetPi / 180.0);
    if (r == 0.0 || r == 90.0 || r == 180.0 || r == 270.0) { // exact: no 6e-17 to spill into a neighbour
        cs = r == 0.0 ? 1.0 : r == 180.0 ? -1.0 : 0.0;
        sn = r == 90.0 ? 1.0 : r == 270.0 ? -1.0 : 0.0;
    }
    const double cx = m_spaceW / 2.0, cy = m_spaceH / 2.0, ox = ow / 2.0, oy = oh / 2.0;
    constexpr double kEdge = 1e-6; // a box that only touches a destination cell's edge does not set it
    const double dcell = out.m_cell;
    for (int row = 0; row < m_rows; ++row) {
        for (int col = 0; col < m_cols; ++col) {
            if (!cell(col, row))
                continue;
            const double xs[2] = {double(col * m_cell), double(std::min((col + 1) * m_cell, m_spaceW))};
            const double ys[2] = {double(row * m_cell), double(std::min((row + 1) * m_cell, m_spaceH))};
            double minX = 1e300, maxX = -1e300, minY = 1e300, maxY = -1e300;
            for (const double sx : xs) {
                for (const double sy : ys) {
                    const double px = (flipH ? m_spaceW - sx : sx) - cx;
                    const double py = (flipV ? m_spaceH - sy : sy) - cy;
                    const double rx = px * cs - py * sn + ox;
                    const double ry = px * sn + py * cs + oy;
                    minX = std::min(minX, rx);
                    maxX = std::max(maxX, rx);
                    minY = std::min(minY, ry);
                    maxY = std::max(maxY, ry);
                }
            }
            const int c0 = std::max(0, int(std::floor((minX + kEdge) / dcell)));
            const int c1 = std::min(out.m_cols - 1, int(std::floor((maxX - kEdge) / dcell)));
            const int r0 = std::max(0, int(std::floor((minY + kEdge) / dcell)));
            const int r1 = std::min(out.m_rows - 1, int(std::floor((maxY - kEdge) / dcell)));
            for (int rr = r0; rr <= r1; ++rr) {
                for (int cc = c0; cc <= c1; ++cc)
                    out.setCell(cc, rr);
            }
        }
    }
    return out;
}

namespace {

struct CellRect {
    int c0, c1, r0, r1; // inclusive
};

std::vector<CellRect> cellRects(const AlphaMask &m)
{
    std::vector<CellRect> out;
    std::vector<int> open; // indexes into `out` of the rectangles that reach the previous row, by column
    std::vector<int> now;
    for (int r = 0; r < m.rows(); ++r) {
        now.clear();
        std::size_t j = 0;
        int c = 0;
        while (c < m.cols()) {
            if (!m.cell(c, r)) {
                ++c;
                continue;
            }
            const int c0 = c;
            while (c < m.cols() && m.cell(c, r))
                ++c;
            const int c1 = c - 1;
            while (j < open.size() && out[std::size_t(open[j])].c0 < c0)
                ++j;
            int found = -1;
            if (j < open.size() && out[std::size_t(open[j])].c0 == c0 && out[std::size_t(open[j])].c1 == c1)
                found = open[j];
            if (found >= 0) {
                out[std::size_t(found)].r1 = r;
            } else {
                out.push_back({c0, c1, r, r});
                found = int(out.size()) - 1;
            }
            now.push_back(found);
        }
        open.swap(now);
    }
    return out;
}

} // namespace

AlphaMask::Rects AlphaMask::toRects(int maxRects) const
{
    Rects result;
    if (isNull() || empty() || maxRects < 1)
        return result;
    AlphaMask current = *this;
    std::vector<CellRect> cells = cellRects(current);
    while (int(cells.size()) > maxRects && current.canCoarsen()) {
        current = current.coarsened();
        ++result.coarsenSteps;
        cells = cellRects(current);
    }
    if (int(cells.size()) > maxRects) {
        // Still too many (a checkerboard of the coarsest grid): the bounding rectangle, a superset.
        CellRect box{current.cols(), -1, current.rows(), -1};
        for (const CellRect &c : cells) {
            box.c0 = std::min(box.c0, c.c0);
            box.c1 = std::max(box.c1, c.c1);
            box.r0 = std::min(box.r0, c.r0);
            box.r1 = std::max(box.r1, c.r1);
        }
        cells = {box};
        result.boundingBox = true;
    }
    for (const CellRect &c : cells) {
        const int x = c.c0 * current.cellPx();
        const int y = c.r0 * current.cellPx();
        result.rects.push_back({x, y, std::min((c.c1 + 1) * current.cellPx(), m_spaceW) - x,
                                std::min((c.r1 + 1) * current.cellPx(), m_spaceH) - y});
    }
    return result;
}

std::optional<AlphaMask> AlphaMask::fromWords(int spaceW, int spaceH, int cols, int rows, int cell, std::vector<std::uint64_t> words)
{
    if (spaceW < 1 || spaceH < 1 || spaceW > kMaxMaskSpace || spaceH > kMaxMaskSpace || cell < 1 || cols < 1 || rows < 1
        || cols > kMaxMaskGrid || rows > kMaxMaskGrid)
        return std::nullopt;
    if (cols != ceilDiv(spaceW, cell) || rows != ceilDiv(spaceH, cell) || words.size() != wordsFor(cols, rows))
        return std::nullopt;
    const int perRow = (cols + 63) / 64;
    const std::uint64_t padding = (cols % 64 == 0) ? 0 : ~((std::uint64_t(1) << (cols % 64)) - 1);
    for (int r = 0; r < rows; ++r) {
        if (words[std::size_t(r) * std::size_t(perRow) + std::size_t(perRow - 1)] & padding)
            return std::nullopt;
    }
    AlphaMask m;
    m.m_spaceW = spaceW;
    m.m_spaceH = spaceH;
    m.m_cols = cols;
    m.m_rows = rows;
    m.m_cell = cell;
    m.m_words = std::move(words);
    return m;
}

// ---- MaskSet ------------------------------------------------------------------------------------------------------------

MaskSet MaskSet::assemble(MaskQuality quality, AlphaMask silhouette, std::vector<AlphaMask> frames)
{
    MaskSet set;
    set.m_quality = silhouette.isNull() ? MaskQuality::None : quality;
    set.m_union = std::move(silhouette);
    set.m_frames = std::move(frames);
    return set;
}

const AlphaMask &MaskSet::maskFor(int index) const
{
    if (m_frames.empty())
        return m_union;
    const int n = int(m_frames.size());
    return m_frames[std::size_t(((index % n) + n) % n)];
}

std::size_t MaskSet::bytes() const
{
    std::size_t n = m_union.bytes();
    for (const AlphaMask &f : m_frames)
        n += f.bytes();
    return n;
}

std::optional<std::string_view> MaskSet::degradeName() const
{
    switch (m_quality) {
    case MaskQuality::None:
        return degrade::kHitMaskNone;
    case MaskQuality::Full:
        return std::nullopt;
    case MaskQuality::Coarse:
        return degrade::kHitMaskCoarse;
    case MaskQuality::Silhouette:
        return degrade::kHitMaskSilhouette;
    }
    return std::nullopt;
}

MaskSet MaskSet::transformed(double degrees, bool flipH, bool flipV) const
{
    MaskSet out;
    out.m_quality = m_quality;
    out.m_union = m_union.transformed(degrees, flipH, flipV);
    out.m_frames.reserve(m_frames.size());
    for (const AlphaMask &f : m_frames)
        out.m_frames.push_back(f.transformed(degrees, flipH, flipV));
    return out;
}

// ---- MaskBuilder --------------------------------------------------------------------------------------------------------

namespace {

// Bytes of one mask of a picture after `steps` coarsenings of the 128 grid.
std::size_t maskBytesAt(int w, int h, int steps)
{
    const AlphaMask shape(w, h, kMaxMaskGrid);
    if (shape.isNull())
        return 0;
    int cols = shape.cols();
    int rows = shape.rows();
    for (int i = 0; i < steps; ++i) {
        cols = (cols + 1) / 2;
        rows = (rows + 1) / 2;
    }
    return AlphaMask::wordsFor(cols, rows) * sizeof(std::uint64_t) + kMaskOverheadBytes;
}

} // namespace

MaskBuilder::MaskBuilder(int spaceW, int spaceH, int expectedFrames, std::size_t budgetBytes)
    : m_w(spaceW)
    , m_h(spaceH)
    , m_expected(std::clamp(expectedFrames, 1, kMaxMaskFrames))
    , m_budget(budgetBytes)
    , m_union(spaceW, spaceH, kMaxMaskGrid)
{
    m_frames.reserve(std::size_t(m_expected)); // no capacity doubling on top of the budget
    fit();
}

int MaskBuilder::level() const
{
    const AlphaMask shape(m_w, m_h, kMaxMaskGrid);
    int level = std::max(shape.cols(), shape.rows());
    for (int i = 0; i < m_steps; ++i)
        level = (level + 1) / 2;
    return level;
}

// Lowers the grid (steps) until the frames still to come fit with the union at the same shape, else keeps the union only. The union is
// always kept at the finest grid and coarsened at the end: a silhouette-only pet gets the best silhouette.
void MaskBuilder::fit()
{
    if (m_unionOnly || m_union.isNull())
        return;
    const AlphaMask shape(m_w, m_h, kMaxMaskGrid);
    int steps = m_steps;
    for (;; ++steps) {
        const std::size_t perMask = maskBytesAt(m_w, m_h, steps);
        const std::size_t frames = std::size_t(std::max(m_expected, m_added + 1));
        if ((frames + 1) * perMask <= m_budget)
            break;
        int level = std::max(shape.cols(), shape.rows());
        for (int i = 0; i < steps; ++i)
            level = (level + 1) / 2;
        if (level <= kMaskGridLevels[2]) { // cannot go lower
            m_unionOnly = true;
            m_frames.clear();
            m_frames.shrink_to_fit();
            return;
        }
    }
    while (m_steps < steps) {
        for (AlphaMask &f : m_frames)
            f = f.coarsened();
        ++m_steps;
    }
}

bool MaskBuilder::addMask(const AlphaMask &fullMask)
{
    if (m_union.isNull() || fullMask.spaceWidth() != m_w || fullMask.spaceHeight() != m_h || fullMask.cellPx() != m_union.cellPx()
        || m_added >= kMaxMaskFrames)
        return false;
    ++m_added;
    m_union = m_union.united(fullMask);
    fit();
    if (!m_unionOnly) {
        AlphaMask f = fullMask;
        for (int i = 0; i < m_steps; ++i)
            f = f.coarsened();
        m_frames.push_back(std::move(f));
    }
    return true;
}

bool MaskBuilder::addFrame(const std::uint8_t *alpha, int stride, int pixelStep, int w, int h)
{
    if (w != m_w || h != m_h)
        return false;
    return addMask(AlphaMask::fromAlpha(alpha, stride, pixelStep, w, h, kMaxMaskGrid));
}

MaskSet MaskBuilder::finish()
{
    if (m_added == 0 || m_union.isNull())
        return {};
    if (m_unionOnly)
        return MaskSet::assemble(MaskQuality::Silhouette, m_union, {});
    AlphaMask u = m_union;
    for (int i = 0; i < m_steps; ++i)
        u = u.coarsened();
    return MaskSet::assemble(m_steps == 0 ? MaskQuality::Full : MaskQuality::Coarse, std::move(u), std::move(m_frames));
}

// ---- the file -------------------------------------------------------------------------------------------------------------

namespace {

void put16(std::vector<std::uint8_t> &b, std::uint32_t v)
{
    b.push_back(std::uint8_t(v));
    b.push_back(std::uint8_t(v >> 8));
}
void put32(std::vector<std::uint8_t> &b, std::uint32_t v)
{
    put16(b, v);
    put16(b, v >> 16);
}
void put64(std::vector<std::uint8_t> &b, std::uint64_t v)
{
    put32(b, std::uint32_t(v));
    put32(b, std::uint32_t(v >> 32));
}
std::uint32_t get16(const std::uint8_t *p)
{
    return std::uint32_t(p[0]) | std::uint32_t(p[1]) << 8;
}
std::uint32_t get32(const std::uint8_t *p)
{
    return get16(p) | get16(p + 2) << 16;
}
std::uint64_t get64(const std::uint8_t *p)
{
    return std::uint64_t(get32(p)) | std::uint64_t(get32(p + 4)) << 32;
}

std::uint64_t fnv1a(const std::uint8_t *p, std::size_t n)
{
    std::uint64_t h = 1469598103934665603ull;
    for (std::size_t i = 0; i < n; ++i) {
        h ^= p[i];
        h *= 1099511628211ull;
    }
    return h;
}

constexpr char kMagic[4] = {'M', 'S', 'K', 'M'};

} // namespace

std::vector<std::uint8_t> serializeMaskFile(const MaskSet &set)
{
    if (set.isNull())
        return {};
    const AlphaMask &u = set.silhouette();
    std::vector<std::uint8_t> body;
    body.reserve(set.bytes());
    const auto addWords = [&body](const AlphaMask &m) {
        for (const std::uint64_t w : m.words())
            put64(body, w);
    };
    addWords(u);
    for (int i = 0; i < set.frameCount(); ++i)
        addWords(set.maskFor(i));
    std::vector<std::uint8_t> out;
    out.reserve(kMaskFileHeaderBytes + body.size());
    out.insert(out.end(), kMagic, kMagic + 4);
    put32(out, kMaskFileVersion);
    put32(out, std::uint32_t(u.spaceWidth()));
    put32(out, std::uint32_t(u.spaceHeight()));
    put32(out, std::uint32_t(set.frameCount()));
    out.push_back(std::uint8_t(set.quality() == MaskQuality::Full ? 1 : set.quality() == MaskQuality::Coarse ? 2 : 3));
    out.push_back(0);
    put16(out, std::uint32_t(u.cols()));
    put16(out, std::uint32_t(u.rows()));
    put16(out, std::uint32_t(u.cellPx()));
    put32(out, 0);
    put64(out, fnv1a(body.data(), body.size()));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

std::optional<MaskSet> parseMaskFile(const std::uint8_t *data, std::size_t size, int expectedW, int expectedH, int expectedFrames)
{
    // Every check that needs no allocation comes first; the numbers of the header are bounded before they size anything.
    if (!data || size < kMaskFileHeaderBytes || size > kMaxMaskFileBytes || std::memcmp(data, kMagic, 4) != 0)
        return std::nullopt;
    if (get32(data + 4) != kMaskFileVersion)
        return std::nullopt;
    const std::uint32_t w = get32(data + 8);
    const std::uint32_t h = get32(data + 12);
    const std::uint32_t frames = get32(data + 16);
    const int quality = data[20];
    const std::uint32_t cols = get16(data + 22);
    const std::uint32_t rows = get16(data + 24);
    const std::uint32_t cell = get16(data + 26);
    if (data[21] != 0 || get32(data + 28) != 0) // reserved
        return std::nullopt;
    if (w < 1 || h < 1 || w > std::uint32_t(kMaxMaskSpace) || h > std::uint32_t(kMaxMaskSpace) || frames > std::uint32_t(kMaxMaskFrames)
        || quality < 1 || quality > 3 || cell < 1 || cols < 1 || rows < 1 || cols > std::uint32_t(kMaxMaskGrid)
        || rows > std::uint32_t(kMaxMaskGrid))
        return std::nullopt;
    if ((expectedW > 0 && int(w) != expectedW) || (expectedH > 0 && int(h) != expectedH))
        return std::nullopt;
    const bool silhouette = quality == 3;
    if (silhouette ? frames != 0 : frames == 0)
        return std::nullopt;
    if (!silhouette && expectedFrames > 0 && int(frames) != expectedFrames)
        return std::nullopt;
    // Full is the 128 grid, Coarse something coarser.
    const AlphaMask finest(int(w), int(h), kMaxMaskGrid);
    // Full and Silhouette are the finest grid; Coarse is the finest halved k >= 1 times, so its cell is the finest cell times 2^k.
    if (quality != 2 && (int(cols) != finest.cols() || int(rows) != finest.rows() || int(cell) != finest.cellPx()))
        return std::nullopt;
    if (quality == 2) {
        const int ratio = int(cell) / finest.cellPx();
        if (int(cell) % finest.cellPx() != 0 || ratio < 2 || (ratio & (ratio - 1)) != 0)
            return std::nullopt;
    }
    const std::size_t perMask = AlphaMask::wordsFor(int(cols), int(rows)) * sizeof(std::uint64_t);
    const std::size_t bodyBytes = (std::size_t(frames) + 1) * perMask;
    if (size != kMaskFileHeaderBytes + bodyBytes)
        return std::nullopt;
    if (get64(data + 32) != fnv1a(data + kMaskFileHeaderBytes, bodyBytes))
        return std::nullopt;

    const auto readMask = [&](std::size_t index) {
        std::vector<std::uint64_t> words(perMask / sizeof(std::uint64_t));
        const std::uint8_t *p = data + kMaskFileHeaderBytes + index * perMask;
        for (std::size_t i = 0; i < words.size(); ++i)
            words[i] = get64(p + i * 8);
        return AlphaMask::fromWords(int(w), int(h), int(cols), int(rows), int(cell), std::move(words));
    };
    auto silhouetteMask = readMask(0);
    if (!silhouetteMask)
        return std::nullopt;
    std::vector<AlphaMask> list;
    list.reserve(frames);
    for (std::uint32_t i = 0; i < frames; ++i) {
        auto m = readMask(std::size_t(i) + 1);
        if (!m || !m->isSubsetOf(*silhouetteMask))
            return std::nullopt;
        list.push_back(std::move(*m));
    }
    return MaskSet::assemble(quality == 1 ? MaskQuality::Full : quality == 2 ? MaskQuality::Coarse : MaskQuality::Silhouette,
                             std::move(*silhouetteMask), std::move(list));
}

bool writeMaskFile(const std::string &path, const MaskSet &set)
{
    const std::vector<std::uint8_t> bytes = serializeMaskFile(set);
    if (bytes.empty() || bytes.size() > kMaxMaskFileBytes)
        return false;
    QSaveFile out(QFile::decodeName(path.c_str()));
    if (!out.open(QIODevice::WriteOnly))
        return false;
    if (out.write(reinterpret_cast<const char *>(bytes.data()), qint64(bytes.size())) != qint64(bytes.size())) {
        out.cancelWriting();
        return false;
    }
    return out.commit(); // flushes, checks, and renames over the target atomically
}

std::optional<MaskSet> readMaskFile(const std::string &path, int expectedW, int expectedH, int expectedFrames)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size < kMaskFileHeaderBytes || size > kMaxMaskFileBytes)
        return std::nullopt;
    std::vector<std::uint8_t> bytes(size);
    std::ifstream in(path, std::ios::binary);
    in.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(size));
    if (in.gcount() != std::streamsize(size))
        return std::nullopt;
    return parseMaskFile(bytes.data(), bytes.size(), expectedW, expectedH, expectedFrames);
}

std::function<bool(PetPoint)> maskTest(std::function<std::shared_ptr<const AlphaMask>()> provider, PetSize window)
{
    return [provider = std::move(provider), window](PetPoint p) {
        if (!(window.w > 0) || !(window.h > 0) || !std::isfinite(p.x) || !std::isfinite(p.y))
            return false;
        if (p.x < 0 || p.y < 0 || p.x >= window.w || p.y >= window.h)
            return false;
        const std::shared_ptr<const AlphaMask> mask = provider ? provider() : nullptr;
        if (!mask || mask->isNull())
            return true; // no mask yet: the whole rectangle takes input (`hit mask: none`)
        return mask->contains(p.x * mask->spaceWidth() / window.w, p.y * mask->spaceHeight() / window.h);
    };
}

} // namespace murasaki
