/* Moves a virtual pointer to X,Y (on a W x H output) and left-clicks there, so click_test.sh can
 * click a HUD in a nested compositor. Usage: vptr_click X Y W H */
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
    if (argc != 5) { fprintf(stderr, "usage: vptr_click X Y W H\n"); return 2; }
    uint32_t x = atoi(argv[1]), y = atoi(argv[2]), w = atoi(argv[3]), h = atoi(argv[4]);
    struct wl_display *d = wl_display_connect(NULL);
    if (!d) { fprintf(stderr, "no Wayland display\n"); return 1; }
    struct wl_registry *r = wl_display_get_registry(d);
    wl_registry_add_listener(r, &reg_listener, NULL);
    wl_display_roundtrip(d);
    if (!seat || !mgr) { fprintf(stderr, "no seat or virtual-pointer manager\n"); return 1; }

    struct zwlr_virtual_pointer_v1 *p = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, seat);
    zwlr_virtual_pointer_v1_motion_absolute(p, 100, x, y, w, h);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(d);
    usleep(400000);   /* let the client under the pointer see it enter */
    zwlr_virtual_pointer_v1_button(p, 600, 0x110, WL_POINTER_BUTTON_STATE_PRESSED);
    zwlr_virtual_pointer_v1_frame(p);
    zwlr_virtual_pointer_v1_button(p, 650, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
    zwlr_virtual_pointer_v1_frame(p);
    wl_display_roundtrip(d);
    usleep(200000);
    zwlr_virtual_pointer_v1_destroy(p);
    wl_display_roundtrip(d);
    return 0;
}
