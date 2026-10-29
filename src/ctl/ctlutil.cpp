#include "ctlutil.h"

#include "petlimits.h"
#include "petstage.h"
#include "petstates.h"
#include "pettiming.h"

#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include <cmath>

using namespace Qt::StringLiterals;

namespace murasaki {

QString resolveDesktopArg(const QString &arg, const QJsonArray &desktops)
{
    for (const QJsonValue &value : desktops) {
        if (value.toObject().value(u"id"_s).toString() == arg)
            return arg;
    }
    bool isNumber = false;
    const int number = arg.toInt(&isNumber);
    if (isNumber && number > 0) {
        for (const QJsonValue &value : desktops) {
            if (value.toObject().value(u"number"_s).toInt() == number)
                return value.toObject().value(u"id"_s).toString();
        }
    }
    for (Qt::CaseSensitivity cs : {Qt::CaseSensitive, Qt::CaseInsensitive}) {
        for (const QJsonValue &value : desktops) {
            if (value.toObject().value(u"name"_s).toString().compare(arg, cs) == 0)
                return value.toObject().value(u"id"_s).toString();
        }
    }
    return arg;
}

bool desktopArgMatches(const QString &arg, const QJsonArray &desktops)
{
    const QString resolved = resolveDesktopArg(arg, desktops);
    for (const QJsonValue &value : desktops) {
        if (value.toObject().value(u"id"_s).toString() == resolved)
            return true;
    }
    return false;
}

QJsonObject findAssignment(const QJsonArray &assignments, const QString &output, const QString &desktop)
{
    for (const QJsonValue &value : assignments) {
        const QJsonObject entry = value.toObject();
        if (entry.value(u"output"_s).toString() == output && entry.value(u"desktop"_s).toString() == desktop)
            return entry;
    }
    return {};
}

QJsonObject optionsWithMuted(const QJsonObject &entry, bool muted)
{
    QJsonObject options{
        {u"muted"_s, muted},
        {u"fill"_s, entry.value(u"fill"_s).toString(u"fill"_s)},
        {u"properties"_s, entry.value(u"properties"_s).toObject()},
    };
    if (entry.value(u"span"_s).toBool())
        options.insert(u"span"_s, true);
    return options;
}

QString formatOutputLine(const QJsonObject &output)
{
    const QString item = output.value(u"itemId"_s).toString();
    const QString state = output.value(u"occluded"_s).toBool() ? u"paused (covered)"_s
        : output.value(u"playing"_s).toBool()                   ? u"playing"_s
                                                                : u"idle"_s;
    return output.value(u"name"_s).toString() + u'\t' + output.value(u"desktop"_s).toString() + u'\t'
        + (item.isEmpty() ? u"-"_s : item) + u'\t' + state;
}

bool parseNumberArg(const QString &text, double *value)
{
    bool ok = false;
    const double parsed = text.trimmed().toDouble(&ok);
    if (!ok || !std::isfinite(parsed))
        return false;
    *value = parsed;
    return true;
}

bool petAddOptions(const QString &x, const QString &y, const QString &width, const QString &fps, QJsonObject *options,
                   QString *error)
{
    const std::pair<const char *, const QString *> args[] = {{"x", &x}, {"y", &y}, {"width", &width}, {"fps", &fps}};
    for (const auto &[key, text] : args) {
        if (text->isEmpty())
            continue;
        double number = 0;
        if (!parseNumberArg(*text, &number)) {
            if (error)
                *error = u"--%1 needs a number, not \"%2\""_s.arg(QLatin1String(key), *text);
            return false;
        }
        options->insert(QLatin1String(key), number);
    }
    return true;
}

bool petBehaviourChanges(const PetBehaviourArgs &args, QJsonObject *changes, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    QJsonObject result;
    const std::pair<const char *, const std::optional<QString> *> switches[] = {
        {"clickThrough", &args.clickThrough}, {"locked", &args.lock},       {"keepAbove", &args.keepAbove}, {"hoverHide", &args.hoverHide},
        {"focusFade", &args.focusFade},       {"smartDodge", &args.smartDodge}, {"flipH", &args.flipH},       {"flipV", &args.flipV}};
    const QHash<QString, QString> options{{u"clickThrough"_s, u"--click-through"_s}, {u"locked"_s, u"--lock"_s},       {u"keepAbove"_s, u"--keep-above"_s},
                                          {u"hoverHide"_s, u"--hover-hide"_s},       {u"focusFade"_s, u"--focus-fade"_s}, {u"smartDodge"_s, u"--smart-dodge"_s},
                                          {u"flipH"_s, u"--flip-h"_s},               {u"flipV"_s, u"--flip-v"_s}};
    for (const auto &[key, value] : switches) {
        if (!value->has_value())
            continue;
        if (**value != u"on" && **value != u"off")
            return fail(u"%1 takes on or off, not \"%2\""_s.arg(options.value(QLatin1String(key)), **value));
        result.insert(QLatin1String(key), **value == u"on");
    }
    if (args.rotation) {
        double degrees = 0;
        if (!parseNumberArg(*args.rotation, &degrees))
            return fail(u"--rotation needs a number of degrees, not \"%1\""_s.arg(*args.rotation));
        result.insert(u"rotation"_s, degrees);
    }
    *changes = result;
    return true;
}

bool petTimingChanges(const QStringList &words, QJsonObject *changes, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const QString mode = words.value(0);
    double number = 0;
    if (mode == u"file" && words.size() == 1) {
        *changes = QJsonObject{{u"fps"_s, 0}, {u"speed"_s, 1}};
        return true;
    }
    if (mode == u"fixed" && words.size() <= 2) {
        if (words.size() == 2 && !parseNumberArg(words.at(1), &number))
            return fail(u"the rate must be a number, not \"%1\""_s.arg(words.at(1)));
        *changes = QJsonObject{{u"fps"_s, words.size() == 2 ? number : double(kDefaultPetFps)}, {u"speed"_s, 1}};
        return true;
    }
    if (mode == u"speed" && words.size() == 2) {
        if (!parseNumberArg(words.at(1), &number))
            return fail(u"the speed must be a number, not \"%1\""_s.arg(words.at(1)));
        *changes = QJsonObject{{u"fps"_s, 0}, {u"speed"_s, number}};
        return true;
    }
    return fail(u"timing is file, fixed [fps] or speed <factor>"_s);
}

bool petStageChanges(const QString &mode, const std::optional<QString> &fit, const std::optional<QString> &overFullscreen, QJsonObject *changes,
                     QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!stageModeFromName(mode))
        return fail(u"the stage mode is off, output or desktop, not \"%1\""_s.arg(mode.left(40)));
    QJsonObject stage{{u"mode"_s, mode}};
    if (fit) {
        if (!stageFitFromName(*fit))
            return fail(u"--fit is contain, cover or stretch, not \"%1\""_s.arg(fit->left(40)));
        stage.insert(u"fit"_s, *fit);
    }
    if (overFullscreen) {
        if (!stageOverFullscreenFromName(*overFullscreen))
            return fail(u"--over-fullscreen is inherit, show or hide, not \"%1\""_s.arg(overFullscreen->left(40)));
        stage.insert(u"overFullscreen"_s, *overFullscreen);
    }
    *changes = QJsonObject{{u"stage"_s, stage}};
    return true;
}

bool petStateChanges(const QStringList &words, const std::optional<QString> &order, const std::optional<QString> &advance, bool once, const std::optional<QString> &holdMs,
                     QJsonObject *changes, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    const auto stateOf = [&](const QString &word) { return petStateFromName(word).has_value(); };
    QJsonObject built;
    if (!words.isEmpty() && words.at(0) == u"clear") {
        if (order || advance || once || holdMs)
            return fail(u"--order, --advance, --once and --hold-ms are for a sequence, not for clear"_s);
        if (words.size() == 1) {
            built = QJsonObject{{u"states"_s, QJsonValue(QJsonValue::Null)}};
        } else if (words.size() == 2 && stateOf(words.at(1))) {
            built = QJsonObject{{u"states"_s, QJsonObject{{words.at(1), QJsonValue(QJsonValue::Null)}}}};
        } else {
            return fail(u"pets state clear takes no word, or one state: idle, grabbed, moving or released"_s);
        }
    } else if (words.size() == 3 && stateOf(words.at(0)) && words.at(1) == u"variant") {
        if (order || advance || once || holdMs)
            return fail(u"--order, --advance, --once and --hold-ms are for a sequence, not a single variant"_s);
        built = QJsonObject{{u"states"_s, QJsonObject{{words.at(0), QJsonObject{{u"variant"_s, words.at(2)}}}}}};
    } else if (words.size() == 3 && stateOf(words.at(0)) && words.at(1) == u"sequence") {
        QJsonArray items;
        for (const QString &item : words.at(2).split(u',')) {
            const QString name = item.trimmed();
            if (name.isEmpty())
                return fail(u"the sequence is a comma separated list of variant names, with no empty one"_s);
            items.append(name);
        }
        QJsonObject sequence{{u"items"_s, items}};
        if (order)
            sequence.insert(u"order"_s, *order);
        if (advance)
            sequence.insert(u"advance"_s, *advance);
        if (once)
            sequence.insert(u"once"_s, true);
        if (holdMs) {
            bool ok = false;
            const int value = holdMs->toInt(&ok);
            if (!ok)
                return fail(u"--hold-ms is a whole number of milliseconds, not \"%1\""_s.arg(holdMs->left(40)));
            sequence.insert(u"holdMs"_s, value);
        }
        built = QJsonObject{{u"states"_s, QJsonObject{{words.at(0), QJsonObject{{u"sequence"_s, sequence}}}}}};
    } else {
        return fail(u"pets state <pet id> <idle|grabbed|moving|released> variant <name> | sequence <a,b,c> [--order linear|random] [--advance auto|click] [--once] [--hold-ms N], "
                    "or pets state <pet id> clear [state]"_s);
    }
    QString problem;
    if (!applyPetStatesChanges(PetStates{}, built.value(u"states"_s), &problem))
        return fail(problem);
    *changes = built;
    return true;
}

bool petTrackChanges(const QString &mode, const std::optional<QString> &distance, const std::optional<QString> &speed, bool returnHome, bool noReturnHome, QJsonObject *changes,
                     QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!petTrackingModeFromName(mode))
        return fail(u"the tracking mode is off, look or follow, not \"%1\""_s.arg(mode.left(40)));
    if (returnHome && noReturnHome)
        return fail(u"--return and --no-return contradict each other"_s);
    QJsonObject tracking{{u"mode"_s, mode}};
    double number = 0;
    if (distance) {
        if (!parseNumberArg(*distance, &number))
            return fail(u"--distance is a number of pixels, not \"%1\""_s.arg(distance->left(40)));
        tracking.insert(u"distance"_s, number);
    }
    if (speed) {
        if (!parseNumberArg(*speed, &number))
            return fail(u"--speed is a number, not \"%1\""_s.arg(speed->left(40)));
        tracking.insert(u"speed"_s, number);
    }
    if (returnHome || noReturnHome)
        tracking.insert(u"returnHome"_s, returnHome);
    QString problem;
    if (!applyPetTrackingChanges(PetTracking{}, tracking, &problem))
        return fail(problem);
    *changes = QJsonObject{{u"tracking"_s, tracking}};
    return true;
}

QString formatPetLine(const QJsonObject &pet)
{
    const auto number = [&](const char *key) { return QString::number(pet.value(QLatin1String(key)).toDouble(), 'g', 10); };
    // A pet in the fullscreen mode says so in a last column (output or desktop); a pet that is not one has no such column, so the lines scripts read are as they were.
    const QString stageMode = pet.value(u"stage"_s).toObject().value(u"mode"_s).toString();
    const QString stageColumn = (stageMode == u"output" || stageMode == u"desktop") ? u'\t' + stageMode : QString();
    // and so for tracking and for states: one more column each, only for a pet that has them
    const QString trackingMode = pet.value(u"tracking"_s).toObject().value(u"mode"_s).toString();
    const QString trackingColumn = (trackingMode == u"look" || trackingMode == u"follow") ? u"\ttracking:"_s + trackingMode : QString();
    QStringList stateNames;
    const QJsonObject states = pet.value(u"states"_s).toObject();
    for (const char *name : {"idle", "grabbed", "moving", "released"}) {
        if (states.contains(QLatin1String(name)))
            stateNames << QString::fromLatin1(name);
    }
    const QString statesColumn = stateNames.isEmpty() ? QString() : u"\tstates:"_s + stateNames.join(u',');
    return QStringList{pet.value(u"petId"_s).toString(),
                       pet.value(u"itemId"_s).toString(),
                       pet.value(u"variant"_s).toString(),
                       pet.value(u"output"_s).toString(),
                       number("x"),
                       number("y"),
                       number("width"),
                       number("opacity"),
                       number("fps"),
                       pet.value(u"paused"_s).toBool() ? u"paused"_s : u"playing"_s,
                       QString::number(pet.value(u"speed"_s).toDouble(1), 'g', 10)}
        .join(u'\t')
        + stageColumn + trackingColumn + statesColumn;
}

std::optional<bool> arrangeTarget(const QString &arg, bool current)
{
    if (arg == u"on")
        return true;
    if (arg == u"off")
        return false;
    if (arg == u"toggle")
        return !current;
    return std::nullopt;
}

QString resolvePlaylistArg(const QString &arg, const QJsonArray &playlists, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    for (const QJsonValue &value : playlists) {
        if (value.toObject().value(u"id"_s).toString() == arg)
            return arg;
    }
    for (const QJsonValue &value : playlists) {
        if (value.toObject().value(u"name"_s).toString() == arg)
            return value.toObject().value(u"id"_s).toString();
    }
    QString found;
    int matches = 0;
    for (const QJsonValue &value : playlists) {
        if (value.toObject().value(u"name"_s).toString().compare(arg, Qt::CaseInsensitive) == 0) {
            found = value.toObject().value(u"id"_s).toString();
            ++matches;
        }
    }
    if (matches == 1)
        return found;
    return fail(matches > 1 ? u"more than one playlist is called \"%1\"; use its id"_s.arg(arg)
                            : u"no playlist matches \"%1\""_s.arg(arg));
}

QString formatPlaylistLine(const QJsonObject &playlist)
{
    return QStringList{playlist.value(u"id"_s).toString(),
                       playlist.value(u"name"_s).toString(),
                       playlist.value(u"order"_s).toString(),
                       QString::number(playlist.value(u"intervalMinutes"_s).toInt()),
                       QString::number(playlist.value(u"items"_s).toArray().size())}
        .join(u'\t');
}

std::optional<QJsonObject> playlistChanges(const QStringList &pairs, QString *error)
{
    const auto fail = [error](const QString &message) -> std::optional<QJsonObject> {
        if (error)
            *error = message;
        return std::nullopt;
    };
    if (pairs.isEmpty())
        return fail(u"nothing to set; use key=value (name, order, interval, changeOnLogin)"_s);
    QJsonObject changes;
    for (const QString &pair : pairs) {
        const qsizetype eq = pair.indexOf(u'=');
        if (eq <= 0)
            return fail(u"\"%1\" is not key=value"_s.arg(pair));
        const QString key = pair.left(eq);
        const QString value = pair.mid(eq + 1);
        if (key == u"name") {
            changes.insert(u"name"_s, value);
        } else if (key == u"order") {
            if (value != u"sequential" && value != u"shuffle")
                return fail(u"order must be sequential or shuffle"_s);
            changes.insert(u"order"_s, value);
        } else if (key == u"interval" || key == u"intervalMinutes") {
            bool ok = false;
            const int minutes = value.toInt(&ok);
            if (!ok)
                return fail(u"interval must be a whole number of minutes, not \"%1\""_s.arg(value));
            changes.insert(u"intervalMinutes"_s, minutes);
        } else if (key == u"changeOnLogin") {
            if (value == u"true" || value == u"on" || value == u"yes")
                changes.insert(u"changeOnLogin"_s, true);
            else if (value == u"false" || value == u"off" || value == u"no")
                changes.insert(u"changeOnLogin"_s, false);
            else
                return fail(u"changeOnLogin must be on or off"_s);
        } else {
            return fail(u"unknown key \"%1\"; use name, order, interval or changeOnLogin"_s.arg(key));
        }
    }
    return changes;
}

QList<std::pair<QString, QString>> playlistTargets(const QJsonArray &assignments, const QString &output,
                                                   const QString &desktop)
{
    QList<std::pair<QString, QString>> targets;
    for (const QJsonValue &value : assignments) {
        const QJsonObject entry = value.toObject();
        if (!entry.value(u"itemId"_s).toString().startsWith(u"playlist:"))
            continue;
        const QString entryOutput = entry.value(u"output"_s).toString();
        const QString entryDesktop = entry.value(u"desktop"_s).toString();
        if (!output.isEmpty() && entryOutput != output && entryOutput != u"*")
            continue;
        if (!desktop.isEmpty() && entryDesktop != desktop && entryDesktop != u"*")
            continue;
        targets.append({entryOutput, entryDesktop});
    }
    return targets;
}

QStringList formatSteamCmdStatus(const QJsonObject &status)
{
    QStringList lines;
    const QString state = status.value(u"state"_s).toString();
    const QString user = status.value(u"user"_s).toString();
    const QString prompt = status.value(u"prompt"_s).toString();
    const QString message = status.value(u"message"_s).toString();
    QStringList head{state};
    if (!user.isEmpty())
        head << user;
    if (!prompt.isEmpty())
        head << u"waiting for "_s + prompt;
    else if (!message.isEmpty())
        head << message;
    lines << head.join(u'\t');
    for (const QJsonValue &value : status.value(u"jobs"_s).toArray()) {
        const QJsonObject job = value.toObject();
        lines << QStringList{job.value(u"itemId"_s).toString(), job.value(u"state"_s).toString(),
                             QString::number(qRound(job.value(u"progress"_s).toDouble() * 100)) + u'%',
                             job.value(u"error"_s).toString()}
                     .join(u'\t');
    }
    return lines;
}

QString steamCmdAppArg(const QString &arg)
{
    const QString lower = arg.toLower();
    if (lower.isEmpty() || lower == u"431960" || lower == u"wallpaper")
        return u"431960"_s;
    if (lower == u"3474900" || lower == u"pets" || lower == u"anima")
        return u"3474900"_s;
    return {};
}

namespace {

bool wholeNumber(const QString &text, int *value)
{
    bool ok = false;
    const int n = text.toInt(&ok);
    if (ok)
        *value = n;
    return ok;
}

// "a,b" or "axb" into two whole numbers.
bool pair(const QString &text, QChar separator, int *a, int *b)
{
    const QStringList parts = text.split(separator);
    return parts.size() == 2 && wholeNumber(parts.at(0), a) && wholeNumber(parts.at(1), b);
}

} // namespace

bool importSpecJson(const ImportArgs &args, QByteArray *json, QString *error)
{
    const auto fail = [error](const QString &message) {
        *error = message;
        return false;
    };
    if (args.kind != u"video"_s) {
        const struct { bool set; const char *option; } videoOnly[] = {{args.maxSide.has_value(), "--max-side"}, {args.maxFps.has_value(), "--max-fps"}, {args.durationSeconds.has_value(), "--duration"},
                                                                       {args.startSeconds.has_value(), "--start"}, {args.keepAudio, "--keep-audio"}};
        for (const auto &v : videoOnly)
            if (v.set)
                return fail(QString::fromLatin1(v.option) + u" only applies to a video"_s);
    }
    if ((args.kind == u"video"_s || args.kind == u"gif"_s || args.kind == u"mdaa"_s) && (args.fps || args.frameMs))
        return fail(u"--fps and --frame-ms do not apply to this kind (it has its own timing; for a video use --max-fps)"_s);
    QJsonObject spec{{u"kind"_s, args.kind}};
    QJsonArray sources;
    for (const QString &source : args.sources)
        sources.append(QDir::cleanPath(QFileInfo(source).absoluteFilePath()));
    spec.insert(u"sources"_s, sources);
    int a = 0, b = 0;
    if (args.frame) {
        if (!pair(*args.frame, QLatin1Char('x'), &a, &b))
            return fail(u"--frame needs WIDTHxHEIGHT, such as 64x64"_s);
        spec.insert(u"frameWidth"_s, a);
        spec.insert(u"frameHeight"_s, b);
    }
    if (args.padding) {
        if (!pair(*args.padding, QLatin1Char(','), &a, &b))
            return fail(u"--padding needs LEFT,TOP"_s);
        spec.insert(u"paddingLeft"_s, a);
        spec.insert(u"paddingTop"_s, b);
    }
    if (args.spacing) {
        if (!pair(*args.spacing, QLatin1Char(','), &a, &b))
            return fail(u"--spacing needs HORIZONTAL,VERTICAL"_s);
        spec.insert(u"spacingH"_s, a);
        spec.insert(u"spacingV"_s, b);
    }
    const struct {
        const std::optional<QString> &value;
        const char *option;
        const char *key;
    } whole[] = {{args.count, "--count", "frameCount"}, {args.start, "--start", "startFrame"}, {args.columns, "--columns", "columns"},
                 {args.rows, "--rows", "rows"}, {args.autoScene, "--auto-scene", "autoScene"}, {args.maxSide, "--max-side", "maxSide"},        {args.frameMs, "--frame-ms", "frameMs"}, {args.quality, "--quality", "webpQuality"}};
    for (const auto &w : whole) {
        if (!w.value)
            continue;
        if (!wholeNumber(*w.value, &a))
            return fail(QString::fromLatin1(w.option) + u" needs a whole number"_s);
        spec.insert(QString::fromLatin1(w.key), a);
    }
    if (args.fps) {
        double fps = 0;
        if (!parseNumberArg(*args.fps, &fps))
            return fail(u"--fps needs a number"_s);
        spec.insert(u"fps"_s, fps);
    }
    if (args.maxFps) {
        double fps = 0;
        if (!parseNumberArg(*args.maxFps, &fps))
            return fail(u"--max-fps needs a number"_s);
        spec.insert(u"maxFps"_s, fps);
    }
    for (const auto &[value, option, key] : {std::tuple{args.startSeconds, "--start", "startMs"}, std::tuple{args.durationSeconds, "--duration", "durationMs"}}) {
        if (!value)
            continue;
        double seconds = 0;
        if (!parseNumberArg(*value, &seconds) || seconds < 0 || seconds > 24.0 * 3600)
            return fail(QString::fromLatin1(option) + u" needs a number of seconds (0 to 86400)"_s);
        const double ms = std::round(seconds * 1000.0);
        if (seconds > 0 && ms < 1) // would be read as 0, which means "to the end"
            return fail(QString::fromLatin1(option) + u" is shorter than a millisecond"_s);
        spec.insert(QString::fromLatin1(key), ms);
    }
    if (args.keepAudio)
        spec.insert(u"keepAudio"_s, true);
    if (args.size) {
        if (*args.size != u"first"_s && *args.size != u"fit"_s && *args.size != u"scale"_s)
            return fail(u"--size must be first, fit or scale"_s);
        spec.insert(u"sizePolicy"_s, *args.size);
    }
    if (args.format) {
        if (*args.format != u"apng"_s && *args.format != u"webp"_s)
            return fail(u"--format must be apng or webp"_s);
        spec.insert(u"format"_s, *args.format);
    }
    if (args.keyColor) {
        QString hex = *args.keyColor;
        if (hex.startsWith(u'#'))
            hex.remove(0, 1);
        bool ok = false;
        if (hex.size() != 6)
            return fail(u"--key-color needs RRGGBB"_s);
        hex.toUInt(&ok, 16);
        if (!ok)
            return fail(u"--key-color needs RRGGBB"_s);
        QJsonObject key{{u"color"_s, hex}};
        for (const auto &[value, name] : {std::pair{args.tolerance, u"tolerance"_s}, std::pair{args.softness, u"softness"_s}}) {
            if (!value)
                continue;
            double number = 0;
            if (!parseNumberArg(*value, &number))
                return fail(u"--"_s + name + u" needs a number from 0 to 100"_s);
            key.insert(name, number);
        }
        spec.insert(u"colorKey"_s, key);
    } else if (args.tolerance || args.softness) {
        return fail(u"--tolerance and --softness need --key-color"_s);
    }
    if (args.name)
        spec.insert(u"name"_s, *args.name);
    if (args.keepSource)
        spec.insert(u"keepSource"_s, true);
    *json = QJsonDocument(spec).toJson(QJsonDocument::Compact);
    return true;
}

QString formatImportLine(const QJsonObject &job)
{
    const QString state = job.value(u"state"_s).toString();
    const QString last = state == u"done"_s ? job.value(u"itemId"_s).toString() : job.value(u"error"_s).toString();
    return job.value(u"id"_s).toString() + u'\t' + state + u'\t' + (job.value(u"progress"_s).toDouble() < 0 ? u"?"_s : QString::number(qRound(job.value(u"progress"_s).toDouble() * 100)) + u"%"_s) + u'\t'
        + job.value(u"name"_s).toString() + u'\t' + last;
}

} // namespace murasaki

namespace murasaki {

bool petAudioChanges(const PetAudioArgs &args, QJsonObject *changes, QString *error)
{
    QJsonObject audio;
    const auto whole = [&](const std::optional<QString> &text, const QString &key, const char *option) {
        if (!text)
            return true;
        bool ok = false;
        const int value = text->toInt(&ok);
        if (!ok || text->trimmed() != *text) {
            if (error)
                *error = QStringLiteral("--%1 needs a whole number").arg(QLatin1String(option));
            return false;
        }
        audio.insert(key, value);
        return true;
    };
    if (args.asset) {
        if (args.asset->size() != 64) {
            if (error)
                *error = QStringLiteral("--asset needs the 64 characters of a sound's id (murasakictl pets audio list)");
            return false;
        }
        audio.insert(QStringLiteral("asset"), *args.asset);
    }
    if (args.mode) {
        static const QStringList modes{QStringLiteral("none"), QStringLiteral("once"), QStringLiteral("animation_loop"), QStringLiteral("continuous"), QStringLiteral("click")};
        if (!modes.contains(*args.mode)) {
            if (error)
                *error = QStringLiteral("--mode must be none, once, animation_loop, continuous or click");
            return false;
        }
        audio.insert(QStringLiteral("mode"), *args.mode);
    }
    if (!whole(args.volume, QStringLiteral("volume"), "volume") || !whole(args.offset, QStringLiteral("offsetMs"), "offset"))
        return false;
    *changes = audio.isEmpty() ? QJsonObject() : QJsonObject{{QStringLiteral("audio"), audio}};
    return true;
}

QString formatPetAudioAssetLine(const QJsonObject &asset)
{
    return QStringList{asset.value(QStringLiteral("asset")).toString(), asset.value(QStringLiteral("name")).toString(),
                       QString::number(asset.value(QStringLiteral("durationMs")).toInt()), QString::number(asset.value(QStringLiteral("refs")).toInt())}
        .join(QLatin1Char('\t'));
}

// A name comes from a file or an import: control characters (a terminal escape among them) never reach the screen.
QString printable(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (const QChar c : text)
        out.append(c.unicode() < 0x20 || (c.unicode() >= 0x7f && c.unicode() < 0xa0) ? QChar(u'?') : c);
    return out;
}

QString resolveNamedArg(const QString &arg, const QJsonArray &entries, const QString &noun, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    for (const QJsonValue &value : entries)
        if (value.toObject().value(u"id"_s).toString() == arg)
            return arg;
    QStringList exact, folded;
    for (const QJsonValue &value : entries) {
        const QString name = value.toObject().value(u"name"_s).toString();
        const QString id = value.toObject().value(u"id"_s).toString();
        if (name == arg)
            exact.append(id);
        else if (name.compare(arg, Qt::CaseInsensitive) == 0)
            folded.append(id);
    }
    const QStringList &found = exact.isEmpty() ? folded : exact;
    if (found.size() == 1)
        return found.first();
    if (found.size() > 1)
        return fail(u"more than one %1 is called \"%2\"; use its id (%3)"_s.arg(noun, printable(arg), found.join(u", "_s)));
    return fail(u"no %1 matches \"%2\""_s.arg(noun, printable(arg)));
}

QString formatProfileLine(const QJsonObject &profile, bool active)
{
    return QStringList{profile.value(u"id"_s).toString(), (active ? u"* "_s : QString()) + printable(profile.value(u"name"_s).toString()),
                       QString::number(profile.value(u"pets"_s).toArray().size())}
        .join(u'\t');
}

QString formatTriggerLine(const QJsonObject &trigger)
{
    return QStringList{printable(trigger.value(u"id"_s).toString()), trigger.value(u"enabled"_s).toBool() ? u"on"_s : u"off"_s, printable(trigger.value(u"name"_s).toString()),
                       trigger.value(u"source"_s).toObject().value(u"kind"_s).toString(), trigger.value(u"action"_s).toObject().value(u"type"_s).toString()}
        .join(u'\t');
}

QString formatTimelineLine(const QJsonObject &timeline)
{
    return QStringList{timeline.value(u"id"_s).toString(), printable(timeline.value(u"name"_s).toString()), QString::number(timeline.value(u"tracks"_s).toArray().size()),
                       QString::number(timeline.value(u"speed"_s).toDouble(1))}
        .join(u'\t');
}

QString formatTriggerStateLine(const QJsonObject &state)
{
    QString outcome = state.value(u"outcome"_s).toString();
    if (outcome.isEmpty())
        outcome = u"-"_s;
    if (!state.value(u"available"_s).toBool(true))
        outcome += u" (source unavailable)"_s;
    return QStringList{printable(state.value(u"id"_s).toString()), state.value(u"enabled"_s).toBool() ? u"on"_s : u"off"_s, outcome}.join(u'\t');
}

bool triggerChangesFromArgs(const QString &name, const QString &sourceKind, const QString &actionType, const QStringList &words, QJsonObject *changes, QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    static const QStringList numeric{u"everyMinutes"_s, u"minutes"_s, u"index"_s, u"cooldownMs"_s};
    QJsonObject source{{u"kind"_s, sourceKind}}, action{{u"type"_s, actionType}}, top{{u"name"_s, name}};
    for (const QString &word : words) {
        const qsizetype eq = word.indexOf(u'=');
        if (eq <= 0)
            return fail(u"\"%1\" is not key=value"_s.arg(word.left(80)));
        const QString key = word.left(eq), value = word.mid(eq + 1);
        QJsonObject *target = &top;
        QString field = key;
        if (key.startsWith(u"source."_s)) {
            target = &source;
            field = key.mid(7);
        } else if (key.startsWith(u"action."_s)) {
            target = &action;
            field = key.mid(7);
        } else if (key != u"cooldownMs" && key != u"mode" && key != u"enabled") {
            return fail(u"\"%1\": use source.KEY=, action.KEY=, cooldownMs=, mode= or enabled="_s.arg(key.left(80)));
        }
        if (field.isEmpty())
            return fail(u"\"%1\" names no field"_s.arg(word.left(80)));
        if (field == u"enabled") {
            const QString v = value.toLower();
            if (v != u"on" && v != u"off" && v != u"true" && v != u"false")
                return fail(u"enabled is on or off"_s);
            target->insert(field, v == u"on" || v == u"true");
        } else if (numeric.contains(field)) {
            bool ok = false;
            const double number = value.toDouble(&ok);
            if (!ok || !std::isfinite(number))
                return fail(u"%1 is a number"_s.arg(field));
            target->insert(field, number);
        } else if (field == u"days") {
            QJsonArray days;
            for (const QString &part : value.split(u',', Qt::SkipEmptyParts)) {
                bool ok = false;
                const int day = part.trimmed().toInt(&ok);
                if (!ok)
                    return fail(u"days is a comma list of numbers, 0 (Sunday) to 6"_s);
                days.append(day);
            }
            target->insert(field, days);
        } else {
            target->insert(field, value);
        }
    }
    top.insert(u"source"_s, source);
    top.insert(u"action"_s, action);
    *changes = top;
    return true;
}

QString formatPuppetItemLine(const QJsonObject &item)
{
    if (item.value(u"format"_s).toString() != u"puppet"_s)
        return {};
    const QJsonObject puppet = item.value(u"puppet"_s).toObject();
    const QJsonArray canvas = puppet.value(u"canvas"_s).toArray();
    QString note;
    for (const char *key : {"reasons", "degraded"}) {
        const QJsonArray list = puppet.value(QLatin1String(key)).toArray();
        if (!list.isEmpty()) {
            note = list.at(0).toString();
            break;
        }
    }
    if (note.isEmpty())
        note = item.value(u"statusDetail"_s).toString();
    return QStringList{printable(item.value(u"id"_s).toString()), printable(item.value(u"status"_s).toString()), printable(puppet.value(u"backend"_s).toString()), printable(item.value(u"title"_s).toString()),
                       puppet.value(u"checked"_s).toBool() ? u"checked"_s : u"unchecked"_s,
                       canvas.size() == 2 ? u"%1x%2"_s.arg(canvas.at(0).toInt()).arg(canvas.at(1).toInt()) : u"-"_s,
                       u"groups:%1"_s.arg(puppet.value(u"motionGroups"_s).toArray().size()), u"expressions:%1"_s.arg(puppet.value(u"expressions"_s).toArray().size()), printable(note)}
        .join(u'\t');
}

QString formatPuppetWindowLine(const QJsonObject &window)
{
    if (!window.contains(u"puppet"_s))
        return {};
    const QJsonObject puppet = window.value(u"puppet"_s).toObject();
    QStringList degraded;
    for (const QJsonValue &name : window.value(u"degraded"_s).toArray())
        degraded << printable(name.toString());
    return QStringList{printable(window.value(u"petId"_s).toString()), printable(puppet.value(u"state"_s).toString()), u"%1fps"_s.arg(puppet.value(u"fps"_s).toInt()),
                       u"crashes:%1"_s.arg(puppet.value(u"crashes"_s).toInt()), u"group:"_s + printable(puppet.value(u"group"_s).toString()),
                       u"expression:"_s + printable(puppet.value(u"expression"_s).toString()), puppet.value(u"look"_s).toBool() ? u"look"_s : u"-"_s, degraded.join(u", "_s)}
        .join(u'\t');
}

} // namespace murasaki
