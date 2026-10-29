#include "trigger.h"

#include "appclass.h"
#include "automation.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

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

constexpr QLatin1String kKindNames[] = {QLatin1String("app-focus"), QLatin1String("app-open"), QLatin1String("fullscreen"), QLatin1String("time"),
                                        QLatin1String("interval"), QLatin1String("idle"),     QLatin1String("power"),      QLatin1String("session"),
                                        QLatin1String("game"),     QLatin1String("output"),   QLatin1String("shortcut"),   QLatin1String("signal-file"),
                                        QLatin1String("fire"),     QLatin1String("dbus"),     QLatin1String("screen")};
constexpr QLatin1String kActionNames[] = {QLatin1String("timeline"),     QLatin1String("scene"),     QLatin1String("profile"),
                                          QLatin1String("hide-all"),     QLatin1String("show-all"),  QLatin1String("toggle-hidden"),
                                          QLatin1String("stop-sounds"),  QLatin1String("mute"),      QLatin1String("play-sound")};

constexpr int kMaxScreenBytes = 64 * 1024;

bool isNamedKind(TriggerKind k)
{
    return k == TriggerKind::AppFocus || k == TriggerKind::AppOpen || k == TriggerKind::Fullscreen;
}

// "enter"/"open" and "leave"/"close" are the same two choices said the way each kind reads.
QString edgeName(TriggerKind kind, bool leave)
{
    if (kind == TriggerKind::AppOpen)
        return leave ? u"close"_s : u"open"_s;
    return leave ? u"leave"_s : u"enter"_s;
}

bool validStateFor(TriggerKind kind, const QString &state)
{
    switch (kind) {
    case TriggerKind::Power:
        return state == u"battery" || state == u"ac";
    case TriggerKind::Session:
        return state == u"locked" || state == u"unlocked";
    case TriggerKind::Game:
        return state == u"started" || state == u"ended";
    case TriggerKind::Output:
        return state == u"added" || state == u"removed";
    case TriggerKind::Idle:
        return state.isEmpty() || state == u"away" || state == u"back";
    default:
        return true;
    }
}

bool validTime(const QString &at)
{
    static const QRegularExpression re(u"^([01]\\d|2[0-3]):[0-5]\\d$"_s);
    return re.match(at).hasMatch();
}

bool dottedName(const QString &s)
{
    static const QRegularExpression re(u"^[A-Za-z_][A-Za-z0-9_]*(\\.[A-Za-z_][A-Za-z0-9_]*)*$"_s);
    return s.size() <= kMaxTriggerFields && re.match(s).hasMatch();
}

} // namespace

QString triggerKindName(TriggerKind kind)
{
    return QString(kKindNames[int(kind)]);
}

std::optional<TriggerKind> triggerKindFromName(const QString &name)
{
    for (int i = 0; i < int(std::size(kKindNames)); ++i)
        if (name == kKindNames[i])
            return TriggerKind(i);
    return std::nullopt;
}

QList<TriggerKind> allTriggerKinds()
{
    QList<TriggerKind> all;
    for (int i = 0; i < int(std::size(kKindNames)); ++i)
        all.append(TriggerKind(i));
    return all;
}

QString triggerActionName(TriggerActionType type)
{
    return QString(kActionNames[int(type)]);
}

std::optional<TriggerActionType> triggerActionFromName(const QString &name)
{
    for (int i = 0; i < int(std::size(kActionNames)); ++i)
        if (name == kActionNames[i])
            return TriggerActionType(i);
    return std::nullopt;
}

QList<TriggerActionType> allTriggerActionTypes()
{
    QList<TriggerActionType> all;
    for (int i = 0; i < int(std::size(kActionNames)); ++i)
        all.append(TriggerActionType(i));
    return all;
}

QJsonObject TriggerSource::toJson() const
{
    QJsonObject o{{u"kind"_s, triggerKindName(kind)}};
    switch (kind) {
    case TriggerKind::AppFocus:
    case TriggerKind::AppOpen:
    case TriggerKind::Fullscreen:
        if (!app.isEmpty())
            o.insert(u"app"_s, app);
        if (kind == TriggerKind::Fullscreen && !output.isEmpty())
            o.insert(u"output"_s, output);
        o.insert(u"on"_s, edgeName(kind, onLeave));
        break;
    case TriggerKind::Time: {
        o.insert(u"at"_s, at);
        QJsonArray d;
        for (const int day : days)
            d.append(day);
        o.insert(u"days"_s, d);
        break;
    }
    case TriggerKind::Interval:
        o.insert(u"everyMinutes"_s, minutes);
        break;
    case TriggerKind::Idle:
        o.insert(u"minutes"_s, minutes);
        if (state == u"back")
            o.insert(u"state"_s, state); // (away is the default and is not written)
        break;
    case TriggerKind::Power:
    case TriggerKind::Session:
    case TriggerKind::Game:
        o.insert(u"state"_s, state);
        break;
    case TriggerKind::Output:
        o.insert(u"state"_s, state);
        if (!name.isEmpty())
            o.insert(u"name"_s, name);
        break;
    case TriggerKind::Shortcut:
        o.insert(u"id"_s, id);
        if (!description.isEmpty())
            o.insert(u"description"_s, description);
        break;
    case TriggerKind::SignalFile:
    case TriggerKind::Fire:
        o.insert(u"name"_s, name);
        break;
    case TriggerKind::DBus:
        o.insert(u"interface"_s, interface);
        o.insert(u"member"_s, member);
        if (!senderNamespace.isEmpty())
            o.insert(u"senderNamespace"_s, senderNamespace);
        break;
    case TriggerKind::Screen:
        o.insert(u"data"_s, screen);
        break;
    }
    return o;
}

QJsonObject TriggerAction::toJson() const
{
    QJsonObject o{{u"type"_s, triggerActionName(type)}};
    switch (type) {
    case TriggerActionType::Timeline:
    case TriggerActionType::Profile:
        o.insert(u"id"_s, id);
        break;
    case TriggerActionType::Scene:
        o.insert(u"index"_s, index);
        if (!petId.isEmpty())
            o.insert(u"petId"_s, petId);
        if (!item.isEmpty())
            o.insert(u"item"_s, item);
        break;
    case TriggerActionType::PlaySound:
        if (!petId.isEmpty())
            o.insert(u"petId"_s, petId);
        if (!item.isEmpty())
            o.insert(u"item"_s, item);
        break;
    case TriggerActionType::Mute:
        o.insert(u"mode"_s, mute);
        break;
    default:
        break;
    }
    return o;
}

QJsonObject Trigger::toJson() const
{
    QJsonObject o{{u"id"_s, id},
                  {u"name"_s, name},
                  {u"enabled"_s, enabled},
                  {u"cooldownMs"_s, cooldownMs},
                  {u"mode"_s, hold ? u"hold"_s : u"press"_s},
                  {u"source"_s, source.toJson()},
                  {u"action"_s, action.toJson()}};
    if (!origin.isEmpty())
        o.insert(u"origin"_s, origin);
    return o;
}

bool validateName(const QString &name, QString *error)
{
    static const QRegularExpression re(u"^[A-Za-z0-9._-]{1,64}$"_s);
    if (!re.match(name).hasMatch() || name == u"." || name == u"..")
        return fail(error, u"a name is 1-64 characters of letters, digits, dot, underscore and dash"_s);
    return true;
}

bool matchesApplication(const QString &pattern, const QString &text)
{
    if (pattern.isEmpty() || text.isEmpty())
        return false;
    if (sameAppClass(pattern, text))
        return true;
    // Linear in the two lengths: one position to go back to at the last `*`, so no pattern can make it explode.
    const QString p = pattern.toCaseFolded(), t = text.toCaseFolded();
    qsizetype pi = 0, ti = 0, star = -1, mark = 0;
    while (ti < t.size()) {
        if (pi < p.size() && (p[pi] == u'?' || p[pi] == t[ti])) {
            ++pi;
            ++ti;
        } else if (pi < p.size() && p[pi] == u'*') {
            star = pi++;
            mark = ti;
        } else if (star >= 0) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == u'*')
        ++pi;
    return pi == p.size();
}

std::optional<TriggerAction> inverseAction(const TriggerAction &a)
{
    TriggerAction inverse = a;
    switch (a.type) {
    case TriggerActionType::HideAll:
        inverse.type = TriggerActionType::ShowAll;
        return inverse;
    case TriggerActionType::ShowAll:
        inverse.type = TriggerActionType::HideAll;
        return inverse;
    case TriggerActionType::ToggleHidden:
        return inverse;
    case TriggerActionType::Mute:
        inverse.mute = a.mute == u"on" ? u"off"_s : a.mute == u"off" ? u"on"_s : u"toggle"_s;
        return inverse;
    case TriggerActionType::Timeline: // stopping a timeline undoes it when it restores; the manager does that, the action stays the same
        return inverse;
    default:
        return std::nullopt;
    }
}

bool validateTrigger(const Trigger &t, QString *error)
{
    if (t.name.trimmed().isEmpty() || t.name.size() > kMaxAutomationNameLength)
        return fail(error, u"name must be 1-%1 characters"_s.arg(kMaxAutomationNameLength));
    if (t.cooldownMs < kMinTriggerCooldownMs || t.cooldownMs > kMaxTriggerCooldownMs)
        return fail(error, u"cooldownMs must be between %1 and %2"_s.arg(kMinTriggerCooldownMs).arg(kMaxTriggerCooldownMs));
    if (t.origin.size() > 200)
        return fail(error, u"origin is at most 200 characters"_s);
    const TriggerSource &s = t.source;
    if (s.app.size() > kMaxTriggerFields || s.output.size() > kMaxTriggerFields || s.description.size() > 200)
        return fail(error, u"app, output and description are too long"_s);
    switch (s.kind) {
    case TriggerKind::AppFocus:
    case TriggerKind::AppOpen:
        if (s.app.isEmpty())
            return fail(error, u"app must name an application class (* and ? match)"_s);
        break;
    case TriggerKind::Fullscreen:
        break; // both optional
    case TriggerKind::Time:
        if (!validTime(s.at))
            return fail(error, u"at must be a time of day, HH:MM"_s);
        if (s.days.size() > 7)
            return fail(error, u"days has at most 7 entries"_s);
        for (const int d : s.days)
            if (d < 0 || d > 6)
                return fail(error, u"days are 0 (Sunday) to 6 (Saturday)"_s);
        break;
    case TriggerKind::Interval:
    case TriggerKind::Idle:
        if (s.minutes < 1 || s.minutes > 1440)
            return fail(error, u"%1 must be between 1 and 1440"_s.arg(s.kind == TriggerKind::Idle ? u"minutes"_s : u"everyMinutes"_s));
        if (s.kind == TriggerKind::Idle && !validStateFor(s.kind, s.state))
            return fail(error, u"state of an idle trigger is away or back"_s);
        break;
    case TriggerKind::Power:
    case TriggerKind::Session:
    case TriggerKind::Game:
    case TriggerKind::Output:
        if (s.state.isEmpty() || !validStateFor(s.kind, s.state))
            return fail(error, u"state is not one of the values of a %1 trigger"_s.arg(triggerKindName(s.kind)));
        if (s.kind == TriggerKind::Output && s.name.size() > kMaxTriggerFields)
            return fail(error, u"name is too long"_s);
        break;
    case TriggerKind::Shortcut:
        if (!validateName(s.id, nullptr))
            return fail(error, u"id is 1-64 characters of letters, digits, dot, underscore and dash"_s);
        break;
    case TriggerKind::SignalFile:
    case TriggerKind::Fire:
        if (!validateName(s.name, nullptr))
            return fail(error, u"name is 1-64 characters of letters, digits, dot, underscore and dash"_s);
        break;
    case TriggerKind::DBus:
        if (!dottedName(s.interface) || s.interface.isEmpty() || !dottedName(s.member) || s.member.isEmpty())
            return fail(error, u"interface and member must be D-Bus names"_s);
        if (!s.senderNamespace.isEmpty() && !dottedName(s.senderNamespace))
            return fail(error, u"senderNamespace must be a D-Bus name prefix"_s);
        break;
    case TriggerKind::Screen:
        if (QJsonDocument(s.screen).toJson(QJsonDocument::Compact).size() > kMaxScreenBytes)
            return fail(error, u"a screen trigger is kept up to 64 KiB"_s);
        break;
    }
    if (s.kind != TriggerKind::Shortcut && t.hold)
        return fail(error, u"mode hold is only for shortcut triggers"_s);
    const TriggerAction &a = t.action;
    switch (a.type) {
    case TriggerActionType::Timeline:
    case TriggerActionType::Profile:
        if (a.id.isEmpty() || a.id.size() > 200)
            return fail(error, u"action.id must name a %1"_s.arg(triggerActionName(a.type)));
        break;
    case TriggerActionType::Scene:
        if (a.index < 0 || a.index > 100000)
            return fail(error, u"action.index must be a scene number"_s);
        if (a.petId.isEmpty() == a.item.isEmpty())
            return fail(error, u"a scene action names the pets by petId or by item, one of them"_s);
        break;
    case TriggerActionType::PlaySound:
        if (a.petId.isEmpty() == a.item.isEmpty())
            return fail(error, u"play-sound names the pets by petId or by item, one of them"_s);
        break;
    case TriggerActionType::Mute:
        if (a.mute != u"on" && a.mute != u"off" && a.mute != u"toggle")
            return fail(error, u"mute must be on, off or toggle"_s);
        break;
    default:
        break;
    }
    if (a.petId.size() > 200 || a.item.size() > 200)
        return fail(error, u"petId and item are at most 200 characters"_s);
    if (t.hold && !inverseAction(a))
        return fail(error, u"mode hold needs an action that can be undone (hide-all, show-all, toggle-hidden, mute, timeline)"_s);
    return true;
}

namespace {

std::optional<TriggerSource> parseSource(const QJsonValue &value, QString *error)
{
    const auto bad = [&](const QString &message) -> std::optional<TriggerSource> {
        fail(error, message);
        return std::nullopt;
    };
    if (!value.isObject())
        return bad(u"source must be an object"_s);
    const QJsonObject o = value.toObject();
    TriggerSource s;
    const auto kind = o.value(u"kind"_s).isString() ? triggerKindFromName(o.value(u"kind"_s).toString()) : std::nullopt;
    if (!kind)
        return bad(u"source.kind is not a kind of trigger"_s);
    s.kind = *kind;
    const auto text = [&](const QString &key, QString *target) {
        const auto it = o.constFind(key);
        if (it == o.constEnd())
            return true;
        if (!it->isString())
            return false;
        *target = it->toString();
        return true;
    };
    const auto whole = [&](const QString &key, int *target) {
        const auto it = o.constFind(key);
        if (it == o.constEnd())
            return true;
        if (!it->isDouble() || it->toDouble() != std::floor(it->toDouble()) || std::abs(it->toDouble()) > 1e9)
            return false;
        *target = int(it->toDouble());
        return true;
    };
    if (!text(u"app"_s, &s.app) || !text(u"output"_s, &s.output) || !text(u"at"_s, &s.at) || !text(u"state"_s, &s.state) || !text(u"name"_s, &s.name)
        || !text(u"id"_s, &s.id) || !text(u"description"_s, &s.description) || !text(u"interface"_s, &s.interface) || !text(u"member"_s, &s.member)
        || !text(u"senderNamespace"_s, &s.senderNamespace))
        return bad(u"a text field of source is not a string"_s);
    int every = 0, minutes = 0;
    if (!whole(u"everyMinutes"_s, &every) || !whole(u"minutes"_s, &minutes))
        return bad(u"everyMinutes and minutes must be whole numbers"_s);
    s.minutes = s.kind == TriggerKind::Interval ? every : minutes;
    if (s.kind == TriggerKind::Idle && s.state == u"away")
        s.state.clear(); // the default
    if (const auto it = o.constFind(u"on"_s); it != o.constEnd()) {
        const QString on = it->toString();
        if (on == u"enter" || on == u"open")
            s.onLeave = false;
        else if (on == u"leave" || on == u"close")
            s.onLeave = true;
        else
            return bad(u"on must be enter or leave (open or close for app-open)"_s);
        if (!isNamedKind(s.kind))
            return bad(u"on is for app-focus, app-open and fullscreen"_s);
    }
    if (const auto it = o.constFind(u"days"_s); it != o.constEnd()) {
        if (!it->isArray() || it->toArray().size() > 7)
            return bad(u"days must be an array of at most 7 numbers"_s);
        for (const QJsonValue &d : it->toArray()) {
            if (!d.isDouble() || d.toDouble() != std::floor(d.toDouble()) || d.toDouble() < 0 || d.toDouble() > 6)
                return bad(u"days are 0 (Sunday) to 6 (Saturday)"_s);
            if (!s.days.contains(int(d.toDouble())))
                s.days.append(int(d.toDouble()));
        }
        std::sort(s.days.begin(), s.days.end());
    }
    if (s.kind == TriggerKind::Screen) {
        if (const auto it = o.constFind(u"data"_s); it != o.constEnd()) {
            if (!it->isObject())
                return bad(u"source.data must be an object"_s);
            s.screen = it->toObject();
        }
    }
    return s;
}

std::optional<TriggerAction> parseAction(const QJsonValue &value, QString *error)
{
    const auto bad = [&](const QString &message) -> std::optional<TriggerAction> {
        fail(error, message);
        return std::nullopt;
    };
    if (!value.isObject())
        return bad(u"action must be an object"_s);
    const QJsonObject o = value.toObject();
    TriggerAction a;
    const auto type = o.value(u"type"_s).isString() ? triggerActionFromName(o.value(u"type"_s).toString()) : std::nullopt;
    if (!type)
        return bad(u"action.type is not one of timeline, scene, profile, hide-all, show-all, toggle-hidden, stop-sounds, mute, play-sound"_s);
    a.type = *type;
    for (const auto &[key, target] : {std::pair<QString, QString *>{u"id"_s, &a.id}, {u"item"_s, &a.item}, {u"petId"_s, &a.petId}, {u"mode"_s, &a.mute}}) {
        const auto it = o.constFind(key);
        if (it == o.constEnd())
            continue;
        if (!it->isString())
            return bad(u"action.%1 must be a string"_s.arg(key));
        *target = it->toString();
    }
    if (const auto it = o.constFind(u"index"_s); it != o.constEnd()) {
        if (!it->isDouble() || it->toDouble() != std::floor(it->toDouble()) || it->toDouble() < 0 || it->toDouble() > 100000)
            return bad(u"action.index must be a whole number"_s);
        a.index = int(it->toDouble());
    }
    if (a.type != TriggerActionType::Mute)
        a.mute.clear();
    return a;
}

} // namespace

std::optional<Trigger> applyTriggerChanges(const Trigger &base, const QJsonObject &changes, QString *error)
{
    Trigger t = base;
    const auto bad = [&](const QString &message) -> std::optional<Trigger> {
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
    if (const auto it = changes.constFind(u"enabled"_s); it != changes.constEnd()) {
        if (!it->isBool())
            return bad(u"enabled must be a boolean"_s);
        t.enabled = it->toBool();
    }
    if (const auto it = changes.constFind(u"cooldownMs"_s); it != changes.constEnd()) {
        if (!it->isDouble() || it->toDouble() != std::floor(it->toDouble()) || it->toDouble() < kMinTriggerCooldownMs || it->toDouble() > kMaxTriggerCooldownMs)
            return bad(u"cooldownMs must be a whole number between %1 and %2"_s.arg(kMinTriggerCooldownMs).arg(kMaxTriggerCooldownMs));
        t.cooldownMs = int(it->toDouble());
    }
    if (const auto it = changes.constFind(u"mode"_s); it != changes.constEnd()) {
        const QString mode = it->toString();
        if (mode != u"press" && mode != u"hold")
            return bad(u"mode must be press or hold"_s);
        t.hold = mode == u"hold";
    }
    QString problem;
    if (const auto it = changes.constFind(u"source"_s); it != changes.constEnd()) {
        const auto source = parseSource(*it, &problem);
        if (!source)
            return bad(problem);
        t.source = *source;
    }
    if (const auto it = changes.constFind(u"action"_s); it != changes.constEnd()) {
        const auto action = parseAction(*it, &problem);
        if (!action)
            return bad(problem);
        t.action = *action;
    }
    if (!validateTrigger(t, error))
        return std::nullopt;
    return t;
}

} // namespace murasaki
