#include "transcodebuckets.h"

#include "assignment.h"
#include "petstage.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

bool isTranscodeBucket(int bucket)
{
    return std::find(kTranscodeBuckets.begin(), kTranscodeBuckets.end(), bucket) != kTranscodeBuckets.end()
        || std::find(kStageBuckets.begin(), kStageBuckets.end(), bucket) != kStageBuckets.end();
}

double displayedLongestSide(double width, QSize native)
{
    if (native.width() <= 0 || native.height() <= 0)
        return width;
    return width * std::max(native.width(), native.height()) / native.width();
}

int transcodeNeed(double displayedLongest, double outputScale, int sourceLongest)
{
    const double pixels = displayedLongest * outputScale;
    if (!std::isfinite(pixels))
        return kTranscodeMaxSide;
    // The small epsilon keeps 300.0000000001 from asking for one pixel more than 300.
    int need = int(std::clamp(std::ceil(pixels - 1e-6), 1.0, double(kTranscodeMaxSide)));
    if (sourceLongest > 0)
        need = std::min(need, sourceLongest);
    return need;
}

int bucketForNeed(int need)
{
    for (const int bucket : kTranscodeBuckets) {
        if (bucket >= need)
            return bucket;
    }
    for (const int bucket : kStageBuckets) {
        if (bucket >= need)
            return bucket;
    }
    return kStageMaxSide;
}

int stageNeed(QSizeF picture, QSize native, double rotation, double maxScale, int sourceLongest)
{
    const QSizeF footprint = stageRotatedFootprint(QSizeF(native), rotation);
    if (footprint.isEmpty() || picture.isEmpty() || !std::isfinite(maxScale))
        return kStageMaxSide;
    // The animation is drawn at s times its native size, s being what makes its turned rectangle cover the picture (the larger factor, so a stretch is not under-sized).
    const double s = std::max(picture.width() / footprint.width(), picture.height() / footprint.height());
    const double pixels = std::max(native.width(), native.height()) * s * maxScale;
    if (!std::isfinite(pixels))
        return kStageMaxSide;
    int need = int(std::clamp(std::ceil(pixels - 1e-6), 1.0, double(kStageMaxSide)));
    if (sourceLongest > 0)
        need = std::min(need, sourceLongest);
    return need;
}

std::optional<int> pickDoneBucket(const QList<int> &done, int need)
{
    std::optional<int> best;
    std::optional<int> largest;
    for (const int bucket : done) {
        if (!largest || bucket > *largest)
            largest = bucket;
        if (bucket >= need && (!best || bucket < *best))
            best = bucket;
    }
    return best ? best : largest;
}

QList<int> bucketsToKeep(const QList<int> &doneNewestFirst, const QList<int> &needs, int limit)
{
    QList<int> keep;
    // The biggest copy always stays: it serves every smaller size, so shrinking a pet and growing it again needs no new conversion.
    if (!doneNewestFirst.isEmpty())
        keep.append(*std::max_element(doneNewestFirst.cbegin(), doneNewestFirst.cend()));
    for (const int need : needs) {
        if (const auto best = pickDoneBucket(doneNewestFirst, need); best && !keep.contains(*best))
            keep.append(*best);
    }
    for (const int bucket : doneNewestFirst) {
        if (keep.size() >= limit)
            break;
        if (!keep.contains(bucket))
            keep.append(bucket);
    }
    return keep;
}

std::optional<QHash<QString, double>> parseOutputScales(const QString &json, QString *problem)
{
    const auto info = parseOutputInfo(json, problem);
    if (!info)
        return std::nullopt;
    QHash<QString, double> scales;
    for (auto it = info->cbegin(); it != info->cend(); ++it)
        scales.insert(it.key(), it->scale);
    return scales;
}

std::optional<QHash<QString, OutputInfo>> parseOutputInfo(const QString &json, QString *problem)
{
    const auto fail = [problem](const QString &message) -> std::optional<QHash<QString, OutputInfo>> {
        if (problem)
            *problem = message;
        return std::nullopt;
    };
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject())
        return fail(u"scales must be a JSON object"_s);
    const QJsonObject object = doc.object();
    if (object.size() > kMaxScaledOutputs)
        return fail(u"at most %1 outputs"_s.arg(kMaxScaledOutputs));
    QHash<QString, OutputInfo> outputs;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (!isValidKeyPart(it.key()))
            return fail(u"output names must be 1-200 characters"_s);
        OutputInfo info;
        QJsonValue scaleValue = it.value();
        if (it.value().isObject()) {
            const QJsonObject o = it.value().toObject();
            scaleValue = o.value(u"scale"_s);
            const auto number = [&](const char *key, double low, double high, double *out) {
                const QJsonValue v = o.value(QLatin1String(key));
                const double d = v.toDouble();
                if (!v.isDouble() || !std::isfinite(d) || d != std::floor(d) || d < low || d > high)
                    return false;
                *out = d;
                return true;
            };
            double x = 0, y = 0, w = 0, h = 0;
            if (!number("x", -100000, 100000, &x) || !number("y", -100000, 100000, &y) || !number("width", 1, kMaxStageSide, &w)
                || !number("height", 1, kMaxStageSide, &h))
                return fail(u"the geometry of an output is x, y (whole numbers within 100000) and width, height (1 to 16384): "_s + it.key().left(200));
            info.rect = QRect(int(x), int(y), int(w), int(h));
            if (const QJsonValue primary = o.value(u"primary"_s); primary.isBool())
                info.primary = primary.toBool();
            else if (!primary.isUndefined())
                return fail(u"primary of an output must be a boolean: "_s + it.key().left(200));
        }
        if (!scaleValue.isDouble())
            return fail(u"the scale of an output must be a number: "_s + it.key().left(200));
        const double scale = scaleValue.toDouble();
        if (!std::isfinite(scale) || scale < kMinOutputScale || scale > kMaxOutputScale)
            return fail(u"scales must be between %1 and %2"_s.arg(kMinOutputScale).arg(kMaxOutputScale));
        info.scale = scale;
        outputs.insert(it.key(), info);
    }
    return outputs;
}

} // namespace murasaki
