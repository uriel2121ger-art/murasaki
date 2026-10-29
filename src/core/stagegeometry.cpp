#include "stagegeometry.h"

#include "transcodebuckets.h"


#include <algorithm>
#include <cmath>

namespace murasaki {

namespace {

constexpr double kMinStageResolutionFactor = 0.1;

bool usable(QSizeF size)
{
    return std::isfinite(size.width()) && std::isfinite(size.height()) && size.width() > 0 && size.height() > 0;
}

bool outputUsable(const StageOutput &o)
{
    return !o.rect.isEmpty() && o.rect.width() <= kMaxStageSide && o.rect.height() <= kMaxStageSide;
}

} // namespace

QRect boundingBox(const QList<QRect> &rects)
{
    QRect box;
    for (const QRect &r : rects) {
        if (!r.isEmpty())
            box = box.isNull() ? r : box.united(r);
    }
    return box;
}

QRectF fitRect(const QRectF &target, QSizeF content, StageFit fit)
{
    if (!usable(content) || !usable(target.size()) || !std::isfinite(target.x()) || !std::isfinite(target.y()))
        return {};
    const QSizeF size = stageFittedSize(target.size(), content, fit);
    if (size.isEmpty())
        return {};
    return {target.x() + (target.width() - size.width()) / 2, target.y() + (target.height() - size.height()) / 2, size.width(), size.height()};
}

StageLayout stageLayout(const QList<StageOutput> &outputs, StageMode mode, const QString &output, QSizeF footprint, StageFit fit)
{
    StageLayout layout;
    if (mode == StageMode::Off || !usable(footprint))
        return layout;

    // The outputs the stage covers
    QList<StageOutput> covered;
    if (mode == StageMode::Output) {
        for (const StageOutput &o : outputs) {
            if (o.name == output && outputUsable(o)) {
                covered.append(o);
                break;
            }
        }
    } else {
        QRect box;
        for (const StageOutput &o : outputs) {
            if (!outputUsable(o))
                continue;
            const QRect grown = box.isNull() ? o.rect : box.united(o.rect);
            if (covered.size() >= kMaxStageOutputs || grown.width() > kMaxStageSide || grown.height() > kMaxStageSide) {
                const QString name = QString::fromUtf8(stagedegrade::kTooManyOutputs.data(), int(stagedegrade::kTooManyOutputs.size()));
                if (!layout.degraded.contains(name))
                    layout.degraded.append(name);
                continue;
            }
            box = grown;
            covered.append(o);
        }
    }
    if (covered.isEmpty())
        return layout;

    QList<QRect> rects;
    for (const StageOutput &o : covered)
        rects.append(o.rect);
    layout.target = boundingBox(rects);

    // The picture, with its four edges rounded once to whole logical pixels
    const QRectF fitted = fitRect(QRectF(layout.target), footprint, fit);
    if (fitted.isNull())
        return layout;
    const double left = std::round(fitted.left());
    const double top = std::round(fitted.top());
    const double right = std::max(std::round(fitted.right()), left + 1);
    const double bottom = std::max(std::round(fitted.bottom()), top + 1);
    layout.picture = QRectF(left, top, right - left, bottom - top);

    for (const StageOutput &o : covered) {
        const QRectF part = QRectF(o.rect).intersected(layout.picture);
        if (part.isEmpty())
            continue;
        StageSlice slice;
        slice.output = o.name;
        slice.scale = o.scale;
        // The edges are whole numbers already (an output and the picture are), so this is exact.
        slice.global = QRect(int(part.left()), int(part.top()), int(part.width()), int(part.height()));
        slice.window = slice.global.translated(-o.rect.topLeft());
        slice.source = QRectF((part.left() - left) / layout.picture.width(), (part.top() - top) / layout.picture.height(),
                              part.width() / layout.picture.width(), part.height() / layout.picture.height());
        layout.slices.append(slice);
    }
    return layout;
}

QSizeF stageFootprint(QSizeF native, double rotation)
{
    return stageRotatedFootprint(native, rotation);
}

QRectF stagePetRect(const StageLayout &layout, const StageSlice &, QSizeF native, double rotation, const QRect &outputRect)
{
    const QSizeF footprint = stageFootprint(native, rotation);
    if (layout.picture.isNull() || !usable(footprint) || !usable(native))
        return {};
    const double sx = layout.picture.width() / footprint.width();
    const double sy = layout.picture.height() / footprint.height();
    double r = std::fmod(std::isfinite(rotation) ? rotation : 0.0, 360.0);
    if (r < 0)
        r += 360.0;
    double w;
    double h;
    if (r == 0.0 || r == 180.0) {
        w = native.width() * sx;
        h = native.height() * sy;
    } else if (r == 90.0 || r == 270.0) {
        w = native.width() * sy;
        h = native.height() * sx;
    } else {
        const double s = std::min(sx, sy);
        w = native.width() * s;
        h = native.height() * s;
    }
    const QPointF centre = layout.picture.center() - QPointF(outputRect.topLeft());
    return {centre.x() - w / 2, centre.y() - h / 2, w, h};
}

QList<StageOutput> sortedStageOutputs(QList<StageOutput> outputs)
{
    std::stable_sort(outputs.begin(), outputs.end(), [](const StageOutput &a, const StageOutput &b) { return a.name < b.name; });
    return outputs;
}

QString resolveStageOutput(const QList<StageOutput> &outputs, const QString &name)
{
    if (name != QLatin1String("*"))
        return name;
    QString best;
    double bestPixels = -1;
    for (const StageOutput &o : sortedStageOutputs(outputs)) {
        if (o.primary)
            return o.name;
        const double pixels = double(o.rect.width()) * o.rect.height() * o.scale * o.scale;
        if (pixels > bestPixels) {
            bestPixels = pixels;
            best = o.name;
        }
    }
    return best;
}

StageLayout stageLayoutOf(const QList<StageOutput> &outputs, const StageRequest &request)
{
    return stageLayout(sortedStageOutputs(outputs), request.mode, resolveStageOutput(outputs, request.output),
                       stageRotatedFootprint(QSizeF(request.native), request.rotation), request.fit);
}

long long stageRequestPixels(const QList<StageOutput> &outputs, const QList<StageRequest> &requests)
{
    long long total = 0;
    for (const StageRequest &request : requests)
        total += stageDevicePixels(stageLayoutOf(outputs, request));
    return total;
}

int stageRequestNeed(const QList<StageOutput> &outputs, const StageRequest &request, double factor, int sourceLongest)
{
    const auto needOf = [&](const StageLayout &layout) {
        return layout.isEmpty() ? 0 : stageNeed(layout.picture.size(), request.native, request.rotation, stageMaxScale(layout) * factor, sourceLongest);
    };
    const bool anyPrimary = std::any_of(outputs.cbegin(), outputs.cend(), [](const StageOutput &o) { return o.primary; });
    if (request.mode == StageMode::Output && request.output == QLatin1String("*") && !anyPrimary) {
        // Nobody said which output is the primary: the copy must serve whichever it is.
        int need = 0;
        for (const StageOutput &o : outputs) {
            StageRequest one = request;
            one.output = o.name;
            need = std::max(need, needOf(stageLayoutOf(outputs, one)));
        }
        return need;
    }
    return needOf(stageLayoutOf(outputs, request));
}

long long stageDevicePixels(const StageLayout &layout)
{
    long long total = 0;
    for (const StageSlice &s : layout.slices) {
        const double scale = std::isfinite(s.scale) && s.scale > 0 ? s.scale : 1;
        total += std::llround(s.window.width() * scale) * std::llround(s.window.height() * scale);
    }
    return total;
}

double stageResolutionFactor(long long pixels, long long maxPixels)
{
    if (pixels <= 0 || maxPixels <= 0 || pixels <= maxPixels)
        return 1;
    return std::max(kMinStageResolutionFactor, std::sqrt(double(maxPixels) / double(pixels)));
}

double stageMaxScale(const StageLayout &layout)
{
    double scale = 1;
    for (const StageSlice &s : layout.slices) {
        if (std::isfinite(s.scale) && s.scale > scale)
            scale = s.scale;
    }
    return scale;
}

} // namespace murasaki
