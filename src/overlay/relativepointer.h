// The relative motion of the pointer (zwp_relative_pointer_v1) on the overlay's Wayland connection: what a drag reads when the
// window's own frame can not be trusted (PetInteraction, F13: a compositor that animates the window's growth). Every pet window
// shares it; the events come to whichever of the overlay's surfaces has the pointer, which during a drag is the dragged pet.
#pragma once

#include <QObject>

#include <cstdint>

struct wl_pointer;
struct wl_registry;
struct zwp_relative_pointer_manager_v1;
struct zwp_relative_pointer_v1;

namespace murasaki {

class RelativePointer : public QObject
{
    Q_OBJECT
public:
    // The one of this process (nullptr outside Wayland). It binds the protocol when the compositor offers it.
    static RelativePointer *instance();
    ~RelativePointer() override;

    // Follows the seat's pointer Qt uses now (Qt makes a new one when the seat's devices change); called on each press.
    void attach();

    // (the Wayland listeners)
    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version);
    static void globalRemove(void *data, wl_registry *registry, uint32_t name);
    static void relativeMotion(void *data, zwp_relative_pointer_v1 *pointer, uint32_t timeHi, uint32_t timeLo, int dx, int dy,
                               int dxUnaccel, int dyUnaccel);

Q_SIGNALS:
    // Accelerated motion, in the units the cursor moves.
    void moved(double dx, double dy);

private:
    RelativePointer();

    wl_registry *m_registry = nullptr;
    zwp_relative_pointer_manager_v1 *m_manager = nullptr;
    uint32_t m_managerName = 0;
    wl_pointer *m_pointer = nullptr; // the wl_pointer m_relative was made for (Qt's)
    zwp_relative_pointer_v1 *m_relative = nullptr;
};

} // namespace murasaki
