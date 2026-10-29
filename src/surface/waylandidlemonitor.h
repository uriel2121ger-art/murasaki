#pragma once

#include "idlesource.h"

#include <QSocketNotifier>
#include <QString>

#include <memory>

struct wl_display;
struct wl_registry;
struct wl_seat;
struct ext_idle_notifier_v1;
struct ext_idle_notification_v1;

namespace murasaki {

// ext-idle-notify-v1: the compositor tells when the seat has had no input for `timeoutMs` (idled) and when it has some again (resumed). It speaks to the compositor over a
// connection of its own, driven by the Qt event loop. Compositors that do not offer the protocol leave the idle unknown.
//
// It asks with get_idle_notification, which also counts an idle inhibitor (a video playing in a player that inhibits, a presentation) as activity: someone watching is not
// away. Murasaki's own hosts take no inhibitor (mpv draws through the render API without a window; nothing here calls an inhibit interface).
class WaylandIdleMonitor : public IdleSource
{
    Q_OBJECT

public:
    // `display`: a socket name; empty means WAYLAND_DISPLAY.
    explicit WaylandIdleMonitor(int timeoutMs, QString display = {}, QObject *parent = nullptr);
    ~WaylandIdleMonitor() override;

    void start() override;
    int afterMs() const override { return m_timeoutMs; }

    // Called by the protocol callbacks (C functions in the .cpp).
    void bindGlobal(quint32 name, const char *interface, quint32 version);
    void removeGlobal(quint32 name);

private:
    void dispatch();
    void fail(const char *why);

    int m_timeoutMs;
    QString m_displayName;
    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    wl_seat *m_seat = nullptr;
    quint32 m_seatName = 0;
    ext_idle_notifier_v1 *m_notifier = nullptr;
    ext_idle_notification_v1 *m_notification = nullptr;
    std::unique_ptr<QSocketNotifier> m_socket;
};

} // namespace murasaki
