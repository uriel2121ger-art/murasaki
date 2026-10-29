#include "automationsources.h"

#include "constants.h"
#include "sessionwatch.h"
#include "trigger.h"

#include <QDBusConnectionInterface>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

#include <unistd.h>

using namespace Qt::StringLiterals;

namespace murasaki {
namespace {

constexpr int kMaxSignalsPerSecond = 20; // D-Bus signals taken from the rules, whatever the rate limits of the engine say later

}

SignalFileWatcher::SignalFileWatcher(QString directory, QObject *parent)
    : QObject(parent)
    , m_directory(std::move(directory))
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(20); // a flood of touches is a scan every 20 ms at most
    connect(&m_coalesce, &QTimer::timeout, this, [this] { scan(true); });
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_coalesce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_coalesce.start(); });
}

bool SignalFileWatcher::prepareDirectory()
{
    if (m_directory.isEmpty())
        return false;
    const QFileInfo info(m_directory);
    if (info.isSymLink())
        return false;
    if (!info.exists() && !QDir().mkpath(m_directory))
        return false;
    // Only the user may write there (a file someone else can create would be a trigger anyone could pull).
    const QFileInfo made(m_directory);
    if (!made.isDir() || made.isSymLink() || made.ownerId() != uint(getuid()))
        return false;
    QFile::setPermissions(m_directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    return true;
}

void SignalFileWatcher::setActive(bool active)
{
    if (active == m_active)
        return;
    m_active = active;
    if (!active) {
        m_coalesce.stop();
        if (!m_watcher.files().isEmpty())
            m_watcher.removePaths(m_watcher.files());
        if (!m_watcher.directories().isEmpty())
            m_watcher.removePaths(m_watcher.directories());
        m_seen.clear();
        return;
    }
    if (!prepareDirectory()) {
        qWarning("murasakid: the signal directory %s is not usable; signal-file triggers do nothing", qPrintable(m_directory));
        m_active = false;
        return;
    }
    m_watcher.addPath(m_directory);
    scan(false); // what is there already is the starting point, not a trigger
}

void SignalFileWatcher::scan(bool announce)
{
    if (!m_active)
        return;
    static const QRegularExpression valid(u"^[A-Za-z0-9._-]{1,64}$"_s);
    const QFileInfoList entries = QDir(m_directory).entryInfoList(QDir::Files | QDir::NoSymLinks | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Unsorted);
    QHash<QString, QPair<qint64, qint64>> now;
    QStringList toWatch;
    int count = 0;
    for (const QFileInfo &info : entries) {
        if (++count > kMaxFiles)
            break;
        const QString name = info.fileName();
        if (!valid.match(name).hasMatch() || name == u"." || name == u"..")
            continue;
        const QPair<qint64, qint64> key{info.lastModified().toMSecsSinceEpoch(), info.size()};
        now.insert(name, key);
        toWatch.append(info.absoluteFilePath());
        const auto before = m_seen.constFind(name);
        if (announce && (before == m_seen.constEnd() || *before != key))
            emit touched(name);
    }
    m_seen = now;
    // The watch of a file ends when it is replaced or deleted: look at what is there again.
    const QStringList watched = m_watcher.files();
    if (!watched.isEmpty())
        m_watcher.removePaths(watched);
    if (!toWatch.isEmpty())
        m_watcher.addPaths(toWatch);
}

AutomationSources::AutomationSources(AutomationManager *manager, const QDBusConnection &bus, SessionWatch *session, QString signalDirectory, QObject *parent,
                                     const QDBusConnection &system)
    : QObject(parent)
    , m_manager(manager)
    , m_bus(bus)
    , m_system(system)
    , m_session(session)
    , m_files(std::move(signalDirectory))
    , m_surfaceWatcher(QString(kSurfaceDBusService), bus, QDBusServiceWatcher::WatchForOwnerChange)
{
    connect(m_manager, &AutomationManager::needsChanged, this, &AutomationSources::sync);
    connect(&m_files, &SignalFileWatcher::touched, m_manager, &AutomationManager::signalFile);
    connect(&m_surfaceWatcher, &QDBusServiceWatcher::serviceOwnerChanged, this, [this] {
        if (m_followsFacts)
            readFacts();
    });
    if (m_session) {
        m_manager->setLocked(m_session->locked());
        connect(m_session, &SessionWatch::pausedChanged, this, [this] { m_manager->setLocked(m_session->locked()); });
    }
    sync();
}

void AutomationSources::sync()
{
    const EngineNeeds n = m_manager->needs();
    followFacts(n.windows || n.power || n.game || n.outputs);
    followIdle(n.idle);
    m_files.setActive(n.signalFiles);
    syncRules();
}

void AutomationSources::followFacts(bool on)
{
    if (on == m_followsFacts)
        return;
    m_followsFacts = on;
    if (on) {
        m_bus.connect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface), u"SessionFactsChanged"_s, this, SLOT(readFacts()));
        readFacts();
    } else {
        m_bus.disconnect(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface), u"SessionFactsChanged"_s, this, SLOT(readFacts()));
        m_manager->setSessionFacts({}); // not followed: unknown, so that following again later only sets a starting point and no edge is invented
    }
}

void AutomationSources::readFacts()
{
    QDBusMessage message = QDBusMessage::createMethodCall(QString(kSurfaceDBusService), QString(kSurfaceDBusPath), QString(kSurfaceDBusInterface), u"SessionFacts"_s);
    message.setAutoStartService(false); // (the surface is not started for this)
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(message, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, watcher] {
        const QDBusPendingReply<QString> reply = *watcher;
        watcher->deleteLater();
        if (!m_followsFacts)
            return;
        if (!reply.isValid()) {
            m_manager->setSessionFacts({}); // no surface: the facts are unknown
            return;
        }
        m_manager->setSessionFacts(QJsonDocument::fromJson(reply.value().toUtf8()).object());
    });
}

void AutomationSources::followIdle(bool on)
{
    if (on == m_followsIdle)
        return;
    m_followsIdle = on;
    const QString service = u"org.freedesktop.login1"_s;
    if (!on) {
        if (!m_sessionPath.isEmpty())
            m_system.disconnect(service, m_sessionPath, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s, this, SLOT(onLogindProperties(QString, QVariantMap, QStringList)));
        m_sessionPath.clear();
        m_manager->setIdle(false, false);
        return;
    }
    // logind lives on the system bus; the session bus does not know it (the hint is unverified here: docs/pets/automation.md). It emits PropertiesChanged on the
    // session's own object, not on the /session/auto alias that answers Get: the path of this process's session is asked first.
    if (!m_system.isConnected()) {
        m_manager->setIdle(false, false);
        return;
    }
    QDBusMessage find = QDBusMessage::createMethodCall(service, u"/org/freedesktop/login1"_s, u"org.freedesktop.login1.Manager"_s, u"GetSessionByPID"_s);
    find.setArguments({quint32(getpid())});
    find.setAutoStartService(false);
    auto *finding = new QDBusPendingCallWatcher(m_system.asyncCall(find, 2000), this);
    connect(finding, &QDBusPendingCallWatcher::finished, this, [this, finding, service] {
        const QDBusPendingReply<QDBusObjectPath> found = *finding;
        finding->deleteLater();
        if (!m_followsIdle)
            return;
        // A session that logind does not know for this process (a service started outside any session): the alias still answers Get, but nothing is emitted on it.
        m_sessionPath = found.isValid() ? found.value().path() : u"/org/freedesktop/login1/session/auto"_s;
        m_system.connect(service, m_sessionPath, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s, this, SLOT(onLogindProperties(QString, QVariantMap, QStringList)));
        QDBusMessage get = QDBusMessage::createMethodCall(service, m_sessionPath, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
        get.setArguments({u"org.freedesktop.login1.Session"_s, u"IdleHint"_s});
        get.setAutoStartService(false);
        auto *reading = new QDBusPendingCallWatcher(m_system.asyncCall(get, 2000), this);
        connect(reading, &QDBusPendingCallWatcher::finished, this, [this, reading] {
            const QDBusPendingReply<QVariant> reply = *reading;
            reading->deleteLater();
            if (m_followsIdle)
                m_manager->setIdle(reply.isValid(), reply.isValid() && reply.value().toBool());
        });
    });
}

void AutomationSources::onLogindProperties(const QString &interface, const QVariantMap &changed, const QStringList &)
{
    if (interface == u"org.freedesktop.login1.Session" && changed.contains(u"IdleHint"_s) && m_followsIdle)
        m_manager->setIdle(true, changed.value(u"IdleHint"_s).toBool());
}

void AutomationSources::syncRules()
{
    const QList<EngineNeeds::DBusRule> wanted = m_manager->needs().dbusRules;
    if (wanted == m_rules)
        return;
    for (const EngineNeeds::DBusRule &rule : std::as_const(m_rules))
        m_bus.disconnect(QString(), QString(), rule.interface, rule.member, this, SLOT(onDBusSignal(QDBusMessage)));
    m_rules = wanted.mid(0, kMaxDBusTriggers);
    followOwners(false); // (the table is made again for the namespaces of the rules that are now)
    followOwners(std::any_of(m_rules.begin(), m_rules.end(), [](const EngineNeeds::DBusRule &r) { return !r.senderNamespace.isEmpty(); }));
    for (const EngineNeeds::DBusRule &rule : std::as_const(m_rules))
        m_bus.connect(QString(), QString(), rule.interface, rule.member, this, SLOT(onDBusSignal(QDBusMessage)));
}

bool AutomationSources::wantedName(const QString &name) const
{
    if (name.startsWith(u':'))
        return false;
    for (const EngineNeeds::DBusRule &rule : m_rules)
        if (!rule.senderNamespace.isEmpty() && (name == rule.senderNamespace || name.startsWith(rule.senderNamespace + u'.')))
            return true;
    return false;
}

// The well-known names a rule's namespace names, and who owns them, are kept from the bus's own NameOwnerChanged: a signal is matched against this table and
// nothing is asked of the bus while it arrives. (A namespace is a filter, not authentication: any program can own a name in it.)
void AutomationSources::followOwners(bool on)
{
    if (on == m_followsOwners)
        return;
    m_followsOwners = on;
    const QString service = u"org.freedesktop.DBus"_s, path = u"/org/freedesktop/DBus"_s;
    if (!on) {
        m_bus.disconnect(service, path, service, u"NameOwnerChanged"_s, this, SLOT(onNameOwnerChanged(QString, QString, QString)));
        m_owners.clear();
        return;
    }
    m_bus.connect(service, path, service, u"NameOwnerChanged"_s, this, SLOT(onNameOwnerChanged(QString, QString, QString)));
    QDBusMessage list = QDBusMessage::createMethodCall(service, path, service, u"ListNames"_s);
    auto *listing = new QDBusPendingCallWatcher(m_bus.asyncCall(list, 2000), this);
    connect(listing, &QDBusPendingCallWatcher::finished, this, [this, listing, service, path] {
        const QDBusPendingReply<QStringList> names = *listing;
        listing->deleteLater();
        if (!names.isValid() || !m_followsOwners)
            return;
        for (const QString &name : names.value()) {
            if (!wantedName(name))
                continue;
            QDBusMessage owner = QDBusMessage::createMethodCall(service, path, service, u"GetNameOwner"_s);
            owner.setArguments({name});
            auto *asking = new QDBusPendingCallWatcher(m_bus.asyncCall(owner, 2000), this);
            connect(asking, &QDBusPendingCallWatcher::finished, this, [this, asking, name] {
                const QDBusPendingReply<QString> reply = *asking;
                asking->deleteLater();
                if (reply.isValid() && m_followsOwners && !m_owners[reply.value()].contains(name))
                    m_owners[reply.value()].append(name);
            });
        }
    });
}

void AutomationSources::onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner)
{
    if (!wantedName(name))
        return;
    if (!oldOwner.isEmpty()) {
        m_owners[oldOwner].removeAll(name);
        if (m_owners[oldOwner].isEmpty())
            m_owners.remove(oldOwner);
    }
    if (!newOwner.isEmpty() && !m_owners[newOwner].contains(name))
        m_owners[newOwner].append(name);
}

void AutomationSources::onDBusSignal(const QDBusMessage &message)
{
    // A flood of matching signals is cut here, before anything is looked up. The arguments are never read.
    const qint64 now = m_manager->now();
    if (now - m_windowStartMs >= 1000) {
        m_windowStartMs = now;
        m_inWindow = 0;
    }
    if (++m_inWindow > kMaxSignalsPerSecond)
        return;
    const QString sender = message.service();
    QStringList names{sender};
    names.append(m_owners.value(sender)); // (a table kept up to date; no call to the bus here)
    m_manager->dbusSignal(message.interface(), message.member(), names);
}

} // namespace murasaki
