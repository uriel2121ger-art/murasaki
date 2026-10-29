#include "foreigntoplevelmonitor.h"

#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <wayland-client.h>

#include <algorithm>
#include <cstring>

using namespace Qt::StringLiterals;

namespace murasaki {

namespace {

// ---- wl_output: only the name matters ----

void outputGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *, const char *, int32_t) {}
void outputMode(void *, wl_output *, uint32_t, int32_t, int32_t, int32_t) {}
void outputScale(void *, wl_output *, int32_t) {}
void outputDescription(void *, wl_output *, const char *) {}
void outputDone(void *data, wl_output *)
{
    auto *output = static_cast<ForeignToplevelMonitor::Output *>(data);
    output->owner->refreshState();
}
void outputName(void *data, wl_output *, const char *name)
{
    static_cast<ForeignToplevelMonitor::Output *>(data)->name = QString::fromUtf8(name);
}
const wl_output_listener kOutputListener = {outputGeometry, outputMode, outputDone, outputScale, outputName, outputDescription};

// ---- toplevel handles ----

using Toplevel = ForeignToplevelMonitor::Toplevel;

void handleTitle(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *title)
{
    auto *t = static_cast<Toplevel *>(data);
    t->owner->tracker().setTitle(t->id, QString::fromUtf8(title));
}
void handleAppId(void *data, zwlr_foreign_toplevel_handle_v1 *, const char *appId)
{
    auto *t = static_cast<Toplevel *>(data);
    t->owner->tracker().setAppId(t->id, QString::fromUtf8(appId));
}
void handleOutputEnter(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_output *output)
{
    auto *t = static_cast<Toplevel *>(data);
    if (auto *known = t->owner->outputFor(output))
        t->owner->tracker().outputEntered(t->id, known->name);
}
void handleOutputLeave(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_output *output)
{
    auto *t = static_cast<Toplevel *>(data);
    if (auto *known = t->owner->outputFor(output))
        t->owner->tracker().outputLeft(t->id, known->name);
}
void handleState(void *data, zwlr_foreign_toplevel_handle_v1 *, wl_array *state)
{
    auto *t = static_cast<Toplevel *>(data);
    int flags = 0;
    const auto *begin = static_cast<const uint32_t *>(state->data);
    for (const uint32_t *it = begin; it < begin + state->size / sizeof(uint32_t); ++it) {
        switch (*it) {
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MAXIMIZED: flags |= ToplevelMaximized; break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED: flags |= ToplevelMinimized; break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED: flags |= ToplevelActivated; break;
        case ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_FULLSCREEN: flags |= ToplevelFullscreen; break;
        default: break; // states of a newer protocol revision
        }
    }
    t->owner->tracker().setFlags(t->id, flags);
}
void handleDone(void *data, zwlr_foreign_toplevel_handle_v1 *)
{
    auto *t = static_cast<Toplevel *>(data);
    t->owner->tracker().done(t->id);
    t->owner->refreshState();
}
void handleClosed(void *data, zwlr_foreign_toplevel_handle_v1 *)
{
    auto *t = static_cast<Toplevel *>(data);
    t->owner->removeToplevel(t);
}
void handleParent(void *, zwlr_foreign_toplevel_handle_v1 *, zwlr_foreign_toplevel_handle_v1 *) {}
const zwlr_foreign_toplevel_handle_v1_listener kHandleListener = {handleTitle, handleAppId, handleOutputEnter, handleOutputLeave,
                                                                  handleState, handleDone, handleClosed, handleParent};

// ---- the manager ----

void managerToplevel(void *data, zwlr_foreign_toplevel_manager_v1 *, zwlr_foreign_toplevel_handle_v1 *handle)
{
    static_cast<ForeignToplevelMonitor *>(data)->addToplevel(handle);
}
void managerFinished(void *data, zwlr_foreign_toplevel_manager_v1 *)
{
    static_cast<ForeignToplevelMonitor *>(data)->managerFinished();
}
const zwlr_foreign_toplevel_manager_v1_listener kManagerListener = {managerToplevel, managerFinished};

// ---- the registry ----

void registryGlobal(void *data, wl_registry *, uint32_t name, const char *interface, uint32_t version)
{
    static_cast<ForeignToplevelMonitor *>(data)->bindGlobal(name, interface, version);
}
void registryGlobalRemove(void *data, wl_registry *, uint32_t name)
{
    static_cast<ForeignToplevelMonitor *>(data)->removeOutput(name);
}
const wl_registry_listener kRegistryListener = {registryGlobal, registryGlobalRemove};

// The registry of a throw-away connection, only to list interface names.
void probeGlobal(void *data, wl_registry *, uint32_t, const char *interface, uint32_t)
{
    static_cast<QSet<QString> *>(data)->insert(QString::fromUtf8(interface));
}
void probeRemove(void *, wl_registry *, uint32_t) {}
const wl_registry_listener kProbeListener = {probeGlobal, probeRemove};

} // namespace

QSet<QString> probeWaylandGlobals(const QString &display)
{
    QSet<QString> globals;
    wl_display *connection = wl_display_connect(display.isEmpty() ? nullptr : display.toUtf8().constData());
    if (!connection)
        return globals;
    wl_registry *registry = wl_display_get_registry(connection);
    wl_registry_add_listener(registry, &kProbeListener, &globals);
    wl_display_roundtrip(connection);
    wl_registry_destroy(registry);
    wl_display_disconnect(connection);
    return globals;
}

ForeignToplevelMonitor::ForeignToplevelMonitor(QString display, bool appsOnly, QObject *parent)
    : SessionMonitor(parent)
    , m_displayName(std::move(display))
    , m_appsOnly(appsOnly)
{
}

ForeignToplevelMonitor::~ForeignToplevelMonitor()
{
    m_notifier.reset();
    for (Toplevel *toplevel : std::as_const(m_toplevels)) {
        zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
        delete toplevel;
    }
    for (Output *output : std::as_const(m_outputs)) {
        wl_output_release(output->proxy);
        delete output;
    }
    if (m_manager)
        zwlr_foreign_toplevel_manager_v1_destroy(m_manager);
    if (m_registry)
        wl_registry_destroy(m_registry);
    if (m_display)
        wl_display_disconnect(m_display);
}

void ForeignToplevelMonitor::start()
{
    if (m_display)
        return;
    m_display = wl_display_connect(m_displayName.isEmpty() ? nullptr : m_displayName.toUtf8().constData());
    if (!m_display) {
        qWarning("murasaki-surface: cannot connect to the Wayland display for the window list");
        return;
    }
    m_registry = wl_display_get_registry(m_display);
    wl_registry_add_listener(m_registry, &kRegistryListener, this);
    // The first roundtrip lists the globals (and binds the outputs), the second brings their names, so that a window's
    // output_enter finds a name when the manager is bound afterwards.
    wl_display_roundtrip(m_display);
    wl_display_roundtrip(m_display);
    if (!m_manager) {
        qWarning("murasaki-surface: the compositor does not offer wlr-foreign-toplevel-management");
        fail("no manager");
        return;
    }
    wl_display_roundtrip(m_display); // the windows that already exist
    m_notifier = std::make_unique<QSocketNotifier>(wl_display_get_fd(m_display), QSocketNotifier::Read);
    connect(m_notifier.get(), &QSocketNotifier::activated, this, &ForeignToplevelMonitor::dispatch);
    refreshState();
}

void ForeignToplevelMonitor::bindGlobal(quint32 name, const char *interface, quint32 version)
{
    if (!std::strcmp(interface, wl_output_interface.name)) {
        // Version 4 brings the output's name, which is how the rest of Murasaki identifies outputs.
        if (version < 4)
            return;
        auto *proxy = static_cast<wl_output *>(wl_registry_bind(m_registry, name, &wl_output_interface, 4));
        m_outputByGlobal.insert(name, proxy);
        addOutput(proxy);
    } else if (!std::strcmp(interface, zwlr_foreign_toplevel_manager_v1_interface.name) && !m_manager) {
        m_manager = static_cast<zwlr_foreign_toplevel_manager_v1 *>(
            wl_registry_bind(m_registry, name, &zwlr_foreign_toplevel_manager_v1_interface, std::min<quint32>(version, 3)));
        zwlr_foreign_toplevel_manager_v1_add_listener(m_manager, &kManagerListener, this);
    }
}

void ForeignToplevelMonitor::addOutput(wl_output *proxy)
{
    auto *output = new Output{this, proxy, {}};
    m_outputs.insert(proxy, output);
    wl_output_add_listener(proxy, &kOutputListener, output);
}

void ForeignToplevelMonitor::removeOutput(quint32 name)
{
    const auto it = m_outputByGlobal.find(name);
    if (it == m_outputByGlobal.end())
        return;
    wl_output *proxy = it.value();
    m_outputByGlobal.erase(it);
    if (Output *output = m_outputs.take(proxy)) {
        wl_output_release(proxy);
        delete output;
    }
    refreshState();
}

ForeignToplevelMonitor::Output *ForeignToplevelMonitor::outputFor(wl_output *proxy)
{
    return m_outputs.value(proxy, nullptr);
}

void ForeignToplevelMonitor::addToplevel(zwlr_foreign_toplevel_handle_v1 *handle)
{
    auto *toplevel = new Toplevel{this, handle, m_nextId++};
    m_toplevels.insert(handle, toplevel);
    m_tracker.added(toplevel->id);
    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &kHandleListener, toplevel);
}

void ForeignToplevelMonitor::removeToplevel(Toplevel *toplevel)
{
    m_tracker.closed(toplevel->id);
    m_toplevels.remove(toplevel->handle);
    zwlr_foreign_toplevel_handle_v1_destroy(toplevel->handle);
    delete toplevel;
    refreshState();
}

void ForeignToplevelMonitor::managerFinished()
{
    // The compositor stops the protocol (for example when it shuts down): nothing is known any more.
    fail("manager finished");
}

void ForeignToplevelMonitor::refreshState()
{
    QStringList names;
    for (const Output *output : std::as_const(m_outputs)) {
        if (!output->name.isEmpty())
            names.append(output->name);
    }
    SessionState state = m_tracker.state(names);
    if (m_appsOnly) {
        // Occlusion is the primary monitor's; this one only adds the application classes.
        state.outputs.clear();
        state.desktops.clear();
    }
    setState(state);
}

void ForeignToplevelMonitor::dispatch()
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

void ForeignToplevelMonitor::fail(const char *why)
{
    qWarning("murasaki-surface: window list stopped: %s", why);
    m_notifier.reset();
    setState(SessionState{}); // nothing covers anything: wallpapers play
}

} // namespace murasaki
