#include "puppet/view.h"

#include "puppetlimits.h"

#include <algorithm>
#include <atomic>
#include <cmath>

namespace murasaki::puppet {

ViewTransform fitBounds(const Bounds &b, uint32_t width, uint32_t height)
{
    ViewTransform v;
    const double w = b.maxX - b.minX;
    const double h = b.maxY - b.minY;
    if (!(w > 0) || !(h > 0) || !std::isfinite(w) || !std::isfinite(h) || width == 0 || height == 0) {
        return v;
    }
    const double s = std::min(static_cast<double>(width) / w, static_cast<double>(height) / h); // pixels per model unit
    v.scaleX = 2.0 * s / width;
    v.scaleY = 2.0 * s / height;
    v.offsetX = -(b.minX + b.maxX) / 2 * v.scaleX;
    v.offsetY = -(b.minY + b.maxY) / 2 * v.scaleY;
    return v;
}

size_t HitMask::count() const
{
    size_t n = 0;
    for (const uint8_t byte : bits)
        n += static_cast<size_t>(__builtin_popcount(byte));
    return n;
}

namespace {

struct P {
    double x, y;
};

double edge(P a, P b, P p)
{
    return (b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x);
}

bool inside(P a, P b, P c, P p)
{
    const double e0 = edge(a, b, p), e1 = edge(b, c, p), e2 = edge(c, a, p);
    return (e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0);
}

} // namespace

namespace {

// Sets the cells c0..c1 of the rows r0..r1: whole bytes where it can, so a box of the picture's size costs a few thousand byte writes.
void fillBox(HitMask &mask, uint32_t c0, uint32_t c1, uint32_t r0, uint32_t r1)
{
    for (uint32_t r = r0; r <= r1; ++r) {
        size_t first = static_cast<size_t>(r) * mask.cols + c0;
        const size_t last = static_cast<size_t>(r) * mask.cols + c1; // inclusive
        while (first <= last && first % 8 != 0) {
            mask.bits[first / 8] |= static_cast<uint8_t>(1u << (first % 8));
            ++first;
        }
        while (first + 8 <= last + 1) {
            mask.bits[first / 8] = 0xFF;
            first += 8;
        }
        while (first <= last) {
            mask.bits[first / 8] |= static_cast<uint8_t>(1u << (first % 8));
            ++first;
        }
    }
}

// Pixel coordinates are held to +-1e9 (a model's numbers are finite but can be huge): nothing downstream converts a value it has not bounded.
constexpr double kPixelBound = 1e9;

double bounded(double v)
{
    return std::clamp(v, -kPixelBound, kPixelBound);
}

} // namespace

std::atomic<std::uint64_t> g_hitMaskRuns{0};

std::uint64_t hitMaskRuns()
{
    return g_hitMaskRuns.load();
}

HitMask hitMaskOf(const Frame &frame, const ModelBackend &model, const ViewTransform &view, uint32_t width, uint32_t height, uint32_t cell)
{
    ++g_hitMaskRuns;
    HitMask mask;
    cell = std::max<uint32_t>(cell, 1);
    while ((width + cell - 1) / cell > 256 || (height + cell - 1) / cell > 256)
        cell *= 2;
    mask.cell = cell;
    mask.cols = std::max<uint32_t>((width + cell - 1) / cell, 1);
    mask.rows = std::max<uint32_t>((height + cell - 1) / cell, 1);
    mask.bits.assign((static_cast<size_t>(mask.cols) * mask.rows + 7) / 8, 0);
    const auto isSet = [&](uint32_t c, uint32_t r) {
        const size_t i = static_cast<size_t>(r) * mask.cols + c;
        return (mask.bits[i / 8] >> (i % 8)) & 1u;
    };
    const auto set = [&](uint32_t c, uint32_t r) {
        const size_t i = static_cast<size_t>(r) * mask.cols + c;
        mask.bits[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
    };
    const auto toPixel = [&](Vec2 m) {
        return P{bounded(((m.x * view.scaleX + view.offsetX) + 1.0) * 0.5 * width), bounded(((m.y * view.scaleY + view.offsetY) + 1.0) * 0.5 * height)};
    };
    // Work is counted in units of about 10 ns: a row of cells costs kRowWork (its span is worked out), a cell tested kCellWork, a cell already set 1.
    constexpr std::int64_t kRowWork = 15, kCellWork = 7;
    // The budget is kMaxHitMaskWork; past the budget the triangles of an item are covered by the one box that holds them (more than they cover, which is the safe side for an input region).
    std::int64_t work = 0;
    for (const DrawItem &item : frame.items) {
        if (item.kind != DrawKind::Mesh || !(item.opacity > 0) || item.drawable < 0)
            continue;
        if (static_cast<size_t>(item.drawable) >= model.drawableCount())
            continue;
        const Shape &shape = model.shape(item.drawable);
        std::vector<P> px(item.positions.size());
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = toPixel(item.positions[i]);
        bool haveBox = false;
        uint32_t boxC0 = 0, boxC1 = 0, boxR0 = 0, boxR1 = 0;
        for (size_t t = 0; t + 2 < shape.indices.size(); t += 3) {
            if (shape.indices[t] >= px.size() || shape.indices[t + 1] >= px.size() || shape.indices[t + 2] >= px.size())
                continue;
            const P a = px[shape.indices[t]], b = px[shape.indices[t + 1]], c = px[shape.indices[t + 2]];
            const double minX = std::min({a.x, b.x, c.x}), maxX = std::max({a.x, b.x, c.x});
            const double minY = std::min({a.y, b.y, c.y}), maxY = std::max({a.y, b.y, c.y});
            if (!(maxX >= 0) || !(maxY >= 0) || !(minX < width) || !(minY < height))
                continue;
            // clamp in double first: a double that does not fit the integer is undefined to convert
            const uint32_t c0 = static_cast<uint32_t>(std::clamp(std::floor(minX / cell), 0.0, static_cast<double>(mask.cols - 1)));
            const uint32_t c1 = static_cast<uint32_t>(std::clamp(std::floor(maxX / cell), 0.0, static_cast<double>(mask.cols - 1)));
            const uint32_t r0 = static_cast<uint32_t>(std::clamp(std::floor(minY / cell), 0.0, static_cast<double>(mask.rows - 1)));
            const uint32_t r1 = static_cast<uint32_t>(std::clamp(std::floor(maxY / cell), 0.0, static_cast<double>(mask.rows - 1)));
            if (work > kMaxHitMaskWork) {
                ++work; // (a triangle past the budget still costs a little)
                if (!haveBox) {
                    boxC0 = c0, boxC1 = c1, boxR0 = r0, boxR1 = r1;
                    haveBox = true;
                } else {
                    boxC0 = std::min(boxC0, c0), boxC1 = std::max(boxC1, c1), boxR0 = std::min(boxR0, r0), boxR1 = std::max(boxR1, r1);
                }
                continue;
            }
            for (uint32_t r = r0; r <= r1; ++r) {
                // the columns the triangle reaches within this row of cells: only they can hold a hit (a sample or a vertex of a cell is inside the cell, so inside the triangle's span of the band)
                const double y0 = r * static_cast<double>(cell), y1 = y0 + cell;
                double spanMin = INFINITY, spanMax = -INFINITY;
                const P corners[3] = {a, b, c};
                for (int e = 0; e < 3; ++e) {
                    const P p = corners[e], q = corners[(e + 1) % 3];
                    double t0 = 0, t1 = 1;
                    const double dy = q.y - p.y;
                    if (dy == 0) {
                        if (p.y < y0 || p.y > y1)
                            continue;
                    } else {
                        const double ta = (y0 - p.y) / dy, tb = (y1 - p.y) / dy;
                        t0 = std::max(t0, std::min(ta, tb));
                        t1 = std::min(t1, std::max(ta, tb));
                        if (t0 > t1)
                            continue;
                    }
                    const double xa = p.x + (q.x - p.x) * t0, xb = p.x + (q.x - p.x) * t1;
                    spanMin = std::min({spanMin, xa, xb});
                    spanMax = std::max({spanMax, xa, xb});
                }
                work += kRowWork;
                if (!(spanMin <= spanMax) || spanMax < 0 || spanMin >= width)
                    continue;
                const uint32_t rowC0 = std::max(c0, static_cast<uint32_t>(std::clamp(std::floor(spanMin / cell), 0.0, static_cast<double>(mask.cols - 1))));
                const uint32_t rowC1 = std::min(c1, static_cast<uint32_t>(std::clamp(std::floor(spanMax / cell), 0.0, static_cast<double>(mask.cols - 1))));
                for (uint32_t col = rowC0; col <= rowC1; ++col) {
                    if (isSet(col, r)) {
                        ++work;
                        continue;
                    }
                    work += kCellWork;
                    const double x0 = col * static_cast<double>(cell), x1 = x0 + cell;
                    const double i = 1.0;
                    const P samples[5] = {{(x0 + x1) / 2, (y0 + y1) / 2}, {x0 + i, y0 + i}, {x1 - i, y0 + i}, {x0 + i, y1 - i}, {x1 - i, y1 - i}};
                    bool hit = false;
                    for (const P &s : samples)
                        hit = hit || inside(a, b, c, s);
                    for (const P &v : {a, b, c})
                        hit = hit || (v.x >= x0 && v.x < x1 && v.y >= y0 && v.y < y1);
                    if (hit)
                        set(col, r);
                }
            }
        }
        if (haveBox)
            fillBox(mask, boxC0, boxC1, boxR0, boxR1);
    }
    return mask;
}

namespace {

// An upper bound of the area one mesh covers on the picture, from the boxes of its triangles (no clipping of the triangle itself): half the box where the box lies inside the picture (a triangle fills at most half
// of its box), the box clipped to the picture where it crosses an edge. Also the union box of the mesh, clipped, for the group it lies in. `work` counts the triangles looked at.
struct MeshCost {
    double area = 0;
    double minX = INFINITY, minY = INFINITY, maxX = -INFINITY, maxY = -INFINITY; // union of the clipped boxes
    bool any = false;
};

MeshCost meshCost(const DrawItem &item, const Shape &shape, const ViewTransform &view, double width, double height, std::int64_t &work)
{
    MeshCost cost;
    const size_t count = item.positions.size();
    const auto toX = [&](double x) { return bounded(((x * view.scaleX + view.offsetX) + 1.0) * 0.5 * width); };
    const auto toY = [&](double y) { return bounded(((y * view.scaleY + view.offsetY) + 1.0) * 0.5 * height); };
    for (size_t t = 0; t + 2 < shape.indices.size(); t += 3) {
        ++work;
        const uint16_t a = shape.indices[t], b = shape.indices[t + 1], c = shape.indices[t + 2];
        if (a >= count || b >= count || c >= count)
            continue;
        const double ax = toX(item.positions[a].x), bx = toX(item.positions[b].x), cx = toX(item.positions[c].x);
        const double ay = toY(item.positions[a].y), by = toY(item.positions[b].y), cy = toY(item.positions[c].y);
        const double x0 = std::min({ax, bx, cx}), x1 = std::max({ax, bx, cx}), y0 = std::min({ay, by, cy}), y1 = std::max({ay, by, cy});
        const double cx0 = std::max(x0, 0.0), cx1 = std::min(x1, width), cy0 = std::max(y0, 0.0), cy1 = std::min(y1, height);
        if (!(cx1 > cx0) || !(cy1 > cy0))
            continue; // off the picture (or no extent)
        const bool inside = cx0 == x0 && cx1 == x1 && cy0 == y0 && cy1 == y1;
        cost.area += (cx1 - cx0) * (cy1 - cy0) * (inside ? 0.5 : 1.0);
        cost.minX = std::min(cost.minX, cx0);
        cost.maxX = std::max(cost.maxX, cx1);
        cost.minY = std::min(cost.minY, cy0);
        cost.maxY = std::max(cost.maxY, cy1);
        cost.any = true;
    }
    return cost;
}

} // namespace

std::atomic<std::uint64_t> g_frameCostRuns{0};

std::uint64_t frameCostRuns()
{
    return g_frameCostRuns.load();
}

FrameCost frameCost(const Frame &frame, const ModelBackend &model, const ViewTransform &view, uint32_t width, uint32_t height)
{
    ++g_frameCostRuns;
    FrameCost cost;
    const double w = width, h = height, picture = w * h;
    std::int64_t work = 0;
    const size_t drawables = model.drawableCount();
    // a mask source is drawn again for every item that uses it; its area is worked out once
    std::vector<double> sourceArea(frame.maskSources.size(), -1.0);
    std::vector<int> sourceOf(drawables, -1);
    for (size_t i = 0; i < frame.maskSources.size(); ++i) {
        const int d = frame.maskSources[i].drawable;
        if (d >= 0 && static_cast<size_t>(d) < sourceOf.size())
            sourceOf[static_cast<size_t>(d)] = static_cast<int>(i);
    }
    // Past the budget of triangles looked at, what is left is counted as pictures: an item by itself, a mask use by its source's whole picture.
    const auto overBudget = [&] { return work > kMaxFrameCostWork; };
    const auto addMasks = [&](const DrawItem &item) {
        if (item.masks.empty())
            return;
        cost.pixels += picture / static_cast<double>(kStencilClearShare); // the stencil is cleared (a fast clear)
        for (const MaskRef &ref : item.masks) {
            if (ref.source < 0 || static_cast<size_t>(ref.source) >= sourceOf.size() || sourceOf[static_cast<size_t>(ref.source)] < 0)
                continue;
            const size_t s = static_cast<size_t>(sourceOf[static_cast<size_t>(ref.source)]);
            const DrawItem &source = frame.maskSources[s];
            if (sourceArea[s] < 0) {
                if (overBudget() || source.drawable < 0 || static_cast<size_t>(source.drawable) >= drawables)
                    sourceArea[s] = overBudget() ? picture : 0;
                else
                    sourceArea[s] = meshCost(source, model.shape(source.drawable), view, w, h, work).area;
            }
            cost.pixels += sourceArea[s];
            cost.vertices += source.positions.size();
        }
    };
    // The group being drawn: the union of its members' boxes is what its image is worth when it is put on the picture.
    bool inGroup = false;
    MeshCost group;
    for (const DrawItem &item : frame.items) {
        switch (item.kind) {
        case DrawKind::Mesh: {
            if (!(item.opacity > 0) || item.drawable < 0 || static_cast<size_t>(item.drawable) >= drawables)
                break;
            if (overBudget()) {
                cost.pixels += picture;
            } else {
                const MeshCost mesh = meshCost(item, model.shape(item.drawable), view, w, h, work);
                cost.pixels += mesh.area;
                if (inGroup && mesh.any) {
                    group.minX = std::min(group.minX, mesh.minX);
                    group.maxX = std::max(group.maxX, mesh.maxX);
                    group.minY = std::min(group.minY, mesh.minY);
                    group.maxY = std::max(group.maxY, mesh.maxY);
                    group.any = true;
                }
            }
            cost.vertices += item.positions.size();
            addMasks(item);
            break;
        }
        case DrawKind::BeginGroup:
            inGroup = true;
            group = {};
            cost.pixels += picture / static_cast<double>(kStencilClearShare); // the group's image is cleared
            break;
        case DrawKind::EndGroup:
            if (group.any)
                cost.pixels += (group.maxX - group.minX) * (group.maxY - group.minY); // put on the picture: by the box of what it holds
            inGroup = false;
            group = {};
            addMasks(item);
            break;
        }
    }
    return cost;
}

bool HitMaskTracker::update(const HitMask &mask)
{
    if (!m_sent || m_last.cols != mask.cols || m_last.rows != mask.rows || m_last.cell != mask.cell) {
        m_last = mask;
        m_sent = true;
        m_since = 0;
        return true;
    }
    size_t changed = 0;
    for (size_t i = 0; i < mask.bits.size(); ++i)
        changed += static_cast<size_t>(__builtin_popcount(static_cast<unsigned>(mask.bits[i] ^ m_last.bits[i])));
    ++m_since;
    const size_t cells = static_cast<size_t>(mask.cols) * mask.rows;
    if (changed == 0)
        return false;
    if (static_cast<double>(changed) >= m_share * static_cast<double>(cells) || m_since >= m_stale) {
        m_last = mask;
        m_since = 0;
        return true;
    }
    return false;
}

} // namespace murasaki::puppet
