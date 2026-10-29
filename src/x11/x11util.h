#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <xcb/xcb.h>

namespace murasaki::x11 {

// The X connection of the running QGuiApplication (the xcb platform plugin's), or null on any other platform.
xcb_connection_t *connection();
xcb_window_t rootWindow();

// Atoms by name, interned once per connection (cached). 0 when the server refuses.
xcb_atom_t atom(const char *name);
QString atomName(xcb_atom_t atom);

// Property readers: an empty result for a missing property or a different format.
QList<quint32> cardinals(xcb_window_t window, xcb_atom_t property);
QList<xcb_atom_t> atoms(xcb_window_t window, xcb_atom_t property);
QList<xcb_window_t> windows(xcb_window_t window, xcb_atom_t property);
// A list of strings stored as one NUL-separated value (WM_CLASS, _NET_DESKTOP_NAMES).
QStringList strings(xcb_window_t window, xcb_atom_t property);

void setAtoms(xcb_window_t window, xcb_atom_t property, const QList<xcb_atom_t> &values);
void setCardinals(xcb_window_t window, xcb_atom_t property, const QList<quint32> &values);

// Where a window is on the screen, in root coordinates (its content area, as the server knows it); false when it is gone.
bool rootGeometry(xcb_window_t window, int *x, int *y, int *width, int *height);

// Whether a compositing manager runs on the screen of the display (the owner of the selection _NET_WM_CM_S<screen>, ICCCM/EWMH): without one a translucent window is an
// opaque one. True when it can not be told (no connection); the screen is the one DISPLAY names (":0.1" is 1, ":0" is 0).
bool compositingManagerRunning();

} // namespace murasaki::x11
