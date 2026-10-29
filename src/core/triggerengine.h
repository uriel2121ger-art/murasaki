#pragma once

// The rules around the triggers (docs/pets/automation.md, "Facts, rules and the engine"): edges, cooldowns, rate limits, the loop guard, missed times. Pure: the clock is
// a parameter and what fires comes out as a list, so a script of events tests it. The daemon feeds it the session's facts and runs the actions it returns.

#include "trigger.h"

#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace murasaki {

// What the engine knows about the session. `known` is false when the session has no window monitor: the window, power and game facts are then unknown, not false.
struct AutomationFacts {
    bool known = false;
    QString activeApp;
    QStringList openApps;
    QHash<QString, QString> fullscreen; // output -> the class of its fullscreen window (empty when unknown); outputs without one are absent
    bool onBattery = false;
    bool gameRunning = false;
    bool locked = false;       // from the session manager: always known
    // The idle facts the triggers read. `idleKnown` / `idle` are the answer of the best source: the surface's (ext-idle-notify: Wayland compositors such as Plasma, which never
    // set logind's IdleHint) when it knows, else logind's. `idleAfterMs` is how long before the fact the user's last input was (the notification fires that long after it): 0
    // for logind. Set the two sources with setLogindIdle() and factsFromSessionJson(), not the effective fields (they are what the engine tests set directly).
    bool idleKnown = false;
    bool idle = false;
    qint64 idleAfterMs = 0;
    bool logindIdleKnown = false;
    bool logindIdle = false;
    bool surfaceIdleKnown = false;
    bool surfaceIdle = false;
    qint64 surfaceIdleAfterMs = 0;
    // Once the surface has known, its answer is the only one: while it is gone (restarted, not followed) the idle is unknown, not logind's (which on Plasma would say
    // "not idle" and end the period).
    bool surfaceIdleSeen = false;
    bool idleFromSurface = false; // the effective answer is the surface's
    QStringList outputs;       // the outputs that exist (from the daemon's own view)
    bool outputsKnown = false;

    void setLogindIdle(bool known, bool value);
    void setSurfaceIdle(bool known, bool value, qint64 afterMs);

    bool operator==(const AutomationFacts &other) const = default;

private:
    void resolveIdle();
};

// The facts of the surface's SessionFacts() JSON (docs/pets/session-facts.md); `locked`, logind's idle hint and the outputs come from elsewhere and are kept from `base`; the surface's idle (when it knows it) wins over logind's.
AutomationFacts factsFromSessionJson(const QJsonObject &json, const AutomationFacts &base);

enum class FireOutcome { Ran, Cooldown, RateLimited, ChainTooDeep, Disabled, SourceUnavailable, ShortcutUnbound };
QString fireOutcomeName(FireOutcome outcome); // "ran", "cooldown", "rate limited", "chain too deep", "disabled", "source unavailable", "shortcut unbound"

struct Fired {
    QString triggerId;
    TriggerAction action;
    bool release = false; // a hold shortcut was released: `action` is the inverse
    qint64 atMs = 0;
};

struct TriggerLogEntry {
    QString triggerId;
    qint64 atMs = 0;
    FireOutcome outcome = FireOutcome::Ran;
};

struct TriggerState {
    bool enabled = false;
    bool available = true;    // the facts the source needs exist on this session
    bool rateLimited = false; // a fire was dropped by a rate limit
    qint64 lastFireMs = -1;
    FireOutcome lastOutcome = FireOutcome::Ran;
    bool hasOutcome = false;
};

// What the engine needs to be told about, so that the daemon subscribes to it only then.
struct EngineNeeds {
    bool windows = false;     // app-focus, app-open, fullscreen
    bool power = false;
    bool game = false;
    bool session = false;
    bool idle = false;
    bool outputs = false;
    bool screen = false;      // an enabled screen trigger: the daemon may watch the screen (when the setting and the user let it)
    bool timer = false;       // time, interval, idle: one single-shot timer
    bool signalFiles = false;
    QStringList shortcutIds;
    struct DBusRule {
        QString interface, member, senderNamespace;
        bool operator==(const DBusRule &other) const = default;
    };
    QList<DBusRule> dbusRules;

    bool any() const { return windows || power || game || session || idle || outputs || timer || signalFiles || screen || !shortcutIds.isEmpty() || !dbusRules.isEmpty(); }
};

class TriggerEngine
{
public:
    // Replaces the rules. The state of a trigger that stays (its edges, its cooldown) is kept when its source did not change.
    void setTriggers(const QList<Trigger> &triggers, qint64 nowMs, const QDateTime &nowLocal);
    // The master switch: off, nothing fires and nothing is needed.
    // Turning it off undoes what a hold is holding (a screen trigger or a held shortcut): the inverse actions come out with the next takeFires().
    void setAutomationEnabled(bool enabled);
    bool automationEnabled() const { return m_enabled; }

    EngineNeeds needs() const;

    // New facts: the edge sources fire when their condition became true (or false, with `on: leave`). The first facts only set the starting point.
    void setFacts(const AutomationFacts &facts, qint64 nowMs);
    // The clock moved: time, interval and idle triggers that are due fire (a time or interval that is more than 2 minutes late is skipped).
    void tick(qint64 nowMs, const QDateTime &nowLocal);
    // Milliseconds from nowMs to the next time something is due (time, interval, idle), or -1.
    qint64 nextWakeMs(qint64 nowMs, const QDateTime &nowLocal) const;

    // The triggers of that name fire (FireTrigger, the signal file, a D-Bus signal that matched, the shortcut's activation). Returns how many triggers had the name.
    int fire(const QString &name, qint64 nowMs);
    int signalFile(const QString &name, qint64 nowMs);
    int dbusSignal(const QString &interface, const QString &member, const QStringList &senderNames, qint64 nowMs);
    // A shortcut trigger was pressed or released (hold ones run the inverse on release). `bound` is false when the desktop holds no key for it.
    int shortcut(const QString &id, bool pressed, qint64 nowMs, bool bound = true);
    // The screen matcher activated (or ended) a screen trigger. It is a hold: the action runs at the activation and its inverse (a timeline is stopped, hide <-> show) at the end.
    // Returns whether the trigger exists. Ignored for a trigger that is not of kind Screen.
    bool screenActive(const QString &triggerId, bool active, qint64 nowMs);
    // Fires one trigger by id regardless of its source ("Test" in the GUI): the cooldown and the limits still apply.
    bool test(const QString &triggerId, qint64 nowMs);

    // A trigger's action started at nowMs: what happens shortly after and fires another trigger is a hop of a chain.
    QList<Fired> takeFires();
    QList<TriggerLogEntry> log() const { return m_log; }
    TriggerState state(const QString &triggerId) const;
    QStringList enabledIds() const;

private:
    struct Entry {
        Trigger trigger;
        bool initialized = false;
        bool last = false;                 // the condition at the last facts
        QSet<QString> lastOutputs;         // output sources: the outputs at the last facts
        qint64 lastFireMs = -1;
        QList<qint64> recent;              // when it fired (the last second)
        QList<qint64> minute;              // when a fact trigger fired (the last minute)
        QDateTime timeCursor;              // time sources: everything up to here is handled
        qint64 intervalFromMs = 0;         // interval sources: counted from here
        qint64 idleSinceMs = -1;           // idle sources: the last input was here (-1: not idle)
        bool idleFired = false;            // away: the nap fired; back: the period does not count (it was there before the engine)
        int idleSource = -1;               // idle sources: whose answer the last facts were (-1: none yet, 0: logind, 1: the surface)
        bool rateLimited = false;
        bool held = false;                 // a hold shortcut is down
        FireOutcome outcome = FireOutcome::Ran;
        bool hasOutcome = false;
    };

    bool eligible(const Entry &e) const { return m_enabled && e.trigger.enabled; }
    void record(Entry &e, FireOutcome outcome, qint64 nowMs);
    // Applies the cooldown, the rate limits and the chain guard; queues the fire when it passes. `external`: a source that is not a consequence of what pets did.
    bool tryFire(Entry &e, qint64 nowMs, bool external, bool release = false);
    bool windowsAvailable() const { return m_facts.known; }
    QDateTime nextTimeTarget(const Entry &e, const QDateTime &after) const;

    QHash<QString, Entry> m_entries;
    QStringList m_order;
    AutomationFacts m_facts;
    bool m_haveFacts = false;
    bool m_enabled = true;
    qint64 m_lastMs = 0;           // the last clock the engine was given, for the releases it makes on its own
    void releaseHeld(Entry &e, qint64 nowMs);
    QList<Fired> m_fires;
    QList<TriggerLogEntry> m_log;
    QList<qint64> m_recentAll;     // when anything fired (the last second)
    struct Hop {
        qint64 atMs;
        int depth;
    };
    QList<Hop> m_hops;
};

} // namespace murasaki
