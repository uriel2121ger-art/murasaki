#include "screenframes.h"

#include "apngwriter.h"
#include "petimport.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QJsonValue>

#include <algorithm>
#include <cmath>
#include <vector>

namespace murasaki::import {

namespace {

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

int intOf(const QJsonValue &v, int fallback)
{
    return v.isDouble() ? int(std::clamp(v.toDouble(), -1e9, 1e9)) : fallback;
}

struct Span {
    int first, count;
    std::vector<float> w;
};

std::vector<Span> spansFor(int source, int destination)
{
    std::vector<Span> out(static_cast<size_t>(destination));
    const double scale = double(source) / destination;
    for (int i = 0; i < destination; ++i) {
        const double lo = i * scale, hi = (i + 1) * scale;
        const int first = std::clamp(int(std::floor(lo)), 0, source - 1);
        const int last = std::clamp(int(std::ceil(hi)) - 1, first, source - 1);
        Span &s = out[size_t(i)];
        s.first = first;
        s.count = last - first + 1;
        double sum = 0;
        for (int j = first; j <= last; ++j) {
            const double w = std::max(0.0, std::min(hi, double(j + 1)) - std::max(lo, double(j)));
            s.w.push_back(float(w));
            sum += w;
        }
        for (float &w : s.w)
            w = sum > 0 ? float(w / sum) : 1.0f / float(s.count);
    }
    return out;
}

} // namespace

ScreenRefPlan screenRefPlanFromPack(const QJsonObject &pack)
{
    ScreenRefPlan plan;
    const QJsonObject match = pack.value(QStringLiteral("match")).toObject();
    const QJsonObject event = pack.value(QStringLiteral("event")).toObject();
    plan.sequencer = match.value(QStringLiteral("refMode")).toString() == QLatin1String("sequencer");
    plan.refFrameIndex = std::max(0, intOf(match.value(QStringLiteral("refFrameIndex")), 0));
    plan.seqLen = std::max(1, intOf(match.value(QStringLiteral("seqLen")), 8));
    plan.seqStep = std::max(1, intOf(match.value(QStringLiteral("seqStep")), 1));
    plan.startIndex = std::max(0, intOf(event.value(QStringLiteral("startIndex")), 0));
    plan.endIndex = intOf(event.value(QStringLiteral("endIndex")), -1);
    return plan;
}

QList<int> screenFramesUsed(const ScreenRefPlan &plan, int frameCount, bool *shortened)
{
    if (shortened)
        *shortened = false;
    if (frameCount < 1)
        return {};
    const int clipStart = std::clamp(plan.startIndex, 0, frameCount - 1);
    const int clipEnd = plan.endIndex < 0 ? frameCount - 1 : std::clamp(plan.endIndex, clipStart, frameCount - 1);
    const int anchor = std::clamp(clipStart + std::max(0, plan.refFrameIndex), clipStart, clipEnd);
    if (!plan.sequencer)
        return {anchor};
    QList<int> out;
    const int wanted = std::max(1, plan.seqLen);
    for (int k = 0; k < wanted && k < kMaxRefFramesPerRegion; ++k) {
        const long long index = (long long)anchor + (long long)k * std::max(1, plan.seqStep);
        if (index > clipEnd)
            break;
        out.append(int(index));
    }
    if (shortened)
        *shortened = out.size() < wanted;
    return out;
}

QByteArray scaledFramePng(const uchar *rgb, int width, int height, QString *error)
{
    const auto fail = [&](const QString &why) {
        if (error)
            *error = why;
        return QByteArray();
    };
    if (!rgb || width < 1 || height < 1 || width > 16384 || height > 16384)
        return fail(named(kNpyShape, QStringLiteral("a frame of %1 x %2").arg(width).arg(height)));
    int dw = width, dh = height;
    if (std::max(width, height) > kScreenRefMaxSide) {
        const double k = double(kScreenRefMaxSide) / std::max(width, height);
        dw = std::max(1, int(std::lround(width * k)));
        dh = std::max(1, int(std::lround(height * k)));
    }
    // Area average, rows first (the destination rows only keep a float line each).
    const std::vector<Span> xs = spansFor(width, dw), ys = spansFor(height, dh);
    QByteArray rgba(qsizetype(dw) * dh * 4, char(255));
    std::vector<float> acc(size_t(width) * 3);
    for (int y = 0; y < dh; ++y) {
        const Span &sy = ys[size_t(y)];
        std::fill(acc.begin(), acc.end(), 0.0f);
        for (int k = 0; k < sy.count; ++k) {
            const uchar *row = rgb + qsizetype(sy.first + k) * width * 3;
            const float w = sy.w[size_t(k)];
            for (int i = 0; i < width * 3; ++i)
                acc[size_t(i)] += w * row[i];
        }
        auto *out = reinterpret_cast<uchar *>(rgba.data()) + qsizetype(y) * dw * 4;
        for (int x = 0; x < dw; ++x) {
            const Span &sx = xs[size_t(x)];
            for (int c = 0; c < 3; ++c) {
                float sum = 0.0f;
                for (int k = 0; k < sx.count; ++k)
                    sum += sx.w[size_t(k)] * acc[size_t(sx.first + k) * 3 + size_t(c)];
                out[x * 4 + c] = uchar(std::clamp(int(sum + 0.5f), 0, 255));
            }
        }
    }
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::ReadWrite);
    ApngWriter writer(&buffer, 8 << 20);
    if (!writer.begin(dw, dh) || !writer.addFrame(reinterpret_cast<const uchar *>(rgba.constData()), qsizetype(dw) * 4, 0) || !writer.finish())
        return fail(writer.error());
    return bytes;
}

ScreenRefRegion extractRegionRefs(QIODevice *npz, int region, const ScreenRefPlan &plan, const QString &folder, long long *budgetLeft, int frameLimit, const std::function<bool()> &alive)
{
    ScreenRefRegion result;
    result.region = region;
    const QString relDir = QStringLiteral("screenrefs/region%1").arg(region);
    const QString dir = QDir(folder).filePath(relDir);
    QStringList written;
    long long spent = 0;
    QList<int> used;
    QString why;
    bool shortened = false;
    bool cut = false;

    const auto choose = [&](const NpyShape &shape) {
        result.sourceFrames = shape.frames;
        if (plan.startIndex >= shape.frames) { // the clip starts outside the data: no frame of it
            why = named(kScreenRefsNoFrames, QStringLiteral("the clip of region %1 starts at frame %2, the array has %3").arg(region).arg(plan.startIndex).arg(shape.frames));
            return QList<int>();
        }
        used = screenFramesUsed(plan, shape.frames, &shortened);
        if (used.size() > frameLimit) {
            used = used.first(std::max(0, frameLimit));
            cut = true;
        }
        return used;
    };
    const auto deliver = [&](int index, const uchar *rgb, int width, int height) {
        const int step = int(used.indexOf(index));
        QString pngError;
        const QByteArray png = scaledFramePng(rgb, width, height, &pngError);
        if (png.isEmpty()) {
            why = pngError;
            return false;
        }
        if (png.size() > *budgetLeft) {
            why = named(kOutputOverBudget, QStringLiteral("a reference picture of region %1").arg(region));
            return false;
        }
        if (!QDir().mkpath(dir)) {
            why = named(kHelperFailed, QStringLiteral("cannot make the folder for region %1").arg(region));
            return false;
        }
        const QString name = QStringLiteral("step%1.png").arg(step);
        QFile out(QDir(dir).filePath(name));
        if (!out.open(QIODevice::WriteOnly)) {
            why = named(kHelperFailed, QStringLiteral("cannot write a reference picture of region %1").arg(region));
            return false;
        }
        written.append(out.fileName()); // listed before the write: a short write leaves a file that is removed with the rest
        if (out.write(png) != png.size()) {
            why = named(kHelperFailed, QStringLiteral("cannot write a reference picture of region %1").arg(region));
            return false;
        }
        *budgetLeft -= png.size();
        spent += png.size();
        result.frames.append({relDir + QLatin1Char('/') + name, index});
        return true;
    };

    NpyLimits limits;
    QString error;
    if (!readNpzFrames(npz, limits, choose, deliver, &error, nullptr, alive)) {
        result.status = why.isEmpty() ? error : why;
        result.frames.clear();
        for (const QString &f : written)
            QFile::remove(f);
        *budgetLeft += spent; // a region that failed leaves nothing behind and gives its bytes back
        QDir().rmdir(dir);
        return result;
    }
    if (result.frames.isEmpty()) {
        result.status = !why.isEmpty() ? why : named(kScreenRefsNoFrames, QStringLiteral("region %1 keeps no frame").arg(region));
        return result;
    }
    if (cut)
        result.notes.append(named(kScreenRefsOverLimit, QStringLiteral("region %1 keeps %2 frames, the import's total cap").arg(region).arg(result.frames.size())));
    if (shortened)
        result.notes.append(named(kScreenRefsShortened, QStringLiteral("region %1 keeps %2 frames").arg(region).arg(result.frames.size())));
    result.status = QStringLiteral("ready");
    return result;
}

} // namespace murasaki::import
