#pragma once

#include <QDBusConnection>
#include <QObject>
#include <QStringList>
#include <QVariant>

class QDBusServiceWatcher;

namespace murasaki {

// Follows one property of a D-Bus service that may or may not exist: GameMode (session bus) and UPower (system
// bus) are optional, so their absence is a state, not an error. The property is read only while the name has an
// owner (a call to a name nobody owns could start the service), again when it appears, on
// org.freedesktop.DBus.Properties.PropertiesChanged and on any of the extra signals. `available()` is false while
// the service is away or the read failed.
class BusPropertyWatcher : public QObject
{
    Q_OBJECT

public:
    BusPropertyWatcher(const QDBusConnection &bus, QString service, QString path, QString interface, QString property,
                       QStringList refreshSignals = {}, QObject *parent = nullptr);

    // Subscribes and reads once. Call after connecting to changed().
    void start();

    bool available() const { return m_available; }
    QVariant value() const { return m_value; }

signals:
    // `available` or `value` changed.
    void changed();

private slots:
    void refresh();
    void onNameOwnerChanged(const QString &name, const QString &oldOwner, const QString &newOwner);

private:
    void set(bool available, const QVariant &value);

    QDBusConnection m_bus;
    QString m_service;
    QString m_path;
    QString m_interface;
    QString m_property;
    QStringList m_refreshSignals;
    QDBusServiceWatcher *m_watcher = nullptr;
    bool m_available = false;
    QVariant m_value;
    quint64 m_request = 0;
};

// GameMode (Feral's gamemoded, session bus): whether any game has asked for it (ClientCount > 0).
class GameModeWatcher : public QObject
{
    Q_OBJECT

public:
    explicit GameModeWatcher(const QDBusConnection &bus, QObject *parent = nullptr);
    void start() { m_property.start(); }
    bool active() const { return m_property.available() && m_property.value().toInt() > 0; }

signals:
    void changed();

private:
    BusPropertyWatcher m_property;
    bool m_last = false;
};

// UPower (system bus): whether the machine runs on battery (OnBattery).
class PowerWatcher : public QObject
{
    Q_OBJECT

public:
    explicit PowerWatcher(const QDBusConnection &bus, QObject *parent = nullptr);
    void start() { m_property.start(); }
    bool onBattery() const { return m_property.available() && m_property.value().toBool(); }

signals:
    void changed();

private:
    BusPropertyWatcher m_property;
    bool m_last = false;
};

// power-profiles-daemon (system bus): whether the active profile is "power-saver". Newer versions export it as
// org.freedesktop.UPower.PowerProfiles, older ones as net.hadess.PowerProfiles (still kept as an alias); both are watched and the
// newer one wins while it is there. Absent service: not saving power.
class PowerProfileWatcher : public QObject
{
    Q_OBJECT

public:
    explicit PowerProfileWatcher(const QDBusConnection &bus, QObject *parent = nullptr);
    void start()
    {
        m_current.start();
        m_legacy.start();
    }
    bool powerSaver() const;

signals:
    void changed();

private:
    void update();

    BusPropertyWatcher m_current;
    BusPropertyWatcher m_legacy;
    bool m_last = false;
};

} // namespace murasaki
