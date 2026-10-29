#include "packautomation.h"

#include "automation.h"

#include <QCryptographicHash>
#include <QJsonArray>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

QString shortName(const QString &name, int max = kMaxAutomationNameLength)
{
    QString n = name.trimmed();
    return n.left(max).trimmed();
}

// Waits that add up to `ms` (one wait is at most kMaxWaitMs).
void waitFor(TimelineTrack *track, qint64 ms)
{
    while (ms > 0 && track->actions.size() < kMaxTrackActions) {
        TimelineAction wait;
        wait.kind = ActionKind::Wait;
        wait.waitMs = int(std::min<qint64>(ms, kMaxWaitMs));
        ms -= wait.waitMs;
        track->actions.append(wait);
    }
}

} // namespace

// Where it came from, and the key by which a second import replaces it: the package's title and ITEM (two packages can share a title).
QString packageOrigin(const QString &packageName, const QString &itemId)
{
    return (u"package "_s + packageName.left(60) + u" ["_s + itemId.left(100) + u"]"_s).left(200);
}

QString fireNameFrom(const QString &text)
{
    QString out;
    for (const QChar c : text.trimmed()) {
        if (out.size() >= 64)
            break;
        out.append((c.isLetterOrNumber() && c.unicode() < 128) || c == u'.' || c == u'_' || c == u'-' ? c : u'-');
    }
    while (out.startsWith(u'.'))
        out.remove(0, 1);
    return out.isEmpty() ? u"pack"_s : out;
}

std::optional<Timeline> timelineFromScene(const QJsonObject &scene, int index, const QString &itemId, const QString &packageName, QString *error)
{
    const auto fail = [&](const QString &message) -> std::optional<Timeline> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (itemId.isEmpty())
        return fail(u"no item"_s);
    Timeline t;
    t.name = shortName(u"%1: scene %2"_s.arg(packageName.isEmpty() ? u"package"_s : packageName).arg(index + 1));
    t.origin = packageOrigin(packageName, itemId);
    t.restore = false;
    TimelineTrack track;
    track.pet.item = itemId;
    const bool show = scene.value(u"show"_s).toBool();
    const QString variant = scene.value(u"variant"_s).toString();
    if (show) {
        if (variant.isEmpty())
            return fail(u"the scene shows no animation"_s);
        TimelineAction a;
        a.kind = ActionKind::Show;
        a.variant = variant;
        track.actions.append(a);
        if (scene.contains(u"x"_s) && scene.contains(u"y"_s)) {
            TimelineAction place;
            place.kind = ActionKind::Place;
            place.x = scene.value(u"x"_s).toDouble();
            place.y = scene.value(u"y"_s).toDouble();
            track.actions.append(place);
        }
    } else {
        TimelineAction a;
        a.kind = ActionKind::Hide;
        track.actions.append(a);
    }
    t.tracks.append(track);
    if (!validateTimeline(t, error))
        return std::nullopt;
    return t;
}

PackAutomation automationFromPack(const QJsonObject &pack, const QString &itemId, const QString &packageName)
{
    PackAutomation out;
    const QString packName = shortName(pack.value(u"name"_s).toString().isEmpty() ? packageName : pack.value(u"name"_s).toString());
    Timeline t;
    t.name = packName.isEmpty() ? u"pack"_s : packName;
    t.origin = packageOrigin(packageName, itemId);
    t.restore = true;
    t.speed = std::clamp(pack.value(u"speedMult"_s).toDouble(1), kMinTimelineSpeed, kMaxTimelineSpeed);
    if (std::abs(pack.value(u"speedMult"_s).toDouble(1) - t.speed) > 1e-9)
        out.notes.append(u"the speed of the pack is out of range; %1 is used"_s.arg(t.speed));
    bool anyPosition = false;
    const QJsonArray tracks = pack.value(u"tracks"_s).toArray();
    for (const QJsonValue &value : tracks) {
        const QJsonObject track = value.toObject();
        const QString variant = track.value(u"variant"_s).toString();
        const QString name = track.value(u"name"_s).toString();
        if (variant.isEmpty()) {
            out.notes.append(u"the animation \"%1\" was not imported; its track is dropped"_s.arg(name.left(60)));
            continue;
        }
        if (t.tracks.size() + 2 > kMaxTimelineTracks) {
            out.notes.append(u"more animations than a timeline holds (%1 tracks); \"%2\" and the rest are dropped"_s.arg(kMaxTimelineTracks).arg(name.left(60)));
            break;
        }
        // Absolute times, in order (a stable sort keeps the order of equal times).
        QList<QJsonObject> actions;
        for (const QJsonValue &a : track.value(u"actions"_s).toArray())
            actions.append(a.toObject());
        std::stable_sort(actions.begin(), actions.end(), [](const QJsonObject &l, const QJsonObject &r) { return l.value(u"t"_s).toDouble() < r.value(u"t"_s).toDouble(); });
        TimelineTrack shown, moving;
        shown.pet.item = moving.pet.item = itemId;
        shown.pet.variant = moving.pet.variant = variant;
        shown.startVisible = track.value(u"startVisible"_s).toBool(true);
        moving.startVisible = shown.startVisible;
        qint64 shownAt = 0, movedAt = 0;
        bool movedAny = false;
        bool truncated = false;
        for (const QJsonObject &a : std::as_const(actions)) {
            const QString kind = a.value(u"kind"_s).toString();
            const qint64 at = qint64(a.value(u"t"_s).toDouble());
            if (kind == u"show" || kind == u"hide") {
                waitFor(&shown, at - shownAt);
                shownAt = std::max(shownAt, at);
                if (shown.actions.size() >= kMaxTrackActions) {
                    truncated = true;
                    break;
                }
                TimelineAction act;
                act.kind = kind == u"show" ? ActionKind::Show : ActionKind::Hide;
                if (kind == u"show")
                    act.variant = variant;
                shown.actions.append(act);
            } else if (kind == u"move" && a.contains(u"x"_s) && a.contains(u"y"_s)) {
                waitFor(&moving, at - movedAt);
                movedAt = std::max(movedAt, at);
                if (moving.actions.size() >= kMaxTrackActions) {
                    truncated = true;
                    break;
                }
                if (!movedAny && at > 0) {
                    // Anima's base position is the move at time 0, else the first move: the pet stands there from the start.
                    TimelineAction base;
                    base.kind = ActionKind::Move;
                    base.centre = true;
                    base.x = a.value(u"x"_s).toDouble();
                    base.y = a.value(u"y"_s).toDouble();
                    moving.actions.prepend(base);
                }
                movedAny = true;
                TimelineAction act;
                act.kind = ActionKind::Move;
                act.centre = true; // a pack's x and y are where the CENTRE of the animation goes; the daemon resolves it with the pet's real size
                act.x = a.value(u"x"_s).toDouble();
                act.y = a.value(u"y"_s).toDouble();
                const int dur = int(a.value(u"dur"_s).toDouble());
                act.smooth = dur > 0; // Anima interpolates when the duration is above zero and jumps otherwise
                act.durationMs = std::clamp(dur, 0, kMaxMoveDurationMs);
                moving.actions.append(act);
                anyPosition = true;
                if (act.smooth)
                    movedAt += act.durationMs;
            } else if (kind == u"sound") {
                out.notes.append(u"a sound of \"%1\" (%2) is not played: its path is the author's machine's"_s.arg(name.left(60), a.value(u"sound"_s).toString().left(60)));
            } // place and wait do nothing in Anima's runner either
        }
        if (truncated)
            out.notes.append(u"\"%1\" has more actions than a track holds; the rest are dropped"_s.arg(name.left(60)));
        t.tracks.append(shown);
        if (!moving.actions.isEmpty())
            t.tracks.append(moving);
    }
    if (anyPosition)
        out.notes.append(u"positions are where the centre of the pet goes, as in Anima (resolved with the pet's size when the timeline runs)"_s);
    out.notes.append(u"Anima repeats an animation's track while its screen trigger holds; the timeline plays once"_s);
    if (const double animSpeed = pack.value(u"animSpeedMult"_s).toDouble(1); std::abs(animSpeed - 1) > 1e-9)
        out.notes.append(u"the pack's animation speed (%1) is not applied: set the speed of the pet itself"_s.arg(animSpeed));
    if (t.tracks.isEmpty()) {
        out.notes.append(u"no track could be made from the pack"_s);
        return out;
    }
    QString problem;
    if (!validateTimeline(t, &problem)) {
        out.notes.append(u"the timeline of the pack is not valid: %1"_s.arg(problem));
        return out;
    }
    out.timelines.append(t);

    const QJsonObject match = pack.value(u"match"_s).toObject();
    const int cooldown = std::clamp(int(match.value(u"cooldownMs"_s).toDouble(kDefaultTriggerCooldownMs)), kMinTriggerCooldownMs, kMaxTriggerCooldownMs);
    Trigger fire;
    fire.name = shortName(t.name + u" (fire)"_s);
    fire.enabled = false; // imported things start disabled
    fire.cooldownMs = cooldown;
    fire.origin = t.origin;
    fire.source.kind = TriggerKind::Fire;
    fire.source.name = fireNameFrom(t.name).left(56) + u"-"_s + QString::fromLatin1(QCryptographicHash::hash(itemId.toUtf8(), QCryptographicHash::Sha1).toHex().left(7));
    fire.action.type = TriggerActionType::Timeline;
    out.triggers.append(fire);

    Trigger screen = fire;
    screen.name = shortName(t.name + u" (screen)"_s);
    screen.source = TriggerSource{};
    screen.source.kind = TriggerKind::Screen;
    screen.source.screen = QJsonObject{{u"regions"_s, pack.value(u"regions"_s)}, {u"match"_s, match}, {u"event"_s, pack.value(u"event"_s)}, {u"pack"_s, packName}};
    out.triggers.append(screen);
    out.notes.append(u"the screen trigger keeps the regions and the match settings and never fires (%1)"_s.arg(QString(kAutomationScreenInactive)));
    return out;
}

} // namespace murasaki
