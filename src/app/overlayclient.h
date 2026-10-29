#pragma once

#include "dbusbackend.h"

#include <QObject>
#include <QVariantList>
#include <QString>

namespace murasaki {

// What the GUI wants to know about murasaki-overlay (the process that draws the desktop pets): whether it runs and
// how its global shortcut is bound. The overlay is D-Bus activatable, so nothing here may call it while it is not
// running: that would start it. `shortcutBackend` is "portal", "kglobalaccel" or empty (none bound, or the
// overlay is not running); it is read when the overlay appears and again on refresh().
class OverlayClient : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available NOTIFY availableChanged)
    Q_PROPERTY(QString defaultShortcut READ defaultShortcut CONSTANT)
    Q_PROPERTY(QString shortcutBackend READ shortcutBackend NOTIFY shortcutBackendChanged)
    // The key bound for each global action: {"toggle-arrange": "Meta+Shift+P", "toggle-hidden": ..., "rescue": ...}; an action that got no key
    // (taken by another application, or cleared) has an empty text. Empty while the overlay is not running.
    Q_PROPERTY(QVariantMap shortcutTriggers READ shortcutTriggers NOTIFY shortcutTriggersChanged)
    // "off", "starting" (still registering: the portal may be asking the user), "bound" or "failed"; empty for an overlay older than revision 12.
    Q_PROPERTY(QString shortcutState READ shortcutState NOTIFY shortcutStateChanged)
    // One entry per placement the overlay knows (its Windows() call): petId, shown, failures, blocked, error, ...
    // Empty while the overlay is not running.
    Q_PROPERTY(QVariantList windows READ windows NOTIFY windowsChanged)

public:
    // Takes ownership of the backend.
    explicit OverlayClient(DBusBackend *backend, QObject *parent = nullptr);
    // Watches the real overlay on the session bus.
    explicit OverlayClient(QObject *parent = nullptr);

    bool available() const { return m_available; }
    // The key the arrange shortcut asks for, as text.
    QString defaultShortcut() const;
    QString shortcutBackend() const { return m_shortcutBackend; }
    QVariantMap shortcutTriggers() const { return m_shortcutTriggers; }
    QString shortcutState() const { return m_shortcutState; }
    QVariantList windows() const { return m_windows; }

    void start();
    Q_INVOKABLE void refresh();
    // "Try again" for a pet the overlay gave up on.
    Q_INVOKABLE void resetFailures(const QString &petId);
    // The test button of a pet's sound: plays it once now (whatever its mode), and stops every sound. Nothing happens while the overlay is not running (it would be started).
    Q_INVOKABLE void playPetSound(const QString &petId);
    // Shows a state of the pet on the desktop for a few seconds (PreviewPetState).
    Q_INVOKABLE void previewPetState(const QString &petId, const QString &state);
    Q_INVOKABLE void stopPetSounds();

signals:
    void availableChanged();
    void shortcutBackendChanged();
    void shortcutTriggersChanged();
    void shortcutStateChanged();
    void windowsChanged();

private:
    void onAvailability(bool up);
    void setShortcutBackend(const QString &backend);
    void setShortcutTriggers(const QVariantMap &triggers);
    void setShortcutState(const QString &state);
    void setWindows(const QVariantList &windows);
    void refreshWindows();

    DBusBackend *m_backend;
    bool m_available = false;
    quint64 m_generation = 0;
    QString m_shortcutBackend;
    QVariantMap m_shortcutTriggers;
    QString m_shortcutState;
    QVariantList m_windows;
};

} // namespace murasaki
