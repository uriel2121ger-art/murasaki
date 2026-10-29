#pragma once

#include <QList>
#include <QRect>

class QWindow;

namespace murasaki::x11 {

// EWMH hints of a wallpaper window: window type DESKTOP, state BELOW + STICKY + SKIP_TASKBAR + SKIP_PAGER, on all
// desktops. Set on the window's properties, so call it after QWindow::create() and before show(); on a window that is
// already mapped the state goes through the root's client message instead. False when this is not an xcb window.
bool applyDesktopHints(QWindow *window);

// EWMH hints of a pet window: type NOTIFICATION, state ABOVE + STICKY + SKIP_TASKBAR + SKIP_PAGER, on all desktops. (The
// window is also override-redirect, which window managers leave alone; the hints serve those that look anyway.)
bool applyPetHints(QWindow *window);

// Sets or clears the ABOVE state of a pet window (keepAbove): on a window not yet mapped it is the property the manager reads, on a mapped one a
// _NET_WM_STATE client message. The window's Qt::WindowStaysOnTopHint is the caller's.
void setPetKeepAbove(QWindow *window, bool above);

// keepAbove off for an override-redirect pet: stacks the window just above the topmost DESKTOP-type window of the manager's stacking list
// (_NET_CLIENT_LIST_STACKING), so it is above the wallpaper and below every ordinary window, as a window that is not "always on top" would be; without such a window
// (no manager, or a manager without the list) at the very bottom of the stack. Windows raised or mapped later stay above it. Does nothing off X11.
void lowerPet(QWindow *window);

// The index in a bottom-to-top list of "is a desktop window" flags of the window to stack above (the topmost desktop one), or -1 for the bottom of the stack.
int stackAboveIndex(const QList<bool> &isDesktopBottomToTop);

// Stacks the window above its siblings (a pet over a window that was just raised). Does nothing off X11.
void raiseWindow(QWindow *window);

// The INPUT shape of the window: only the given rectangles (window coordinates) take the pointer, the rest of the window lets it through to
// what is under it. Unlike QWindow::setMask(), which clips what is drawn too (and would cut the faint edges of a pet), only the input is
// touched. An empty list is an empty shape (nothing takes the pointer). Call it after Qt::WindowTransparentForInput was turned off (Qt resets
// the input shape when that flag changes). False when this is not an xcb window or the SHAPE extension is missing.
bool setInputShape(QWindow *window, const QList<QRect> &rects);

} // namespace murasaki::x11
