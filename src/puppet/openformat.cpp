#include "puppet/openformat.h"

#include "puppet/manifest.h"
#include "puppetlimits.h"
#include "puppet/strictjson.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <map>
#include <set>

namespace murasaki::puppet {

namespace {

constexpr std::array<const char *, kBlendModeCount> kBlendNames = {"Normal", "Multiply", "Screen", "Overlay", "Darken", "Lighten", "ColorDodge", "LinearDodge", "AddGlow", "ColorBurn",
                                                                   "HardLight", "SoftLight", "Difference", "Exclusion", "Subtract", "Inverse", "DestinationIn", "ClipToLower", "SliceFromLower"};
constexpr std::uint32_t kNoTexture = 0xFFFFFFFFu;

// ---- small linear algebra: 4x4 for the node chain, 2D affine for the mesh group maths ----
struct Mat4 {
    double m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}; // row-major
};

Mat4 mul(const Mat4 &a, const Mat4 &b)
{
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            double s = 0;
            for (int k = 0; k < 4; ++k)
                s += a.m[i * 4 + k] * b.m[k * 4 + j];
            r.m[i * 4 + j] = s;
        }
    return r;
}

Vec2 apply(const Mat4 &a, Vec2 p)
{
    return {a.m[0] * p.x + a.m[1] * p.y + a.m[3], a.m[4] * p.x + a.m[5] * p.y + a.m[7]};
}

struct Aff2 {
    double a = 1, b = 0, c = 0, d = 1, tx = 0, ty = 0; // x' = a x + b y + tx ; y' = c x + d y + ty
};

Aff2 toAff(const Mat4 &m)
{
    return {m.m[0], m.m[1], m.m[4], m.m[5], m.m[3], m.m[7]};
}

Vec2 apply(const Aff2 &f, Vec2 p)
{
    return {f.a * p.x + f.b * p.y + f.tx, f.c * p.x + f.d * p.y + f.ty};
}

bool invert(const Aff2 &f, Aff2 &out)
{
    const double det = f.a * f.d - f.b * f.c;
    if (!(std::abs(det) > 1e-12) || !std::isfinite(det))
        return false;
    out.a = f.d / det;
    out.b = -f.b / det;
    out.c = -f.c / det;
    out.d = f.a / det;
    out.tx = -(out.a * f.tx + out.b * f.ty);
    out.ty = -(out.c * f.tx + out.d * f.ty);
    return true;
}

Aff2 compose(const Aff2 &outer, const Aff2 &inner) // outer after inner
{
    return {outer.a * inner.a + outer.b * inner.c, outer.a * inner.b + outer.b * inner.d, outer.c * inner.a + outer.d * inner.c, outer.c * inner.b + outer.d * inner.d,
            outer.a * inner.tx + outer.b * inner.ty + outer.tx, outer.c * inner.tx + outer.d * inner.ty + outer.ty};
}

Mat4 eulerMatrix(double rx, double ry, double rz)
{
    const double cr = std::cos(rx / 2), sr = std::sin(rx / 2), cp = std::cos(ry / 2), sp = std::sin(ry / 2), cy = std::cos(rz / 2), sy = std::sin(rz / 2);
    const double w = cr * cp * cy + sr * sp * sy;
    const double x = sr * cp * cy - cr * sp * sy;
    const double y = cr * sp * cy + sr * cp * sy;
    const double z = cr * cp * sy - sr * sp * cy;
    Mat4 r;
    r.m[0] = 1 - 2 * (y * y + z * z);
    r.m[1] = 2 * (x * y - w * z);
    r.m[2] = 2 * (x * z + w * y);
    r.m[4] = 2 * (x * y + w * z);
    r.m[5] = 1 - 2 * (x * x + z * z);
    r.m[6] = 2 * (y * z - w * x);
    r.m[8] = 2 * (x * z - w * y);
    r.m[9] = 2 * (y * z + w * x);
    r.m[10] = 1 - 2 * (x * x + y * y);
    return r;
}

double clamp01(double v)
{
    return std::clamp(v, 0.0, 1.0);
}

// ---- the puppet ----
enum class NodeType { Node, Part, Composite, MeshGroup, SimplePhysics };
enum class Interp { Nearest, Linear, Cubic };
enum class Prop {
    ZSort, Tx, Ty, Tz, Rx, Ry, Rz, Sx, Sy, Opacity, TintR, TintG, TintB, ScreenR, ScreenG, ScreenB, Deform, Ignored
};

struct MaskBinding {
    std::uint32_t sourceUuid = 0;
    int source = -1;
    bool dodge = false;
};

struct Node {
    std::uint32_t uuid = 0;
    std::string name;
    NodeType type = NodeType::Node;
    bool enabled = true;
    double zsort = 0;
    double trans[3] = {0, 0, 0};
    double rot[3] = {0, 0, 0};
    double scale[2] = {1, 1};
    bool lockToRoot = false;
    int parent = -1;
    std::vector<int> children;
    // drawable
    std::vector<Vec2> verts; // already minus the mesh origin
    Shape shape;
    // part / composite
    int texture = -1;
    BlendMode blend = BlendMode::Normal;
    double tint[3] = {1, 1, 1};
    double screen[3] = {0, 0, 0};
    double opacity = 1;
    double maskThreshold = 0.5;
    std::vector<MaskBinding> masks;
    bool propagate = false;
    bool dynamic = false; // mesh group
    int partIndex = -1;
    int drawableOrdinal = -1; // index among drawables for Shape lookup
};

struct Binding {
    int node = -1;
    Prop prop = Prop::Ignored;
    Interp mode = Interp::Linear;
    std::vector<double> scalars;                 // [x * ny + y]
    std::vector<std::vector<Vec2>> deforms;      // [x * ny + y][vertex]
};

struct Param {
    bool is2d = false;
    double minX = 0, maxX = 1, minY = 0, maxY = 1;
    std::vector<double> axisX{0, 1};
    std::vector<double> axisY{0, 1};
    int indexX = -1; // in the ParameterSet
    int indexY = -1;
    std::vector<Binding> bindings;
};

struct Cell {
    int x, y;
    double w;
};

void catmullWeights(double t, double w[4])
{
    const double t2 = t * t, t3 = t2 * t;
    w[0] = 0.5 * (-t + 2 * t2 - t3);
    w[1] = 0.5 * (2 - 5 * t2 + 3 * t3);
    w[2] = 0.5 * (t + 4 * t2 - 3 * t3);
    w[3] = 0.5 * (-t2 + t3);
}

void locate(const std::vector<double> &axis, double v, int &index, double &t)
{
    const int n = static_cast<int>(axis.size());
    if (n < 2) {
        index = 0;
        t = 0;
        return;
    }
    int i = n - 2;
    for (int k = 0; k + 1 < n; ++k) {
        if (axis[static_cast<size_t>(k) + 1] > v) {
            i = k;
            break;
        }
    }
    index = i;
    t = (v - axis[static_cast<size_t>(i)]) / (axis[static_cast<size_t>(i) + 1] - axis[static_cast<size_t>(i)]);
}

// The cells (with weights) a binding reads for the normalised place (nx, ny) of its parameter.
void cellsFor(const Param &p, Interp mode, double nx, double ny, std::vector<Cell> &out)
{
    out.clear();
    const int cx = static_cast<int>(p.axisX.size());
    const int cy = p.is2d ? static_cast<int>(p.axisY.size()) : 1;
    int ix, iy = 0;
    double tx, ty = 0;
    locate(p.axisX, nx, ix, tx);
    if (p.is2d)
        locate(p.axisY, ny, iy, ty);
    switch (mode) {
    case Interp::Nearest:
        out.push_back({std::min(ix + (tx >= 0.5 ? 1 : 0), cx - 1), p.is2d ? std::min(iy + (ty >= 0.5 ? 1 : 0), cy - 1) : 0, 1.0});
        break;
    case Interp::Linear:
        for (int dx = 0; dx < 2; ++dx) {
            const double wx = dx ? tx : 1 - tx;
            const int x = std::min(ix + dx, cx - 1);
            if (!p.is2d) {
                out.push_back({x, 0, wx});
                continue;
            }
            for (int dy = 0; dy < 2; ++dy)
                out.push_back({x, std::min(iy + dy, cy - 1), wx * (dy ? ty : 1 - ty)});
        }
        break;
    case Interp::Cubic: {
        double wx[4], wy[4];
        catmullWeights(tx, wx);
        catmullWeights(ty, wy);
        const int xs[4] = {std::max(ix - 1, 0), ix, std::min(ix + 1, cx - 1), std::min(ix + 2, cx - 1)};
        const int ys[4] = {std::max(iy - 1, 0), iy, std::min(iy + 1, cy - 1), std::min(iy + 2, cy - 1)};
        for (int a = 0; a < 4; ++a) {
            if (!p.is2d) {
                out.push_back({xs[a], 0, wx[a]});
                continue;
            }
            for (int b = 0; b < 4; ++b)
                out.push_back({xs[a], ys[b], wx[a] * wy[b]});
        }
        break;
    }
    }
}

// The mesh group's warp of points of its rest mesh (docs/pets/puppets.md, section 12.5).
struct Warp {
    struct Tri {
        std::uint32_t a, b, c;
        Vec2 p1, e1, e2;
        double invDet;
    };
    std::vector<Tri> tris;
    double minX = 0, minY = 0, cellW = 1, cellH = 1;
    int grid = 1;
    std::vector<std::uint32_t> offsets; // CSR: the triangles of cell c are entries[offsets[c] .. offsets[c + 1])
    std::vector<std::uint32_t> entries; // triangle indices, ascending within a cell

    // False (and `why` set) when the mesh would need more than kMaxWarpTriangles triangles or more cell entries than `budget` has left. Repeated triangles (the same three vertices in any order) are
    // kept once, at the place of their last occurrence, and flat ones are dropped, so a hostile index list cannot multiply the work.
    bool build(const std::vector<Vec2> &rest, const std::vector<std::uint16_t> &indices, std::int64_t &budget, std::string &why)
    {
        tris.clear();
        offsets.clear();
        entries.clear();
        if (rest.empty())
            return true;
        double maxX = rest[0].x, maxY = rest[0].y;
        minX = rest[0].x;
        minY = rest[0].y;
        for (const Vec2 &p : rest) {
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
        std::map<std::array<std::uint16_t, 3>, size_t> last; // sorted corners -> position of the last occurrence
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            std::array<std::uint16_t, 3> key{indices[i], indices[i + 1], indices[i + 2]};
            std::sort(key.begin(), key.end());
            last[key] = i;
        }
        for (size_t i = 0; i + 2 < indices.size(); i += 3) {
            std::array<std::uint16_t, 3> key{indices[i], indices[i + 1], indices[i + 2]};
            std::sort(key.begin(), key.end());
            if (last[key] != i)
                continue;
            const Vec2 p1 = rest[indices[i]], p2 = rest[indices[i + 1]], p3 = rest[indices[i + 2]];
            const Vec2 e1{p2.x - p1.x, p2.y - p1.y}, e2{p3.x - p1.x, p3.y - p1.y};
            const double det = e1.x * e2.y - e1.y * e2.x;
            if (!(std::abs(det) > 1e-12))
                continue; // a flat triangle contains nothing
            if (static_cast<std::int64_t>(tris.size()) >= kMaxWarpTriangles) {
                why = "a mesh group has more than kMaxWarpTriangles distinct triangles";
                return false;
            }
            tris.push_back({indices[i], indices[i + 1], indices[i + 2], p1, e1, e2, 1.0 / det});
        }
        grid = std::clamp(static_cast<int>(std::ceil(std::sqrt(static_cast<double>(tris.size())))), 1, 64);
        cellW = std::max((maxX - minX) / grid, 1e-9);
        cellH = std::max((maxY - minY) / grid, 1e-9);
        struct Range {
            int x0, x1, y0, y1;
        };
        std::vector<Range> ranges(tris.size());
        std::int64_t total = 0;
        for (size_t t = 0; t < tris.size(); ++t) {
            const Tri &tr = tris[t];
            const Vec2 q2{tr.p1.x + tr.e1.x, tr.p1.y + tr.e1.y}, q3{tr.p1.x + tr.e2.x, tr.p1.y + tr.e2.y};
            ranges[t] = {cellIndex((std::min({tr.p1.x, q2.x, q3.x}) - minX) / cellW), cellIndex((std::max({tr.p1.x, q2.x, q3.x}) - minX) / cellW),
                         cellIndex((std::min({tr.p1.y, q2.y, q3.y}) - minY) / cellH), cellIndex((std::max({tr.p1.y, q2.y, q3.y}) - minY) / cellH)};
            total += static_cast<std::int64_t>(ranges[t].x1 - ranges[t].x0 + 1) * (ranges[t].y1 - ranges[t].y0 + 1);
            if (total > budget) {
                why = "the mesh groups need more than kMaxWarpCellEntries grid entries";
                return false;
            }
        }
        budget -= total;
        const size_t cellCount = static_cast<size_t>(grid) * static_cast<size_t>(grid);
        offsets.assign(cellCount + 1, 0);
        for (const Range &r : ranges)
            for (int y = r.y0; y <= r.y1; ++y)
                for (int x = r.x0; x <= r.x1; ++x)
                    ++offsets[static_cast<size_t>(y) * static_cast<size_t>(grid) + static_cast<size_t>(x) + 1];
        for (size_t c = 0; c < cellCount; ++c)
            offsets[c + 1] += offsets[c];
        entries.assign(static_cast<size_t>(total), 0);
        std::vector<std::uint32_t> fill(offsets.begin(), offsets.end() - 1);
        for (size_t t = 0; t < ranges.size(); ++t)
            for (int y = ranges[t].y0; y <= ranges[t].y1; ++y)
                for (int x = ranges[t].x0; x <= ranges[t].x1; ++x)
                    entries[fill[static_cast<size_t>(y) * static_cast<size_t>(grid) + static_cast<size_t>(x)]++] = static_cast<std::uint32_t>(t);
        return true;
    }

    int cellIndex(double v) const { return std::isfinite(v) ? std::clamp(static_cast<int>(std::floor(std::clamp(v, -1.0, 1e6))), 0, grid - 1) : 0; }

    // The point q (rest space) carried by the deformed corners `moved` (rest + deformation); unchanged when no triangle holds it. The last triangle holding the point wins; at most
    // kMaxWarpCandidates triangles of the point's cell are tried.
    Vec2 carry(Vec2 q, const std::vector<Vec2> &moved) const
    {
        if (tris.empty())
            return q;
        const size_t cell = static_cast<size_t>(cellIndex((q.y - minY) / cellH)) * static_cast<size_t>(grid) + static_cast<size_t>(cellIndex((q.x - minX) / cellW));
        std::int64_t tried = 0;
        for (size_t e = offsets[cell + 1]; e > offsets[cell] && tried < kMaxWarpCandidates; --e, ++tried) {
            const Tri &t = tris[entries[e - 1]];
            const Vec2 w{q.x - t.p1.x, q.y - t.p1.y};
            const double u = (w.x * t.e2.y - w.y * t.e2.x) * t.invDet;
            const double v = (t.e1.x * w.y - t.e1.y * w.x) * t.invDet;
            constexpr double eps = 1e-9;
            if (u >= -eps && v >= -eps && u + v <= 1 + eps) {
                const Vec2 &a = moved[t.a], &b = moved[t.b], &c = moved[t.c];
                return {a.x + u * (b.x - a.x) + v * (c.x - a.x), a.y + u * (b.y - a.y) + v * (c.y - a.y)};
            }
        }
        return q;
    }
};

} // namespace

const char *blendModeName(BlendMode mode)
{
    const size_t i = static_cast<size_t>(mode);
    return i < kBlendNames.size() ? kBlendNames[i] : "Normal";
}

struct OpenModel::Impl {
    OpenModelInfo info;
    ParameterSet parameters;
    PartSet parts;
    std::vector<Node> nodes;
    std::vector<Param> params;
    std::vector<int> drawables;       // node index by drawable ordinal (Part and MeshGroup)
    std::vector<int> rootList;        // Parts and Composites in the order the puppet draws them from
    std::vector<std::vector<int>> subParts; // by node index (Composites)
    std::vector<int> groupOf;         // nearest mesh group that carries this node, -1 none
    std::vector<Warp> warps;          // by node index (mesh groups), others empty
    std::string file;                 // the bytes of the .inp (the textures are ranges of it)
    std::vector<std::vector<Vec2>> groupMoved; // per mesh group and frame: its rest vertices plus their deformation
    std::vector<Aff2> groupWorld, groupInverse; // per mesh group and frame
    std::vector<char> groupOk;
    std::vector<int> maskSources;     // node indices some mask uses
    Bounds bounds;
    // per frame
    std::vector<double> offSort, offT, offR, offS, opMul, tintMul, screenAdd;
    std::vector<std::vector<Vec2>> deform;
    std::vector<Mat4> world;
    std::vector<char> enabled;
    std::vector<double> zKey;
    std::vector<Cell> cellScratch;

    void prepareFrame();
    void evaluateParameters(const ParameterSet &values);
    void computeWorld();
    void prepareGroups();
    std::vector<Vec2> positionsOf(int node) const;
    bool build(Frame &out, const PartSet &partOpacities);
    void fillItem(DrawItem &item, int node, const PartSet &partOpacities, bool withPositions) const;
};

OpenModel::OpenModel(std::unique_ptr<Impl> impl) : m_impl(std::move(impl)) {}
OpenModel::~OpenModel() = default;

const OpenModelInfo &OpenModel::info() const { return m_impl->info; }
std::string OpenModel::name() const { return m_impl->info.name; }
const ParameterSet &OpenModel::parameters() const { return m_impl->parameters; }
const PartSet &OpenModel::parts() const { return m_impl->parts; }
size_t OpenModel::drawableCount() const { return m_impl->nodes.size(); }
std::string OpenModel::drawableId(int drawable) const
{
    return drawable >= 0 && static_cast<size_t>(drawable) < m_impl->nodes.size() ? m_impl->nodes[static_cast<size_t>(drawable)].name : std::string();
}
const Shape &OpenModel::shape(int drawable) const
{
    static const Shape none;
    if (drawable < 0 || static_cast<size_t>(drawable) >= m_impl->nodes.size())
        return none;
    return m_impl->nodes[static_cast<size_t>(drawable)].shape;
}
Bounds OpenModel::bounds() const { return m_impl->bounds; }

// ---------------------------------------------------------------------------------------------------------------------------------------------
// evaluation

void OpenModel::Impl::prepareFrame()
{
    const size_t n = nodes.size();
    std::fill(offSort.begin(), offSort.end(), 0.0);
    for (size_t i = 0; i < n; ++i) {
        offT[i * 3] = offT[i * 3 + 1] = offT[i * 3 + 2] = 0;
        offR[i * 3] = offR[i * 3 + 1] = offR[i * 3 + 2] = 0;
        offS[i * 2] = offS[i * 2 + 1] = 1;
        opMul[i] = 1;
        tintMul[i * 3] = tintMul[i * 3 + 1] = tintMul[i * 3 + 2] = 1;
        screenAdd[i * 3] = screenAdd[i * 3 + 1] = screenAdd[i * 3 + 2] = 0;
        std::fill(deform[i].begin(), deform[i].end(), Vec2{});
    }
}

void OpenModel::Impl::evaluateParameters(const ParameterSet &values)
{
    for (const Param &p : params) {
        const auto norm = [](double v, double lo, double hi) {
            const double n = hi > lo ? (v - lo) / (hi - lo) : 0.0;
            return std::isfinite(n) ? clamp01(n) : 0.0;
        };
        const double nx = norm(values.value(p.indexX), p.minX, p.maxX);
        const double ny = p.is2d ? norm(values.value(p.indexY), p.minY, p.maxY) : 0.0;
        for (const Binding &b : p.bindings) {
            cellsFor(p, b.mode, nx, ny, cellScratch);
            const size_t i = static_cast<size_t>(b.node);
            const size_t ny0 = p.axisY.size(); // a one-dimensional parameter's bindings are stored with the file's y count too
            if (b.prop == Prop::Deform) {
                std::vector<Vec2> &d = deform[i];
                for (const Cell &c : cellScratch) {
                    const std::vector<Vec2> &cell = b.deforms[static_cast<size_t>(c.x) * ny0 + static_cast<size_t>(c.y)];
                    for (size_t k = 0; k < d.size(); ++k) {
                        d[k].x += c.w * cell[k].x;
                        d[k].y += c.w * cell[k].y;
                    }
                }
                continue;
            }
            double v = 0;
            for (const Cell &c : cellScratch)
                v += c.w * b.scalars[static_cast<size_t>(c.x) * ny0 + static_cast<size_t>(c.y)];
            switch (b.prop) {
            case Prop::ZSort: offSort[i] += v; break;
            case Prop::Tx: offT[i * 3] += v; break;
            case Prop::Ty: offT[i * 3 + 1] += v; break;
            case Prop::Tz: offT[i * 3 + 2] += v; break;
            case Prop::Rx: offR[i * 3] += v; break;
            case Prop::Ry: offR[i * 3 + 1] += v; break;
            case Prop::Rz: offR[i * 3 + 2] += v; break;
            case Prop::Sx: offS[i * 2] *= v; break;
            case Prop::Sy: offS[i * 2 + 1] *= v; break;
            case Prop::Opacity: opMul[i] *= v; break;
            case Prop::TintR: tintMul[i * 3] *= v; break;
            case Prop::TintG: tintMul[i * 3 + 1] *= v; break;
            case Prop::TintB: tintMul[i * 3 + 2] *= v; break;
            case Prop::ScreenR: screenAdd[i * 3] += v; break;
            case Prop::ScreenG: screenAdd[i * 3 + 1] += v; break;
            case Prop::ScreenB: screenAdd[i * 3 + 2] += v; break;
            case Prop::Deform:
            case Prop::Ignored: break;
            }
        }
    }
}

void OpenModel::Impl::computeWorld()
{
    const auto local = [&](size_t i, bool withOffsets) {
        const Node &n = nodes[i];
        const double t[3] = {n.trans[0] + (withOffsets ? offT[i * 3] : 0), n.trans[1] + (withOffsets ? offT[i * 3 + 1] : 0), n.trans[2] + (withOffsets ? offT[i * 3 + 2] : 0)};
        const double r[3] = {n.rot[0] + (withOffsets ? offR[i * 3] : 0), n.rot[1] + (withOffsets ? offR[i * 3 + 1] : 0), n.rot[2] + (withOffsets ? offR[i * 3 + 2] : 0)};
        const double s[2] = {n.scale[0] * (withOffsets ? offS[i * 2] : 1), n.scale[1] * (withOffsets ? offS[i * 2 + 1] : 1)};
        Mat4 rotation = eulerMatrix(r[0], r[1], r[2]);
        Mat4 m;
        for (int row = 0; row < 3; ++row) {
            m.m[row * 4 + 0] = rotation.m[row * 4 + 0] * s[0];
            m.m[row * 4 + 1] = rotation.m[row * 4 + 1] * s[1];
            m.m[row * 4 + 2] = rotation.m[row * 4 + 2];
        }
        m.m[3] = t[0];
        m.m[7] = t[1];
        m.m[11] = t[2];
        return m;
    };
    const Mat4 rootLocal = local(0, false);
    for (size_t i = 0; i < nodes.size(); ++i) {
        const Node &n = nodes[i];
        const Mat4 own = local(i, true);
        if (n.parent < 0)
            world[i] = own;
        else
            world[i] = mul(n.lockToRoot ? rootLocal : world[static_cast<size_t>(n.parent)], own);
        for (const double v : world[i].m) {
            if (!std::isfinite(v)) { // a chain of huge scales: the node collapses to a point rather than carry infinities
                world[i] = Mat4{};
                std::fill(std::begin(world[i].m), std::end(world[i].m), 0.0);
                break;
            }
        }
        enabled[i] = n.enabled && (n.parent < 0 || enabled[static_cast<size_t>(n.parent)]);
        const double z = n.zsort + offSort[i] + (n.parent < 0 ? 0.0 : zKey[static_cast<size_t>(n.parent)]);
        zKey[i] = std::isfinite(z) ? z : 0.0; // the sort needs a number it can order
    }
}

void OpenModel::Impl::prepareGroups()
{
    for (size_t g = 0; g < nodes.size(); ++g) {
        if (nodes[g].type != NodeType::MeshGroup)
            continue;
        groupWorld[g] = toAff(world[g]);
        groupOk[g] = invert(groupWorld[g], groupInverse[g]) ? 1 : 0;
        std::vector<Vec2> &moved = groupMoved[g];
        moved.resize(nodes[g].verts.size());
        for (size_t k = 0; k < moved.size(); ++k)
            moved[k] = {nodes[g].verts[k].x + deform[g][k].x, nodes[g].verts[k].y + deform[g][k].y};
    }
}

std::vector<Vec2> OpenModel::Impl::positionsOf(int node) const
{
    const size_t i = static_cast<size_t>(node);
    const Node &n = nodes[i];
    const Mat4 &w = world[i];
    std::vector<Vec2> out(n.verts.size());
    const std::vector<Vec2> &d = deform[i];
    const int g = groupOf[i];
    Aff2 toGroup, fromGroupLinear;
    bool carried = false;
    if (g >= 0 && groupOk[static_cast<size_t>(g)]) {
        toGroup = compose(groupInverse[static_cast<size_t>(g)], toAff(w));
        Aff2 inv2;
        if (invert(toGroup, inv2)) {
            fromGroupLinear = inv2;
            fromGroupLinear.tx = fromGroupLinear.ty = 0;
            carried = true;
        }
    }
    for (size_t k = 0; k < out.size(); ++k) {
        Vec2 p{n.verts[k].x + d[k].x, n.verts[k].y + d[k].y};
        if (carried) {
            const size_t gi = static_cast<size_t>(g);
            const Warp &warp = warps[gi];
            if (nodes[gi].dynamic) {
                const Vec2 q = apply(toGroup, p);
                out[k] = apply(groupWorld[gi], warp.carry(q, groupMoved[gi]));
                continue;
            }
            const Vec2 q = apply(toGroup, n.verts[k]);
            const Vec2 q2 = warp.carry(q, groupMoved[gi]);
            const Vec2 delta = apply(fromGroupLinear, Vec2{q2.x - q.x, q2.y - q.y});
            p = {p.x + delta.x, p.y + delta.y};
        }
        out[k] = apply(w, p);
    }
    // whatever the numbers did on the way, what leaves is finite and within kMaxOutputCoordinate (a double beyond a float's range is not a number the renderer may convert)
    constexpr double kBound = static_cast<double>(kMaxOutputCoordinate);
    for (Vec2 &v : out) {
        if (!std::isfinite(v.x) || !std::isfinite(v.y))
            v = {};
        v.x = std::clamp(v.x, -kBound, kBound);
        v.y = std::clamp(v.y, -kBound, kBound);
    }
    return out;
}

void OpenModel::Impl::fillItem(DrawItem &item, int node, const PartSet &partOpacities, bool withPositions) const
{
    const size_t i = static_cast<size_t>(node);
    const Node &n = nodes[i];
    item.drawable = node;
    item.blend = n.blend;
    item.texture = n.texture;
    item.maskThreshold = clamp01(n.maskThreshold);
    const double part = n.partIndex >= 0 ? partOpacities.opacity(n.partIndex) : 1.0;
    item.opacity = clamp01(n.opacity * opMul[i]) * part;
    for (int c = 0; c < 3; ++c) {
        item.tint[c] = clamp01(n.tint[c] * tintMul[i * 3 + static_cast<size_t>(c)]);
        item.screenTint[c] = clamp01(n.screen[c] + screenAdd[i * 3 + static_cast<size_t>(c)]);
    }
    for (const MaskBinding &m : n.masks)
        item.masks.push_back({m.source, m.dodge});
    if (withPositions)
        item.positions = positionsOf(node);
}

bool OpenModel::Impl::build(Frame &out, const PartSet &partOpacities)
{
    out.items.clear();
    out.maskSources.clear();
    std::vector<int> order = rootList;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return zKey[static_cast<size_t>(a)] > zKey[static_cast<size_t>(b)]; });
    const auto drawPart = [&](int node) {
        const Node &n = nodes[static_cast<size_t>(node)];
        if (!enabled[static_cast<size_t>(node)] || n.shape.indices.empty())
            return;
        DrawItem item;
        item.kind = DrawKind::Mesh;
        fillItem(item, node, partOpacities, true);
        out.items.push_back(std::move(item));
    };
    for (const int node : order) {
        const Node &n = nodes[static_cast<size_t>(node)];
        if (!enabled[static_cast<size_t>(node)])
            continue;
        if (n.type == NodeType::Part) {
            drawPart(node);
            continue;
        }
        // a composite: its parts, back to front, in a group
        DrawItem begin;
        begin.kind = DrawKind::BeginGroup;
        begin.drawable = node;
        out.items.push_back(std::move(begin));
        std::vector<int> subs = subParts[static_cast<size_t>(node)];
        std::stable_sort(subs.begin(), subs.end(), [&](int a, int b) { return zKey[static_cast<size_t>(a)] > zKey[static_cast<size_t>(b)]; });
        for (const int sub : subs)
            drawPart(sub);
        DrawItem end;
        end.kind = DrawKind::EndGroup;
        fillItem(end, node, partOpacities, false);
        out.items.push_back(std::move(end));
    }
    for (const int source : maskSources) {
        DrawItem item;
        item.kind = DrawKind::Mesh;
        fillItem(item, source, partOpacities, true);
        item.masks.clear();
        out.maskSources.push_back(std::move(item));
    }
    return true;
}

bool OpenModel::evaluate(const ParameterSet &values, const PartSet &parts, Frame &out)
{
    Impl &m = *m_impl;
    if (values.size() != m.parameters.size() || parts.size() != m.parts.size())
        return false;
    for (size_t i = 0; i < values.size(); ++i)
        if (values.at(i).id != m.parameters.at(i).id)
            return false;
    m.prepareFrame();
    m.evaluateParameters(values);
    m.computeWorld();
    m.prepareGroups();
    return m.build(out, parts);
}

// ---------------------------------------------------------------------------------------------------------------------------------------------
// loading

namespace {

class Loader
{
public:
    explicit Loader(OpenModel::Impl &d) : m_d(d) {}

    bool run(std::string_view inp)
    {
        std::string_view jsonText;
        if (!container(inp, jsonText))
            return false;
        // nesting: the node tree (each level is an object and a list) plus the fixed depth of the rest
        const json::ParseResult parsed = json::parse(jsonText, {kMaxDeformerDepth * 2 + 8, kMaxPuppetJsonValues, kMaxJsonStringBytes});
        if (!parsed.ok)
            return fail("the puppet is not strict JSON: " + parsed.error);
        const json::Value &root = parsed.value;
        if (!root.isObject())
            return fail("the puppet is not an object");
        if (const json::Value *meta = root.find("meta"); meta && meta->isObject()) {
            if (const json::Value *name = meta->find("name"); name && name->isString())
                m_d.info.name = name->text;
            if (const json::Value *version = meta->find("version"); version && version->isString())
                m_d.info.version = version->text;
            if (const json::Value *pp = meta->find("preservePixels"); pp && pp->isBool())
                m_d.info.preservePixels = pp->boolean;
        }
        const json::Value *nodes = root.find("nodes");
        if (!nodes || !nodes->isObject())
            return fail("the puppet has no node tree");
        if (!node(*nodes, -1, 0))
            return false;
        if (m_d.nodes.empty())
            return fail("the root node has an unknown type");
        if (!resolve())
            return false;
        if (const json::Value *params = root.find("param")) {
            if (!parameters(*params))
                return false;
        }
        return finish();
    }

    std::string error;

private:
    bool fail(std::string why)
    {
        if (error.empty())
            error = std::move(why);
        return false;
    }
    void warn(std::string text)
    {
        if (m_d.info.warnings.size() < 64)
            m_d.info.warnings.push_back(std::move(text));
    }

    static std::uint32_t be32(std::string_view s, size_t at)
    {
        return (std::uint32_t(static_cast<unsigned char>(s[at])) << 24) | (std::uint32_t(static_cast<unsigned char>(s[at + 1])) << 16) | (std::uint32_t(static_cast<unsigned char>(s[at + 2])) << 8)
            | std::uint32_t(static_cast<unsigned char>(s[at + 3]));
    }

    bool container(std::string_view in, std::string_view &jsonText)
    {
        if (static_cast<std::int64_t>(in.size()) > kMaxInpBytes)
            return fail("the file is over kMaxInpBytes");
        if (in.size() < 12 || in.substr(0, 8) != std::string_view("TRNSRTS\0", 8))
            return fail("not an INP version 1 container");
        const std::uint32_t length = be32(in, 8);
        if (static_cast<std::int64_t>(length) > kMaxInpJsonBytes)
            return fail("the JSON payload is over kMaxInpJsonBytes");
        if (12 + static_cast<std::uint64_t>(length) > in.size())
            return fail("the JSON length runs past the end of the file");
        jsonText = in.substr(12, length);
        size_t pos = 12 + length;
        if (in.size() - pos >= 8 && in.substr(pos, 8) == "TEX_SECT") {
            pos += 8;
            if (in.size() - pos < 4)
                return fail("the texture section is cut");
            const std::uint32_t count = be32(in, pos);
            pos += 4;
            if (static_cast<std::int64_t>(count) > kMaxTextures)
                return fail("too many textures (kMaxTextures)");
            for (std::uint32_t i = 0; i < count; ++i) {
                if (in.size() - pos < 5)
                    return fail("a texture is cut");
                const std::uint32_t size = be32(in, pos);
                const int encoding = static_cast<unsigned char>(in[pos + 4]);
                pos += 5;
                if (static_cast<std::int64_t>(size) > kMaxTextureFileBytes)
                    return fail("a texture is over kMaxTextureFileBytes");
                if (in.size() - pos < size)
                    return fail("a texture runs past the end of the file");
                if (encoding == 2)
                    return fail("BC7 textures are not supported");
                if (encoding != 0 && encoding != 1)
                    return fail("a texture has an unknown encoding");
                m_textureBytes += size;
                if (static_cast<std::int64_t>(m_textureBytes) > kMaxTotalTextureBytes)
                    return fail("the textures are over kMaxTotalTextureBytes");
                m_d.info.textures.push_back({encoding, pos, size});
                pos += size;
            }
            if (pos < in.size() && in.substr(pos, 8) != "EXT_SECT")
                warn("bytes after the textures that are not an EXT_SECT were ignored");
        } else if (pos < in.size() && in.substr(pos, 8) != "EXT_SECT") {
            warn("bytes after the JSON that are not a TEX_SECT or an EXT_SECT were ignored");
        }
        return true;
    }

    // ---- values ----
    static bool uint32Of(const json::Value *v, std::uint32_t &out)
    {
        if (!v || !v->isNumber() || v->number < 0 || v->number > 4294967295.0 || v->number != std::floor(v->number))
            return false;
        out = static_cast<std::uint32_t>(v->number);
        return true;
    }
    static bool tame(double v) { return std::abs(v) <= static_cast<double>(kMaxCoordinate); }
    static bool numbers(const json::Value *v, size_t atLeast, double *out)
    {
        if (!v || !v->isArray() || !v->items.empty() || v->numbers.size() < atLeast)
            return false;
        for (size_t i = 0; i < atLeast; ++i) {
            if (!tame(v->numbers[i]))
                return false;
            out[i] = v->numbers[i];
        }
        return true;
    }
    bool optNumber(const json::Value &o, const char *key, double &out, const char *what)
    {
        if (const json::Value *v = o.find(key)) {
            if (!v->isNumber() || !tame(v->number))
                return fail(std::string(what) + " " + key + " is not a number within kMaxCoordinate");
            out = v->number;
        }
        return true;
    }
    bool optVec(const json::Value &o, const char *key, size_t n, double *out, const char *what)
    {
        if (const json::Value *v = o.find(key)) {
            if (!numbers(v, n, out))
                return fail(std::string(what) + " " + key + " is not a list of " + std::to_string(n) + " numbers");
        }
        return true;
    }

    bool mesh(const json::Value *v, Node &n, const char *what)
    {
        if (!v || !v->isObject())
            return fail(std::string(what) + " has no mesh");
        const json::Value *verts = v->find("verts");
        const json::Value *indices = v->find("indices");
        if (!verts || !verts->isArray() || !verts->items.empty() || !indices || !indices->isArray() || !indices->items.empty())
            return fail(std::string(what) + " mesh needs verts and indices as lists of numbers");
        for (const double c : verts->numbers)
            if (!tame(c))
                return fail(std::string(what) + " mesh has a coordinate over kMaxCoordinate");
        if (verts->numbers.size() % 2 != 0)
            return fail(std::string(what) + " mesh has an odd number of coordinates");
        const std::int64_t count = static_cast<std::int64_t>(verts->numbers.size() / 2);
        if (count > kMaxVerticesPerMesh)
            return fail("a mesh is over kMaxVerticesPerMesh");
        if (static_cast<std::int64_t>(indices->numbers.size()) > kMaxIndicesPerMesh)
            return fail("a mesh is over kMaxIndicesPerMesh");
        if (indices->numbers.size() % 3 != 0)
            return fail(std::string(what) + " mesh indices are not triangles");
        m_vertexTotal += count;
        m_indexTotal += static_cast<std::int64_t>(indices->numbers.size());
        if (m_vertexTotal > kMaxVerticesTotal)
            return fail("the meshes are over kMaxVerticesTotal");
        if (m_indexTotal > kMaxIndicesTotal)
            return fail("the meshes are over kMaxIndicesTotal");
        double origin[2] = {0, 0};
        if (const json::Value *o = v->find("origin"); o && !numbers(o, 2, origin))
            return fail(std::string(what) + " mesh origin is not two numbers");
        n.verts.resize(static_cast<size_t>(count));
        for (std::int64_t k = 0; k < count; ++k)
            n.verts[static_cast<size_t>(k)] = {verts->numbers[static_cast<size_t>(k) * 2] - origin[0], verts->numbers[static_cast<size_t>(k) * 2 + 1] - origin[1]};
        n.shape.indices.reserve(indices->numbers.size());
        for (const double idx : indices->numbers) {
            if (idx != std::floor(idx) || idx < 0 || idx >= static_cast<double>(count))
                return fail(std::string(what) + " mesh has an index outside its vertices");
            n.shape.indices.push_back(static_cast<std::uint16_t>(idx));
        }
        n.shape.uvs.assign(static_cast<size_t>(count), Vec2{});
        if (const json::Value *uvs = v->find("uvs")) {
            if (!uvs->isArray() || !uvs->items.empty() || uvs->numbers.size() != verts->numbers.size() || !std::all_of(uvs->numbers.begin(), uvs->numbers.end(), tame))
                return fail(std::string(what) + " mesh uvs do not match its vertices");
            for (std::int64_t k = 0; k < count; ++k)
                n.shape.uvs[static_cast<size_t>(k)] = {uvs->numbers[static_cast<size_t>(k) * 2], uvs->numbers[static_cast<size_t>(k) * 2 + 1]};
        } else if (count > 0) {
            warn(std::string(what) + " has no uvs");
        }
        return true;
    }

    bool blend(const json::Value &o, Node &n, const char *what)
    {
        if (const json::Value *b = o.find("blend_mode")) {
            if (!b->isString())
                return fail(std::string(what) + " blend_mode is not a string");
            for (int i = 0; i < kBlendModeCount; ++i) {
                if (b->text == kBlendNames[static_cast<size_t>(i)]) {
                    n.blend = static_cast<BlendMode>(i);
                    return true;
                }
            }
            return fail(std::string(what) + " has an unknown blend mode");
        }
        return true;
    }

    bool masks(const json::Value &o, Node &n, const char *what)
    {
        const auto mode = [&](const json::Value *m, bool &dodge) {
            if (!m)
                return true;
            if (!m->isString() || (m->text != "Mask" && m->text != "DodgeMask"))
                return fail(std::string(what) + " has an unknown mask mode");
            dodge = m->text == "DodgeMask";
            return true;
        };
        if (const json::Value *list = o.find("masks")) {
            if (!list->isArray() || !list->numbers.empty())
                return fail(std::string(what) + " masks is not a list of objects");
            if (static_cast<std::int64_t>(list->items.size()) > kMaxMasksPerPart)
                return fail("a part is over kMaxMasksPerPart");
            for (const json::Value &item : list->items) {
                MaskBinding binding;
                if (!item.isObject() || !uint32Of(item.find("source"), binding.sourceUuid) || !mode(item.find("mode"), binding.dodge))
                    return fail(std::string(what) + " has a mask without a usable source or mode");
                n.masks.push_back(binding);
            }
        } else if (const json::Value *legacy = o.find("masked_by")) {
            if (!legacy->isArray() || !legacy->items.empty())
                return fail(std::string(what) + " masked_by is not a list of numbers");
            if (static_cast<std::int64_t>(legacy->numbers.size()) > kMaxMasksPerPart)
                return fail("a part is over kMaxMasksPerPart");
            bool dodge = false;
            if (!mode(o.find("mask_mode"), dodge))
                return false;
            for (const double u : legacy->numbers) {
                if (u < 0 || u > 4294967295.0 || u != std::floor(u))
                    return fail(std::string(what) + " masked_by holds a bad uuid");
                n.masks.push_back({static_cast<std::uint32_t>(u), -1, dodge});
            }
        }
        return true;
    }

    bool node(const json::Value &v, int parent, int depth)
    {
        if (!v.isObject())
            return fail("a node is not an object");
        if (depth > kMaxDeformerDepth)
            return fail("the node tree is deeper than kMaxDeformerDepth");
        if (static_cast<std::int64_t>(m_d.nodes.size()) >= kMaxNodes)
            return fail("the puppet is over kMaxNodes");
        const json::Value *type = v.find("type");
        if (!type || !type->isString())
            return fail("a node has no type");
        Node n;
        if (type->text == "Node")
            n.type = NodeType::Node;
        else if (type->text == "Part")
            n.type = NodeType::Part;
        else if (type->text == "Composite")
            n.type = NodeType::Composite;
        else if (type->text == "MeshGroup")
            n.type = NodeType::MeshGroup;
        else if (type->text == "SimplePhysics")
            n.type = NodeType::SimplePhysics;
        else {
            warn("a node of type " + std::string(type->text.substr(0, 40)) + " was skipped with what is below it");
            return true;
        }
        const char *what = "a node";
        if (!uint32Of(v.find("uuid"), n.uuid))
            return fail("a node has no usable uuid");
        if (const json::Value *name = v.find("name"); name && name->isString())
            n.name = name->text;
        if (n.name.empty())
            n.name = "Unnamed Node";
        if (const json::Value *e = v.find("enabled")) {
            if (!e->isBool())
                return fail("enabled is not a boolean");
            n.enabled = e->boolean;
        }
        if (!optNumber(v, "zsort", n.zsort, what))
            return false;
        if (const json::Value *l = v.find("lockToRoot")) {
            if (!l->isBool())
                return fail("lockToRoot is not a boolean");
            n.lockToRoot = l->boolean;
        }
        if (const json::Value *t = v.find("transform")) {
            if (!t->isObject())
                return fail("transform is not an object");
            if (!optVec(*t, "trans", 3, n.trans, what) || !optVec(*t, "rot", 3, n.rot, what) || !optVec(*t, "scale", 2, n.scale, what))
                return false;
        }
        n.parent = parent;
        if (n.type == NodeType::Part || n.type == NodeType::MeshGroup) {
            if (static_cast<std::int64_t>(++m_drawables) > kMaxDrawables)
                return fail("the puppet is over kMaxDrawables");
            if (n.type == NodeType::Part)
                ++m_d.info.partCount;
            if (!mesh(v.find("mesh"), n, n.type == NodeType::Part ? "a part" : "a mesh group"))
                return false;
        }
        if (n.type == NodeType::Part || n.type == NodeType::Composite) {
            const char *w = n.type == NodeType::Part ? "a part" : "a composite";
            if (!blend(v, n, w) || !optNumber(v, "opacity", n.opacity, w) || !optNumber(v, "mask_threshold", n.maskThreshold, w) || !optVec(v, "tint", 3, n.tint, w)
                || !optVec(v, "screenTint", 3, n.screen, w) || !masks(v, n, w))
                return false;
            if (n.type == NodeType::Part) {
                if (const json::Value *tex = v.find("textures")) {
                    if (!tex->isArray() || !tex->items.empty())
                        return fail("a part's textures is not a list of numbers");
                    if (!tex->numbers.empty()) {
                        const double albedo = tex->numbers[0];
                        if (albedo < 0 || albedo > 4294967295.0 || albedo != std::floor(albedo))
                            return fail("a part's texture index is not an index");
                        if (static_cast<std::uint32_t>(albedo) != kNoTexture) {
                            if (albedo < static_cast<double>(m_d.info.textures.size()))
                                n.texture = static_cast<int>(albedo);
                            else
                                warn("a part names a texture the file does not have");
                        }
                    }
                }
            } else if (const json::Value *p = v.find("propagate_meshgroup")) {
                if (!p->isBool())
                    return fail("propagate_meshgroup is not a boolean");
                n.propagate = p->boolean;
            }
        }
        if (n.type == NodeType::MeshGroup) {
            if (const json::Value *d = v.find("dynamic_deformation")) {
                if (!d->isBool())
                    return fail("dynamic_deformation is not a boolean");
                n.dynamic = d->boolean;
            }
        }
        if (n.type == NodeType::SimplePhysics) {
            const json::Value *model = v.find("model_type");
            const json::Value *map = v.find("map_mode");
            if ((model && (!model->isString() || (model->text != "pendulum" && model->text != "spring_pendulum")))
                || (map && (!map->isString() || (map->text != "angle_length" && map->text != "xy" && map->text != "length_angle" && map->text != "yx"))))
                return fail("a SimplePhysics node has an unknown model_type or map_mode");
            ++m_d.info.physicsNodes;
        }
        if (!m_uuids.emplace(n.uuid, static_cast<int>(m_d.nodes.size())).second)
            return fail("two nodes share a uuid");
        const int self = static_cast<int>(m_d.nodes.size());
        if (parent >= 0)
            m_d.nodes[static_cast<size_t>(parent)].children.push_back(self);
        m_d.nodes.push_back(std::move(n));
        if (const json::Value *children = v.find("children")) {
            if (!children->isArray() || !children->numbers.empty())
                return fail("children is not a list of nodes");
            for (const json::Value &child : children->items)
                if (!node(child, self, depth + 1))
                    return false;
        }
        return true;
    }

    bool resolve()
    {
        for (size_t i = 0; i < m_d.nodes.size(); ++i) {
            Node &n = m_d.nodes[i];
            if (n.type == NodeType::Part || n.type == NodeType::Composite) {
                std::string id = n.name.size() > 100 ? n.name.substr(0, 100) : n.name;
                if (m_d.parts.find(id) >= 0)
                    id += "#" + std::to_string(n.uuid);
                if (!m_d.parts.add(id))
                    return fail("the puppet is over kMaxParts");
                n.partIndex = m_d.parts.find(id);
            }
            for (auto it = n.masks.begin(); it != n.masks.end();) {
                const auto found = m_uuids.find(it->sourceUuid);
                if (found == m_uuids.end() || m_d.nodes[static_cast<size_t>(found->second)].type != NodeType::Part) {
                    warn("a mask names a source that is not a part of the puppet and was dropped");
                    it = n.masks.erase(it);
                    continue;
                }
                it->source = found->second;
                if (std::find(m_d.maskSources.begin(), m_d.maskSources.end(), found->second) == m_d.maskSources.end())
                    m_d.maskSources.push_back(found->second);
                ++it;
            }
        }
        return true;
    }

    // ---- parameters ----
    static bool parseProp(const std::string &name, NodeType type, Prop &out)
    {
        static const std::map<std::string, Prop> all = {
            {"zSort", Prop::ZSort}, {"transform.t.x", Prop::Tx}, {"transform.t.y", Prop::Ty}, {"transform.t.z", Prop::Tz}, {"transform.r.x", Prop::Rx}, {"transform.r.y", Prop::Ry},
            {"transform.r.z", Prop::Rz}, {"transform.s.x", Prop::Sx}, {"transform.s.y", Prop::Sy}};
        static const std::map<std::string, Prop> look = {
            {"opacity", Prop::Opacity}, {"tint.r", Prop::TintR}, {"tint.g", Prop::TintG}, {"tint.b", Prop::TintB}, {"screenTint.r", Prop::ScreenR}, {"screenTint.g", Prop::ScreenG},
            {"screenTint.b", Prop::ScreenB}};
        if (const auto it = all.find(name); it != all.end()) {
            out = it->second;
            return true;
        }
        if (type == NodeType::Part || type == NodeType::Composite) {
            if (const auto it = look.find(name); it != look.end()) {
                out = it->second;
                return true;
            }
            if (type == NodeType::Part && (name == "emissionStrength" || name == "alphaThreshold")) {
                out = Prop::Ignored; // emission has no light to act on; alphaThreshold does nothing in the reference
                return true;
            }
        }
        if (name == "deform" && (type == NodeType::Part || type == NodeType::MeshGroup)) {
            out = Prop::Deform;
            return true;
        }
        if (type == NodeType::SimplePhysics && (name == "gravity" || name == "length" || name == "frequency" || name == "angleDamping" || name == "lengthDamping" || name == "outputScale.x"
                                                || name == "outputScale.y")) {
            out = Prop::Ignored;
            return true;
        }
        return false;
    }

    bool axis(const json::Value *v, bool needTwo, std::vector<double> &out)
    {
        if (!v || !v->isArray() || !v->items.empty())
            return false;
        const std::vector<double> &pts = v->numbers;
        if (static_cast<std::int64_t>(pts.size()) > kMaxAxisPoints || pts.size() < (needTwo ? 2u : 1u))
            return false;
        for (size_t i = 0; i < pts.size(); ++i)
            if (pts[i] < 0 || pts[i] > 1 || (i > 0 && !(pts[i] > pts[i - 1])))
                return false;
        out = pts;
        return true;
    }

    bool parameters(const json::Value &list)
    {
        if (!list.isArray() || !list.numbers.empty())
            return fail("param is not a list of parameters");
        if (static_cast<std::int64_t>(list.items.size()) > kMaxParameters)
            return fail("the puppet is over kMaxParameters");
        std::int64_t bindingTotal = 0;
        for (const json::Value &item : list.items) {
            if (!item.isObject())
                return fail("a parameter is not an object");
            Param p;
            const json::Value *name = item.find("name");
            if (!name || !name->isString() || name->text.empty())
                return fail("a parameter has no name");
            if (const json::Value *is = item.find("is_vec2")) {
                if (!is->isBool())
                    return fail("is_vec2 is not a boolean");
                p.is2d = is->boolean;
            }
            double lo[2] = {0, 0}, hi[2] = {1, 1}, def[2] = {0, 0};
            if (!optVec(item, "min", 2, lo, "a parameter") || !optVec(item, "max", 2, hi, "a parameter") || !optVec(item, "defaults", 2, def, "a parameter"))
                return false;
            p.minX = lo[0];
            p.minY = lo[1];
            p.maxX = hi[0];
            p.maxY = hi[1];
            if (const json::Value *ap = item.find("axis_points")) {
                if (!ap->isArray() || ap->size() != 2 || !ap->numbers.empty() || !axis(&ap->items[0], true, p.axisX) || !axis(&ap->items[1], p.is2d, p.axisY))
                    return fail("a parameter's axis_points are not two lists of increasing places in 0..1 (at least two on each used axis, at most kMaxAxisPoints)");
            } else if (!p.is2d) {
                p.axisY = {0};
            }
            std::string base = name->text.size() > 100 ? name->text.substr(0, 100) : name->text;
            const auto addParam = [&](const std::string &id, double mn, double df, double mx) {
                std::string unique = id;
                if (m_d.parameters.find(unique) >= 0)
                    unique += "#" + std::to_string(m_d.parameters.size());
                if (!m_d.parameters.add(unique, mn, df, mx))
                    return -1;
                return m_d.parameters.find(unique);
            };
            if (p.is2d) {
                p.indexX = addParam(base + ".x", p.minX, def[0], p.maxX);
                p.indexY = addParam(base + ".y", p.minY, def[1], p.maxY);
            } else {
                p.indexX = addParam(base, p.minX, def[0], p.maxX);
            }
            if (p.indexX < 0 || (p.is2d && p.indexY < 0))
                return fail("a parameter has a range or default that is not valid, or the puppet is over kMaxParameters");
            if (const json::Value *bindings = item.find("bindings")) {
                if (!bindings->isArray() || !bindings->numbers.empty())
                    return fail("bindings is not a list of bindings");
                bindingTotal += static_cast<std::int64_t>(bindings->items.size());
                if (bindingTotal > kMaxBindings)
                    return fail("the puppet is over kMaxBindings");
                for (const json::Value &b : bindings->items) {
                    if (!binding(b, p))
                        return false;
                }
            }
            m_d.params.push_back(std::move(p));
        }
        return true;
    }

    bool binding(const json::Value &v, Param &p)
    {
        if (!v.isObject())
            return fail("a binding is not an object");
        std::uint32_t uuid = 0;
        const json::Value *paramName = v.find("param_name");
        if (!uint32Of(v.find("node"), uuid) || !paramName || !paramName->isString())
            return fail("a binding needs a node and a param_name");
        if (paramName->text.empty())
            return true;
        const auto found = m_uuids.find(uuid);
        if (found == m_uuids.end()) {
            warn("a binding names a node the puppet does not have and was dropped");
            return true;
        }
        Binding b;
        b.node = found->second;
        const Node &target = m_d.nodes[static_cast<size_t>(b.node)];
        if (!parseProp(paramName->text, target.type, b.prop)) {
            warn("a binding of " + paramName->text.substr(0, 40) + " is not one this node has and was dropped");
            return true;
        }
        if (b.prop == Prop::Ignored)
            return true;
        if (const json::Value *mode = v.find("interpolate_mode")) {
            if (!mode->isString())
                return fail("interpolate_mode is not a string");
            if (mode->text == "Nearest")
                b.mode = Interp::Nearest;
            else if (mode->text == "Linear")
                b.mode = Interp::Linear;
            else if (mode->text == "Cubic")
                b.mode = Interp::Cubic;
            else if (mode->text == "Stepped") {
                b.mode = Interp::Nearest;
                warn("a Stepped binding runs as Nearest");
            } else if (mode->text == "Bezier") {
                b.mode = Interp::Linear;
                warn("a Bezier binding runs as Linear");
            } else
                return fail("a binding has an unknown interpolate_mode");
        }
        const json::Value *values = v.find("values");
        const size_t nx = p.axisX.size(), ny = p.axisY.size();
        if (!values || !values->isArray() || !values->numbers.empty() || values->items.size() != nx)
            return fail("a binding's values do not have one column per axis point");
        if (const json::Value *isSet = v.find("isSet"); isSet && !isSet->isArray())
            return fail("isSet is not a list");
        const bool deform = b.prop == Prop::Deform;
        if (deform)
            b.deforms.reserve(nx * ny);
        else
            b.scalars.reserve(nx * ny);
        for (const json::Value &column : values->items) {
            if (!column.isArray() || column.size() != ny)
                return fail("a binding's values do not have one row per axis point");
            if (!deform) {
                if (!column.items.empty())
                    return fail("a value binding holds something that is not a number");
                for (const double s : column.numbers)
                    if (!tame(s))
                        return fail("a binding value is over kMaxCoordinate");
                b.scalars.insert(b.scalars.end(), column.numbers.begin(), column.numbers.end());
                continue;
            }
            if (!column.numbers.empty())
                return fail("a deform binding holds a number where a list of offsets is expected");
            for (const json::Value &cell : column.items) {
                if (!cell.isArray() || !cell.numbers.empty())
                    return fail("a deform cell is not a list of offsets");
                if (cell.size() != target.verts.size()) {
                    warn("a deform binding does not have one offset per vertex and was dropped");
                    return true;
                }
                std::vector<Vec2> offsets(target.verts.size());
                for (size_t k = 0; k < offsets.size(); ++k) {
                    double xy[2];
                    if (!numbers(&cell.items[k], 2, xy))
                        return fail("a deform offset is not two numbers");
                    offsets[k] = {xy[0], xy[1]};
                }
                b.deforms.push_back(std::move(offsets));
            }
        }
        p.bindings.push_back(std::move(b));
        return true;
    }

    bool finish()
    {
        const size_t n = m_d.nodes.size();
        // which mesh group carries which node, which parts a composite holds, in which order the puppet draws from
        m_d.groupOf.assign(n, -1);
        m_d.warps.assign(n, Warp{});
        m_d.groupMoved.assign(n, {});
        m_d.groupWorld.assign(n, Aff2{});
        m_d.groupInverse.assign(n, Aff2{});
        m_d.groupOk.assign(n, 0);
        m_d.subParts.assign(n, {});
        for (size_t g = 0; g < n; ++g) {
            if (m_d.nodes[g].type != NodeType::MeshGroup)
                continue;
            std::string why;
            if (!m_d.warps[g].build(m_d.nodes[g].verts, m_d.nodes[g].shape.indices, m_warpBudget, why))
                return fail(why);
            std::vector<int> stack(m_d.nodes[g].children.rbegin(), m_d.nodes[g].children.rend());
            while (!stack.empty()) {
                const int c = stack.back();
                stack.pop_back();
                const Node &cn = m_d.nodes[static_cast<size_t>(c)];
                if (cn.type == NodeType::MeshGroup)
                    continue;
                if (cn.type == NodeType::Composite && !cn.propagate)
                    continue;
                if (cn.type == NodeType::Part)
                    m_d.groupOf[static_cast<size_t>(c)] = static_cast<int>(g);
                for (auto it = cn.children.rbegin(); it != cn.children.rend(); ++it)
                    stack.push_back(*it);
            }
        }
        std::vector<int> stack{0};
        while (!stack.empty()) {
            const int i = stack.back();
            stack.pop_back();
            const Node &node = m_d.nodes[static_cast<size_t>(i)];
            if (node.type == NodeType::Part || node.type == NodeType::Composite)
                m_d.rootList.push_back(i);
            if (node.type == NodeType::Composite) {
                std::vector<int> inner(node.children.rbegin(), node.children.rend());
                while (!inner.empty()) {
                    const int c = inner.back();
                    inner.pop_back();
                    const Node &cn = m_d.nodes[static_cast<size_t>(c)];
                    if (cn.type == NodeType::Composite)
                        warn("a composite inside a composite is flattened into the outer one");
                    if (cn.type == NodeType::Part)
                        m_d.subParts[static_cast<size_t>(i)].push_back(c);
                    for (auto it = cn.children.rbegin(); it != cn.children.rend(); ++it)
                        inner.push_back(*it);
                }
                continue;
            }
            for (auto it = node.children.rbegin(); it != node.children.rend(); ++it)
                stack.push_back(*it);
        }
        m_d.offSort.assign(n, 0);
        m_d.offT.assign(n * 3, 0);
        m_d.offR.assign(n * 3, 0);
        m_d.offS.assign(n * 2, 1);
        m_d.opMul.assign(n, 1);
        m_d.tintMul.assign(n * 3, 1);
        m_d.screenAdd.assign(n * 3, 0);
        m_d.deform.assign(n, {});
        for (size_t i = 0; i < n; ++i)
            if (m_d.nodes[i].type == NodeType::Part || m_d.nodes[i].type == NodeType::MeshGroup)
                m_d.deform[i].assign(m_d.nodes[i].verts.size(), Vec2{});
        m_d.world.assign(n, Mat4{});
        m_d.enabled.assign(n, 1);
        m_d.zKey.assign(n, 0);
        return true;
    }

    OpenModel::Impl &m_d;
    std::map<std::uint32_t, int> m_uuids;
    std::int64_t m_vertexTotal = 0;
    std::int64_t m_indexTotal = 0;
    std::int64_t m_drawables = 0;
    std::uint64_t m_textureBytes = 0;
    std::int64_t m_warpBudget = kMaxWarpCellEntries;
};

} // namespace

OpenLoadResult OpenModel::load(std::string_view inp)
{
    return load(std::string(inp));
}

OpenLoadResult OpenModel::load(std::string &&inp)
{
    OpenLoadResult result;
    auto impl = std::make_unique<Impl>();
    impl->file = std::move(inp); // the textures stay in it, as ranges: nothing is copied
    Loader loader(*impl);
    if (!loader.run(impl->file)) {
        result.error = loader.error.empty() ? "the puppet could not be read" : loader.error;
        return result;
    }
    // where the puppet lies at rest
    std::unique_ptr<OpenModel> model(new OpenModel(std::move(impl)));
    Frame frame;
    PartSet parts = model->parts();
    ParameterSet defaults = model->parameters();
    if (model->evaluate(defaults, parts, frame)) {
        bool any = false;
        Bounds b;
        for (const DrawItem &item : frame.items) {
            for (const Vec2 &p : item.positions) {
                if (!any) {
                    b = {p.x, p.y, p.x, p.y};
                    any = true;
                }
                b.minX = std::min(b.minX, p.x);
                b.maxX = std::max(b.maxX, p.x);
                b.minY = std::min(b.minY, p.y);
                b.maxY = std::max(b.maxY, p.y);
            }
        }
        model->m_impl->bounds = b;
    }
    result.model = std::move(model);
    return result;
}

OpenLoadResult OpenModel::loadFile(const std::string &folder, std::string_view relative)
{
    OpenLoadResult result;
    std::string bytes, why;
    if (!readItemFile(folder, relative, static_cast<size_t>(kMaxInpBytes), bytes, &why)) {
        result.error = "the file cannot be read: " + why;
        return result;
    }
    if (static_cast<std::int64_t>(bytes.size()) > kMaxInpBytes) {
        result.error = "the file is over kMaxInpBytes";
        return result;
    }
    return load(std::move(bytes));
}

std::string_view OpenModel::textureBytes(int index) const
{
    const auto &textures = m_impl->info.textures;
    if (index < 0 || static_cast<size_t>(index) >= textures.size())
        return {};
    const TextureBlob &t = textures[static_cast<size_t>(index)];
    const std::string &file = m_impl->file;
    if (t.offset > file.size() || t.size > file.size() - t.offset) // (also what is left after releaseFile)
        return {};
    return std::string_view(file).substr(t.offset, t.size);
}

bool OpenModel::textureSize(int index, uint32_t &width, uint32_t &height) const
{
    const auto &textures = m_impl->info.textures;
    if (index < 0 || static_cast<size_t>(index) >= textures.size())
        return false;
    const std::string_view bytes = textureBytes(index);
    const auto be32 = [&](size_t at) {
        return (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) << 24) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 16) |
               (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 2])) << 8) | static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 3]));
    };
    const auto le16 = [&](size_t at) { return static_cast<uint32_t>(static_cast<uint8_t>(bytes[at])) | (static_cast<uint32_t>(static_cast<uint8_t>(bytes[at + 1])) << 8); };
    if (textures[static_cast<size_t>(index)].encoding == 0) {
        // signature, then the IHDR chunk: length (4), "IHDR", width (4), height (4)
        if (bytes.size() < 24 || bytes.compare(0, 8, "\x89PNG\r\n\x1a\n") != 0 || bytes.compare(12, 4, "IHDR") != 0)
            return false;
        width = be32(16);
        height = be32(20);
        return true;
    }
    if (textures[static_cast<size_t>(index)].encoding == 1) {
        if (bytes.size() < 18)
            return false;
        width = le16(12);
        height = le16(14);
        return true;
    }
    return false;
}

void OpenModel::releaseFile()
{
    std::string().swap(m_impl->file);
}

bool OpenModel::fileKept() const
{
    return !m_impl->file.empty();
}

bool textureBudgetOk(const OpenModel &model, std::string &error)
{
    std::int64_t pixels = 0;
    const size_t count = model.info().textures.size();
    for (size_t i = 0; i < count; ++i) {
        uint32_t w = 0, h = 0;
        if (!model.textureSize(static_cast<int>(i), w, h)) {
            error = "texture " + std::to_string(i) + ": the header cannot be read";
            return false;
        }
        const std::uint64_t area = static_cast<std::uint64_t>(w) * h; // (two 32 bit sizes fit 64 bits unsigned; a signed product of them can overflow)
        if (area == 0 || area > static_cast<std::uint64_t>(kMaxTexturePixels)) {
            error = "texture " + std::to_string(i) + " is over kMaxTexturePixels";
            return false;
        }
        pixels += static_cast<std::int64_t>(area);
        if (pixels > kMaxTotalTexturePixels) {
            error = "the textures are over kMaxTotalTexturePixels";
            return false;
        }
    }
    return true;
}

} // namespace murasaki::puppet
