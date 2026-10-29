#include "x11monitor.h"

#include "x11util.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QScreen>

namespace murasaki {
namespace {

constexpr int kCoalesceMs = 100;
constexpr int kPollMs = 2000;

QList<X11Output> screensAsOutputs()
{
    QList<X11Output> outputs;
    for (QScreen *screen : QGuiApplication::screens())
        outputs.append({screen->name(), screen->geometry()});
    return outputs;
}

} // namespace

X11Monitor::X11Monitor(QObject *parent)
    : SessionMonitor(parent)
    , m_outputs(screensAsOutputs)
{
    m_coalesce.setSingleShot(true);
    m_coalesce.setInterval(kCoalesceMs);
    connect(&m_coalesce, &QTimer::timeout, this, &X11Monitor::refresh);
    m_poll.setInterval(kPollMs);
    connect(&m_poll, &QTimer::timeout, this, &X11Monitor::refresh);
}

X11Monitor::~X11Monitor()
{
    QCoreApplication::instance()->removeNativeEventFilter(this);
}

void X11Monitor::start()
{
    xcb_connection_t *c = x11::connection();
    if (!c) {
        qWarning("murasaki-surface: not an X11 platform; wallpapers will not pause or follow desktops");
        return;
    }
    // PropertyChange on the root: the manager keeps _NET_CLIENT_LIST, _NET_ACTIVE_WINDOW and the desktops there.
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE;
    xcb_change_window_attributes(c, x11::rootWindow(), XCB_CW_EVENT_MASK, &mask);
    xcb_flush(c);
    QCoreApplication::instance()->installNativeEventFilter(this);
    connect(qGuiApp, &QGuiApplication::screenAdded, this, [this] { m_coalesce.start(); });
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, [this] { m_coalesce.start(); });
    m_poll.start();
    refresh();
}

bool X11Monitor::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *)
{
    if (eventType != "xcb_generic_event_t")
        return false;
    const auto *event = static_cast<xcb_generic_event_t *>(message);
    if ((event->response_type & ~0x80) == XCB_PROPERTY_NOTIFY && !m_coalesce.isActive())
        m_coalesce.start(); // not consumed: Qt and others see it too
    return false;
}

X11Snapshot X11Monitor::snapshot() const
{
    using namespace x11;
    X11Snapshot s;
    const xcb_window_t root = rootWindow();
    const auto count = cardinals(root, atom("_NET_NUMBER_OF_DESKTOPS"));
    s.desktopCount = count.isEmpty() ? 1 : int(qBound<quint32>(1, count.first(), 256));
    const auto current = cardinals(root, atom("_NET_CURRENT_DESKTOP"));
    s.currentDesktop = current.isEmpty() ? 0 : current.first();
    s.desktopNames = strings(root, atom("_NET_DESKTOP_NAMES"));
    const auto active = windows(root, atom("_NET_ACTIVE_WINDOW"));
    s.activeWindow = active.isEmpty() ? 0 : active.first();
    s.ownPid = quint32(QCoreApplication::applicationPid());
    s.outputs = m_outputs();

    const xcb_atom_t typeNormal = atom("_NET_WM_WINDOW_TYPE_NORMAL");
    const xcb_atom_t stateFullscreen = atom("_NET_WM_STATE_FULLSCREEN");
    const xcb_atom_t stateHidden = atom("_NET_WM_STATE_HIDDEN");
    const xcb_atom_t stateVert = atom("_NET_WM_STATE_MAXIMIZED_VERT");
    const xcb_atom_t stateHorz = atom("_NET_WM_STATE_MAXIMIZED_HORZ");
    const auto clients = windows(root, atom("_NET_CLIENT_LIST"));
    for (const xcb_window_t id : clients) {
        X11Window w;
        w.id = id;
        const QStringList wmClass = strings(id, XCB_ATOM_WM_CLASS); // instance, class
        w.resourceClass = wmClass.size() >= 2 ? wmClass.at(1) : wmClass.value(0);
        const auto pid = cardinals(id, atom("_NET_WM_PID"));
        w.pid = pid.isEmpty() ? 0 : pid.first();
        const auto desktop = cardinals(id, atom("_NET_WM_DESKTOP"));
        w.desktop = desktop.isEmpty() ? kX11AllDesktops : desktop.first();
        const auto types = atoms(id, atom("_NET_WM_WINDOW_TYPE"));
        w.normal = types.isEmpty() || types.contains(typeNormal);
        const auto states = atoms(id, atom("_NET_WM_STATE"));
        w.fullscreen = states.contains(stateFullscreen);
        w.hidden = states.contains(stateHidden);
        w.maximizedBoth = states.contains(stateVert) && states.contains(stateHorz);
        int x = 0, y = 0, width = 0, height = 0;
        if (rootGeometry(id, &x, &y, &width, &height))
            w.geometry = QRect(x, y, width, height);
        s.windows.append(w);
    }
    return s;
}

void X11Monitor::watchClients(const QList<quint32> &clients)
{
    xcb_connection_t *c = x11::connection();
    const uint32_t mask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
    for (const quint32 id : clients) {
        if (!m_watched.contains(id))
            xcb_change_window_attributes(c, id, XCB_CW_EVENT_MASK, &mask);
    }
    m_watched = clients;
    xcb_flush(c);
}

void X11Monitor::refresh()
{
    if (!x11::connection())
        return;
    const X11Snapshot s = snapshot();
    QList<quint32> ids;
    for (const X11Window &w : s.windows)
        ids.append(w.id);
    watchClients(ids);
    setState(buildX11State(s));
}

} // namespace murasaki
