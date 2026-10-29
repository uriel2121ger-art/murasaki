#pragma once

#include "constants.h"

#include <QDBusContext>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointF>
#include <QSizeF>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <functional>

class QDBusServiceWatcher;

namespace murasaki {

// D-Bus facade of murasaki-surface (docs/dbus-api.md, "Surface API").
class SurfaceService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki.Surface1")

public:
    using QObject::QObject;

    // The object path this service is exported at (the path of the signals addressed to hosts).
    QString dbusPath = QString(kSurfaceDBusPath);

    std::function<QJsonArray()> outputsProvider;
    std::function<QJsonArray()> desktopsProvider;
    // What a host of this output should do now: {desktop, paused, audible, frameRateLimit}.
    std::function<QJsonObject(const QString &)> hostStateProvider;

    // Hosts (the Plasma plugin's murasaki-surface --host processes) register per output, one host per output and
    // at most a few outputs per connection. A registration belongs to the caller's unique bus name and
    // disappears with it (and so does its hidden flag).
    bool isHosted(const QString &output) const { return m_owners.contains(output); }
    // False while the host of the output reports its containment hidden; true when not hosted.
    bool hostVisible(const QString &output) const { return !m_hidden.contains(output); }
    // True while at least one registered host has its containment visible: somebody can use the cursor.
    bool hasVisibleHost() const;
    // Sends the cursor to the host of the output, and only to it (a signal addressed to its bus name). False when
    // the output has no visible host. `local` and `size` are in the output's logical pixels.
    bool sendHostCursor(const QString &output, const QPointF &local, const QSizeF &size);
    // Sends a pointer event of the overlay's interaction mode to the host of the output, and only to it: the position as in sendHostCursor,
    // `buttons` (bit 0 left, 1 right, 2 middle) and the wheel (1/8 degree). False when the output has no visible host.
    bool sendHostPointer(const QString &output, const QPointF &local, const QSizeF &size, uint buttons, int wheelX, int wheelY);
    // The cursor left the output; the host keeps its last position.
    bool sendHostCursorLeft(const QString &output);
    // Emits HostStateChanged for every hosted output whose HostState() changed since it was last announced.
    void notifyHostStates();

    // Why the wallpaper of a hosted output is not shown, as its host reported it (empty: it is). Part of Outputs().
    QJsonObject hostFailure(const QString &output) const { return m_hostFailures.value(output); }
    // Tells the host of the output to do what the user asked: "retry".
    bool sendHostCommand(const QString &output, const QString &command);

    // Emits OutputsChanged when Outputs() now returns something other than it last did (initially an empty list).
    void notifyOutputsChanged();

    // Whether the sounds of the desktop pets should be muted now: a window is fullscreen (on any output: the monitors do not say which), or a game
    // (GameMode) or a window of the user's pauseApps rules is up. Not "covered": pets sit above windows. Emits PetAudioMutedChanged when it changes.
    void setPetAudioMuted(bool muted);
    bool petAudioMuted() const { return m_petAudioMuted; }

    // The session facts (revision 16, docs/pets/session-facts.md): `facts` is the JSON SessionFacts() returns, `outputFullscreen` the one OutputFullscreen() returns.
    // The values are current at once; the signals are coalesced to at most four a second (the first change goes out at once, what follows within 250 ms as one).
    // `publicFacts` and `publicOutputFullscreen` are the same facts without any application class, game or battery fact: what a caller that is not Murasaki's own
    // daemon or overlay gets (docs/security.md).
    struct SessionFactsText {
        QString facts, outputFullscreen, publicFacts, publicOutputFullscreen;
    };
    void setSessionFacts(const SessionFactsText &text);
    void setSessionFactsInterval(int milliseconds) { m_factsTimer.setInterval(milliseconds); }

public slots:
    Q_SCRIPTABLE QString Outputs() const;
    Q_SCRIPTABLE QString Desktops() const;
    // Called by kwin-monitor.js.
    Q_SCRIPTABLE void ReportKWinState(const QString &json);
    // Called by the GNOME Shell extension (data/gnome-shell/): the same JSON as ReportKWinState, from org.gnome.Shell.
    Q_SCRIPTABLE void ReportShellState(const QString &json);
    // Called by kwin-cursor.js: the cursor's position in the compositor's global logical pixels (whole numbers: KWin's cursor position is a QPoint).
    Q_SCRIPTABLE void ReportKWinCursor(int x, int y);
    // Called by murasaki-overlay in the interaction mode: the pointer above the desktop, forwarded to the host of the output. Only the overlay may
    // call it (the owner of its bus name); `x`, `y`, `width`, `height` are as in HostCursor.
    Q_SCRIPTABLE void ReportHostPointer(const QString &output, double x, double y, double width, double height, uint buttons, int wheelX, int wheelY);
    // Host coordination. Registering twice for the same output from the same name is harmless; another name is refused (AccessDenied).
    Q_SCRIPTABLE void RegisterHost(const QString &output);
    Q_SCRIPTABLE void UnregisterHost(const QString &output);
    // The plugin's containment is visible (true) or hidden (false); only a registered host may say so.
    Q_SCRIPTABLE void SetHostVisible(const QString &output, bool visible);
    // JSON {desktop, paused, audible, frameRateLimit}; for an output this process does not know, a playing,
    // audible state on the wildcard desktop, so a host without a counterpart still shows its wallpaper.
    Q_SCRIPTABLE QString HostState(const QString &output) const;
    // A host says why its output shows no wallpaper: JSON {kind, reason, engine, canTryOther}, or "" when it does.
    Q_SCRIPTABLE void ReportOutputHealth(const QString &output, const QString &json);
    // The user asks again for the wallpaper of this output.
    Q_SCRIPTABLE void RetryOutput(const QString &output);
    // Revision 15: see setPetAudioMuted(). The overlay reads it at start and on PetAudioMutedChanged.
    Q_SCRIPTABLE bool PetAudioMuted() const { return m_petAudioMuted; }
    // Revision 16: JSON `{outputs: {name: {fullscreen, app, occluded}}, anyFullscreen, activeApp, openApps: [..], gameRunning, onBattery, quiet, known, monitor}`:
    // what the compositor monitor says about the session, per output. `quiet`: a game or a window of the pauseApps rules is up (not fullscreen). `known` is false
    // when the session has no monitor (everything else is then empty, not "false"). Revision 23: `idleKnown`, `idle`, `idleAfterMs`, present only when the compositor
    // tells (ext-idle-notify): idle is no input for idleAfterMs; absent means unknown, not "awake".
    Q_SCRIPTABLE QString SessionFacts() const;
    // Revision 16: JSON `{name: {fullscreen, app}}`, the part of SessionFacts() the pets' rules read (the sound's mute, the fullscreen mode of phase 6).
    Q_SCRIPTABLE QString OutputFullscreen() const;

signals:
    Q_SCRIPTABLE void OutputsChanged();
    Q_SCRIPTABLE void PetAudioMutedChanged(bool muted);
    Q_SCRIPTABLE void SessionFactsChanged();
    Q_SCRIPTABLE void OutputFullscreenChanged();
    Q_SCRIPTABLE void HostStateChanged(const QString &output);
    // Not on the bus: a registration came or went, or the host's visibility changed.
    void hostsChanged(const QString &output);
    void kwinStateReported(const QString &json);
    void shellStateReported(const QString &json);
    void kwinCursorReported(const QPointF &global);
    // Not on the bus: the user asked (through RetryOutput) for another try at this output.
    void retryRequested(const QString &output);
    // Addressed to the host of the output: the pointer of the interaction mode.
    Q_SCRIPTABLE void HostPointer(const QString &output, double x, double y, double width, double height, uint buttons, int wheelX, int wheelY);
    // Addressed to the host of the output: "retry" or "other".
    Q_SCRIPTABLE void HostCommand(const QString &output, const QString &command);

private:
    bool m_petAudioMuted = false;
    // Before the first publication (and always in host mode) the facts say `known: false`: a missing `known` is unknown too.
    SessionFactsText m_text{QStringLiteral("{\"known\":false,\"outputs\":{}}"), QStringLiteral("{}"), QStringLiteral("{\"known\":false,\"outputs\":{}}"), QStringLiteral("{}")};
    QString m_emittedOutputFullscreen = QStringLiteral("{}");
    bool calledByTrusted() const;
    bool m_factsDirty = false;
    QTimer m_factsTimer;
    void flushFacts();
    bool rejectOutput(const QString &output) const;
    bool calledByKWin() const;
    bool calledByGnomeShell() const;
    bool calledByOverlay() const;
    bool sendToHost(const QString &output, const QString &signal, const QVariantList &arguments);
    void watchOwner(const QString &name);
    void dropOwner(const QString &name);
    void removeOwner(const QString &output, const QString &name);
    QString hostStateJson(const QString &output) const;

    QDBusServiceWatcher *m_watcher = nullptr;
    // The interaction mode: the last pointer sent to each host, so that a button still down when the overlay goes away is let go.
    struct LastPointer {
        QPointF position;
        QSizeF size;
        uint buttons = 0;
    };
    void watchOverlay();
    void releaseHeldButtons();
    QHash<QString, LastPointer> m_lastPointer;
    QDBusServiceWatcher *m_overlayWatcher = nullptr;
    QHash<QString, QString> m_owners;     // output -> unique bus name of its host (one host per output)
    QSet<QString> m_hidden;               // hosted outputs whose host reported "hidden"
    QHash<QString, QString> m_lastHostState;
    QHash<QString, QJsonObject> m_hostFailures; // per hosted output, what its host reported (absent: none)
    QString m_lastOutputs = QStringLiteral("[]");
};

} // namespace murasaki
