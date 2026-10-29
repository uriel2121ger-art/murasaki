#include "screenmatch.h"

#include <QtGlobal>

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <vector>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

qint64 saturatingAdd(qint64 a, qint64 b) // b is never negative
{
    return a > std::numeric_limits<qint64>::max() - b ? std::numeric_limits<qint64>::max() : a + b;
}

constexpr double kScoreEpsilon = 1e-9; // 48/64 and 0.75 must compare equal whatever the decimal rounding

// For each destination index, the source range it covers and the share of each source pixel in it.
struct AxisSpan {
    int first = 0;
    int count = 0;
    int weightOffset = 0; // into `weights`
};

void buildAxis(int source, int destination, std::vector<AxisSpan> &spans, std::vector<float> &weights)
{
    spans.resize(destination);
    weights.clear();
    const double scale = double(source) / destination;
    for (int i = 0; i < destination; ++i) {
        const double lo = i * scale;
        const double hi = (i + 1) * scale;
        const int first = std::clamp(int(std::floor(lo)), 0, source - 1);
        const int last = std::clamp(int(std::ceil(hi)) - 1, first, source - 1);
        AxisSpan &span = spans[i];
        span.first = first;
        span.count = last - first + 1;
        span.weightOffset = int(weights.size());
        float sum = 0.0f;
        for (int j = first; j <= last; ++j) {
            const float w = float(std::max(0.0, std::min(hi, double(j + 1)) - std::max(lo, double(j))));
            weights.push_back(w);
            sum += w;
        }
        if (sum <= 0.0f) { // a degenerate span: take the nearest pixel whole
            weights[span.weightOffset] = 1.0f;
            sum = 1.0f;
        }
        for (int k = 0; k < span.count; ++k)
            weights[span.weightOffset + k] /= sum;
    }
}

quint8 roundToByte(float v)
{
    return quint8(std::clamp(int(v + 0.5f), 0, 255));
}

int bytesPerPixel(ScreenPixelFormat format)
{
    switch (format) {
    case ScreenPixelFormat::Grey8:
        return 1;
    case ScreenPixelFormat::Rgb24:
        return 3;
    case ScreenPixelFormat::Rgba32:
    case ScreenPixelFormat::Bgra32:
        return 4;
    }
    return 0; // not a format of ours
}

// Whether `data` may be read as a picture of this shape: a known format, sizes inside the caps, a stride that holds a row, and a buffer that holds every row the picture touches.
// All in 64 bits, so no size can wrap.
bool pixelsFit(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format)
{
    const int bpp = bytesPerPixel(format);
    if (!data || bpp == 0 || width < 1 || height < 1 || width > kScreenMatchMaxDimension || height > kScreenMatchMaxDimension || qint64(width) * height > kScreenMatchMaxPixels)
        return false;
    const qint64 rowBytes = qint64(width) * bpp;
    if (stride < rowBytes || stride > (qint64(1) << 31))
        return false;
    return qint64(dataSize) >= qint64(stride) * (height - 1) + rowBytes;
}

bool imageInCaps(const GreyImage &image)
{
    return !image.isEmpty() && image.width <= kScreenMatchMaxDimension && image.height <= kScreenMatchMaxDimension && qint64(image.width) * image.height <= kScreenMatchMaxPixels;
}

// Area scaling driven by a row source (`fillRow(y, float *row)` writes source row y as floats). The rows of a destination row are summed first, across the whole width in one
// tight loop; the horizontal pass then runs over the destination rows only. A source row shared by two destination rows is read once.
template <class FillRow>
GreyImage resizeRows(int srcW, int srcH, int dstW, int dstH, int rowStep, FillRow &&fillRow)
{
    std::vector<float> row(static_cast<size_t>(srcW));
    if (srcW == dstW && srcH == dstH) { // nothing to scale: the rows are the picture
        GreyImage same;
        same.width = dstW;
        same.height = dstH;
        same.pixels.resize(qsizetype(dstW) * dstH);
        auto *out = reinterpret_cast<uchar *>(same.pixels.data());
        for (int y = 0; y < dstH; ++y) {
            fillRow(y, row.data());
            for (int x = 0; x < dstW; ++x)
                *out++ = roundToByte(row[size_t(x)]);
        }
        return same;
    }

    std::vector<AxisSpan> xs, ys;
    std::vector<float> xw, yw;
    buildAxis(srcW, dstW, xs, xw);
    buildAxis(srcH, dstH, ys, yw);

    std::vector<float> acc(static_cast<size_t>(srcW));
    int rowY = -1;
    GreyImage out;
    out.width = dstW;
    out.height = dstH;
    out.pixels.resize(qsizetype(dstW) * dstH);
    auto *dst = reinterpret_cast<uchar *>(out.pixels.data());
    for (int y = 0; y < dstH; ++y) {
        const AxisSpan &sy = ys[y];
        const float *wy = yw.data() + sy.weightOffset;
        std::fill(acc.begin(), acc.end(), 0.0f);
        // With rowStep > 1 only every rowStep-th source row is read (the first of the span when none of its rows is one), and the weights of the rows used are renormalised.
        float used = 0.0f;
        for (int k = 0; k < sy.count; ++k)
            used += (rowStep <= 1 || (sy.first + k) % rowStep == 0) ? wy[k] : 0.0f;
        for (int k = 0; k < sy.count; ++k) {
            const bool take = rowStep <= 1 || (sy.first + k) % rowStep == 0 || (used <= 0.0f && k == 0);
            if (!take)
                continue;
            if (rowY != sy.first + k) {
                rowY = sy.first + k;
                fillRow(rowY, row.data());
            }
            const float w = used > 0.0f ? wy[k] / used : 1.0f;
            for (int x = 0; x < srcW; ++x)
                acc[size_t(x)] += w * row[size_t(x)];
        }
        for (int x = 0; x < dstW; ++x) {
            const AxisSpan &sx = xs[x];
            const float *wx = xw.data() + sx.weightOffset;
            float sum = 0.0f;
            for (int k = 0; k < sx.count; ++k)
                sum += wx[k] * acc[size_t(sx.first + k)];
            *dst++ = roundToByte(sum);
        }
    }
    return out;
}

bool sameSize(const GreyImage &a, int w, int h)
{
    return a.width == w && a.height == h;
}

} // namespace

GreyImage greyFromPixels(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format)
{
    const int bpp = bytesPerPixel(format);
    if (!pixelsFit(data, dataSize, width, height, stride, format))
        return {};

    GreyImage out;
    out.width = width;
    out.height = height;
    out.pixels.resize(qsizetype(width) * height);
    auto *dst = reinterpret_cast<uchar *>(out.pixels.data());
    for (int y = 0; y < height; ++y) {
        const uchar *row = data + y * stride;
        for (int x = 0; x < width; ++x) {
            switch (format) {
            case ScreenPixelFormat::Grey8:
                *dst++ = row[x];
                break;
            case ScreenPixelFormat::Rgb24:
            case ScreenPixelFormat::Rgba32: {
                const uchar *p = row + x * bpp;
                *dst++ = uchar((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
                break;
            }
            case ScreenPixelFormat::Bgra32: {
                const uchar *p = row + x * 4;
                *dst++ = uchar((77 * p[2] + 150 * p[1] + 29 * p[0] + 128) >> 8);
                break;
            }
            }
        }
    }
    return out;
}

GreyImage resizeArea(const GreyImage &image, int width, int height)
{
    if (!imageInCaps(image) || width < 1 || height < 1 || width > kScreenMatchMaxDimension || height > kScreenMatchMaxDimension)
        return {};
    if (sameSize(image, width, height))
        return image;
    const auto *src = reinterpret_cast<const uchar *>(image.pixels.constData());
    return resizeRows(image.width, image.height, width, height, 1, [&](int y, float *row) {
        const uchar *p = src + qsizetype(y) * image.width;
        for (int x = 0; x < image.width; ++x)
            row[x] = float(p[x]);
    });
}

GreyImage greyScaledFromPixels(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format, int maxSide, int rowStep)
{
    if (maxSide < 1 || !pixelsFit(data, dataSize, width, height, stride, format))
        return {};
    int dw = width;
    int dh = height;
    if (std::max(width, height) > maxSide) {
        const double k = double(maxSide) / std::max(width, height);
        dw = std::max(1, int(std::lround(width * k)));
        dh = std::max(1, int(std::lround(height * k)));
    }
    return resizeRows(width, height, dw, dh, rowStep, [&](int y, float *row) {
        const uchar *p = data + y * stride;
        switch (format) {
        case ScreenPixelFormat::Grey8:
            for (int x = 0; x < width; ++x)
                row[x] = float(p[x]);
            break;
        case ScreenPixelFormat::Rgb24:
            for (int x = 0; x < width; ++x, p += 3)
                row[x] = float((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
            break;
        case ScreenPixelFormat::Rgba32:
            for (int x = 0; x < width; ++x, p += 4)
                row[x] = float((77 * p[0] + 150 * p[1] + 29 * p[2] + 128) >> 8);
            break;
        case ScreenPixelFormat::Bgra32:
            for (int x = 0; x < width; ++x, p += 4)
                row[x] = float((77 * p[2] + 150 * p[1] + 29 * p[0] + 128) >> 8);
            break;
        }
    });
}

GreyImage fitLongerSide(const GreyImage &image, int maxSide)
{
    if (!imageInCaps(image) || maxSide < 1)
        return {};
    const int longer = std::max(image.width, image.height);
    if (longer <= maxSide)
        return image;
    const double k = double(maxSide) / longer;
    const int w = std::max(1, int(std::lround(image.width * k)));
    const int h = std::max(1, int(std::lround(image.height * k)));
    return resizeArea(image, w, h);
}

ScreenPicture prepareScreenPicture(const GreyImage &image, bool withStructure)
{
    ScreenPicture pic;
    const GreyImage fitted = imageInCaps(image) ? fitLongerSide(image) : GreyImage();
    if (fitted.isEmpty())
        return pic;

    const GreyImage cells = resizeArea(fitted, kScreenHashSide, kScreenHashSide);
    const auto *c = reinterpret_cast<const uchar *>(cells.pixels.constData());
    int sum = 0;
    for (int i = 0; i < kScreenHashSide * kScreenHashSide; ++i)
        sum += c[i];
    for (int i = 0; i < kScreenHashSide * kScreenHashSide; ++i) {
        if (c[i] * 64 > sum) // cell > mean, in integers
            pic.hash |= quint64(1) << i;
    }
    pic.valid = true;

    if (withStructure) {
        const GreyImage grid = resizeArea(fitted, kScreenStructureSide, kScreenStructureSide);
        const auto *g = reinterpret_cast<const uchar *>(grid.pixels.constData());
        const int n = kScreenStructureSide * kScreenStructureSide;
        double total = 0.0;
        for (int i = 0; i < n; ++i)
            total += g[i];
        const double mean = total / n;
        double squares = 0.0;
        for (int i = 0; i < n; ++i) {
            const double d = g[i] - mean;
            pic.centred[size_t(i)] = float(d);
            squares += d * d;
        }
        pic.flatMean = float(mean);
        pic.norm = float(std::sqrt(squares));
        pic.hasStructure = true;
    }
    return pic;
}

namespace {

// One source row to integer grey, one loop per format, no branches in the loops (so the compiler can vectorise them).
void greyRow(const uchar *p, int width, ScreenPixelFormat format, quint32 *out)
{
    switch (format) {
    case ScreenPixelFormat::Grey8:
        for (int x = 0; x < width; ++x)
            out[x] = p[x];
        break;
    case ScreenPixelFormat::Rgb24:
        for (int x = 0; x < width; ++x)
            out[x] = (77u * p[3 * x] + 150u * p[3 * x + 1] + 29u * p[3 * x + 2] + 128u) >> 8;
        break;
    case ScreenPixelFormat::Rgba32:
        for (int x = 0; x < width; ++x)
            out[x] = (77u * p[4 * x] + 150u * p[4 * x + 1] + 29u * p[4 * x + 2] + 128u) >> 8;
        break;
    case ScreenPixelFormat::Bgra32:
        for (int x = 0; x < width; ++x)
            out[x] = (77u * p[4 * x + 2] + 150u * p[4 * x + 1] + 29u * p[4 * x] + 128u) >> 8;
        break;
    }
}

quint32 sumRange(const quint32 *g, int from, int to) // [from, to)
{
    quint32 s = 0;
    for (int j = from; j < to; ++j)
        s += g[j];
    return s;
}

struct ColumnBin {
    int first = 0;      // first source pixel touched
    int last = 0;       // last source pixel touched
    double firstCover = 0.0; // how much of `first` and `last` lies in the bin (in pixels, 0..1)
    double lastCover = 0.0;
    double area = 1.0;  // hi - lo, in source pixels
};

} // namespace

ScreenPicture prepareScreenPictureFromPixels(const uchar *data, qsizetype dataSize, int width, int height, qsizetype stride, ScreenPixelFormat format, bool withStructure, int rowStep)
{
    ScreenPicture pic;
    if (!pixelsFit(data, dataSize, width, height, stride, format))
        return pic;
    constexpr int N = kScreenStructureSide;
    rowStep = std::max(1, rowStep);

    std::array<ColumnBin, N> cols;
    const double xs = double(width) / N;
    for (int i = 0; i < N; ++i) {
        const double lo = i * xs;
        const double hi = (i + 1) * xs;
        ColumnBin &c = cols[size_t(i)];
        c.first = std::clamp(int(std::floor(lo)), 0, width - 1);
        c.last = std::clamp(int(std::ceil(hi)) - 1, c.first, width - 1);
        c.area = hi - lo;
        c.firstCover = std::min(hi, double(c.first + 1)) - lo;
        c.lastCover = hi - std::max(lo, double(c.last));
    }
    // Vertical weights in double, normalised per bin: a flat picture must come out exactly flat (float sums drift by a few ulps and would look like structure).
    std::vector<int> spanFirst(N), spanCount(N), spanOffset(N);
    std::vector<double> spanWeights;
    {
        const double ysize = double(height) / N;
        for (int by = 0; by < N; ++by) {
            const double lo = by * ysize, hi = (by + 1) * ysize;
            const int first = std::clamp(int(std::floor(lo)), 0, height - 1);
            const int last = std::clamp(int(std::ceil(hi)) - 1, first, height - 1);
            spanFirst[size_t(by)] = first;
            spanCount[size_t(by)] = last - first + 1;
            spanOffset[size_t(by)] = int(spanWeights.size());
            for (int j = first; j <= last; ++j)
                spanWeights.push_back(std::max(0.0, std::min(hi, double(j + 1)) - std::max(lo, double(j))));
        }
    }

    std::vector<quint32> grey(static_cast<size_t>(width));
    std::array<double, N> rowBins{};
    int rowBinsY = -1;
    std::array<double, N * N> bins{};

    // With rowStep above 1 only every rowStep-th row is read. The step is held to half the rows of a bin, so each bin keeps at least two rows to average.
    rowStep = std::clamp(rowStep, 1, std::max(1, height / (2 * N)));

    for (int by = 0; by < N; ++by) {
        const int first = spanFirst[size_t(by)];
        const int count = spanCount[size_t(by)];
        const double *wy = spanWeights.data() + spanOffset[size_t(by)];
        double used = 0.0;
        for (int k = 0; k < count; ++k)
            used += (rowStep == 1 || (first + k) % rowStep == 0) ? wy[k] : 0.0;
        double *dst = bins.data() + by * N;
        for (int k = 0; k < count; ++k) {
            const int y = first + k;
            const bool take = rowStep == 1 || y % rowStep == 0 || (used <= 0.0 && k == 0);
            if (!take)
                continue;
            if (rowBinsY != y) {
                rowBinsY = y;
                greyRow(data + y * stride, width, format, grey.data());
                for (int i = 0; i < N; ++i) {
                    const ColumnBin &c = cols[size_t(i)];
                    if (c.last == c.first) {
                        rowBins[size_t(i)] = double(grey[size_t(c.first)]);
                    } else {
                        const double edges = c.firstCover * double(grey[size_t(c.first)]) + c.lastCover * double(grey[size_t(c.last)]);
                        rowBins[size_t(i)] = (edges + double(sumRange(grey.data(), c.first + 1, c.last))) / c.area;
                    }
                }
            }
            const double w = used > 0.0 ? wy[k] / used : 1.0;
            for (int i = 0; i < N; ++i)
                dst[i] += w * rowBins[size_t(i)];
        }
    }

    // 8x8 cells: the mean of 4x4 bins, rounded to a byte as the reference path does.
    constexpr int G = kScreenHashSide;
    constexpr int R = N / G;
    std::array<int, G * G> cell{};
    int total = 0;
    for (int cy = 0; cy < G; ++cy)
        for (int cx = 0; cx < G; ++cx) {
            double acc = 0.0;
            for (int y = 0; y < R; ++y)
                for (int x = 0; x < R; ++x)
                    acc += bins[size_t((cy * R + y) * N + cx * R + x)];
            const int v = std::clamp(int(acc / (R * R) + 0.5), 0, 255);
            cell[size_t(cy * G + cx)] = v;
            total += v;
        }
    for (int i = 0; i < G * G; ++i) {
        if (cell[size_t(i)] * 64 > total)
            pic.hash |= quint64(1) << i;
    }
    pic.valid = true;

    if (withStructure) {
        double sum = 0.0;
        for (double b : bins)
            sum += b;
        const double mean = sum / (N * N);
        double squares = 0.0;
        for (size_t i = 0; i < bins.size(); ++i) {
            const double d = bins[i] - mean;
            pic.centred[i] = float(d);
            squares += d * d;
        }
        pic.flatMean = float(mean);
        pic.norm = float(std::sqrt(squares));
        pic.hasStructure = true;
    }
    return pic;
}

int screenRowStepFor(int height)
{
    return std::max(1, height / 256);
}

ScreenPicture screenPictureFromGrid(const uchar *grid)
{
    ScreenPicture pic;
    if (!grid)
        return pic;
    constexpr int N = kScreenStructureSide;
    constexpr int G = kScreenHashSide;
    constexpr int R = N / G;
    std::array<int, G * G> cell{};
    int total = 0;
    for (int cy = 0; cy < G; ++cy)
        for (int cx = 0; cx < G; ++cx) {
            int acc = 0;
            for (int y = 0; y < R; ++y)
                for (int x = 0; x < R; ++x)
                    acc += grid[(cy * R + y) * N + cx * R + x];
            const int v = (acc + (R * R) / 2) / (R * R);
            cell[size_t(cy * G + cx)] = v;
            total += v;
        }
    for (int i = 0; i < G * G; ++i) {
        if (cell[size_t(i)] * 64 > total)
            pic.hash |= quint64(1) << i;
    }
    pic.valid = true;
    double sum = 0.0;
    for (int i = 0; i < N * N; ++i)
        sum += grid[i];
    const double mean = sum / (N * N);
    double squares = 0.0;
    for (int i = 0; i < N * N; ++i) {
        const double d = grid[i] - mean;
        pic.centred[size_t(i)] = float(d);
        squares += d * d;
    }
    pic.flatMean = float(mean);
    pic.norm = float(std::sqrt(squares));
    pic.hasStructure = true;
    return pic;
}

QByteArray screenGridOf(const GreyImage &image)
{
    if (!imageInCaps(image))
        return {};
    const GreyImage grid = resizeArea(fitLongerSide(image), kScreenStructureSide, kScreenStructureSide);
    return grid.isEmpty() ? QByteArray() : grid.pixels;
}

QString screenMatchModeName(ScreenMatchMode mode)
{
    return mode == ScreenMatchMode::Hash ? u"hash"_s : u"structure"_s;
}

ScreenMatchMode screenMatchModeFromPack(const QString &name, bool *fromFeatureMode)
{
    if (fromFeatureMode)
        *fromFeatureMode = false;
    const QString lower = name.trimmed().toLower();
    if (lower.isEmpty())
        return ScreenMatchMode::Hash;
    const bool sift = lower.contains(u"sift"_s);
    if (!sift && lower.contains(u"hash"_s))
        return ScreenMatchMode::Hash;
    if (fromFeatureMode)
        *fromFeatureMode = lower != u"structure"_s;
    return ScreenMatchMode::Structure;
}

double hashScore(const ScreenPicture &reference, const ScreenPicture &live)
{
    if (!reference.valid || !live.valid)
        return 0.0;
    const int distance = std::popcount(reference.hash ^ live.hash);
    return 1.0 - distance / 64.0;
}

double structureScore(const ScreenPicture &reference, const ScreenPicture &live)
{
    if (!reference.valid || !live.valid || !reference.hasStructure || !live.hasStructure)
        return 0.0;

    double ncc01 = 0.0;
    const bool flatA = reference.norm <= 1e-3f;
    const bool flatB = live.norm <= 1e-3f;
    if (flatA && flatB) {
        ncc01 = 1.0 - std::abs(double(reference.flatMean) - live.flatMean) / 255.0;
    } else if (flatA || flatB) {
        ncc01 = 0.5;
    } else {
        double dot = 0.0;
        for (size_t i = 0; i < reference.centred.size(); ++i)
            dot += double(reference.centred[i]) * live.centred[i];
        const double ncc = std::clamp(dot / (double(reference.norm) * live.norm), -1.0, 1.0);
        ncc01 = (ncc + 1.0) / 2.0;
    }
    return std::clamp(0.8 * ncc01 + 0.2 * hashScore(reference, live), 0.0, 1.0);
}

double screenScore(ScreenMatchMode mode, const ScreenPicture &reference, const ScreenPicture &live)
{
    return mode == ScreenMatchMode::Hash ? hashScore(reference, live) : structureScore(reference, live);
}

QString screenAggName(ScreenAgg agg)
{
    switch (agg) {
    case ScreenAgg::Any:
        return u"any"_s;
    case ScreenAgg::All:
        return u"all"_s;
    case ScreenAgg::Mean:
        return u"mean"_s;
    }
    return u"any"_s;
}

ScreenAgg screenAggFromName(const QString &name)
{
    const QString lower = name.toLower();
    if (lower == u"all"_s || lower == u"min"_s)
        return ScreenAgg::All;
    if (lower == u"mean"_s)
        return ScreenAgg::Mean;
    return ScreenAgg::Any;
}

double aggregateScores(const QList<double> &regionScores, ScreenAgg agg)
{
    if (regionScores.isEmpty())
        return 0.0;
    switch (agg) {
    case ScreenAgg::Any:
        return *std::max_element(regionScores.cbegin(), regionScores.cend());
    case ScreenAgg::All:
        return *std::min_element(regionScores.cbegin(), regionScores.cend());
    case ScreenAgg::Mean: {
        double sum = 0.0;
        for (double s : regionScores)
            sum += s;
        return sum / regionScores.size();
    }
    }
    return 0.0;
}

ScreenMatcher::ScreenMatcher(const ScreenMatchConfig &config) : m_config(config)
{
    m_config.threshold = std::clamp(m_config.threshold, 0.0, 1.0);
    m_config.startFrames = std::max(1, m_config.startFrames);
    m_config.seqLen = std::max(1, m_config.seqLen);
    m_config.graceMs = std::clamp<qint64>(m_config.graceMs, 0, kScreenMaxDurationMs);
    m_config.cooldownMs = std::clamp<qint64>(m_config.cooldownMs, 0, kScreenMaxDurationMs);
    m_config.loopDelayMs = std::clamp<qint64>(m_config.loopDelayMs, 0, kScreenMaxDurationMs);
    reset();
}

void ScreenMatcher::reset()
{
    m_active = false;
    m_streak = 0;
    m_step = 0;
    m_lastDetectedMs = 0;
    m_lastTickMs = std::numeric_limits<qint64>::min();
    m_cooldownUntilMs = std::numeric_limits<qint64>::min();
    m_blockedUntilMs = std::numeric_limits<qint64>::min();
}

void ScreenMatcher::deactivate(qint64 nowMs)
{
    m_active = false;
    m_streak = 0;
    m_step = 0;
    if (m_config.cooldownFrom == ScreenCooldownFrom::Deactivate)
        m_cooldownUntilMs = saturatingAdd(nowMs, m_config.cooldownMs);
}

bool ScreenMatcher::end(qint64 nowMs)
{
    if (!m_active)
        return false;
    nowMs = std::max(nowMs, m_lastTickMs);
    deactivate(nowMs);
    return true;
}

ScreenTickResult ScreenMatcher::tick(qint64 nowMs, const QList<double> &regionScores)
{
    nowMs = std::max(nowMs, m_lastTickMs); // the clock never runs back
    m_lastTickMs = nowMs;
    ScreenTickResult r;
    r.score = aggregateScores(regionScores, m_config.agg);
    r.detected = !regionScores.isEmpty() && r.score + kScoreEpsilon >= m_config.threshold;

    if (m_active) {
        if (r.detected)
            m_lastDetectedMs = nowMs;
        else if (m_config.stopOnMismatch && nowMs - m_lastDetectedMs > m_config.graceMs) {
            deactivate(nowMs);
            r.event = ScreenEvent::Deactivated;
        }
    } else if (nowMs < m_cooldownUntilMs || nowMs < m_blockedUntilMs) {
        // A tick inside the cooldown (or the loop delay) counts for nothing and breaks any run.
        m_streak = 0;
        m_step = 0;
    } else if (!r.detected) {
        m_streak = 0;
        m_step = 0;
    } else if (++m_streak >= m_config.startFrames) {
        m_streak = 0;
        if (m_config.sequencer && ++m_step < m_config.seqLen) {
            // Next reference; the machine stays inactive until the last one has passed.
        } else {
            m_active = true;
            m_lastDetectedMs = nowMs;
            m_step = m_config.sequencer ? m_config.seqLen - 1 : 0;
            m_cooldownUntilMs = m_config.cooldownFrom == ScreenCooldownFrom::Activate ? saturatingAdd(nowMs, m_config.cooldownMs) : std::numeric_limits<qint64>::max();
            if (m_config.sequencer)
                m_blockedUntilMs = saturatingAdd(nowMs, m_config.loopDelayMs);
            r.event = ScreenEvent::Activated;
        }
    }

    r.active = m_active;
    r.cooling = nowMs < m_cooldownUntilMs;
    r.step = m_step;
    return r;
}

} // namespace murasaki
