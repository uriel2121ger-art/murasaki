#include "shortcut/daemonlauncher.h"

#include "constants.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>

namespace murasaki::shortcut {

void requestLaunch(const QString &output, const QString &desktop, const QString &itemDir, const QString &key)
{
    QDBusMessage call = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath), QString(kDBusInterface), QStringLiteral("LaunchShortcut"));
    call.setArguments({output.isEmpty() ? QStringLiteral("*") : output, desktop.isEmpty() ? QStringLiteral("*") : desktop, itemDir, key});
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(call, 5000));
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, watcher, [watcher, key] {
        const QDBusPendingReply<bool> reply = *watcher;
        if (reply.isError()) {
            qWarning("murasaki: shortcut %s: the daemon said: %s", qPrintable(key.left(64)), qPrintable(reply.error().message()));
        }
        watcher->deleteLater();
    });
}

} // namespace murasaki::shortcut
