#pragma once

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QObject>
#include <QString>

namespace murasaki {

// A thing that lives only as long as the bus connection that asked for it (the guided screen walk's watch: a GUI that dies must not leave the screen being read). take() binds
// it to a unique bus name; when that name leaves the bus, lost() is emitted once and the lease is over. release() ends it quietly (the owner stopped it on purpose).
class BusLease : public QObject
{
    Q_OBJECT

public:
    explicit BusLease(const QDBusConnection &bus, QObject *parent = nullptr);

    // Binds to `owner` (a unique name such as ":1.42"); a lease that was held is replaced. An owner that is already gone is lost() at once (queued).
    void take(const QString &owner);
    void release();
    bool held() const { return !m_owner.isEmpty(); }
    QString owner() const { return m_owner; }

signals:
    void lost();

private:
    void ownerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);

    QDBusConnection m_bus;
    QDBusServiceWatcher m_watcher;
    QString m_owner;
};

} // namespace murasaki
