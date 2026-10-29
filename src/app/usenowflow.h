#pragma once

#include <QObject>
#include <QStringList>
#include <QTimer>

namespace murasaki {

class DaemonClient;
class LibraryModel;

// The Workshop page's "Use now": put an item on all screens and say so only when the daemon has it.
//
//   waiting   the item is not in the library yet (Steam has just finished it and the daemon's watcher has not caught
//             up): wait for it, up to waitForItemMs
//   applying  the assignment was sent (after clearing the single screens, when asked) and the daemon has not yet
//             reported the result
//   applied   the daemon's assignments show the item on all screens (and, when replacing, no screen of their own)
//   failed    `message` says why: the daemon's own error, no service, the wait ran out, or no confirmation
//   idle      nothing started, or reset()
//
// One flow at a time: start() replaces whatever was running.
class UseNowFlow : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(QString itemId READ itemId NOTIFY stateChanged)

public:
    UseNowFlow(DaemonClient *client, LibraryModel *library, QObject *parent = nullptr);

    QString state() const { return m_state; }
    QString message() const { return m_message; }
    QString itemId() const { return m_itemId; }

    // `replaceOverrides` clears the assignments of single screens and desktops inside "all screens" first (the ones that
    // would hide the item); without it the item is set on the wildcard only and they keep winning. `knownDesktops` are the
    // compositor's desktop ids (empty: unknown).
    Q_INVOKABLE void start(const QString &itemId, bool replaceOverrides, const QStringList &knownDesktops = {});
    Q_INVOKABLE void reset();

    void setTimeouts(int waitForItemMs, int confirmMs)
    {
        m_waitForItemMs = waitForItemMs;
        m_confirmMs = confirmMs;
    }

signals:
    void stateChanged();

private:
    void set(const QString &state, const QString &message = {});
    void apply();
    bool satisfied() const;
    void onLibraryChanged();
    void onAssignmentsChanged();
    void onError(const QString &message);

    DaemonClient *m_client;
    LibraryModel *m_library;
    QString m_state = QStringLiteral("idle");
    QString m_message;
    QString m_itemId;
    bool m_replace = false;
    QStringList m_knownDesktops;
    int m_waitForItemMs = 30000;
    int m_confirmMs = 5000;
    QTimer m_timer;
};

} // namespace murasaki
