#pragma once

#include <QObject>
#include <QString>
#include <QVariant>

#include <functional>

class QDBusMessage;

namespace murasaki {

// The slice of a session-bus object the GUI clients need, behind an interface so tests can fake the daemon.
// Calls are asynchronous: the callback runs from the event loop, never from inside call()/property().
class DBusBackend : public QObject
{
    Q_OBJECT

public:
    struct Reply
    {
        bool ok = false;
        QString error;   // D-Bus error name when !ok, for example org.freedesktop.DBus.Error.InvalidArgs
        QString message; // human readable error text
        QVariant value;  // first return value (a method's first out argument, or the unwrapped property)

        bool isInvalidArgs() const;
        bool isFailed() const;
    };
    using Callback = std::function<void(const Reply &)>;

    using QObject::QObject;

    // Starts watching the service; availabilityChanged() reports the initial state too.
    virtual void start() = 0;
    virtual void call(const QString &method, const QVariantList &args, Callback callback) = 0;
    virtual void property(const QString &name, Callback callback) = 0;

signals:
    void availabilityChanged(bool available);
    void signalReceived(const QString &name, const QVariantList &args);
};

// Talks to a real service on the session bus.
class SessionBusBackend : public DBusBackend
{
    Q_OBJECT

public:
    SessionBusBackend(QString service, QString path, QString interface, QStringList signalNames,
                      QObject *parent = nullptr);

    void start() override;
    void call(const QString &method, const QVariantList &args, Callback callback) override;
    void property(const QString &name, Callback callback) override;

private slots:
    void onSignal(const QDBusMessage &message);

private:
    QString m_service;
    QString m_path;
    QString m_interface;
    QStringList m_signalNames;
};

} // namespace murasaki
