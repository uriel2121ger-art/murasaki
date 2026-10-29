#pragma once

#include <QDBusContext>
#include <QObject>
#include <QString>

#include <functional>

namespace murasaki {

class OverlayController;

// D-Bus facade of murasaki-overlay (docs/dbus-api.md, "Overlay API").
class OverlayService : public QObject, protected QDBusContext
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.uriel2121ger_art.Murasaki.Overlay1")
    // "portal", "kglobalaccel", or empty when no global shortcut is bound.
    Q_PROPERTY(QString ShortcutBackend READ shortcutBackend)
    // JSON object: the key bound for each global action ("toggle-arrange", "toggle-hidden", "rescue"), empty for one that got none.
    Q_PROPERTY(QString ShortcutTriggers READ shortcutTriggers)
    // "off" (MURASAKI_NO_SHORTCUT), "starting" (a backend is still registering: the portal may be asking the user), "bound" or "failed".
    Q_PROPERTY(QString ShortcutState READ shortcutState)
    // Whether the pets are paused by the session (locked or screen off).
    Q_PROPERTY(bool SessionPaused READ sessionPaused)

public:
    explicit OverlayService(OverlayController *controller, QObject *parent = nullptr);

    std::function<QString()> shortcutBackendProvider;
    std::function<QString()> shortcutTriggersProvider;
    std::function<QString()> shortcutStateProvider;
    std::function<void()> quitHandler;

    QString shortcutBackend() const { return shortcutBackendProvider ? shortcutBackendProvider() : QString(); }
    QString shortcutTriggers() const { return shortcutTriggersProvider ? shortcutTriggersProvider() : QStringLiteral("{}"); }
    QString shortcutState() const { return shortcutStateProvider ? shortcutStateProvider() : QStringLiteral("off"); }
    bool sessionPaused() const;

public slots:
    // JSON array: one entry per placement of the daemon, with whether it has a window and where.
    Q_SCRIPTABLE QString Windows() const;
    // Flips the arrange mode (through the daemon), for sessions without a global shortcut.
    Q_SCRIPTABLE void ToggleArrange();
    // "Try again" for a pet that was given up on (see Windows(): blocked); forgets its failures and crashes.
    Q_SCRIPTABLE void ResetFailures(const QString &petId);
    // The pet's sound once, as its mode would play it (the test button of the GUI and, later, the triggers); an error for a pet without a sound.
    Q_SCRIPTABLE void PlayPetSound(const QString &petId);
    // Shows a state of a pet's states for three seconds (the GUI's try button); an error for an unknown pet, one without states, or a state that is not idle, grabbed, moving or released.
    Q_SCRIPTABLE void PreviewPetState(const QString &petId, const QString &state);
    // Stops every sound of every pet.
    Q_SCRIPTABLE void StopPetSounds();
    // Revision 18: stops what plays once (clicks, wraps, starts) and leaves the pets in the mode continuous playing; the automation's "silence the pets".
    Q_SCRIPTABLE void StopOneShotPetSounds();
    // Test hook, only with MURASAKI_OVERLAY_TEST_HOOKS=1 in the environment: applies the edit the pointer would
    // ("move" dx dy, "scale" factor, "resize" dwidth) to a pet's window; it is committed like a real one.
    Q_SCRIPTABLE void SimulateEdit(const QString &petId, const QString &kind, double a, double b);
    Q_SCRIPTABLE void Quit();

signals:
    // What Windows() returns changed (a pet appeared, failed, was blocked, ...).
    Q_SCRIPTABLE void WindowsChanged();
    // The global shortcuts finished registering (or failed): read ShortcutBackend, ShortcutTriggers and ShortcutState again.
    Q_SCRIPTABLE void ShortcutsChanged();

private:
    OverlayController *m_controller;
    QString m_lastWindows;
};

} // namespace murasaki
