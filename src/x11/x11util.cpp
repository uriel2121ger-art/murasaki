#include "x11util.h"

#include <QGuiApplication>
#include <QHash>
#include <QtGui/qguiapplication_platform.h>

#include <cstdlib>

namespace murasaki::x11 {

xcb_connection_t *connection()
{
    if (!qGuiApp)
        return nullptr;
    if (auto *native = qGuiApp->nativeInterface<QNativeInterface::QX11Application>())
        return native->connection();
    return nullptr;
}

xcb_window_t rootWindow()
{
    xcb_connection_t *c = connection();
    if (!c)
        return XCB_NONE;
    const xcb_setup_t *setup = xcb_get_setup(c);
    // EWMH lives on the root of the (first) X screen; RandR outputs are all on it.
    const xcb_screen_iterator_t it = xcb_setup_roots_iterator(setup);
    return it.data ? it.data->root : XCB_NONE;
}

xcb_atom_t atom(const char *name)
{
    static QHash<QByteArray, xcb_atom_t> cache;
    xcb_connection_t *c = connection();
    if (!c)
        return XCB_ATOM_NONE;
    const auto it = cache.constFind(name);
    if (it != cache.cend())
        return *it;
    const xcb_intern_atom_cookie_t cookie = xcb_intern_atom(c, 0, uint16_t(qstrlen(name)), name);
    xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(c, cookie, nullptr);
    const xcb_atom_t value = reply ? reply->atom : xcb_atom_t(XCB_ATOM_NONE);
    free(reply);
    if (value != XCB_ATOM_NONE)
        cache.insert(name, value);
    return value;
}

QString atomName(xcb_atom_t value)
{
    xcb_connection_t *c = connection();
    if (!c || value == XCB_ATOM_NONE)
        return {};
    xcb_get_atom_name_reply_t *reply = xcb_get_atom_name_reply(c, xcb_get_atom_name(c, value), nullptr);
    if (!reply)
        return {};
    const QString name = QString::fromLatin1(xcb_get_atom_name_name(reply), xcb_get_atom_name_name_length(reply));
    free(reply);
    return name;
}

namespace {

// The raw value of a property, or an empty array; `format` is checked (8, 32).
QByteArray value(xcb_window_t window, xcb_atom_t property, int format)
{
    xcb_connection_t *c = connection();
    if (!c || window == XCB_NONE || property == XCB_ATOM_NONE)
        return {};
    xcb_get_property_reply_t *reply =
        xcb_get_property_reply(c, xcb_get_property(c, 0, window, property, XCB_ATOM_ANY, 0, 65536), nullptr);
    if (!reply)
        return {};
    QByteArray data;
    if (reply->format == format && reply->type != XCB_ATOM_NONE)
        data = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)), xcb_get_property_value_length(reply));
    free(reply);
    return data;
}

QList<quint32> longs(xcb_window_t window, xcb_atom_t property)
{
    const QByteArray data = value(window, property, 32);
    QList<quint32> out;
    out.reserve(data.size() / 4);
    for (qsizetype i = 0; i + 4 <= data.size(); i += 4) {
        quint32 v;
        memcpy(&v, data.constData() + i, 4);
        out.append(v);
    }
    return out;
}

} // namespace

QList<quint32> cardinals(xcb_window_t window, xcb_atom_t property)
{
    return longs(window, property);
}

QList<xcb_atom_t> atoms(xcb_window_t window, xcb_atom_t property)
{
    return longs(window, property);
}

QList<xcb_window_t> windows(xcb_window_t window, xcb_atom_t property)
{
    return longs(window, property);
}

QStringList strings(xcb_window_t window, xcb_atom_t property)
{
    const QByteArray data = value(window, property, 8);
    QStringList out;
    for (const QByteArray &part : data.split('\0')) {
        if (!part.isEmpty())
            out.append(QString::fromUtf8(part));
    }
    return out;
}

void setAtoms(xcb_window_t window, xcb_atom_t property, const QList<xcb_atom_t> &values)
{
    xcb_connection_t *c = connection();
    if (!c)
        return;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, property, XCB_ATOM_ATOM, 32, uint32_t(values.size()), values.constData());
}

void setCardinals(xcb_window_t window, xcb_atom_t property, const QList<quint32> &values)
{
    xcb_connection_t *c = connection();
    if (!c)
        return;
    xcb_change_property(c, XCB_PROP_MODE_REPLACE, window, property, XCB_ATOM_CARDINAL, 32, uint32_t(values.size()),
                        values.constData());
}

bool rootGeometry(xcb_window_t window, int *x, int *y, int *width, int *height)
{
    xcb_connection_t *c = connection();
    if (!c)
        return false;
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(c, xcb_get_geometry(c, window), nullptr);
    if (!geometry)
        return false;
    xcb_translate_coordinates_reply_t *origin =
        xcb_translate_coordinates_reply(c, xcb_translate_coordinates(c, window, rootWindow(), 0, 0), nullptr);
    if (!origin) {
        free(geometry);
        return false;
    }
    *x = origin->dst_x;
    *y = origin->dst_y;
    *width = geometry->width;
    *height = geometry->height;
    free(geometry);
    free(origin);
    return true;
}

bool compositingManagerRunning()
{
    xcb_connection_t *c = connection();
    if (!c)
        return true;
    const QByteArray display = qgetenv("DISPLAY");
    int screen = 0;
    const int dot = display.lastIndexOf('.');
    if (dot >= 0 && dot > display.lastIndexOf(':')) {
        bool ok = false;
        const int n = display.mid(dot + 1).toInt(&ok);
        if (ok && n >= 0 && n < 64)
            screen = n;
    }
    const xcb_atom_t selection = atom(("_NET_WM_CM_S" + QByteArray::number(screen)).constData());
    if (selection == XCB_ATOM_NONE)
        return true;
    xcb_get_selection_owner_reply_t *reply = xcb_get_selection_owner_reply(c, xcb_get_selection_owner(c, selection), nullptr);
    const bool owned = reply && reply->owner != XCB_NONE;
    const bool answered = reply != nullptr;
    free(reply);
    return answered ? owned : true;
}

} // namespace murasaki::x11
