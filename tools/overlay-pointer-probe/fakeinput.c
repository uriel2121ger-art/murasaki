// Spike tool (not installed): moves and clicks the pointer of a nested compositor, through org_kde_kwin_fake_input on KWin
// and zwlr_virtual_pointer_v1 on wlroots compositors and Hyprland (whichever the compositor offers).
// Usage: fakeinput [--extent WxH] CMD...   or   fakeinput [--extent WxH] --stdin   (one line of commands at a time; quit ends it)
// CMD is one of:  abs X Y | rel DX DY | down | up | click | sleep MS | mark ID (prints "mark ID" when reached)
// --extent is the size of the output layout, which the virtual pointer's absolute motion is relative to (default 1280x720).
// KWin's permission check must be off (KWIN_WAYLAND_NO_PERMISSION_CHECKS=1), which the spike script does for the private
// compositor it starts. Never run it against a real session.
#include <wayland-client.h>

#include "fake-input-client-protocol.h"
#include "wlr-virtual-pointer-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static struct org_kde_kwin_fake_input *fake;
static struct zwlr_virtual_pointer_manager_v1 *manager;
static struct zwlr_virtual_pointer_v1 *pointer;
static uint32_t extent_w = 1280, extent_h = 720;

static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (strcmp(interface, "org_kde_kwin_fake_input") == 0)
        fake = wl_registry_bind(registry, name, &org_kde_kwin_fake_input_interface, version < 5 ? version : 5);
    else if (strcmp(interface, "zwlr_virtual_pointer_manager_v1") == 0)
        manager = wl_registry_bind(registry, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    (void)name;
}
static const struct wl_registry_listener registry_listener = {global, global_remove};

static void sleep_ms(long ms)
{
    struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void motion_absolute(double x, double y)
{
    if (fake) {
        org_kde_kwin_fake_input_pointer_motion_absolute(fake, wl_fixed_from_double(x), wl_fixed_from_double(y));
        return;
    }
    zwlr_virtual_pointer_v1_motion_absolute(pointer, now_ms(), (uint32_t)(x < 0 ? 0 : x), (uint32_t)(y < 0 ? 0 : y), extent_w,
                                            extent_h);
    zwlr_virtual_pointer_v1_frame(pointer);
}

static void motion(double dx, double dy)
{
    if (fake) {
        org_kde_kwin_fake_input_pointer_motion(fake, wl_fixed_from_double(dx), wl_fixed_from_double(dy));
        return;
    }
    zwlr_virtual_pointer_v1_motion(pointer, now_ms(), wl_fixed_from_double(dx), wl_fixed_from_double(dy));
    zwlr_virtual_pointer_v1_frame(pointer);
}

static void button(uint32_t state)
{
    if (fake) {
        org_kde_kwin_fake_input_button(fake, 0x110, state);
        return;
    }
    zwlr_virtual_pointer_v1_button(pointer, now_ms(), 0x110, state);
    zwlr_virtual_pointer_v1_frame(pointer);
}

static int run(struct wl_display *display, int argc, char **argv)
{
    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "abs") && i + 2 < argc) {
            motion_absolute(atof(argv[i + 1]), atof(argv[i + 2]));
            i += 2;
        } else if (!strcmp(argv[i], "rel") && i + 2 < argc) {
            motion(atof(argv[i + 1]), atof(argv[i + 2]));
            i += 2;
        } else if (!strcmp(argv[i], "down")) {
            button(1);
        } else if (!strcmp(argv[i], "up")) {
            button(0);
        } else if (!strcmp(argv[i], "click")) {
            button(1);
            wl_display_flush(display);
            sleep_ms(30);
            button(0);
        } else if (!strcmp(argv[i], "mark") && i + 1 < argc) {
            printf("mark %s\n", argv[++i]);
            fflush(stdout);
        } else if (!strcmp(argv[i], "sleep") && i + 1 < argc) {
            wl_display_flush(display);
            sleep_ms(atol(argv[++i]));
        } else {
            fprintf(stderr, "fakeinput: bad command %s\n", argv[i]);
            return 2;
        }
        wl_display_flush(display);
        wl_display_roundtrip(display);
    }
    return 0;
}

int main(int argc, char **argv)
{
    // Never the user's session: only the private sockets of the nested tests are accepted, KWin's (wayland-mon.XXXXXX) or
    // any socket in their private runtime directory (/tmp/mon.XXXXXX/, where sway and Hyprland pick wayland-N themselves).
    const char *socket_name = getenv("WAYLAND_DISPLAY");
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    const int private_socket = socket_name && strncmp(socket_name, "wayland-mon.", 12) == 0;
    const int private_runtime = socket_name && !strchr(socket_name, '/') && runtime && strncmp(runtime, "/tmp/mon.", 9) == 0;
    if (!private_socket && !private_runtime) {
        fprintf(stderr, "fakeinput: WAYLAND_DISPLAY is not a private nested socket (wayland-mon.* or under /tmp/mon.*), refusing\n");
        return 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "--extent")) {
        if (sscanf(argv[2], "%ux%u", &extent_w, &extent_h) != 2 || !extent_w || !extent_h) {
            fprintf(stderr, "fakeinput: bad extent %s\n", argv[2]);
            return 2;
        }
        argc -= 2;
        argv += 2;
    }
    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "fakeinput: cannot connect to the compositor\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (fake) {
        org_kde_kwin_fake_input_authenticate(fake, "murasaki spike", "pointer tests in a private nested session");
    } else if (manager) {
        pointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(manager, NULL);
    } else {
        fprintf(stderr, "fakeinput: the compositor offers neither org_kde_kwin_fake_input nor zwlr_virtual_pointer_manager_v1\n");
        return 1;
    }
    wl_display_roundtrip(display);
    if (argc == 2 && !strcmp(argv[1], "--stdin")) {
        // One connection for the whole test: a new fake-input device per process makes the compositor announce the seat
        // again, and the client then recreates its wl_pointer (which loses the pointer focus and the leave events).
        char *line = NULL;
        size_t cap = 0;
        while (getline(&line, &cap, stdin) > 0) {
            static char *words[65536];
            int n = 0;
            for (char *w = strtok(line, " \t\r\n"); w && n < 65536; w = strtok(NULL, " \t\r\n"))
                words[n++] = w;
            if (n == 1 && !strcmp(words[0], "quit"))
                break;
            int rc = run(display, n, words);
            if (rc)
                return rc;
        }
        free(line);
    } else {
        int rc = run(display, argc - 1, argv + 1);
        if (rc)
            return rc;
    }
    if (fake)
        org_kde_kwin_fake_input_destroy(fake);
    if (pointer)
        zwlr_virtual_pointer_v1_destroy(pointer);
    if (manager)
        zwlr_virtual_pointer_manager_v1_destroy(manager);
    wl_display_roundtrip(display);
    wl_display_disconnect(display);
    return 0;
}
