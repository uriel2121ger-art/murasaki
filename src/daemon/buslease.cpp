#include "buslease.h"

#include <QDBusConnectionInterface>
#include <QTimer>

namespace murasaki {

BusLease::BusLease(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_watcher(QString(), bus, QDBusServiceWatcher::WatchForUnregistration)
{
    connect(&m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &BusLease::ownerChanged);
}

void BusLease::release()
{
    if (!m_owner.isEmpty())
        m_watcher.removeWatchedService(m_owner);
    m_owner.clear();
}

void BusLease::take(const QString &owner)
{
    release();
    if (owner.isEmpty())
        return;
    m_owner = owner;
    m_watcher.addWatchedService(owner);
    // (it may have left before the watch was in place)
    QTimer::singleShot(0, this, [this, owner] {
        if (m_owner == owner && m_bus.interface() && !m_bus.interface()->isServiceRegistered(owner))
            ownerChanged(owner, owner, QString());
    });
}

void BusLease::ownerChanged(const QString &name, const QString &, const QString &newOwner)
{
    if (name != m_owner || !newOwner.isEmpty())
        return;
    release();
    emit lost();
}

} // namespace murasaki
