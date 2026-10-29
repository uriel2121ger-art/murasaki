#include "dbusbackend.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>

using namespace Qt::StringLiterals;

namespace murasaki {

bool DBusBackend::Reply::isInvalidArgs() const
{
    return error == u"org.freedesktop.DBus.Error.InvalidArgs"_s;
}

bool DBusBackend::Reply::isFailed() const
{
    return error == u"org.freedesktop.DBus.Error.Failed"_s;
}

SessionBusBackend::SessionBusBackend(QString service, QString path, QString interface, QStringList signalNames,
                                     QObject *parent)
    : DBusBackend(parent)
    , m_service(std::move(service))
    , m_path(std::move(path))
    , m_interface(std::move(interface))
    , m_signalNames(std::move(signalNames))
{
}

void SessionBusBackend::start()
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    auto *watcher = new QDBusServiceWatcher(m_service, bus, QDBusServiceWatcher::WatchForOwnerChange, this);
    // One handler for appearing, vanishing and a new process taking over the name: a replacement is reported as
    // down then up so clients drop what they knew about the old owner and fetch everything again.
    connect(watcher, &QDBusServiceWatcher::serviceOwnerChanged, this,
            [this](const QString &, const QString &oldOwner, const QString &newOwner) {
                if (!oldOwner.isEmpty())
                    emit availabilityChanged(false);
                if (!newOwner.isEmpty())
                    emit availabilityChanged(true);
            });
    for (const QString &name : std::as_const(m_signalNames))
        bus.connect(m_service, m_path, m_interface, name, this, SLOT(onSignal(QDBusMessage)));

    if (!bus.isConnected()) {
        emit availabilityChanged(false);
        return;
    }
    QDBusMessage hasOwner = QDBusMessage::createMethodCall(u"org.freedesktop.DBus"_s, u"/org/freedesktop/DBus"_s,
                                                           u"org.freedesktop.DBus"_s, u"NameHasOwner"_s);
    hasOwner.setArguments({m_service});
    auto *pending = new QDBusPendingCallWatcher(bus.asyncCall(hasOwner), this);
    connect(pending, &QDBusPendingCallWatcher::finished, this, [this](QDBusPendingCallWatcher *done) {
        done->deleteLater();
        const QDBusPendingReply<bool> reply = *done;
        // The watcher may already have reported the service; repeating the state is harmless.
        emit availabilityChanged(reply.isValid() && reply.value());
    });
}

namespace {

DBusBackend::Reply toReply(const QDBusMessage &message, bool unwrapVariant)
{
    DBusBackend::Reply reply;
    if (message.type() == QDBusMessage::ErrorMessage) {
        reply.error = message.errorName();
        reply.message = message.errorMessage();
        return reply;
    }
    reply.ok = true;
    if (!message.arguments().isEmpty()) {
        reply.value = message.arguments().first();
        if (unwrapVariant && reply.value.canConvert<QDBusVariant>())
            reply.value = reply.value.value<QDBusVariant>().variant();
    }
    return reply;
}

void send(QObject *owner, const QDBusMessage &message, bool unwrapVariant, DBusBackend::Callback callback)
{
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), owner);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, owner,
                     [watcher, unwrapVariant, callback = std::move(callback)] {
                         watcher->deleteLater();
                         const QDBusPendingReply<> reply = *watcher;
                         callback(toReply(reply.reply(), unwrapVariant));
                     });
}

} // namespace

void SessionBusBackend::call(const QString &method, const QVariantList &args, Callback callback)
{
    QDBusMessage message = QDBusMessage::createMethodCall(m_service, m_path, m_interface, method);
    message.setArguments(args);
    send(this, message, false, std::move(callback));
}

void SessionBusBackend::property(const QString &name, Callback callback)
{
    QDBusMessage message = QDBusMessage::createMethodCall(m_service, m_path,
                                                          u"org.freedesktop.DBus.Properties"_s, u"Get"_s);
    message.setArguments({m_interface, name});
    send(this, message, true, std::move(callback));
}

void SessionBusBackend::onSignal(const QDBusMessage &message)
{
    emit signalReceived(message.member(), message.arguments());
}

} // namespace murasaki
