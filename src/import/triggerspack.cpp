#include "triggerspack.h"

#include "petimport.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

#include <cmath>

namespace murasaki::import {

namespace {

QString named(std::string_view name, const QString &detail = {})
{
    const QString text = QString::fromUtf8(name.data(), qsizetype(name.size()));
    return detail.isEmpty() ? text : text + QStringLiteral(": ") + detail;
}

bool number(const QJsonValue &v, double low, double high, double *out)
{
    if (!v.isDouble())
        return false;
    const double d = v.toDouble();
    if (!std::isfinite(d) || d < low || d > high)
        return false;
    *out = d;
    return true;
}

QString text(const QJsonValue &v, int max = 100)
{
    return v.isString() ? v.toString().left(max) : QString();
}

// The last component of a path written on any system, without control characters: all that is kept of a sound path.
QString baseName(QString path)
{
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    QString leaf = path.mid(path.lastIndexOf(QLatin1Char('/')) + 1).left(120);
    QString clean;
    for (const QChar c : leaf)
        clean.append(c.isPrint() ? c : QLatin1Char('?'));
    return clean;
}

constexpr double kMaxCoordinate = 100000;
constexpr double kMaxTimeMs = 24.0 * 3600 * 1000;

} // namespace

TriggersPack parseTriggersPack(const QByteArray &packJson, const QByteArray &metaJson, const QHash<QString, QString> &variantOfBasename)
{
    TriggersPack result;
    const auto refuse = [&](const QString &why) {
        result.error = named(kMdaaPackRefused, why);
        return result;
    };
    if (packJson.size() > kMaxPackJsonBytes)
        return refuse(QStringLiteral("pack.json over %1 MiB").arg(kMaxPackJsonBytes >> 20));
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(packJson, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject())
        return refuse(QStringLiteral("pack.json is not a JSON object"));
    const QJsonObject pack = doc.object();

    QJsonObject out;
    out.insert(QStringLiteral("name"), text(pack.value(QStringLiteral("name"))));
    out.insert(QStringLiteral("id"), text(pack.value(QStringLiteral("id")), 64));

    // meta.json: the speeds the runner reads.
    double speedMult = 1, animSpeedMult = 1;
    if (!metaJson.isEmpty() && metaJson.size() <= kMaxPackJsonBytes) {
        const QJsonDocument meta = QJsonDocument::fromJson(metaJson);
        if (meta.isObject()) {
            double v = 0;
            if (number(meta.object().value(QStringLiteral("speed_mult")), 0.05, 20, &v))
                speedMult = v;
            if (number(meta.object().value(QStringLiteral("anim_speed_mult")), 0.05, 20, &v))
                animSpeedMult = v;
        } else {
            result.notes.append(named(kMdaaPackRefused, QStringLiteral("meta.json is not a JSON object; the speeds are 1")));
        }
    }
    out.insert(QStringLiteral("speedMult"), speedMult);
    out.insert(QStringLiteral("animSpeedMult"), animSpeedMult);

    const QJsonObject event = pack.value(QStringLiteral("event")).toObject();
    double fps = 0, frames = 0;
    number(event.value(QStringLiteral("fps")), 0, 1000, &fps);
    number(event.value(QStringLiteral("frame_count")), 0, 1e7, &frames);
    // The clip of captured frames the trigger is matched against: start_index (0) to end_index (null = to the end, kept as -1).
    double startIndex = 0, endIndex = -1;
    number(event.value(QStringLiteral("start_index")), 0, 1e7, &startIndex);
    number(event.value(QStringLiteral("end_index")), 0, 1e7, &endIndex);
    out.insert(QStringLiteral("event"), QJsonObject{{QStringLiteral("name"), text(event.value(QStringLiteral("name")))}, {QStringLiteral("fps"), fps}, {QStringLiteral("frameCount"), frames},
                                                    {QStringLiteral("startIndex"), startIndex}, {QStringLiteral("endIndex"), endIndex}});

    // The match settings are kept as data for the screen trigger (it never fires); nothing here uses them.
    const QJsonObject match = pack.value(QStringLiteral("match")).toObject();
    QJsonObject keptMatch{{QStringLiteral("mode"), text(match.value(QStringLiteral("mode")), 20)}, {QStringLiteral("agg"), text(match.value(QStringLiteral("agg")), 20)},
                          {QStringLiteral("refMode"), text(match.value(QStringLiteral("ref_mode")), 20)}};
    double cooldown = 500, threshold = 0.75, v = 0;
    number(match.value(QStringLiteral("cooldown_ms")), 0, 86'400'000, &cooldown);
    number(match.value(QStringLiteral("threshold")), 0, 1, &threshold);
    keptMatch.insert(QStringLiteral("cooldownMs"), cooldown);
    keptMatch.insert(QStringLiteral("threshold"), threshold);
    for (const auto &[key, name] : {std::pair<const char *, const char *>{"start_frames_to_check", "startFramesToCheck"}, {"stop_grace_ms", "stopGraceMs"}, {"seq_len", "seqLen"},
                                    {"seq_step", "seqStep"}, {"ref_frame_index", "refFrameIndex"}})
        if (number(match.value(QLatin1String(key)), 0, 1e6, &v))
            keptMatch.insert(QLatin1String(name), v);
    if (match.value(QStringLiteral("stop_on_mismatch")).isBool())
        keptMatch.insert(QStringLiteral("stopOnMismatch"), match.value(QStringLiteral("stop_on_mismatch")).toBool());
    out.insert(QStringLiteral("match"), keptMatch);

    // Regions: kept for the screen trigger.
    QJsonArray regions;
    const QJsonArray rawRegions = pack.value(QStringLiteral("regions")).toArray();
    for (const QJsonValue &value : rawRegions) {
        if (regions.size() >= kMaxPackRegions) {
            result.notes.append(named(kMdaaPackRefused, QStringLiteral("more than %1 regions; the rest are dropped").arg(kMaxPackRegions)));
            break;
        }
        const QJsonObject r = value.toObject();
        QString kind = text(r.value(QStringLiteral("kind")), 10);
        if (kind.isEmpty())
            kind = text(r.value(QStringLiteral("shape")), 10);
        if (kind != QStringLiteral("rect") && kind != QStringLiteral("circle") && kind != QStringLiteral("poly") && kind != QStringLiteral("path"))
            kind = QStringLiteral("rect");
        double x = 0, y = 0, w = 0, h = 0;
        number(r.value(QStringLiteral("x")), -kMaxCoordinate, kMaxCoordinate, &x);
        number(r.value(QStringLiteral("y")), -kMaxCoordinate, kMaxCoordinate, &y);
        number(r.value(QStringLiteral("w")), 0, kMaxCoordinate, &w);
        number(r.value(QStringLiteral("h")), 0, kMaxCoordinate, &h);
        QJsonObject region{{QStringLiteral("kind"), kind}, {QStringLiteral("x"), x}, {QStringLiteral("y"), y}, {QStringLiteral("w"), w}, {QStringLiteral("h"), h},
                           {QStringLiteral("id"), text(r.value(QStringLiteral("id")), 64)}};
        QJsonArray points;
        for (const QJsonValue &p : r.value(QStringLiteral("points")).toArray()) {
            const QJsonArray pair = p.toArray();
            double px = 0, py = 0;
            if (points.size() < kMaxPackPoints && pair.size() == 2 && number(pair.at(0), -kMaxCoordinate, kMaxCoordinate, &px) && number(pair.at(1), -kMaxCoordinate, kMaxCoordinate, &py))
                points.append(QJsonArray{px, py});
        }
        if (!points.isEmpty())
            region.insert(QStringLiteral("points"), points);
        regions.append(region);
    }
    out.insert(QStringLiteral("regions"), regions);

    // The tracks: the part that becomes a timeline.
    QJsonArray tracks;
    const QJsonArray rawTracks = pack.value(QStringLiteral("animations")).toArray();
    int soundsNotFollowed = 0;
    for (const QJsonValue &value : rawTracks) {
        if (tracks.size() >= kMaxPackTracks) {
            result.notes.append(named(kMdaaPackRefused, QStringLiteral("more than %1 animations in the pack; the rest are dropped").arg(kMaxPackTracks)));
            break;
        }
        const QJsonObject t = value.toObject();
        const QString asset = baseName(text(t.value(QStringLiteral("asset_path")), 400)).toLower();
        QString variant = variantOfBasename.value(asset);
        const QString trackName = text(t.value(QStringLiteral("name")));
        if (variant.isEmpty())
            result.notes.append(named(kMdaaMediaRefused, QStringLiteral("the animation \"%1\" of the pack is not one that was imported").arg(trackName.isEmpty() ? asset.left(80) : trackName)));
        QJsonArray actions;
        const QJsonArray rawActions = t.value(QStringLiteral("actions")).toArray();
        for (const QJsonValue &a : rawActions) {
            if (actions.size() >= kMaxPackActions) {
                result.notes.append(named(kMdaaPackRefused, QStringLiteral("more than %1 actions in an animation; the rest are dropped").arg(kMaxPackActions)));
                break;
            }
            const QJsonObject o = a.toObject();
            const QString kind = text(o.value(QStringLiteral("kind")), 10);
            if (kind != QStringLiteral("place") && kind != QStringLiteral("show") && kind != QStringLiteral("hide") && kind != QStringLiteral("move") && kind != QStringLiteral("wait")
                && kind != QStringLiteral("sound")) {
                result.notes.append(named(kMdaaPackRefused, QStringLiteral("an action of kind \"%1\" is not known; dropped").arg(kind.left(20))));
                continue;
            }
            double at = 0;
            if (!number(o.value(QStringLiteral("t_ms")), 0, kMaxTimeMs, &at)) {
                result.notes.append(named(kMdaaPackRefused, QStringLiteral("an action has no usable t_ms; dropped")));
                continue;
            }
            QJsonObject action{{QStringLiteral("t"), qint64(at)}, {QStringLiteral("kind"), kind}};
            double x = 0, y = 0, dur = 0, volume = 1;
            if (number(o.value(QStringLiteral("x")), -kMaxCoordinate, kMaxCoordinate, &x) && number(o.value(QStringLiteral("y")), -kMaxCoordinate, kMaxCoordinate, &y)) {
                action.insert(QStringLiteral("x"), x);
                action.insert(QStringLiteral("y"), y);
            }
            number(o.value(QStringLiteral("dur_ms")), 0, 60'000, &dur);
            action.insert(QStringLiteral("dur"), qint64(dur));
            action.insert(QStringLiteral("mode"), text(o.value(QStringLiteral("move_mode")), 10) == QStringLiteral("tp") ? QStringLiteral("tp") : QStringLiteral("smooth"));
            number(o.value(QStringLiteral("volume")), 0, 1, &volume);
            action.insert(QStringLiteral("volume"), volume);
            if (kind == QStringLiteral("sound")) {
                QString path = text(o.value(QStringLiteral("path")), 400);
                if (path.isEmpty())
                    path = text(o.value(QStringLiteral("sound_path")), 400);
                action.insert(QStringLiteral("sound"), baseName(path)); // the file name only: the path is the author's machine, never followed
                ++soundsNotFollowed;
            }
            actions.append(action);
        }
        tracks.append(QJsonObject{{QStringLiteral("name"), trackName},
                                  {QStringLiteral("variant"), variant},
                                  {QStringLiteral("startVisible"), t.value(QStringLiteral("start_visible")).isBool() ? t.value(QStringLiteral("start_visible")).toBool() : true},
                                  {QStringLiteral("actions"), actions}});
    }
    out.insert(QStringLiteral("tracks"), tracks);
    if (soundsNotFollowed > 0)
        result.notes.append(named(kMdaaSoundNotFollowed, QStringLiteral("%1 sound action(s); their paths are the author's own machine's").arg(soundsNotFollowed)));
    result.json = out;
    return result;
}

} // namespace murasaki::import
