#pragma once

#include "sessionmonitor.h"
#include "toplevelstate.h"

#include <QHash>
#include <QSet>
#include <QSocketNotifier>
#include <QString>

#include <memory>

struct wl_display;
struct wl_registry;
struct wl_output;
struct zwlr_foreign_toplevel_manager_v1;
struct zwlr_foreign_toplevel_handle_v1;

namespace murasaki {

// The interface names the compositor's registry lists, from a short connection of its own (empty when it can not
// connect). `display` is a socket name; empty means WAYLAND_DISPLAY.
QSet<QString> probeWaylandGlobals(const QString &display = {});

// Feeds SessionState from wlr-foreign-toplevel-management-unstable-v1: which windows are fullscreen or maximized on which
// output, which one has the focus, and the app ids the pause rules match. It works on any compositor that offers the
// protocol (labwc, river, wayfire, niri, ...; Sway and Hyprland use it for the app ids only, see compositorprofile.h).
// It speaks to the compositor over a connection of its own, driven by the Qt event loop; the rules are ToplevelTracker's.
class ForeignToplevelMonitor : public SessionMonitor
{
    Q_OBJECT

public:
    // `display`: a socket name; empty means WAYLAND_DISPLAY. `appsOnly`: report only activeApp and fullscreenApps,
    // leave occlusion to the primary monitor.
    explicit ForeignToplevelMonitor(QString display = {}, bool appsOnly = false, QObject *parent = nullptr);
    ~ForeignToplevelMonitor() override;

    void start() override;
    bool connected() const { return m_display != nullptr; }

    // Called by the protocol callbacks (C functions in the .cpp).
    struct Output {
        ForeignToplevelMonitor *owner = nullptr;
        wl_output *proxy = nullptr;
        QString name;
    };
    struct Toplevel {
        ForeignToplevelMonitor *owner = nullptr;
        zwlr_foreign_toplevel_handle_v1 *handle = nullptr;
        ToplevelTracker::Id id = 0;
    };
    ToplevelTracker &tracker() { return m_tracker; }
    Output *outputFor(wl_output *proxy);
    void addToplevel(zwlr_foreign_toplevel_handle_v1 *handle);
    void removeToplevel(Toplevel *toplevel);
    void refreshState();
    void addOutput(wl_output *proxy);
    void removeOutput(quint32 name);
    void managerFinished();
    void bindGlobal(quint32 name, const char *interface, quint32 version);

private:
    void dispatch();
    void fail(const char *why);

    QString m_displayName;
    bool m_appsOnly;
    wl_display *m_display = nullptr;
    wl_registry *m_registry = nullptr;
    zwlr_foreign_toplevel_manager_v1 *m_manager = nullptr;
    std::unique_ptr<QSocketNotifier> m_notifier;
    ToplevelTracker m_tracker;
    QHash<wl_output *, Output *> m_outputs;
    QHash<quint32, wl_output *> m_outputByGlobal;
    QHash<zwlr_foreign_toplevel_handle_v1 *, Toplevel *> m_toplevels;
    ToplevelTracker::Id m_nextId = 1;
};

} // namespace murasaki
