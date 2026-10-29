#pragma once

#include "sessionstate.h"

#include <QList>
#include <QRect>
#include <QString>
#include <QStringList>

namespace murasaki {

inline constexpr quint32 kX11AllDesktops = 0xFFFFFFFFu;

// One top-level window as the EWMH properties of the root and of the window itself describe it.
struct X11Window {
    quint32 id = 0;
    QString resourceClass; // the class half of WM_CLASS
    quint32 pid = 0;       // _NET_WM_PID, 0 when unknown
    quint32 desktop = 0;   // _NET_WM_DESKTOP; kX11AllDesktops for sticky windows
    bool normal = true;    // _NET_WM_WINDOW_TYPE is NORMAL (or absent): not a dock, desktop, menu, splash...
    bool fullscreen = false;
    bool hidden = false;   // minimised
    bool maximizedBoth = false;
    QRect geometry;        // root coordinates
};

struct X11Output {
    QString name;
    QRect geometry;
};

struct X11Snapshot {
    int desktopCount = 1;
    quint32 currentDesktop = 0;
    QStringList desktopNames;
    QList<X11Window> windows;
    quint32 activeWindow = 0;
    QList<X11Output> outputs;
    quint32 ownPid = 0; // our own windows never cover anything
};

// The session state the policy needs: desktops (ids are their numbers, 0 based; names from _NET_DESKTOP_NAMES or
// "Desktop N"), the current one as the default desktop of every output, and which outputs are covered. A window covers the
// output it overlaps most when it is on the current desktop (or all of them), not hidden, and fullscreen, maximised both
// ways, or has a geometry that contains the output. Our own windows and windows that are not ordinary ones are ignored.
// The class of the active window and of every fullscreen one feed the application list of the pause rules.
SessionState buildX11State(const X11Snapshot &snapshot);

} // namespace murasaki
