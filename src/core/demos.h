#pragma once

// The demonstrations ("Try it", docs/pets/automation.md, "Examples"): small working examples of the timelines, triggers, profiles and Mouse Tracking, made out of the
// user's own pets. Pure: the GUI (DaemonClient) and murasakictl both build the same JSON here and send it with the calls that exist. Everything a demo makes carries
// origin "demo"; that marker, never the name, is what "Remove examples" matches.

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSize>
#include <QString>

#include <optional>

namespace murasaki {

// The origin of every profile, trigger and timeline a demo makes.
inline constexpr QLatin1StringView kDemoOrigin{"demo"};
// The id of the example shortcut trigger (the desktop asks the user for the key).
inline constexpr QLatin1StringView kDemoShortcutId{"murasaki-demo"};
// How long the tracking example follows the cursor.
inline constexpr int kDemoTrackingMs = 20000;
// The screen the example is laid out in when the caller does not know it.
inline constexpr QSize kDemoDefaultScreen{1280, 720};

// The names of what a demo makes: the GUI passes translated ones ("Ejemplo: ..."), the CLI these.
struct DemoNames {
    QString timeline;
    QString trigger;
    QString profile;
    QString tracking;
    QString screen;

    static DemoNames english();
};

// origin == "demo".
bool isDemo(const QJsonObject &object);
// The id of the demo with that name in a List<noun> answer, or empty (a user's entry of the same name is never found).
QString demoIdByName(const QJsonArray &list, const QString &name);
// A name for a demo that no entry of the user has: `name`, or `name (2)`, `name (3)`... when the user made one called the same (a demo never replaces or adopts it).
QString demoFreeName(const QJsonArray &list, const QString &name);
// The ids of every demo in a List<noun> answer.
QStringList demoIds(const QJsonArray &list);

// The timeline "pets across the screen" for the pets of a ListPets answer: two or more pets (one crosses the screen with a smooth move, the other waits hidden and
// appears in a corner), one pet (it crosses, hides and comes back in the corner). Laid out inside `screen`. nullopt without pets. The tracks name their pet by id.
std::optional<QJsonObject> demoTimeline(const QJsonArray &pets, const QSize &screen, const QString &name);
// The pet of the tracking example: the first that is neither a stage nor locked (those can not walk), else the first that is not a stage, else the first; empty without pets.
QJsonObject demoTrackingPet(const QJsonArray &pets);
// What the pet can do in the example: "follow", or "look" for a pet that is a stage or locked (it can only turn to the cursor).
QString demoTrackingMode(const QJsonObject &pet);
// The timeline that makes `pet` follow (or look at) the cursor for kDemoTrackingMs and then undoes it (the runtime layer: nothing is stored).
QJsonObject demoTrackingTimeline(const QJsonObject &pet, const QString &name);
// The shortcut trigger that plays a timeline.
QJsonObject demoTrigger(const QString &timelineId, const QString &name);
// The screen trigger "hide the pets while a picture is on the screen" (docs/pets/automation.md, "Screen triggers"). It ships OFF, with one placeholder region and NO reference
// picture: what it looks for is whatever the user captures (nothing of the screen is ever bundled or guessed), and it needs the user to start watching and to answer the
// desktop's question. Its action can be undone, so the pets come back when the picture goes.
QJsonObject demoScreenTrigger(const QString &name);

} // namespace murasaki
