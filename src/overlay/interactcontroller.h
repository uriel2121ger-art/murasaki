#pragma once

#include "dbusbackend.h"

#include <QElapsedTimer>
#include <QHash>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QSet>
#include <QSizeF>
#include <QString>
#include <QTimer>

#include <functional>
#include <map>
#include <memory>

namespace murasaki {

// The buttons of an interaction pointer event, the bits the host expects (bit 0 left, 1 right, 2 middle).
inline constexpr quint32 kInteractLeft = 1u << 0;
inline constexpr quint32 kInteractRight = 1u << 1;
inline constexpr quint32 kInteractMiddle = 1u << 2;

// What a catcher window saw: the pointer inside the output (logical pixels of the window, which covers the output) and the size of the output, the
// buttons held and the wheel movement of this event (1/8 degree, 120 per notch).
struct InteractPointer {
    QPointF position;
    QSizeF size;
    quint32 buttons = 0;
    QPoint wheel;
    bool operator==(const InteractPointer &other) const = default;
};

// The position kept inside the output, so that a drag that leaves it still points at its edge; an empty size is kept as it is.
InteractPointer clampedInteractPointer(InteractPointer pointer);

// The window that catches the pointer above the desktop on one output. It draws nothing opaque, covers the whole output and tells what it sees;
// the controller owns it and deletes it when the mode ends. The tests fake it.
class InteractSurface
{
public:
    virtual ~InteractSurface() = default;
    virtual QString output() const = 0;

    std::function<void(const InteractPointer &)> onPointer;
    std::function<void()> onEscape;
};

// The interaction with the wallpaper (docs/dbus-api.md, "Interacting with the wallpaper"): while the daemon's WallpaperInteract is on, one catcher
// window per output whose wallpaper is hosted and interactive, and the pointer it sees goes to the host of that output through the surface's
// ReportHostPointer. The mode ends by itself (SetWallpaperInteract(false)) on Esc, after kInteractIdleMs without pointer input, when the session
// locks and when no output has an interactive wallpaper left. It knows nothing about windows: the factory makes them, which is also how the tests
// fake them.
class InteractController : public QObject
{
    Q_OBJECT

public:
    using SurfaceFactory = std::function<std::unique_ptr<InteractSurface>(const QString &output)>;

    // No input for this long ends the mode.
    static constexpr int kInteractIdleMs = 60'000;
    // The mode ends this long after it began whatever the pointer does (the idle timeout is reset by movement).
    static constexpr int kInteractHardCapMs = 5 * 60'000;
    // An output list without an interactive wallpaper ends the mode only if it stays so this long (a wallpaper change shows one for a moment).
    static constexpr int kInteractGraceMs = 1500;
    // Moves are sent at most this often (the last one is never lost); a button or the wheel goes at once.
    static constexpr int kMoveIntervalMs = 16;

    InteractController(DBusBackend *daemon, DBusBackend *surface, SurfaceFactory factory, QObject *parent = nullptr);
    ~InteractController() override;

    // The idle timeout (tests shorten it).
    void setIdleTimeoutMs(int ms) { m_idleMs = ms; }
    void setHardCapMs(int ms) { m_hardCapMs = ms; }
    void setGraceMs(int ms) { m_graceMs = ms; }
    // The session is locked or the screens are off: the mode ends, and cannot start while it lasts.
    void setSessionPaused(bool paused);
    // A global shortcut was pressed: "toggle-interact" flips the mode through the daemon; anything else is ignored.
    void onShortcut(const QString &actionId);
    // Flips the mode through the daemon (it announces the result).
    void toggle();

    bool active() const { return m_interact; }
    QStringList catcherOutputs() const;

private:
    void onDaemonAvailability(bool available);
    void onDaemonSignal(const QString &name, const QVariantList &args);
    void onSurfaceAvailability(bool available);
    void onSurfaceSignal(const QString &name);
    void setInteract(bool interact);
    void refreshOutputs();
    void applyOutputs(const QSet<QString> &wanted);
    void dropCatcher(const QString &output);
    void dropAllCatchers();
    void requestOff(const char *why);
    void onCatcherPointer(const QString &output, const InteractPointer &pointer);
    void send(const QString &output, const InteractPointer &pointer);
    void flushPending(const QString &output);
    void touchIdle();

    struct Catcher {
        std::unique_ptr<InteractSurface> surface;
        InteractPointer last;     // the last one sent
        bool sentAny = false;
        bool hasPending = false;
        InteractPointer pending;  // a move held back for the rate limit
        QElapsedTimer sinceSend;
        std::unique_ptr<QTimer> flush;
    };

    DBusBackend *m_daemon;
    DBusBackend *m_surface;
    SurfaceFactory m_factory;
    std::map<QString, Catcher> m_catchers;
    bool m_interact = false;
    bool m_sessionPaused = false;
    bool m_daemonUp = false;
    bool m_surfaceUp = false;
    quint64 m_refreshSeq = 0;
    int m_idleMs = kInteractIdleMs;
    int m_hardCapMs = kInteractHardCapMs;
    int m_graceMs = kInteractGraceMs;
    QTimer m_idle;
    QTimer m_hardCap;
    QTimer m_grace;
};

} // namespace murasaki
