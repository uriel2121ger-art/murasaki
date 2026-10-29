#include "relativepointer.h"

#include <QGuiApplication>

#include <wayland-client.h>

#include "relative-pointer-unstable-v1-client-protocol.h"

#include <cstring>

namespace murasaki {

namespace {
const wl_registry_listener kRegistryListener = {&RelativePointer::global, &RelativePointer::globalRemove};
} // namespace

RelativePointer *RelativePointer::instance()
{
    static RelativePointer *self = [] {
        auto *wayland = qGuiApp ? qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>() : nullptr;
        return wayland && wayland->display() ? new RelativePointer : nullptr;
    }();
    return self;
}

RelativePointer::RelativePointer()
    : QObject(qGuiApp)
{
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    // On the default queue, which Qt dispatches on this thread: the globals and then the motion arrive like Qt's own events.
    m_registry = wl_display_get_registry(wayland->display());
    wl_registry_add_listener(m_registry, &kRegistryListener, this);
}

RelativePointer::~RelativePointer()
{
    if (m_relative)
        zwp_relative_pointer_v1_destroy(m_relative);
    if (m_manager)
        zwp_relative_pointer_manager_v1_destroy(m_manager);
    if (m_registry)
        wl_registry_destroy(m_registry);
}

void RelativePointer::global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t)
{
    auto *self = static_cast<RelativePointer *>(data);
    if (self->m_manager || std::strcmp(interface, zwp_relative_pointer_manager_v1_interface.name) != 0)
        return;
    self->m_manager = static_cast<zwp_relative_pointer_manager_v1 *>(
        wl_registry_bind(registry, name, &zwp_relative_pointer_manager_v1_interface, 1));
    self->m_managerName = name;
    self->attach();
}

void RelativePointer::globalRemove(void *data, wl_registry *, uint32_t name)
{
    auto *self = static_cast<RelativePointer *>(data);
    if (!self->m_manager || name != self->m_managerName)
        return;
    if (self->m_relative)
        zwp_relative_pointer_v1_destroy(self->m_relative);
    self->m_relative = nullptr;
    self->m_pointer = nullptr;
    zwp_relative_pointer_manager_v1_destroy(self->m_manager);
    self->m_manager = nullptr;
}

void RelativePointer::attach()
{
    if (!m_manager)
        return;
    auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
    wl_pointer *pointer = wayland ? wayland->pointer() : nullptr;
    if (pointer == m_pointer)
        return;
    if (m_relative)
        zwp_relative_pointer_v1_destroy(m_relative);
    m_relative = nullptr;
    m_pointer = pointer;
    if (!pointer)
        return;
    static const zwp_relative_pointer_v1_listener listener = {&RelativePointer::relativeMotion};
    m_relative = zwp_relative_pointer_manager_v1_get_relative_pointer(m_manager, pointer);
    zwp_relative_pointer_v1_add_listener(m_relative, &listener, this);
}

void RelativePointer::relativeMotion(void *data, zwp_relative_pointer_v1 *, uint32_t, uint32_t, int dx, int dy, int, int)
{
    Q_EMIT static_cast<RelativePointer *>(data)->moved(wl_fixed_to_double(dx), wl_fixed_to_double(dy));
}

} // namespace murasaki
