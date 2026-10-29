#pragma once

// Watching the screen for the screen triggers: the daemon's side of it. It owns the portal session (consent), the
// sandboxed helper that reads the pixels (src/screenmatch) and one ScreenMatcher per screen trigger, and turns the helper's scores into activations that the trigger engine runs as holds.
//
// Nothing exists until the user asks. A fresh daemon has no portal object, no helper, no timer and no bus call from here (idle() says so, a test pins it). start() needs, all of them:
// the setting screenTriggersEnabled, at least one enabled screen trigger, an unlocked session; it opens the desktop's dialog (the user answers it), then starts the helper on the PipeWire
// remote the portal gave. There is no automatic restart: a desktop that ends the share, a refusal or a helper that crashes leaves the state `stopped`, `refused` or `failed` with the reason, and the
// user starts again. stop() (the user, the rescue, the setting going off, the last screen trigger going away) ends the helper and the portal session and releases whatever was active.
//
// Not a pixel is read here: the daemon sends commands and receives scores. The only picture that comes back is the capture the user asked for (capture()), held in memory until commitCapture().

#include "automationstore.h"
#include "buslease.h"
#include "screenmatch/plan.h"
#include "screenmatch/protocol.h"
#include "screencastportal.h"
#include "screenmatchhost.h"
#include "screenrefstore.h"
#include "securebytes.h"
#include "settings.h"

#include <vector>

#include <QDBusConnection>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QTimer>

#include <functional>
#include <memory>

namespace murasaki {

class CrashLedger;

class ScreenWatch : public QObject
{
    Q_OBJECT

public:
    enum class State { Off, Asking, Watching, Stopped, Refused, Failed };
    Q_ENUM(State)

    struct Config {
        QString helperPath;          // murasaki-screenmatch; empty: none found
        QString sandboxPath;         // murasaki-sandbox
        bool requireSandbox = true;
        QString crashDir;            // the crash ledger's folder (empty: no ledger)
        QString statePath;           // screen-state.json: the restore token, only while "remember" is on
        QDBusConnection bus = QDBusConnection::sessionBus();
        int tickGraceMs = 2000;      // how long a tick may be late before the scores are old (not used by the matchers; reserved)
        int petsDebounceMs = 200;
        int refreshDebounceMs = 200;
        int pendingMs = 60000;   // how long a capture waits to be kept
        int jobMs = 4000;        // how long the helper has to answer one capture
    };

    struct Hooks {
        std::function<qint64()> nowMs;                                   // CLOCK_MONOTONIC in ms
        std::function<QList<screenmatch::LogicalRect>()> petRects;       // where our own pets are, in logical desktop coordinates
        std::function<double(int x, int y, int width, int height)> scaleOf; // the device pixel ratio of the output at that place (1 when unknown)
        std::function<bool(const QString &triggerId)> actionRunning;     // a trigger's action (a timeline) is still running
        std::function<void(const QString &text)> notify;                 // "Murasaki is watching N regions of your screen"
    };

    ScreenWatch(TriggerStore *triggers, SettingsStore *settings, ScreenRefStore *refs, Config config, Hooks hooks, QObject *parent = nullptr);
    ~ScreenWatch() override;

    // The user pressed Start. False with *error when it can not start (the setting is off, no screen trigger is enabled, the session is locked, no helper); true also when it is already
    // asking or watching.
    bool start(QString *error = nullptr);
    // Stops watching and releases the portal session. `reason` is shown in the state (empty: the user asked).
    void stop(const QString &reason = {});
    // Binds the running watch to the bus connection `owner` (a unique name) that started it: when that connection leaves the bus the watch stops, with a reason, so that a program that
    // died does not leave the screen being read (and the desktop's sharing indicator on). Ended by any stop() and by the watch ending by itself; a no-op while nothing is asking or watching.
    void lease(const QString &owner);
    // The settings, the triggers or the pets changed (debounced where it is frequent).
    void settingsChanged();
    void triggersChanged();
    void petsMoved();
    void setLocked(bool locked);
    void setOnBattery(bool onBattery);
    // The login hook: starts when the setting asks for it and everything else holds.
    void startAtLogin();

    State state() const { return m_state; }
    QString reason() const { return m_reason; }
    // {state, reason, regions, ticksPerSecond, since, paused, notes}
    QJsonObject stateJson() const;
    // {score: [permille per region], detected, step, active} of a screen trigger while watching, else empty.
    QJsonObject triggerJson(const QString &triggerId) const;
    // Nothing is open: no portal, no helper, no timer.
    bool idle() const;

    // The explicit capture of the editor: one rectangle of a region of a trigger, or `steps` of them (a sequence) one after another. Returns the request id, or empty with *error. The
    // result comes as captured(); nothing is stored until commitCapture().
    QString capture(const QString &triggerId, int region, int steps, QString *error = nullptr);
    bool commitCapture(const QString &request, QString *error = nullptr);
    // The user closed the preview: the held capture (or the running one, if it is that request) is dropped. False for a request that is not there.
    bool discardCapture(const QString &request);

signals:
    void changed();
    void activated(const QString &triggerId);
    void deactivated(const QString &triggerId);
    // JSON {request, trigger, region, steps: [{step, width, height, png (base64)}]}, or {request, error}.
    void captured(const QString &request, const QString &json);

private:
    struct Live {
        screenmatch::PlannedTrigger planned;
        ScreenMatcher matcher;
        QList<int> permille;
        double score = 0;
        bool detected = false;
        int step = 0;
    };
    struct Shot {
        int request = 0, width = 0, height = 0, bytes = 0;
        QByteArray data;
        bool done = false;
    };
    struct Job {
        QString id, trigger;
        int region = 0, steps = 1, next = 0;
        std::vector<SecureBytes> pngs; // (the pictures kept in memory the dump leaves out and that is wiped when it goes: src/core/securebytes.h)
        QList<QSize> sizes;
        int stream = 0;
        screenmatch::Mapped where;
        int helperRequest = 0;
        Shot shot;
        bool waiting = false;
        ~Job() { SecureBytes::wipe(shot.data); } // (the raw picture of the screen)
    };

    enum class Gate { Ok, SettingOff, NoTrigger, Locked, NoHelper };
    Gate gate() const;
    QList<Trigger> enabledScreenTriggers() const;
    void setState(State state, const QString &reason = {});
    void onPortalState();
    void startHelper();
    void onHostState(ScreenMatchHost::State state);
    void onReply(const screenmatch::Reply &reply);
    void sendSetup();
    void sendPets();
    void sendRate();
    int effectiveRate() const;
    void releaseAll();
    void endTick();
    void failHelper(ScreenMatchHost::State state);
    void dropCapture();
    void nextShot();
    void finishJob(const QString &error);
    QByteArray snapshotOfTriggers() const;
    QByteArray gridOf(const QString &triggerId, int region, int step) const;
    QByteArray loadToken() const;
    void saveToken(const QString &token) const;
    qint64 now() const;

    TriggerStore *m_triggers;
    SettingsStore *m_settings;
    ScreenRefStore *m_refs;
    Config m_config;
    Hooks m_hooks;
    std::unique_ptr<BusLease> m_lease;

    State m_state = State::Off;
    QString m_reason;
    qint64 m_sinceMs = 0;
    bool m_locked = false;
    bool m_onBattery = false;
    bool m_paused = false;       // the session is locked: the helper waits
    bool m_wasEnabled = false;
    int m_lastRate = 0;
    QStringList m_notes;
    QList<screenmatch::StreamGeometry> m_streams;
    QHash<QString, Live> m_live;
    QStringList m_order;

    std::unique_ptr<CrashLedger> m_ledger;
    std::unique_ptr<ScreenCastPortal> m_portal;
    std::unique_ptr<ScreenMatchHost> m_host;
    QTimer m_petsTimer;
    QTimer m_refreshTimer;
    QTimer m_captureTimer;
    QTimer m_jobTimer;      // a capture the helper does not answer is failed
    QTimer m_pendingTimer;  // a capture nobody kept is dropped
    QString m_hostWhy;      // the last reason the host gave (its `degraded` comes after its state change)
    quint64 m_hostGeneration = 0;
    QByteArray m_lastSignature;
    QByteArray m_snapshot;  // the screen triggers' own settings at the last setup: an edit of any other trigger does not touch the watch
    bool m_forceRefresh = false;
    bool m_dying = false;
    int m_pendingMs = 60000;
    int m_jobMs = 4000;
    std::unique_ptr<Job> m_job;
    int m_nextRequest = 1;
    struct Pending {
        QString id, trigger;
        int region = 0;
        std::vector<SecureBytes> pngs;
    };
    Pending m_pending;
};

} // namespace murasaki
