#pragma once

#include "x11state.h"

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

#include <xcb/xcb.h>

#include <cstring>

// The window manager's side: its own connection, so that what it sets reaches the monitor as property events.
// A fake window manager for X11 tests: it sets the EWMH properties of the root and of windows it owns, as a manager would.
class FakeWm
{
public:
    FakeWm()
    {
        m_connection = xcb_connect(nullptr, &m_screenNumber);
        if (xcb_connection_has_error(m_connection)) {
            m_connection = nullptr;
            return;
        }
        m_screen = xcb_setup_roots_iterator(xcb_get_setup(m_connection)).data;
        m_root = m_screen->root;
    }
    ~FakeWm()
    {
        if (m_connection)
            xcb_disconnect(m_connection);
    }
    bool ok() const { return m_connection != nullptr; }

    xcb_atom_t atom(const char *name)
    {
        xcb_intern_atom_reply_t *reply = xcb_intern_atom_reply(m_connection, xcb_intern_atom(m_connection, 0, uint16_t(strlen(name)), name), nullptr);
        const xcb_atom_t value = reply ? reply->atom : 0;
        free(reply);
        return value;
    }
    void cardinals(xcb_window_t window, const char *property, const QList<quint32> &values)
    {
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, window, atom(property), XCB_ATOM_CARDINAL, 32, uint32_t(values.size()), values.constData());
        xcb_flush(m_connection);
    }
    void atoms(xcb_window_t window, const char *property, const QStringList &names)
    {
        QList<xcb_atom_t> values;
        for (const QString &name : names)
            values.append(atom(name.toLatin1().constData()));
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, window, atom(property), XCB_ATOM_ATOM, 32, uint32_t(values.size()), values.constData());
        xcb_flush(m_connection);
    }
    void windows(xcb_window_t window, const char *property, const QList<xcb_window_t> &values)
    {
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, window, atom(property), XCB_ATOM_WINDOW, 32, uint32_t(values.size()), values.constData());
        xcb_flush(m_connection);
    }
    void text(xcb_window_t window, xcb_atom_t property, xcb_atom_t type, const QByteArray &data)
    {
        xcb_change_property(m_connection, XCB_PROP_MODE_REPLACE, window, property, type, 8, uint32_t(data.size()), data.constData());
        xcb_flush(m_connection);
    }
    xcb_window_t addWindow(const QString &appClass, QRect geometry, const QStringList &state = {}, quint32 desktop = 0, quint32 pid = 1)
    {
        const xcb_window_t id = xcb_generate_id(m_connection);
        xcb_create_window(m_connection, XCB_COPY_FROM_PARENT, id, m_root, int16_t(geometry.x()), int16_t(geometry.y()),
                          uint16_t(geometry.width()), uint16_t(geometry.height()), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
                          m_screen->root_visual, 0, nullptr);
        text(id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING, QByteArray("instance\0", 9) + appClass.toLatin1() + QByteArray("\0", 1));
        cardinals(id, "_NET_WM_PID", {pid});
        cardinals(id, "_NET_WM_DESKTOP", {desktop});
        atoms(id, "_NET_WM_STATE", state);
        xcb_map_window(m_connection, id);
        xcb_flush(m_connection);
        m_clients.append(id);
        publish();
        return id;
    }
    void setState(xcb_window_t id, const QStringList &state) { atoms(id, "_NET_WM_STATE", state); }
    void activate(xcb_window_t id) { windows(m_root, "_NET_ACTIVE_WINDOW", id ? QList<xcb_window_t>{id} : QList<xcb_window_t>{}); }
    void desktops(int count, int current, const QStringList &names)
    {
        cardinals(m_root, "_NET_NUMBER_OF_DESKTOPS", {quint32(count)});
        cardinals(m_root, "_NET_CURRENT_DESKTOP", {quint32(current)});
        QByteArray data;
        for (const QString &name : names)
            data += name.toUtf8() + '\0';
        text(m_root, atom("_NET_DESKTOP_NAMES"), atom("UTF8_STRING"), data);
    }
    void removeWindow(xcb_window_t id)
    {
        xcb_destroy_window(m_connection, id);
        m_clients.removeAll(id);
        publish();
    }
    void publish() { windows(m_root, "_NET_CLIENT_LIST", m_clients); }

private:
    xcb_connection_t *m_connection = nullptr;
    int m_screenNumber = 0;
    xcb_screen_t *m_screen = nullptr;
    xcb_window_t m_root = 0;
    QList<xcb_window_t> m_clients;
};

