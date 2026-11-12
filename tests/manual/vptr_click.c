/* A virtual pointer for click_test.sh, to click a HUD in a nested compositor. It stays alive for
 * the whole test, so the seat keeps one pointer, and reads commands from stdin, one per line:
 *   move X Y       put the pointer there (on a W x H output)
 *   click [BUTTON] press and release a button: 0x110 left (the default), 0x111 right
 * It exits when stdin closes.
 * Usage: vptr_click W H */
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

static struct wl_seat *seat;
static struct zwlr_virtual_pointer_manager_v1 *mgr;

static void global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t v) {
    (void)data; (void)v;
    if (!strcmp(iface, wl_seat_interface.name))
        seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
    else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name))
        mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 1);
}
static void global_remove(void *data, struct wl_registry *reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct wl_registry_listener reg_listener = { global, global_remove };

int main(int argc, char **argv) {
    if (argc != 3) { fprintf(stderr, "usage: vptr_click W H  (commands on stdin)\n"); return 2; }
    uint32_t w = atoi(argv[1]), h = atoi(argv[2]);
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "no Wayland display\n"); return 1; }
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &reg_listener, NULL);
    wl_display_roundtrip(d);
    if (!seat || !mgr) { fprintf(stderr, "no seat or virtual-pointer manager\n"); return 1; }
    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    wl_display_roundtrip(d);

    uint32_t t = 1000;
    char line[128];
    while (fgets(line, sizeof line, stdin)) {
        unsigned x, y, button = 0x110;
        if (sscanf(line, "move %u %u", &x, &y) == 2) {
            zwlr_virtual_pointer_v1_motion_absolute(p, t += 50, x, y, w, h);
            zwlr_virtual_pointer_v1_frame(p);
        } else if (!strncmp(line, "click", 5)) {
            if (sscanf(line, "click %i", &button) < 1) button = 0x110;
            zwlr_virtual_pointer_v1_button(p, t += 100, button, WL_POINTER_BUTTON_STATE_PRESSED);
            zwlr_virtual_pointer_v1_frame(p);
            zwlr_virtual_pointer_v1_button(p, t += 50, button, WL_POINTER_BUTTON_STATE_RELEASED);
            zwlr_virtual_pointer_v1_frame(p);
        }
        wl_display_roundtrip(d);
    }
    zwlr_virtual_pointer_v1_destroy(p);
    wl_display_roundtrip(d);
    return 0;
}
