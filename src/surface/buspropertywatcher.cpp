#include "buspropertywatcher.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

using namespace Qt::StringLiterals;

namespace murasaki {

BusPropertyWatcher::BusPropertyWatcher(const QDBusConnection &bus, QString service, QString path, QString interface,
                                       QString property, QStringList refreshSignals, QObject *parent)
    : QObject(parent)
    , m_bus(bus)
    , m_service(std::move(service))
    , m_path(std::move(path))
    , m_interface(std::move(interface))
    , m_property(std::move(property))
    , m_refreshSignals(std::move(refreshSignals))
{
}

void BusPropertyWatcher::start()
{
    if (!m_bus.isConnected())
        return;
    m_watcher = new QDBusServiceWatcher(m_service, m_bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    connect(m_watcher, &QDBusServiceWatcher::serviceOwnerChanged, this, &BusPropertyWatcher::onNameOwnerChanged);
    // Any sender: the signals come from whoever owns the name, now or later.
    m_bus.connect(m_service, m_path, u"org.freedesktop.DBus.Properties"_s, u"PropertiesChanged"_s, this,
                  SLOT(refresh()));
    for (const QString &signal : std::as_const(m_refreshSignals))
        m_bus.connect(m_service, m_path, m_interface, signal, this, SLOT(refresh()));
    refresh();
}

void BusPropertyWatcher::onNameOwnerChanged(const QString &, const QString &, const QString &newOwner)
{
    if (newOwner.isEmpty())
        set(false, {});
    else
        refresh();
}

void BusPropertyWatcher::refresh()
{
    // Asking a name nobody owns could start the service (UPower and gamemoded are both activatable).
    const auto iface = m_bus.interface();
    if (!iface || !iface->isServiceRegistered(m_service)) {
        set(false, {});
        return;
    }
    QDBusMessage call = QDBusMessage::createMethodCall(m_service, m_path, u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    call.setArguments({m_interface, m_property});
    call.setAutoStartService(false);
    const quint64 request = ++m_request;
    auto *watcher = new QDBusPendingCallWatcher(m_bus.asyncCall(call, 2000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        if (request != m_request)
            return; // a newer read superseded this one
        const QDBusPendingReply<QDBusVariant> reply = *done;
        if (reply.isError())
            set(false, {});
        else
            set(true, reply.value().variant());
    });
}

void BusPropertyWatcher::set(bool available, const QVariant &value)
{
    if (available == m_available && value == m_value)
        return;
    m_available = available;
    m_value = value;
    emit changed();
}

GameModeWatcher::GameModeWatcher(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_property(bus, u"com.feralinteractive.GameMode"_s, u"/com/feralinteractive/GameMode"_s,
                 u"com.feralinteractive.GameMode"_s, u"ClientCount"_s, {u"GameRegistered"_s, u"GameUnregistered"_s})
{
    connect(&m_property, &BusPropertyWatcher::changed, this, [this] {
        if (std::exchange(m_last, active()) != m_last) // only a change of the answer
            emit changed();
    });
}

PowerWatcher::PowerWatcher(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_property(bus, u"org.freedesktop.UPower"_s, u"/org/freedesktop/UPower"_s, u"org.freedesktop.UPower"_s,
                 u"OnBattery"_s)
{
    connect(&m_property, &BusPropertyWatcher::changed, this, [this] {
        if (std::exchange(m_last, onBattery()) != m_last)
            emit changed();
    });
}

PowerProfileWatcher::PowerProfileWatcher(const QDBusConnection &bus, QObject *parent)
    : QObject(parent)
    , m_current(bus, u"org.freedesktop.UPower.PowerProfiles"_s, u"/org/freedesktop/UPower/PowerProfiles"_s,
                u"org.freedesktop.UPower.PowerProfiles"_s, u"ActiveProfile"_s)
    , m_legacy(bus, u"net.hadess.PowerProfiles"_s, u"/net/hadess/PowerProfiles"_s, u"net.hadess.PowerProfiles"_s,
               u"ActiveProfile"_s)
{
    connect(&m_current, &BusPropertyWatcher::changed, this, &PowerProfileWatcher::update);
    connect(&m_legacy, &BusPropertyWatcher::changed, this, &PowerProfileWatcher::update);
}

bool PowerProfileWatcher::powerSaver() const
{
    const BusPropertyWatcher &source = m_current.available() ? m_current : m_legacy;
    return source.available() && source.value().toString() == QLatin1String("power-saver");
}

void PowerProfileWatcher::update()
{
    if (std::exchange(m_last, powerSaver()) != m_last)
        emit changed();
}

} // namespace murasaki
