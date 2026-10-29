#pragma once

#include <QDBusConnection>
#include <QObject>

namespace murasaki {

// Whether the session is locked or the screen is off, i.e. whether the pets should stop playing. Fullscreen apps
// are deliberately not an input: the pets stay on top of them and keep playing.
class SessionWatch : public QObject
{
    Q_OBJECT

public:
    using QObject::QObject;

    // Follows org.freedesktop.ScreenSaver (KDE's lock screen, GNOME, and the others that implement it).
    void watchScreenSaver(const QDBusConnection &bus);

    void setLocked(bool locked);
    // True when the compositor shows no output at all.
    void setScreenOff(bool off);

    bool locked() const { return m_locked; }
    bool screenOff() const { return m_screenOff; }
    bool paused() const { return m_locked || m_screenOff; }

signals:
    void pausedChanged(bool paused);

private slots:
    void onActiveChanged(bool active) { setLocked(active); }

private:
    void update(bool wasPaused);

    bool m_locked = false;
    bool m_screenOff = false;
};

} // namespace murasaki
