#pragma once

#include "sessionstate.h"

#include <QHash>
#include <QSet>
#include <QString>
#include <QStringList>

namespace murasaki {

// The state flags of a toplevel window (zwlr_foreign_toplevel_handle_v1.state, as bits).
enum ToplevelFlag : int {
    ToplevelMaximized = 1,
    ToplevelMinimized = 2,
    ToplevelActivated = 4,
    ToplevelFullscreen = 8,
};

// Turns what a foreign-toplevel protocol says (windows appear, change their app id, state and outputs, and disappear)
// into the SessionState Murasaki works with. It knows no Wayland: the protocol binding feeds it, and the tests feed it
// a script of events. Changes of one window are collected and take effect on done(), like the protocol's `done` event.
//
// The rules (a stacking compositor such as labwc or a COSMIC-like one; tiling compositors report their tiles through
// their own IPC, see swaymonitor.h):
//  - a window covers an output when it is fullscreen or maximized, not minimized, and shown on that output;
//  - activeApp is the app id of the activated window that is not minimized;
//  - fullscreenApps are the app ids of the fullscreen windows that are not minimized, wherever they are;
//  - there are no desktops: every output is on desktop "*".
class ToplevelTracker
{
public:
    using Id = quint64;

    void added(Id id);
    void closed(Id id);
    void setAppId(Id id, const QString &appId);
    void setTitle(Id id, const QString &title);
    void setFlags(Id id, int flags);
    void outputEntered(Id id, const QString &output);
    void outputLeft(Id id, const QString &output);
    // The window's pending changes become visible.
    void done(Id id);

    int count() const { return int(m_windows.size()); }
    QString appIdOf(Id id) const;

    // `outputs` are the outputs that exist; each gets an entry even when nothing covers it.
    SessionState state(const QStringList &outputs) const;

private:
    struct Window {
        QString appId;
        int flags = 0;
        QSet<QString> outputs;
        // The pending copy, committed by done().
        QString pendingAppId;
        int pendingFlags = 0;
        QSet<QString> pendingOutputs;
        bool committed = false; // done() was seen at least once: before it the window is half-described
    };
    QHash<Id, Window> m_windows;
};

} // namespace murasaki
