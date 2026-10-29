#pragma once

#include "automationmanager.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusServiceWatcher>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QTimer>

namespace murasaki {

class SessionWatch;

// The files of the "signal-file" triggers: $XDG_RUNTIME_DIR/murasaki/triggers/<name> appears or is touched. The contents are never read; names are [A-Za-z0-9._-]{1,64};
// symbolic links, directories and other names are ignored. Nothing is watched (no directory is made) until setActive(true).
class SignalFileWatcher : public QObject
{
    Q_OBJECT

public:
    explicit SignalFileWatcher(QString directory, QObject *parent = nullptr);

    void setActive(bool active);
    bool active() const { return m_active; }
    QString directory() const { return m_directory; }
    // The most files looked at in one scan (a directory full of files is not read whole).
    static constexpr int kMaxFiles = 256;

signals:
    void touched(const QString &name);

private:
    void scan(bool announce);
    bool prepareDirectory();

    QString m_directory;
    bool m_active = false;
    QFileSystemWatcher m_watcher;
    QTimer m_coalesce;
    QHash<QString, QPair<qint64, qint64>> m_seen; // name -> (modified ms, size)
};

// Feeds the AutomationManager the things its enabled triggers need, and only those: the surface's session facts, the session manager's idle hint, the signal files and
// the D-Bus signals of the rules. Each is subscribed when needs() says so and dropped when it does not (docs/pets/automation.md, "Wakeups").
class AutomationSources : public QObject
{
    Q_OBJECT

public:
    // `system` is the bus logind lives on (the system bus; a test passes a bus with a fake).
    AutomationSources(AutomationManager *manager, const QDBusConnection &bus, SessionWatch *session, QString signalDirectory, QObject *parent = nullptr,
                      const QDBusConnection &system = QDBusConnection::systemBus());

    // For tests and introspection: what is subscribed right now.
    bool followsFacts() const { return m_followsFacts; }
    bool followsOwners() const { return m_followsOwners; }
    bool followsIdle() const { return m_followsIdle; }
    bool watchesFiles() const { return m_files.active(); }
    int dbusRules() const { return m_rules.size(); }

private slots:
    void sync();
    void readFacts();
    void onDBusSignal(const QDBusMessage &message);
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);
    void onLogindProperties(const QString &interface, const QVariantMap &changed, const QStringList &);

private:
    void followFacts(bool on);
    void followIdle(bool on);
    void syncRules();
    void followOwners(bool on);
    bool wantedName(const QString &name) const;

    AutomationManager *m_manager;
    QDBusConnection m_bus;
    QDBusConnection m_system;
    QString m_sessionPath; // the object logind emits this session's PropertiesChanged on
    bool m_followsOwners = false;
    QHash<QString, QStringList> m_owners; // unique name -> the well-known names in a rule's namespaces that it owns (kept by NameOwnerChanged)
    SessionWatch *m_session;
    SignalFileWatcher m_files;
    QDBusServiceWatcher m_surfaceWatcher;
    bool m_followsFacts = false;
    bool m_followsIdle = false;
    QList<EngineNeeds::DBusRule> m_rules;
    qint64 m_windowStartMs = 0;
    int m_inWindow = 0;
};

} // namespace murasaki
