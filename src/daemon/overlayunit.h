#pragma once

#include <QObject>
#include <QString>

namespace murasaki {

// Starts and stops the process that draws the desktop pets. The daemon asks for it when the first pet is added
// and lets it go when the last one is removed. Behind an interface so tests (and sessions without systemd) can
// swap it.
class OverlayUnit : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    virtual void start() = 0;
    virtual void stop() = 0;
};

// How SystemdOverlayUnit starts the overlay: through the user's systemd when it is on the session bus, otherwise by
// D-Bus activation of the overlay's own name (the service file the Flatpak exports; a bus that has none just refuses).
enum class OverlayStartPath { Systemd, Activation };
OverlayStartPath chooseOverlayStartPath(bool systemdOnBus);

// Drives murasaki-overlay.service in the user's systemd instance, which exports org.freedesktop.systemd1 on the
// session bus. When that name is absent (a private test bus, a session without systemd, the Flatpak sandbox) start()
// asks the bus to activate the overlay's service name instead; stop() then does nothing.
class SystemdOverlayUnit : public OverlayUnit
{
    Q_OBJECT

public:
    explicit SystemdOverlayUnit(QString unit = QStringLiteral("murasaki-overlay.service"), QObject *parent = nullptr);

    void start() override { call(QStringLiteral("StartUnit")); }
    void stop() override { call(QStringLiteral("StopUnit")); }

private:
    void call(const QString &method);

    QString m_unit;
    bool m_warned = false;
};

} // namespace murasaki
