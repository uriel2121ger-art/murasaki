// murasaki-fakecm: owns the compositing-manager selection (_NET_WM_CM_S<screen>) of the X display until it is killed, and composites nothing. For
// tools/test-overlay-x11.sh: the overlay shows a stage on X11 only when a compositing manager owns that selection (docs/pets/stage.md). Test tool, not installed.

#include <xcb/xcb.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

int main()
{
    int screenNumber = 0;
    xcb_connection_t *c = xcb_connect(nullptr, &screenNumber);
    if (xcb_connection_has_error(c)) {
        std::fprintf(stderr, "murasaki-fakecm: cannot open the display\n");
        return 1;
    }
    const xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    const xcb_window_t window = xcb_generate_id(c);
    xcb_create_window(c, XCB_COPY_FROM_PARENT, window, screen->root, 0, 0, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_ONLY, XCB_COPY_FROM_PARENT, 0, nullptr);
    const std::string name = "_NET_WM_CM_S" + std::to_string(screenNumber);
    xcb_intern_atom_reply_t *atom = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, uint16_t(name.size()), name.c_str()), nullptr);
    if (!atom)
        return 1;
    xcb_set_selection_owner(c, window, atom->atom, XCB_CURRENT_TIME);
    xcb_flush(c);
    std::free(atom);
    std::printf("owning %s\n", name.c_str());
    std::fflush(stdout);
    for (;;)
        pause();
}
