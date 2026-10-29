#include "sessionwatch.h"

#include <QDBusConnectionInterface>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

using namespace Qt::StringLiterals;

namespace murasaki {

void SessionWatch::watchScreenSaver(const QDBusConnection &busConnection)
{
    QDBusConnection bus = busConnection;
    if (!bus.isConnected())
        return;
    // KDE exports it at /ScreenSaver and /org/freedesktop/ScreenSaver, GNOME also as org.gnome.ScreenSaver. Any
    // sender: the lock screen is not always the name's owner.
    const std::pair<QString, QString> sources[] = {
        {u"/ScreenSaver"_s, u"org.freedesktop.ScreenSaver"_s},
        {u"/org/freedesktop/ScreenSaver"_s, u"org.freedesktop.ScreenSaver"_s},
        {u"/org/gnome/ScreenSaver"_s, u"org.gnome.ScreenSaver"_s},
    };
    for (const auto &[path, interface] : sources)
        bus.connect(QString(), path, interface, u"ActiveChanged"_s, this, SLOT(onActiveChanged(bool)));
    // Started while already locked? (Only if the service exists: asking must not activate anything.)
    if (bus.interface() && bus.interface()->isServiceRegistered(u"org.freedesktop.ScreenSaver"_s)) {
        QDBusMessage message = QDBusMessage::createMethodCall(u"org.freedesktop.ScreenSaver"_s,
                                                              u"/org/freedesktop/ScreenSaver"_s,
                                                              u"org.freedesktop.ScreenSaver"_s, u"GetActive"_s);
        auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message, 2000), this);
        connect(watcher, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *done) {
            done->deleteLater();
            const QDBusPendingReply<bool> reply = *done;
            if (reply.isValid())
                setLocked(reply.value());
        });
    }
}

void SessionWatch::setLocked(bool locked)
{
    if (locked == m_locked)
        return;
    const bool was = paused();
    m_locked = locked;
    update(was);
}

void SessionWatch::setScreenOff(bool off)
{
    if (off == m_screenOff)
        return;
    const bool was = paused();
    m_screenOff = off;
    update(was);
}

void SessionWatch::update(bool wasPaused)
{
    if (paused() != wasPaused)
        emit pausedChanged(paused());
}

} // namespace murasaki
