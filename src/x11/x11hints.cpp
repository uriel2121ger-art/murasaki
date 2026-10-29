#include "x11hints.h"

#include "x11util.h"

#include <QGuiApplication>
#include <QWindow>

#ifdef MURASAKI_HAVE_X11_SHAPE
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>
#include <QtGui/qguiapplication_platform.h>
#endif

#include <cstring>

namespace murasaki::x11 {
namespace {

constexpr quint32 kAllDesktops = 0xFFFFFFFFu;

// Sets or adds the EWMH state of a window: on an unmapped one it is the property the manager reads when mapping; on a
// mapped one a _NET_WM_STATE client message to the root asks the manager to change it.
void setState(QWindow *window, const QList<xcb_atom_t> &states)
{
    xcb_connection_t *c = connection();
    const auto id = xcb_window_t(window->winId());
    if (!window->isExposed()) {
        setAtoms(id, atom("_NET_WM_STATE"), states);
        return;
    }
    for (const xcb_atom_t state : states) {
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = id;
        event.type = atom("_NET_WM_STATE");
        event.data.data32[0] = 1; // _NET_WM_STATE_ADD
        event.data.data32[1] = state;
        event.data.data32[3] = 1; // from an application
        xcb_send_event(c, 0, rootWindow(), XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&event));
    }
}

bool apply(QWindow *window, const char *type, const QList<const char *> &stateNames)
{
    if (!window || !connection() || !QGuiApplication::platformName().startsWith(u"xcb"))
        return false;
    window->create(); // the native window must exist to carry properties
    if (!window->winId())
        return false;
    const auto id = xcb_window_t(window->winId());
    setAtoms(id, atom("_NET_WM_WINDOW_TYPE"), {atom(type)});
    QList<xcb_atom_t> states;
    for (const char *name : stateNames)
        states.append(atom(name));
    setState(window, states);
    if (!window->isExposed())
        setCardinals(id, atom("_NET_WM_DESKTOP"), {kAllDesktops});
    xcb_flush(connection());
    return true;
}

} // namespace

bool applyDesktopHints(QWindow *window)
{
    return apply(window, "_NET_WM_WINDOW_TYPE_DESKTOP",
                 {"_NET_WM_STATE_BELOW", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"});
}

bool applyPetHints(QWindow *window)
{
    return apply(window, "_NET_WM_WINDOW_TYPE_NOTIFICATION",
                 {"_NET_WM_STATE_ABOVE", "_NET_WM_STATE_STICKY", "_NET_WM_STATE_SKIP_TASKBAR", "_NET_WM_STATE_SKIP_PAGER"});
}

void setPetKeepAbove(QWindow *window, bool above)
{
    if (!window || !connection() || !QGuiApplication::platformName().startsWith(u"xcb"))
        return;
    window->create();
    if (!window->winId())
        return;
    const auto id = xcb_window_t(window->winId());
    if (!window->isExposed()) {
        QList<xcb_atom_t> states{atom("_NET_WM_STATE_STICKY"), atom("_NET_WM_STATE_SKIP_TASKBAR"), atom("_NET_WM_STATE_SKIP_PAGER")};
        if (above)
            states.prepend(atom("_NET_WM_STATE_ABOVE"));
        setAtoms(id, atom("_NET_WM_STATE"), states);
    } else {
        xcb_client_message_event_t event{};
        event.response_type = XCB_CLIENT_MESSAGE;
        event.format = 32;
        event.window = id;
        event.type = atom("_NET_WM_STATE");
        event.data.data32[0] = above ? 1 : 0; // _NET_WM_STATE_ADD or _NET_WM_STATE_REMOVE
        event.data.data32[1] = atom("_NET_WM_STATE_ABOVE");
        event.data.data32[3] = 1; // from an application
        xcb_send_event(connection(), 0, rootWindow(), XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                       reinterpret_cast<const char *>(&event));
    }
    xcb_flush(connection());
}

int stackAboveIndex(const QList<bool> &isDesktopBottomToTop)
{
    for (qsizetype i = isDesktopBottomToTop.size() - 1; i >= 0; --i) {
        if (isDesktopBottomToTop.at(i))
            return int(i);
    }
    return -1;
}

void lowerPet(QWindow *window)
{
    if (!window || !connection() || !QGuiApplication::platformName().startsWith(u"xcb"))
        return;
    window->create();
    if (!window->winId())
        return;
    const auto id = xcb_window_t(window->winId());
    const QList<xcb_window_t> stack = windows(rootWindow(), atom("_NET_CLIENT_LIST_STACKING"));
    QList<bool> desktop;
    for (const xcb_window_t other : stack)
        desktop.append(other != id && atoms(other, atom("_NET_WM_WINDOW_TYPE")).contains(atom("_NET_WM_WINDOW_TYPE_DESKTOP")));
    const int above = stackAboveIndex(desktop);
    uint32_t values[2];
    uint16_t mask = XCB_CONFIG_WINDOW_STACK_MODE;
    if (above >= 0) {
        values[0] = stack.at(above);
        values[1] = XCB_STACK_MODE_ABOVE;
        mask |= XCB_CONFIG_WINDOW_SIBLING;
    } else {
        values[0] = XCB_STACK_MODE_BELOW;
    }
    xcb_configure_window(connection(), id, mask, values);
    xcb_flush(connection());
}

void raiseWindow(QWindow *window)
{
    if (window && connection() && QGuiApplication::platformName().startsWith(u"xcb"))
        window->raise();
}

bool setInputShape(QWindow *window, const QList<QRect> &rects)
{
#ifdef MURASAKI_HAVE_X11_SHAPE
    if (!window || !QGuiApplication::platformName().startsWith(u"xcb") || window->winId() == 0)
        return false;
    const auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
    Display *display = native ? native->display() : nullptr;
    int eventBase = 0, errorBase = 0;
    if (!display || !XShapeQueryExtension(display, &eventBase, &errorBase))
        return false;
    QList<XRectangle> shape;
    shape.reserve(rects.size());
    for (const QRect &r : rects)
        shape.append({short(r.x()), short(r.y()), static_cast<unsigned short>(r.width()), static_cast<unsigned short>(r.height())});
    XShapeCombineRectangles(display, Window(window->winId()), ShapeInput, 0, 0, shape.data(), int(shape.size()), ShapeSet, Unsorted); // (the rectangles of a mask are merged vertically: not banded, and the server trusts the hint it is given)
    XFlush(display);
    return true;
#else
    Q_UNUSED(window);
    Q_UNUSED(rects);
    return false;
#endif
}

} // namespace murasaki::x11
