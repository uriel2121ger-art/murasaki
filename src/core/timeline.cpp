#include "timeline.h"

#include "assignment.h"
#include "automation.h"

#include <QJsonArray>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <iterator>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

bool fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
    return false;
}

bool inRange(double value, double low, double high)
{
    return std::isfinite(value) && value >= low && value <= high;
}

// An integer in range, as JSON has one number type.
bool wholeNumber(const QJsonValue &value, double low, double high, int *out)
{
    if (!value.isDouble())
        return false;
    const double v = value.toDouble();
    if (!inRange(v, low, high) || v != std::floor(v))
        return false;
    *out = int(v);
    return true;
}

constexpr QLatin1String kKindNames[] = {QLatin1String("place"), QLatin1String("show"), QLatin1String("hide"), QLatin1String("move"),
                                        QLatin1String("sound"), QLatin1String("wait"), QLatin1String("stopSounds"), QLatin1String("track")};

std::optional<TimelineAction> parseAction(const QJsonObject &o, QString *error)
{
    TimelineAction a;
    const auto bad = [&](const QString &message) -> std::optional<TimelineAction> {
        fail(error, message);
        return std::nullopt;
    };
    const auto kind = o.value(u"kind"_s).isString() ? actionKindFromName(o.value(u"kind"_s).toString()) : std::nullopt;
    if (!kind)
        return bad(u"kind must be one of place, show, hide, move, sound, wait, stopSounds, track"_s);
    a.kind = *kind;
    const auto number = [&](const QString &key, double *target, bool required) {
        const auto it = o.constFind(key);
        if (it == o.constEnd())
            return !required;
        if (!it->isDouble())
            return false;
        *target = it->toDouble();
        return true;
    };
    if (a.kind == ActionKind::Place || a.kind == ActionKind::Move) {
        if (!number(u"x"_s, &a.x, true) || !number(u"y"_s, &a.y, true))
            return bad(u"x and y must be numbers"_s);
    }
    if (const auto it = o.constFind(u"output"_s); it != o.constEnd()) {
        if (!it->isString())
            return bad(u"output must be a string"_s);
        a.output = it->toString();
    }
    if (const auto it = o.constFind(u"variant"_s); it != o.constEnd()) {
        if (!it->isString())
            return bad(u"variant must be a string"_s);
        a.variant = it->toString();
    }
    if (const auto it = o.constFind(u"anchor"_s); it != o.constEnd()) {
        const QString anchor = it->toString();
        if ((anchor != u"centre" && anchor != u"corner") || (a.kind != ActionKind::Place && a.kind != ActionKind::Move))
            return bad(u"anchor is centre or corner, for place and move"_s);
        a.centre = anchor == u"centre";
    }
    if (a.kind == ActionKind::Move) {
        const QString mode = o.value(u"mode"_s).toString(u"teleport"_s);
        if (mode != u"teleport" && mode != u"smooth")
            return bad(u"mode must be teleport or smooth"_s);
        a.smooth = mode == u"smooth";
        if (o.contains(u"durationMs"_s) && !wholeNumber(o.value(u"durationMs"_s), 0, kMaxMoveDurationMs, &a.durationMs))
            return bad(u"durationMs must be a whole number of milliseconds, 0 to %1"_s.arg(kMaxMoveDurationMs));
    }
    if (a.kind == ActionKind::Track) {
        const auto mode = o.value(u"mode"_s).isString() ? petTrackingModeFromName(o.value(u"mode"_s).toString()) : std::nullopt;
        if (!mode)
            return bad(u"mode must be off, look or follow"_s);
        a.tracking = *mode;
    }
    if (a.kind == ActionKind::Wait) {
        if (!wholeNumber(o.value(u"ms"_s), 0, kMaxWaitMs, &a.waitMs))
            return bad(u"ms must be a whole number of milliseconds, 0 to %1"_s.arg(kMaxWaitMs));
    }
    return a;
}

std::optional<PetRef> parseRef(const QJsonValue &value, QString *error)
{
    const auto bad = [&](const QString &message) -> std::optional<PetRef> {
        fail(error, message);
        return std::nullopt;
    };
    if (!value.isObject())
        return bad(u"pet must be an object"_s);
    const QJsonObject o = value.toObject();
    PetRef ref;
    int ways = 0;
    if (o.contains(u"petId"_s)) {
        if (!o.value(u"petId"_s).isString())
            return bad(u"pet.petId must be a string"_s);
        ref.petId = o.value(u"petId"_s).toString();
        ++ways;
    }
    if (o.contains(u"item"_s)) {
        if (!o.value(u"item"_s).isString())
            return bad(u"pet.item must be a string"_s);
        ref.item = o.value(u"item"_s).toString();
        ++ways;
        if (o.contains(u"variant"_s)) {
            if (!o.value(u"variant"_s).isString())
                return bad(u"pet.variant must be a string"_s);
            ref.variant = o.value(u"variant"_s).toString();
        }
    }
    if (o.contains(u"slot"_s)) {
        if (!wholeNumber(o.value(u"slot"_s), 0, 1000, &ref.slot))
            return bad(u"pet.slot must be a whole number, 0 to 1000"_s);
        ++ways;
    }
    if (ways != 1)
        return bad(u"pet names the pet in exactly one way: petId, item or slot"_s);
    return ref;
}

} // namespace

QString actionKindName(ActionKind kind)
{
    return QString(kKindNames[int(kind)]);
}

std::optional<ActionKind> actionKindFromName(const QString &name)
{
    for (int i = 0; i < int(std::size(kKindNames)); ++i)
        if (name == kKindNames[i])
            return ActionKind(i);
    return std::nullopt;
}

QList<ActionKind> allActionKinds()
{
    return {ActionKind::Place, ActionKind::Show, ActionKind::Hide, ActionKind::Move, ActionKind::Sound, ActionKind::Wait, ActionKind::StopSounds, ActionKind::Track};
}

QJsonObject TimelineAction::toJson() const
{
    QJsonObject o{{u"kind"_s, actionKindName(kind)}};
    if (kind == ActionKind::Place || kind == ActionKind::Move) {
        o.insert(u"x"_s, x);
        o.insert(u"y"_s, y);
        if (!output.isEmpty())
            o.insert(u"output"_s, output);
        if (centre)
            o.insert(u"anchor"_s, u"centre"_s);
    }
    if (kind == ActionKind::Show && !variant.isEmpty())
        o.insert(u"variant"_s, variant);
    if (kind == ActionKind::Move) {
        o.insert(u"mode"_s, smooth ? u"smooth"_s : u"teleport"_s);
        o.insert(u"durationMs"_s, durationMs);
    }
    if (kind == ActionKind::Track)
        o.insert(u"mode"_s, QLatin1StringView(petTrackingModeName(tracking)));
    if (kind == ActionKind::Wait)
        o.insert(u"ms"_s, waitMs);
    return o;
}

QJsonObject PetRef::toJson() const
{
    if (!petId.isEmpty())
        return QJsonObject{{u"petId"_s, petId}};
    if (!item.isEmpty()) {
        QJsonObject o{{u"item"_s, item}};
        if (!variant.isEmpty())
            o.insert(u"variant"_s, variant);
        return o;
    }
    return QJsonObject{{u"slot"_s, slot}};
}

qint64 Timeline::lengthMs() const
{
    qint64 longest = 0;
    for (const TimelineTrack &track : tracks) {
        qint64 length = 0;
        for (const TimelineAction &a : track.actions) {
            if (a.kind == ActionKind::Wait)
                length += a.waitMs;
            else if (a.kind == ActionKind::Move && a.smooth)
                length += a.durationMs;
        }
        longest = std::max(longest, length);
    }
    return longest;
}

QJsonObject Timeline::toJson() const
{
    QJsonArray trackArray;
    for (const TimelineTrack &track : tracks) {
        QJsonArray actionArray;
        for (const TimelineAction &a : track.actions)
            actionArray.append(a.toJson());
        trackArray.append(QJsonObject{{u"pet"_s, track.pet.toJson()}, {u"startVisible"_s, track.startVisible}, {u"actions"_s, actionArray}});
    }
    QJsonObject o{{u"id"_s, id}, {u"name"_s, name}, {u"speed"_s, speed}, {u"restore"_s, restore}, {u"tracks"_s, trackArray}};
    if (!origin.isEmpty())
        o.insert(u"origin"_s, origin);
    return o;
}

bool validateAction(const TimelineAction &a, QString *error)
{
    if (a.kind == ActionKind::Place || a.kind == ActionKind::Move) {
        if (!inRange(a.x, -kMaxPetCoordinate, kMaxPetCoordinate) || !inRange(a.y, -kMaxPetCoordinate, kMaxPetCoordinate))
            return fail(error, u"x and y must be within +-100000"_s);
    }
    if (a.output.size() > 200 || a.variant.size() > 200)
        return fail(error, u"output and variant are at most 200 characters"_s);
    if (a.kind == ActionKind::Move && (a.durationMs < 0 || a.durationMs > kMaxMoveDurationMs))
        return fail(error, u"durationMs must be 0 to %1"_s.arg(kMaxMoveDurationMs));
    if (a.kind == ActionKind::Wait && (a.waitMs < 0 || a.waitMs > kMaxWaitMs))
        return fail(error, u"ms must be 0 to %1"_s.arg(kMaxWaitMs));
    return true;
}

bool validateTimeline(const Timeline &t, QString *error)
{
    if (t.name.trimmed().isEmpty() || t.name.size() > kMaxAutomationNameLength)
        return fail(error, u"name must be 1-%1 characters"_s.arg(kMaxAutomationNameLength));
    if (!inRange(t.speed, kMinTimelineSpeed, kMaxTimelineSpeed))
        return fail(error, u"speed must be between %1 and %2"_s.arg(kMinTimelineSpeed).arg(kMaxTimelineSpeed));
    if (t.origin.size() > 200)
        return fail(error, u"origin is at most 200 characters"_s);
    if (t.tracks.size() > kMaxTimelineTracks)
        return fail(error, u"at most %1 tracks"_s.arg(kMaxTimelineTracks));
    for (int i = 0; i < t.tracks.size(); ++i) {
        const TimelineTrack &track = t.tracks.at(i);
        const PetRef &ref = track.pet;
        const int ways = !ref.petId.isEmpty() + !ref.item.isEmpty() + (ref.slot >= 0);
        if (ways != 1 || ref.petId.size() > 200 || ref.item.size() > 200 || ref.variant.size() > 200)
            return fail(error, u"track %1: pet names the pet in exactly one way (petId, item or slot)"_s.arg(i));
        if (track.actions.size() > kMaxTrackActions)
            return fail(error, u"track %1: at most %2 actions"_s.arg(i).arg(kMaxTrackActions));
        qint64 length = 0;
        for (int j = 0; j < track.actions.size(); ++j) {
            QString problem;
            if (!validateAction(track.actions.at(j), &problem))
                return fail(error, u"track %1, action %2: %3"_s.arg(i).arg(j).arg(problem));
            const TimelineAction &a = track.actions.at(j);
            length += a.kind == ActionKind::Wait ? a.waitMs : (a.kind == ActionKind::Move && a.smooth) ? a.durationMs : 0;
        }
        if (length > kMaxTimelineLengthMs)
            return fail(error, u"track %1 lasts more than 24 hours (%2)"_s.arg(i).arg(kTimelineTooLong));
    }
    return true;
}

std::optional<Timeline> applyTimelineChanges(const Timeline &base, const QJsonObject &changes, QString *error)
{
    Timeline t = base;
    const auto bad = [&](const QString &message) -> std::optional<Timeline> {
        fail(error, message);
        return std::nullopt;
    };
    if (const auto it = changes.constFind(u"name"_s); it != changes.constEnd()) {
        if (!it->isString())
            return bad(u"name must be a string"_s);
        t.name = it->toString();
    }
    if (const auto it = changes.constFind(u"origin"_s); it != changes.constEnd()) {
        if (!it->isString())
            return bad(u"origin must be a string"_s);
        t.origin = it->toString();
    }
    if (const auto it = changes.constFind(u"speed"_s); it != changes.constEnd()) {
        if (!it->isDouble())
            return bad(u"speed must be a number"_s);
        t.speed = it->toDouble();
    }
    if (const auto it = changes.constFind(u"restore"_s); it != changes.constEnd()) {
        if (!it->isBool())
            return bad(u"restore must be a boolean"_s);
        t.restore = it->toBool();
    }
    if (const auto it = changes.constFind(u"tracks"_s); it != changes.constEnd()) {
        if (!it->isArray())
            return bad(u"tracks must be an array"_s);
        const QJsonArray tracks = it->toArray();
        if (tracks.size() > kMaxTimelineTracks)
            return bad(u"at most %1 tracks"_s.arg(kMaxTimelineTracks));
        t.tracks.clear();
        for (int i = 0; i < tracks.size(); ++i) {
            if (!tracks.at(i).isObject())
                return bad(u"track %1 must be an object"_s.arg(i));
            const QJsonObject o = tracks.at(i).toObject();
            TimelineTrack track;
            QString problem;
            const auto ref = parseRef(o.value(u"pet"_s), &problem);
            if (!ref)
                return bad(u"track %1: %2"_s.arg(i).arg(problem));
            track.pet = *ref;
            if (const auto v = o.constFind(u"startVisible"_s); v != o.constEnd()) {
                if (!v->isBool())
                    return bad(u"track %1: startVisible must be a boolean"_s.arg(i));
                track.startVisible = v->toBool();
            }
            if (!o.value(u"actions"_s).isArray())
                return bad(u"track %1: actions must be an array"_s.arg(i));
            const QJsonArray actions = o.value(u"actions"_s).toArray();
            if (actions.size() > kMaxTrackActions)
                return bad(u"track %1: at most %2 actions"_s.arg(i).arg(kMaxTrackActions));
            for (int j = 0; j < actions.size(); ++j) {
                if (!actions.at(j).isObject())
                    return bad(u"track %1, action %2 must be an object"_s.arg(i).arg(j));
                const auto action = parseAction(actions.at(j).toObject(), &problem);
                if (!action)
                    return bad(u"track %1, action %2: %3"_s.arg(i).arg(j).arg(problem));
                track.actions.append(*action);
            }
            t.tracks.append(track);
        }
    }
    if (!validateTimeline(t, error))
        return std::nullopt;
    return t;
}

} // namespace murasaki
