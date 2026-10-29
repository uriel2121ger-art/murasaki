#include "waylandidlemonitor.h"

#include "ext-idle-notify-v1-client-protocol.h"

#include <wayland-client.h>

#include <cstring>

namespace murasaki {

namespace {

void notificationIdled(void *data, ext_idle_notification_v1 *)
{
    static_cast<WaylandIdleMonitor *>(data)->setIdle(true, true);
}
void notificationResumed(void *data, ext_idle_notification_v1 *)
{
    static_cast<WaylandIdleMonitor *>(data)->setIdle(true, false);
}
const ext_idle_notification_v1_listener kNotificationListener = {notificationIdled, notificationResumed};

void registryGlobal(void *data, wl_registry *, uint32_t name, const char *interface, uint32_t version)
{
    static_cast<WaylandIdleMonitor *>(data)->bindGlobal(name, interface, version);
}
void registryGlobalRemove(void *data, wl_registry *, uint32_t name)
{
    static_cast<WaylandIdleMonitor *>(data)->removeGlobal(name);
}
const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

} // namespace

WaylandIdleMonitor::WaylandIdleMonitor(int timeoutMs, QString display, QObject *parent)
    : IdleSource(parent)
    , m_timeoutMs(timeoutMs)
    , m_displayName(std::move(display))
{
}

WaylandIdleMonitor::~WaylandIdleMonitor()
{
    m_socket.reset();
    if (m_notification)
        ext_idle_notification_v1_destroy(m_notification);
    if (m_notifier)
        ext_idle_notifier_v1_destroy(m_notifier);
    if (m_seat)
        wl_seat_destroy(m_seat);
    if (m_registry)
        wl_registry_destroy(m_registry);
    if (m_display)
        wl_display_disconnect(m_display);
}

void WaylandIdleMonitor::start()
{
    if (m_display)
        return;
    m_display = wl_display_connect(m_displayName.isEmpty() ? nullptr : m_displayName.toUtf8().constData());
    if (!m_display) {
        qWarning("murasaki-surface: cannot connect to the Wayland display for the idle time");
        return;
    }
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &kRegistryListener, this);
    if (wl_display_roundtrip(m_display) < 0) {
        fail("the compositor did not answer");
        return;
    }
    if (!m_notifier || !m_seat) {
        qInfo("murasaki-surface: the compositor does not offer ext-idle-notify (or a seat): the idle time stays unknown");
        fail(nullptr);
        return;
    }
    m_notification = ext_idle_notifier_v1_get_idle_notification(m_notifier, uint32_t(m_timeoutMs), m_seat);
    ext_idle_notification_v1_add_listener(m_notification, &kNotificationListener, this);
    if (wl_display_roundtrip(m_display) < 0) {
        fail("the compositor refused the idle notification");
        return;
    }
    m_socket = std::make_unique<QSocketNotifier>(wl_display_get_fd(m_display), QSocketNotifier::Read);
    connect(m_socket.get(), &QSocketNotifier::activated, this, &WaylandIdleMonitor::dispatch);
    setIdle(true, idle()); // the notification exists: awake until it says otherwise (it may have said so already)
}

void WaylandIdleMonitor::bindGlobal(quint32 name, const char *interface, quint32 version)
{
    if (!std::strcmp(interface, wl_seat_interface.name) && !m_seat) {
        m_seat = static_cast<wl_seat *>(wl_registry_bind(m_registry, name, &wl_seat_interface, 1));
        m_seatName = name;
    } else if (!std::strcmp(interface, ext_idle_notifier_v1_interface.name) && !m_notifier) {
        m_notifier = static_cast<ext_idle_notifier_v1 *>(wl_registry_bind(m_registry, name, &ext_idle_notifier_v1_interface, 1));
        Q_UNUSED(version)
    }
}

void WaylandIdleMonitor::removeGlobal(quint32 name)
{
    if (m_seat && name == m_seatName)
        fail("the seat went away"); // the notification was the seat's: nothing more is known
}

void WaylandIdleMonitor::dispatch()
{
    if (!m_display)
        return;
    // The documented read loop: dispatch what is queued, then read what arrived.
    while (wl_display_prepare_read(m_display) != 0)
        wl_display_dispatch_pending(m_display);
    wl_display_flush(m_display);
    if (wl_display_read_events(m_display) < 0 || wl_display_dispatch_pending(m_display) < 0) {
        fail("the connection broke");
        return;
    }
    wl_display_flush(m_display);
}

void WaylandIdleMonitor::fail(const char *why)
{
    if (why)
        qWarning("murasaki-surface: idle time stopped: %s", why);
    m_socket.reset();
    setIdle(false, false); // unknown (until the surface restarts: once it has answered, the daemon waits for it and does not take logind's)
}

} // namespace murasaki
