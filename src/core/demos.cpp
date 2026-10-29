#include "demos.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr double kMargin = 40;
constexpr double kDefaultWidth = 200;
constexpr int kCrossMs = 6000;

QJsonObject action(const QString &kind, std::initializer_list<std::pair<QString, QJsonValue>> fields = {})
{
    QJsonObject o{{u"kind"_s, kind}};
    for (const auto &[key, value] : fields)
        o.insert(key, value);
    return o;
}

QJsonObject place(double x, double y)
{
    return action(u"place"_s, {{u"x"_s, x}, {u"y"_s, y}});
}

QJsonObject wait(int ms)
{
    return action(u"wait"_s, {{u"ms"_s, ms}});
}

// A pet's width on the screen, kept sane (the height is not known here: the width stands for it in the margins).
double widthOf(const QJsonObject &pet, const QSize &screen)
{
    const double w = pet.value(u"width"_s).toDouble(kDefaultWidth);
    return std::clamp(w, 16.0, std::max(16.0, double(screen.width()) - 2 * kMargin));
}

double clampTo(double value, double low, double high)
{
    return std::clamp(value, low, std::max(low, high));
}

// Where the pet goes in the bottom right corner.
QJsonObject corner(const QJsonObject &pet, const QSize &screen)
{
    const double w = widthOf(pet, screen);
    return place(clampTo(screen.width() - w - kMargin, 0, screen.width()), clampTo(screen.height() - w - 2 * kMargin, 0, screen.height()));
}

QJsonObject track(const QString &petId, bool startVisible, const QJsonArray &actions)
{
    return QJsonObject{{u"pet"_s, QJsonObject{{u"petId"_s, petId}}}, {u"startVisible"_s, startVisible}, {u"actions"_s, actions}};
}

QJsonObject timeline(const QString &name, const QJsonArray &tracks)
{
    return QJsonObject{{u"name"_s, name}, {u"speed"_s, 1}, {u"restore"_s, true}, {u"origin"_s, QString(kDemoOrigin)}, {u"tracks"_s, tracks}};
}

} // namespace

DemoNames DemoNames::english()
{
    return {u"Example: pets across the screen"_s, u"Example: a key that plays the timeline"_s, u"Example: my desktop"_s, u"Example: follow the cursor"_s,
            u"Example: hide the pets while a picture is on the screen"_s};
}

bool isDemo(const QJsonObject &object)
{
    return object.value(u"origin"_s).toString() == kDemoOrigin;
}

QString demoIdByName(const QJsonArray &list, const QString &name)
{
    for (const QJsonValue &value : list) {
        const QJsonObject o = value.toObject();
        if (isDemo(o) && o.value(u"name"_s).toString() == name)
            return o.value(u"id"_s).toString();
    }
    return {};
}

QString demoFreeName(const QJsonArray &list, const QString &name)
{
    const auto taken = [&list](const QString &candidate) {
        for (const QJsonValue &value : list) {
            const QJsonObject o = value.toObject();
            if (!isDemo(o) && o.value(u"name"_s).toString().compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    if (!taken(name))
        return name;
    for (int n = 2; n < 100; ++n)
        if (const QString candidate = u"%1 (%2)"_s.arg(name).arg(n); !taken(candidate))
            return candidate;
    return name;
}

QStringList demoIds(const QJsonArray &list)
{
    QStringList ids;
    for (const QJsonValue &value : list) {
        const QJsonObject o = value.toObject();
        if (isDemo(o) && !o.value(u"id"_s).toString().isEmpty())
            ids.append(o.value(u"id"_s).toString());
    }
    return ids;
}

std::optional<QJsonObject> demoTimeline(const QJsonArray &pets, const QSize &screenSize, const QString &name)
{
    if (pets.isEmpty())
        return std::nullopt;
    const QSize screen = screenSize.isValid() ? screenSize : kDemoDefaultScreen;
    const QJsonObject first = pets.at(0).toObject();
    const double w = widthOf(first, screen);
    const double y = clampTo(first.value(u"y"_s).toDouble(kMargin), 0, screen.height() - w);
    const double right = clampTo(screen.width() - w - kMargin, kMargin, screen.width());

    QJsonArray crossing{place(kMargin, y), action(u"move"_s, {{u"x"_s, right}, {u"y"_s, y}, {u"mode"_s, u"smooth"_s}, {u"durationMs"_s, kCrossMs}}), wait(600)};
    if (pets.size() == 1) {
        // The same pet takes both parts: it crosses, hides and comes back in the corner.
        crossing.append(action(u"hide"_s));
        crossing.append(wait(800));
        crossing.append(corner(first, screen));
        crossing.append(action(u"show"_s));
        crossing.append(wait(2500));
        return timeline(name, {track(first.value(u"petId"_s).toString(), true, crossing)});
    }
    const QJsonObject second = pets.at(1).toObject();
    const QJsonArray appearing{wait(1500), corner(second, screen), action(u"show"_s), wait(3500), action(u"hide"_s)};
    return timeline(name, {track(first.value(u"petId"_s).toString(), true, crossing), track(second.value(u"petId"_s).toString(), false, appearing)});
}

namespace {

bool isStage(const QJsonObject &pet)
{
    return pet.value(u"stage"_s).toObject().value(u"mode"_s).toString(u"off"_s) != u"off";
}

} // namespace

QString demoTrackingMode(const QJsonObject &pet)
{
    return isStage(pet) || pet.value(u"locked"_s).toBool() ? u"look"_s : u"follow"_s;
}

QJsonObject demoTrackingPet(const QJsonArray &pets)
{
    for (const QJsonValue &value : pets)
        if (demoTrackingMode(value.toObject()) == u"follow")
            return value.toObject();
    for (const QJsonValue &value : pets)
        if (!isStage(value.toObject()))
            return value.toObject();
    return pets.isEmpty() ? QJsonObject() : pets.at(0).toObject();
}

QJsonObject demoTrackingTimeline(const QJsonObject &pet, const QString &name)
{
    const QJsonArray actions{action(u"track"_s, {{u"mode"_s, demoTrackingMode(pet)}}), wait(kDemoTrackingMs)};
    return timeline(name, {track(pet.value(u"petId"_s).toString(), true, actions)});
}

QJsonObject demoTrigger(const QString &timelineId, const QString &name)
{
    return QJsonObject{{u"name"_s, name},
                       {u"enabled"_s, true},
                       {u"origin"_s, QString(kDemoOrigin)},
                       {u"source"_s, QJsonObject{{u"kind"_s, u"shortcut"_s}, {u"id"_s, QString(kDemoShortcutId)}, {u"description"_s, name}}},
                       {u"action"_s, QJsonObject{{u"type"_s, u"timeline"_s}, {u"id"_s, timelineId}}}};
}

QJsonObject demoScreenTrigger(const QString &name)
{
    // The match settings are the engine's own defaults, written out so that the editor shows them.
    const QJsonObject match{{u"mode"_s, u"hash"_s},
                            {u"agg"_s, u"any"_s},
                            {u"threshold"_s, 0.65},
                            {u"cooldownMs"_s, 500},
                            {u"cooldownFrom"_s, u"activate"_s},
                            {u"startFramesToCheck"_s, 3},
                            {u"stopGraceMs"_s, 250},
                            {u"stopOnMismatch"_s, true},
                            {u"refMode"_s, u"anchor"_s}};
    const QJsonObject region{{u"kind"_s, u"rect"_s}, {u"x"_s, 100}, {u"y"_s, 100}, {u"w"_s, 320}, {u"h"_s, 180}};
    return QJsonObject{{u"name"_s, name},
                       {u"enabled"_s, false},
                       {u"origin"_s, QString(kDemoOrigin)},
                       {u"source"_s, QJsonObject{{u"kind"_s, u"screen"_s}, {u"data"_s, QJsonObject{{u"regions"_s, QJsonArray{region}}, {u"match"_s, match}}}}},
                       {u"action"_s, QJsonObject{{u"type"_s, u"hide-all"_s}}}};
}

} // namespace murasaki
