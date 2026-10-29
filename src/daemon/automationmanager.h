#pragma once

#include "automationstore.h"
#include "petruntime.h"
#include "petstore.h"
#include "profileswitch.h"
#include "settings.h"
#include "timelinerunner.h"
#include "triggerengine.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QSizeF>
#include <QTimer>

#include <functional>

namespace murasaki {

// What the automation may do to the world, and nothing else: the closed list of docs/pets/automation.md. No member runs a program, opens a file or a web address
// (tst_automationclosed pins this struct and the manager's sources).
struct AutomationHooks {
    std::function<void(bool hidden)> setPetsHidden;
    std::function<bool()> petsHidden;
    std::function<void(const QString &petId)> playSound;
    std::function<void()> stopSounds;
    std::function<bool(const QString &petId, int index, QString *problem)> startScene;
    std::function<void(bool muted)> setMuted;
    std::function<bool()> muted;
    // Says why an item or variant can not be shown (kProfileItemMissing, kProfileVariantMissing) or returns an empty string.
    ProfileItemCheck checkItem;
    // How big the pet is shown (width and height in pixels) with the animation `variant` (empty: its stored one), for an action that places its centre; 0x0 when unknown.
    std::function<QSizeF(const QString &petId, const QString &variant)> petSize;
    std::function<qint64()> nowMs;       // CLOCK_MONOTONIC in milliseconds
    std::function<QDateTime()> nowLocal;
};

AutomationHooks systemClockHooks(AutomationHooks hooks);

// The daemon's automation: profiles (save, switch), the runtime layer of the pets, the timelines that run, and the triggers with their engine (docs/pets/automation.md).
// It owns no store; it reads and writes the ones it is given. Nothing runs, ticks or is subscribed while there is nothing to do: the engine timer is armed only for a
// time, interval or idle rule, the timelines' timer only while one runs.
class AutomationManager : public QObject
{
    Q_OBJECT

public:
    AutomationManager(PetStore *pets, ProfileStore *profiles, TriggerStore *triggers, TimelineStore *timelines, SettingsStore *settings, AutomationHooks hooks,
                      QString statePath = {}, QObject *parent = nullptr);

    // Reads the state file (the active profile) and arms what the stores ask for. Call once, after the stores are loaded.
    void load();

    // Profiles.
    // The pets on the desktop become the profile `name` (an existing one of that name is replaced). The profile's id, or an empty string with *error set.
    QString saveProfile(const QString &name, QString *error = nullptr);
    // `idOrName` is a profile's id or its name. *report (when given) says what happened: {kept, updated, added, removed, skipped: [{slot, itemId, reason}]}.
    // Revision 22: the pets as an example profile (origin "demo"). Unlike saveProfile it leaves the active profile and the slots alone, and refuses a name that a profile
    // of the user has. The profile's id, or an empty string with *error set.
    QString saveExampleProfile(const QString &name, QString *error = nullptr);
    bool switchProfile(const QString &idOrName, QString *error = nullptr, QJsonObject *report = nullptr);
    bool setProfileOrigin(const QString &idOrName, const QString &origin, QString *error = nullptr);
    bool renameProfile(const QString &idOrName, const QString &newName, QString *error = nullptr);
    bool deleteProfile(const QString &idOrName, QString *error = nullptr);
    QString activeProfile() const { return m_activeProfile; }
    bool profileModified() const;
    // The last switch's report (what was skipped and why), for the GUI.
    QJsonObject lastSwitchReport() const { return m_lastReport; }

    // The runtime layer.
    QHash<QString, PetRuntime> layer() const { return m_layer; }
    QString layerJson() const;
    // Hides or shows a pet in the layer (a scene of an imported package that hides the pet). False for a pet that does not exist.
    bool setPetHidden(const QString &petId, bool hidden);
    // The layer is cleared (all pets, or one): hidden, moved and re-animated pets go back to their stored state.
    void clearRuntime(const QString &petId = {});
    // The way out: every timeline stops (restoring what it touched) and the layer is cleared.
    void rescue();
    // The rescue also pauses the triggers (a loop that keeps putting the pets where the user does not want them must not win against the one-call way out): for this
    // long, or until resumeAutomation(). The setting automationEnabled is not touched.
    void setRescueSuspendMs(int ms) { m_rescueSuspendMs = ms; }
    qint64 suspendedMs() const;
    void resumeAutomation();

    // Timelines.
    bool runTimeline(const QString &idOrName, QString *error = nullptr);
    bool stopTimeline(const QString &idOrName);
    QStringList runningTimelines() const { return m_runner.runningIds(); }

    // Triggers.
    // The surface's SessionFacts() JSON. Without a window monitor the window, power and game facts are unknown.
    void setSessionFacts(const QJsonObject &json);
    void setLocked(bool locked);
    void setIdle(bool known, bool idle);
    // FireTrigger: the triggers with that name fire; with none, and the setting fireByTimelineName, the timeline of that name runs. True when something matched.
    bool fireTrigger(const QString &name);
    // A signal file of that name was touched; a D-Bus signal arrived; a shortcut was pressed or released.
    void signalFile(const QString &name);
    void dbusSignal(const QString &interface, const QString &member, const QStringList &senderNames);
    void shortcut(const QString &id, bool pressed);
    // The screen matcher activated or ended a screen trigger (a hold: the action runs at the activation and is undone at the end).
    void screenActive(const QString &triggerId, bool active);
    bool locked() const { return m_facts.locked; }
    bool onBattery() const { return m_facts.known && m_facts.onBattery; }
    // The trigger runs now as if its source had happened ("Test").
    bool testTrigger(const QString &triggerId);
    // JSON: [{id, enabled, available, outcome, ago, rateLimited}] and the last 50 fires.
    QJsonArray triggerStates() const;
    QJsonArray triggerLog() const;
    EngineNeeds needs() const { return m_engine.needs(); }
    // For tests and introspection: is the engine's timer armed, is a timeline's.
    bool engineTimerActive() const { return m_engineTimer.isActive(); }
    bool timelineTimerActive() const { return m_timelineTimer.isActive(); }
    qint64 now() const { return m_hooks.nowMs(); }

signals:
    void activeProfileChanged();
    void runtimeChanged(const QString &petId);
    void needsChanged();
    void triggerFired(const QString &triggerId, const QString &outcome);
    void timelinesChanged();
    void suspendedChanged();
    void lockedChanged(bool locked);
    void batteryChanged(bool onBattery);
    // The rescue ran: whatever watches for the triggers must stop (the screen watch).
    void rescued();

private:
    void onPetsChanged();
    void onTriggersChanged();
    void onSettingsChanged();
    void applyEffects(const QList<TimelineEffect> &effects);
    PetPosition shifted(const TimelineEffect &e, const PetRuntime &layer, const QString &storedVariant) const;
    void scheduleTimeline();
    void onTimelineTimer();
    void scheduleEngine();
    void onEngineTimer();
    void processFires();
    void runAction(const TriggerAction &action, bool release);
    ResolvedPet resolve(const PetRef &ref) const;
    QList<QString> petsOfItem(const QString &item) const;
    void setLayer(const QString &petId, const PetRuntime &runtime);
    void saveState() const;
    void publishNeeds();
    void pushFacts();

    PetStore *m_pets;
    ProfileStore *m_profiles;
    TriggerStore *m_triggers;
    TimelineStore *m_timelines;
    SettingsStore *m_settings;
    AutomationHooks m_hooks;
    QString m_statePath;

    QString m_activeProfile;
    QString m_profileBeforeExample; // the active profile when an example profile was switched to: deleting the example while it is active goes back to it
    QJsonObject m_lastReport;
    QHash<int, QString> m_slotPets; // the pets of the last switch, by the slot of their entry (a timeline names a pet by slot)
    QHash<QString, PetRuntime> m_layer;
    TimelineRunner m_runner;
    QTimer m_timelineTimer;
    TriggerEngine m_engine;
    // What a hold changed, as it was when the hold began (trigger id -> the pets' hidden flag / the mute flag): the end puts that back instead of the opposite, so pets the user
    // hid by hand stay hidden and a mute they set stays set.
    QHash<QString, bool> m_heldHidden;
    QHash<QString, bool> m_heldMuted;
    QTimer m_engineTimer;
    QTimer m_suspendTimer;
    qint64 m_suspendedUntilMs = 0;
    int m_rescueSuspendMs = 60'000;
    bool engineWanted() const;
    AutomationFacts m_facts;
    EngineNeeds m_lastNeeds;
    QStringList m_knownPets;
    bool m_loaded = false;
};

} // namespace murasaki
