#pragma once

// What an imported .mdaa package becomes in the automation (docs/pets/automation.md, "Packs"): the scenes of its action table as one-step timelines, and the
// tracks of a Triggers pack as a timeline with a trigger that fires it. Pure: the record of the import (source.json, "mdaa") in, timelines and triggers out; the daemon
// stores them. Everything made is disabled and says where it came from.

#include "timeline.h"
#include "trigger.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace murasaki {

struct PackAutomation {
    QList<Timeline> timelines;
    QList<Trigger> triggers; // action.id is empty: the caller sets the id of the stored timeline (the first one) before storing them
    QStringList notes;       // what could not be carried over, and why
};

// The scene `index` of the package as a one-step timeline: a show (and a place when the scene moves the pet), or a hide. It does not restore (it would undo itself at once).
// Empty `error` and a valid timeline, or `error` set.
std::optional<Timeline> timelineFromScene(const QJsonObject &scene, int index, const QString &itemId, const QString &packageName, QString *error = nullptr);

// The tracks of a Triggers pack's `pack` object (src/import/triggerspack.h) as one timeline of the pack's name, plus two disabled triggers: a `fire` trigger named after the
// pack, and the `screen` trigger that keeps the regions and the match settings and never fires. Anima's times are absolute (`t_ms`) and its tracks loop while the trigger is
// active; the timeline plays once, with waits between the actions. Positions are the centre of the animation there: the actions carry `anchor: centre` and the daemon resolves them with the pet's real size. The `fire` name is the pack's name plus a short hash of the item, so that two packages never share one.
// A sound action is dropped (its path is the author's machine's), a track with no imported animation is dropped, each animation takes two tracks (what is shown and when,
// and where it moves) so that a long move does not hold back a show or a hide.
PackAutomation automationFromPack(const QJsonObject &pack, const QString &itemId, const QString &packageName);

// The origin text of everything an import of that package makes, and the key of its replacement: title and item.
QString packageOrigin(const QString &packageName, const QString &itemId);

// A name made of letters, digits, dot, underscore and dash, at most 64 characters, from any text (the name of a pack as a `fire` name).
QString fireNameFrom(const QString &text);

} // namespace murasaki
