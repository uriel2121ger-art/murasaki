#pragma once

#include "automation.h"

#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

namespace murasaki {

// The settings of the automation (docs/pets/automation.md), in the daemon's settings.json: one master switch, whether `triggers fire NAME` also runs a timeline of
// that name when no trigger has the name, the cooldown a new trigger starts with, and which first-visit cards of the examples were closed.
struct AutomationSettings {
    bool enabled = true;               // automationEnabled: off, nothing fires and nothing is subscribed
    bool fireByTimelineName = false;   // fireByTimelineName
    int triggerCooldownMs = kDefaultTriggerCooldownMs; // triggerCooldownMs: kMinTriggerCooldownMs..kMaxTriggerCooldownMs
    // Screen triggers (revision 21, docs/pets/automation.md "Screen triggers"): off until the user turns them on, and even then watching starts only when the user presses Start.
    bool screenTriggersEnabled = false;  // screenTriggersEnabled
    int screenTicksPerSecond = 5;        // screenTicksPerSecond: 1 to 16 (2 at most on battery)
    bool screenRemember = false;         // screenRemember: the portal keeps the choice of the screens (a restore token); off: the desktop asks every session
    bool screenWatchAtLogin = false;     // screenWatchAtLogin: ask for the screens once the session starts
    bool screenIgnorePets = true;        // screenIgnorePets: a region more than half covered by our own pets is skipped for that tick
    QStringList demoCardsDismissed;    // demoCardsDismissed: the pages (timelines, triggers, profiles, screen) whose first-visit "Try it" card was closed; never shown again

    bool operator==(const AutomationSettings &) const = default;

    enum class Result { NotMine, Ok, Error };
    Result apply(const QString &key, const QJsonValue &value, QString *error);
    void write(QJsonObject &into) const;
    static QStringList keys();
};

} // namespace murasaki
