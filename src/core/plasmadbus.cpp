#include "plasmadbus.h"

#include "constants.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QJsonDocument>
#include <QJsonObject>

using namespace Qt::StringLiterals;

namespace murasaki::plasma {
namespace {

constexpr int kCallTimeoutMs = 15000;

bool call(const QDBusMessage &message, QVariant *reply, QString *error)
{
    const QDBusMessage answer = QDBusConnection::sessionBus().call(message, QDBus::Block, kCallTimeoutMs);
    if (answer.type() == QDBusMessage::ErrorMessage) {
        *error = answer.errorName() == u"org.freedesktop.DBus.Error.ServiceUnknown"_s
            ? u"%1 is not running"_s.arg(message.service())
            : answer.errorMessage();
        return false;
    }
    if (reply)
        *reply = answer.arguments().value(0);
    return true;
}

} // namespace

bool DBusBackend::evaluate(const QString &script, QString *output, QString *error)
{
    QDBusMessage message = QDBusMessage::createMethodCall(u"org.kde.plasmashell"_s, u"/PlasmaShell"_s,
                                                          u"org.kde.PlasmaShell"_s, u"evaluateScript"_s);
    message.setArguments({script});
    QVariant reply;
    if (!call(message, &reply, error))
        return false;
    *output = reply.toString();
    return true;
}

bool DBusBackend::presenter(QString *value, QString *error)
{
    const QDBusMessage message = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                                QString(kDBusInterface), u"Settings"_s);
    QVariant reply;
    if (!call(message, &reply, error))
        return false;
    *value = QJsonDocument::fromJson(reply.toString().toUtf8()).object().value(u"presenter"_s).toString();
    return true;
}

bool DBusBackend::setPresenter(const QString &presenter, QString *error)
{
    QDBusMessage message = QDBusMessage::createMethodCall(QString(kDBusService), QString(kDBusPath),
                                                          QString(kDBusInterface), u"SetSettings"_s);
    message.setArguments({QString::fromUtf8(QJsonDocument(QJsonObject{{u"presenter"_s, presenter}}).toJson(
        QJsonDocument::Compact))});
    return call(message, nullptr, error);
}

} // namespace murasaki::plasma
