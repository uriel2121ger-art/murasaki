#pragma once

#include "pointersource.h"

#include <QByteArray>
#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QTimer>

#include <optional>

namespace murasaki {

// Hyprland: the cursor through the request socket (`j/cursorpos`, what `hyprctl cursorpos -j` sends), polled while wanted. The position is
// in the layout's global logical coordinates, which is what PointerSource::moved() carries. One request at a time (a slow Hyprland is not
// piled up), one connection per request as Hyprland closes it after the reply, the same shape as HyprlandMonitor's queries.
class HyprlandPointerSource : public PointerSource
{
    Q_OBJECT

public:
    static constexpr int kDefaultIntervalMs = 34; // just under 30 Hz
    static constexpr int kRequestTimeoutMs = 500;
    static constexpr int kFailuresBeforeUnavailable = 10;
    static constexpr int kDefaultGiveUpWindowMs = 3000; // a Hyprland reload or hiccup shorter than this does not end the global level

    // `socketDir`: the directory with .socket.sock (see socketDir()).
    explicit HyprlandPointerSource(QString socketDir, int intervalMs = kDefaultIntervalMs, QObject *parent = nullptr);

    // $XDG_RUNTIME_DIR/hypr/<signature> (Hyprland >= 0.40) or /tmp/hypr/<signature>; empty if neither has the sockets. The same rule as
    // HyprlandMonitor::socketDir (which lives in the surface's monitor library, not linked here).
    static QString socketDir(const QProcessEnvironment &env);

    // The reply of `j/cursorpos` ({"x": 1234, "y": 567}, numbers integer or fractional) or of plain `cursorpos` ("1234, 567"); nothing for
    // anything else, a non-finite number or a value beyond +-1e6.
    static std::optional<QPointF> parseCursor(const QByteArray &reply);
    // Whether `path` belongs to the current user (the /tmp/hypr fallback is in a world-writable directory).
    static bool ownedByCurrentUser(const QString &path);
    // Gives up (unavailable) only after at least kFailuresBeforeUnavailable failures in a row spread over this long without one success.
    void setGiveUpWindowMs(int ms) { m_giveUpWindowMs = ms; }

    Level level() const override { return Level::Global; }
    void wanted(bool on) override;
    bool isRunning() const { return m_timer.isActive(); }
    // One request now (the timer calls this; tests too). A request still in flight makes it a no-op.
    void poll();

private:
    void finish(const QByteArray &reply, bool ok);

    QString m_dir;
    QTimer m_timer;
    bool m_inFlight = false;
    int m_failures = 0;
    int m_giveUpWindowMs = kDefaultGiveUpWindowMs;
    QElapsedTimer m_failingSince;
    bool m_gaveUp = false;
    std::optional<QPointF> m_last;
};

} // namespace murasaki
