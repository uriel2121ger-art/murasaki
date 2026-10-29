#include "petruntime.h"

#include "petlimits.h"
#include "petstore.h"

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

bool inRange(double value, double low, double high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

constexpr int kMaxMotionMs = 60'000;

} // namespace

PetPosition positionAt(const PetMotion &m, qint64 nowMs)
{
    if (m.durationMs <= 0 || nowMs >= m.startMs + m.durationMs)
        return {m.toX, m.toY};
    if (nowMs <= m.startMs)
        return {m.fromX, m.fromY};
    const double t = double(nowMs - m.startMs) / double(m.durationMs);
    return {m.fromX + (m.toX - m.fromX) * t, m.fromY + (m.toY - m.fromY) * t};
}

bool motionDone(const PetMotion &m, qint64 nowMs)
{
    return m.durationMs <= 0 || nowMs >= m.startMs + m.durationMs;
}

QJsonObject PetRuntime::toJson() const
{
    QJsonObject o;
    if (hidden)
        o.insert(u"hidden"_s, true);
    if (position) {
        o.insert(u"x"_s, position->x);
        o.insert(u"y"_s, position->y);
    }
    if (!output.isEmpty())
        o.insert(u"output"_s, output);
    if (motion)
        o.insert(u"motion"_s, QJsonObject{{u"fromX"_s, motion->fromX},
                                          {u"fromY"_s, motion->fromY},
                                          {u"toX"_s, motion->toX},
                                          {u"toY"_s, motion->toY},
                                          {u"startMs"_s, double(motion->startMs)},
                                          {u"durationMs"_s, motion->durationMs}});
    if (opacity)
        o.insert(u"opacity"_s, *opacity);
    if (!variant.isEmpty())
        o.insert(u"variant"_s, variant);
    if (tracking)
        o.insert(u"tracking"_s, QLatin1StringView(petTrackingModeName(*tracking)));
    return o;
}

bool validatePetRuntime(const PetRuntime &r, QString *error)
{
    if (r.position && (!inRange(r.position->x, -kMaxRuntimeCoordinate, kMaxRuntimeCoordinate) || !inRange(r.position->y, -kMaxRuntimeCoordinate, kMaxRuntimeCoordinate)))
        return fail(error, u"x and y must be within +-100000"_s);
    if (r.output.size() > 200)
        return fail(error, u"output must be at most 200 characters"_s);
    if (r.motion) {
        const PetMotion &m = *r.motion;
        for (const double v : {m.fromX, m.fromY, m.toX, m.toY})
            if (!inRange(v, -kMaxRuntimeCoordinate, kMaxRuntimeCoordinate))
                return fail(error, u"a motion must stay within +-100000"_s);
        if (m.durationMs < 0 || m.durationMs > kMaxMotionMs)
            return fail(error, u"a motion lasts 0 to 60000 ms"_s);
        if (m.startMs < 0)
            return fail(error, u"a motion starts at a time of the monotonic clock"_s);
    }
    if (r.variant.size() > 200)
        return fail(error, u"variant must be at most 200 characters"_s);
    if (r.opacity && !inRange(*r.opacity, kMinPetOpacity, 1.0))
        return fail(error, u"opacity must be between 0.1 and 1"_s);
    return true;
}

std::optional<PetRuntime> parsePetRuntime(const QJsonObject &json, QString *error)
{
    PetRuntime r;
    const auto bad = [&](const QString &message) -> std::optional<PetRuntime> {
        fail(error, message);
        return std::nullopt;
    };
    if (const auto it = json.constFind(u"hidden"_s); it != json.constEnd()) {
        if (!it->isBool())
            return bad(u"hidden must be a boolean"_s);
        r.hidden = it->toBool();
    }
    const bool hasX = json.contains(u"x"_s), hasY = json.contains(u"y"_s);
    if (hasX != hasY)
        return bad(u"x and y come together"_s);
    if (hasX) {
        if (!json.value(u"x"_s).isDouble() || !json.value(u"y"_s).isDouble())
            return bad(u"x and y must be numbers"_s);
        r.position = PetPosition{json.value(u"x"_s).toDouble(), json.value(u"y"_s).toDouble()};
    }
    if (const auto it = json.constFind(u"output"_s); it != json.constEnd()) {
        if (!it->isString())
            return bad(u"output must be a string"_s);
        r.output = it->toString();
    }
    if (const auto it = json.constFind(u"motion"_s); it != json.constEnd()) {
        if (!it->isObject())
            return bad(u"motion must be an object"_s);
        const QJsonObject o = it->toObject();
        PetMotion m;
        for (const auto &[key, target] : {std::pair<QString, double *>{u"fromX"_s, &m.fromX}, {u"fromY"_s, &m.fromY}, {u"toX"_s, &m.toX}, {u"toY"_s, &m.toY}}) {
            if (!o.value(key).isDouble())
                return bad(u"motion.%1 must be a number"_s.arg(key));
            *target = o.value(key).toDouble();
        }
        const double start = o.value(u"startMs"_s).toDouble(-1), duration = o.value(u"durationMs"_s).toDouble(-1);
        if (!inRange(start, 0, 9e15) || !inRange(duration, 0, kMaxMotionMs) || duration != std::floor(duration))
            return bad(u"motion.startMs and motion.durationMs must be numbers (0 to 60000 ms)"_s);
        m.startMs = qint64(start);
        m.durationMs = int(duration);
        r.motion = m;
    }
    if (const auto it = json.constFind(u"opacity"_s); it != json.constEnd()) {
        if (!it->isDouble())
            return bad(u"opacity must be a number"_s);
        r.opacity = it->toDouble();
    }
    if (const auto it = json.constFind(u"variant"_s); it != json.constEnd()) {
        if (!it->isString())
            return bad(u"variant must be a string"_s);
        r.variant = it->toString();
    }
    if (const auto it = json.constFind(u"tracking"_s); it != json.constEnd()) {
        const auto mode = it->isString() ? petTrackingModeFromName(it->toString()) : std::nullopt;
        if (!mode)
            return bad(u"tracking must be off, look or follow"_s);
        r.tracking = mode;
    }
    if (!validatePetRuntime(r, error))
        return std::nullopt;
    return r;
}

PetPosition effectivePosition(const PetRuntime &runtime, const PetPosition &stored, qint64 nowMs)
{
    if (runtime.motion)
        return positionAt(*runtime.motion, nowMs);
    return runtime.position.value_or(stored);
}

QJsonObject runtimeLayerToJson(const QHash<QString, PetRuntime> &layer)
{
    QJsonObject o;
    for (auto it = layer.constBegin(); it != layer.constEnd(); ++it)
        if (!it->isDefault())
            o.insert(it.key(), it->toJson());
    return o;
}

QHash<QString, PetRuntime> runtimeLayerFromJson(const QJsonObject &json, int *skipped)
{
    QHash<QString, PetRuntime> layer;
    int bad = 0;
    for (auto it = json.constBegin(); it != json.constEnd(); ++it) {
        const auto runtime = it->isObject() ? parsePetRuntime(it->toObject()) : std::nullopt;
        if (runtime)
            layer.insert(it.key(), *runtime);
        else
            ++bad;
    }
    if (skipped)
        *skipped = bad;
    return layer;
}

} // namespace murasaki
