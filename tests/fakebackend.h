#pragma once

#include "dbusbackend.h"

#include <QHash>
#include <QPair>
#include <QTimer>

#include <functional>
#include <utility>

// A scripted stand-in for the daemon or surface. Answers are delivered from the event loop like real D-Bus ones.
class FakeBackend : public murasaki::DBusBackend
{
public:
    using DBusBackend::DBusBackend;

    void start() override { ++startCount; }

    void call(const QString &method, const QVariantList &args, Callback callback) override
    {
        calls.append({method, args});
        const Reply reply = replies.value(method, Reply{false, QStringLiteral("org.freedesktop.DBus.Error.UnknownMethod"),
                                                        QStringLiteral("no reply scripted"), {}});
        deliver([callback = std::move(callback), reply] { callback(reply); });
    }

    void property(const QString &name, Callback callback) override
    {
        const Reply reply = properties.value(name, Reply{false, QStringLiteral("org.freedesktop.DBus.Error.UnknownProperty"),
                                                         QStringLiteral("no property"), {}});
        deliver([callback = std::move(callback), reply] { callback(reply); });
    }

    // While `hold` is set answers are parked; release() delivers them (in order) as if they arrived late.
    void release()
    {
        const auto parked = std::exchange(pending, {});
        for (const auto &fn : parked)
            fn();
    }

    void answer(const QString &method, const QVariant &value) { replies.insert(method, Reply{true, {}, {}, value}); }
    void answerJson(const QString &method, const QByteArray &json) { answer(method, QString::fromUtf8(json)); }
    void fail(const QString &method, const QString &error, const QString &message)
    {
        replies.insert(method, Reply{false, error, message, {}});
    }
    void setProperty(const QString &name, const QVariant &value) { properties.insert(name, Reply{true, {}, {}, value}); }
    void setAvailable(bool up) { emit availabilityChanged(up); }
    void emitSignal(const QString &name, const QVariantList &args = {}) { emit signalReceived(name, args); }

    void deliver(std::function<void()> fn)
    {
        if (hold)
            pending.append(std::move(fn));
        else
            QTimer::singleShot(0, this, std::move(fn));
    }

    int callCount(const QString &method) const
    {
        int n = 0;
        for (const auto &call : calls)
            n += call.first == method;
        return n;
    }

    bool hold = false;
    QList<std::function<void()>> pending;
    QHash<QString, Reply> replies;
    QHash<QString, Reply> properties;
    QList<QPair<QString, QVariantList>> calls;
    int startCount = 0;
};
