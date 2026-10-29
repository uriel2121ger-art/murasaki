#include "overlayunit.h"

#include "constants.h"

#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

using namespace Qt::StringLiterals;

namespace murasaki {

SystemdOverlayUnit::SystemdOverlayUnit(QString unit, QObject *parent)
    : OverlayUnit(parent)
    , m_unit(std::move(unit))
{
}

OverlayStartPath chooseOverlayStartPath(bool systemdOnBus)
{
    return systemdOnBus ? OverlayStartPath::Systemd : OverlayStartPath::Activation;
}

void SystemdOverlayUnit::call(const QString &method)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    const auto name = u"org.freedesktop.systemd1"_s;
    // isServiceRegistered never activates the name, so a bus without systemd is left alone.
    const bool systemd = bus.isConnected() && bus.interface() && bus.interface()->isServiceRegistered(name);
    if (chooseOverlayStartPath(systemd) == OverlayStartPath::Activation) {
        if (bus.isConnected() && method == u"StartUnit"_s) {
            QDBusMessage activate = QDBusMessage::createMethodCall(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s,
                                                                   u"org.freedesktop.DBus"_s, u"StartServiceByName"_s);
            activate.setArguments({QString(kOverlayDBusService), 0u});
            auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(activate, 10000), this);
            connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *done) {
                done->deleteLater();
                const QDBusPendingReply<uint> reply = *done;
                if (reply.isError() && !m_warned) {
                    qInfo("murasakid: no systemd user instance and the bus cannot activate murasaki-overlay (%s); start it yourself",
                          qPrintable(reply.error().message()));
                    m_warned = true;
                }
            });
        }
        return;
    }
    QDBusMessage message = QDBusMessage::createMethodCall(name, u"/org/freedesktop/systemd1"_s,
                                                          u"org.freedesktop.systemd1.Manager"_s, method);
    message.setArguments({m_unit, u"replace"_s});
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 10000), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, method](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        const QDBusPendingReply<QDBusObjectPath> reply = *done;
        // Stopping a unit that is not loaded is normal (the overlay was started by hand).
        if (reply.isError() && reply.error().name() != u"org.freedesktop.systemd1.NoSuchUnit"_s)
            qWarning("murasakid: %s %s failed: %s", qPrintable(method), qPrintable(m_unit),
                     qPrintable(reply.error().message()));
    });
}

} // namespace murasaki
