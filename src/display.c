#include "display.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define MARGIN_BOTTOM 64
#define DEFAULT_FADE_MS 120

struct Buf {
    struct wl_buffer *wl;
    cairo_surface_t *img;
    void *data;
    size_t size;
    int w, h;  /* logical */
};

struct Display {
    struct wl_display *wl;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_alpha_modifier_v1 *alpha_mgr;  /* optional: without it HUDs just appear and vanish */

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    Buf *pending;      /* waiting for the next configure */
    bool configured;
    int w, h;          /* logical size last sent to the compositor */
    int scale;

    /* Fade: the compositor applies the opacity (no redraw), paced by frame callbacks, so
     * nothing runs while no HUD is on screen. */
    struct wp_alpha_modifier_surface_v1 *alpha;
    struct wl_callback *frame;
    int fade_ms;
    double cur_alpha, from_alpha, to_alpha;
    uint32_t t0;
    bool t0_set, animating, hiding, mapped, fade_in_pending;
};

static void buf_free(Buf *b) {
    if (!b) return;
    if (b->img) cairo_surface_destroy(b->img);
    if (b->wl) wl_buffer_destroy(b->wl);
    munmap(b->data, b->size);
    free(b);
}

static void buffer_release(void *data, struct wl_buffer *wl) {
    (void)wl;
    buf_free(data);
}

static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    Display *d = data;
    if (!strcmp(iface, wl_compositor_interface.name))
        d->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wl_shm_interface.name))
        d->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        d->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wp_alpha_modifier_v1_interface.name))
        d->alpha_mgr = wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_remove,
};

static void destroy_surface(Display *d);

static void set_alpha(Display *d, double a) {
    d->cur_alpha = a;
    wp_alpha_modifier_surface_v1_set_multiplier(d->alpha, (uint32_t)(a * UINT32_MAX));
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = { .done = frame_done };

/* Takes effect with the next commit of the surface. */
static void request_frame(Display *d) {
    if (d->frame) return;
    d->frame = wl_surface_frame(d->surface);
    wl_callback_add_listener(d->frame, &frame_listener, d);
}

static void begin_fade(Display *d, double to) {
    d->from_alpha = d->cur_alpha;
    d->to_alpha = to;
    d->animating = true;
    d->t0_set = false;  /* the first frame callback sets the start time */
    request_frame(d);
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    Display *d = data;
    wl_callback_destroy(cb);
    d->frame = NULL;
    if (!d->animating || !d->surface) return;

    if (!d->t0_set) { d->t0 = time; d->t0_set = true; }
    double t = (double)(time - d->t0) / d->fade_ms;
    if (t > 1) t = 1;
    double eased = t * t * (3 - 2 * t);  /* smoothstep */
    set_alpha(d, d->from_alpha + (d->to_alpha - d->from_alpha) * eased);

    if (t >= 1) {
        d->animating = false;
        if (d->hiding) { destroy_surface(d); return; }
    } else {
        request_frame(d);
    }
    wl_surface_commit(d->surface);
}

static void attach(Display *d, Buf *b) {
    wl_surface_set_buffer_scale(d->surface, d->scale);
    wl_surface_attach(d->surface, b->wl, 0, 0);
    wl_surface_damage_buffer(d->surface, 0, 0, INT32_MAX, INT32_MAX);
    d->mapped = true;
    if (d->fade_in_pending) {  /* first content of a new surface: fade it in */
        d->fade_in_pending = false;
        begin_fade(d, 1.0);
    }
    wl_surface_commit(d->surface);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                            uint32_t w, uint32_t h) {
    Display *d = data;
    (void)w; (void)h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    d->configured = true;
    if (d->pending) {
        attach(d, d->pending);
        d->pending = NULL;  /* now owned by the compositor until release */
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    destroy_surface(data);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

Display *display_open(void) {
    Display *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->wl = wl_display_connect(NULL);
    if (!d->wl) { free(d); return NULL; }

    d->registry = wl_display_get_registry(d->wl);
    wl_registry_add_listener(d->registry, &registry_listener, d);
    wl_display_roundtrip(d->wl);
    if (!d->compositor || !d->shm || !d->layer_shell) {
        display_close(d);
        return NULL;
    }

    const char *s = getenv("PACHUD_SCALE");
    d->scale = s && atoi(s) > 0 && atoi(s) <= 4 ? atoi(s) : 1;
    const char *fade = getenv("PACHUD_FADE_MS");  /* 0 turns the fade off */
    d->fade_ms = fade && *fade ? atoi(fade) : DEFAULT_FADE_MS;
    if (d->fade_ms < 0 || d->fade_ms > 2000) d->fade_ms = DEFAULT_FADE_MS;
    if (d->fade_ms == 0 && d->alpha_mgr) {
        wp_alpha_modifier_v1_destroy(d->alpha_mgr);
        d->alpha_mgr = NULL;
    }
    return d;
}

void display_close(Display *d) {
    if (!d) return;
    destroy_surface(d);
    if (d->alpha_mgr) wp_alpha_modifier_v1_destroy(d->alpha_mgr);
    if (d->layer_shell) zwlr_layer_shell_v1_destroy(d->layer_shell);
    if (d->shm) wl_shm_destroy(d->shm);
    if (d->compositor) wl_compositor_destroy(d->compositor);
    if (d->registry) wl_registry_destroy(d->registry);
    wl_display_flush(d->wl);
    wl_display_disconnect(d->wl);
    free(d);
}

int display_fd(Display *d) { return wl_display_get_fd(d->wl); }
int display_scale(Display *d) { return d->scale; }

int display_before_poll(Display *d) {
    while (wl_display_prepare_read(d->wl) != 0)
        if (wl_display_dispatch_pending(d->wl) < 0) return -1;
    if (wl_display_flush(d->wl) < 0 && errno != EAGAIN) {
        wl_display_cancel_read(d->wl);
        return -1;
    }
    return 0;
}

int display_after_poll(Display *d, bool readable) {
    if (readable) {
        if (wl_display_read_events(d->wl) < 0) return -1;
    } else {
        wl_display_cancel_read(d->wl);
    }
    return wl_display_dispatch_pending(d->wl) < 0 ? -1 : 0;
}

Buf *display_buf(Display *d, int w, int h, cairo_t **cr) {
    int pw = w * d->scale, ph = h * d->scale;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
    size_t size = (size_t)stride * ph;

    int fd = memfd_create("pachud", MFD_CLOEXEC);
    if (fd < 0) return NULL;
    if (ftruncate(fd, size) < 0) { close(fd); return NULL; }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return NULL; }

    Buf *b = calloc(1, sizeof *b);
    if (!b) { munmap(data, size); close(fd); return NULL; }
    b->data = data;
    b->size = size;
    b->w = w;
    b->h = h;

    struct wl_shm_pool *pool = wl_shm_create_pool(d->shm, fd, size);
    b->wl = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->wl, &buffer_listener, b);

    b->img = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, pw, ph, stride);
    *cr = cairo_create(b->img);
    cairo_scale(*cr, d->scale, d->scale);
    return b;
}

static void create_surface(Display *d) {
    d->surface = wl_compositor_create_surface(d->compositor);
    d->layer = zwlr_layer_shell_v1_get_layer_surface(d->layer_shell, d->surface, NULL,
                                                     ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "pachud");
    zwlr_layer_surface_v1_set_anchor(d->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM);
    zwlr_layer_surface_v1_set_margin(d->layer, 0, 0, MARGIN_BOTTOM, 0);
    zwlr_layer_surface_v1_set_exclusive_zone(d->layer, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(d->layer,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(d->layer, &layer_listener, d);

    if (d->alpha_mgr) {  /* start invisible; the first buffer fades in */
        d->alpha = wp_alpha_modifier_v1_get_surface(d->alpha_mgr, d->surface);
        set_alpha(d, 0);
        d->fade_in_pending = true;
    } else {
        d->cur_alpha = 1;
    }

    /* An empty input region makes the HUD click-through. */
    struct wl_region *empty = wl_compositor_create_region(d->compositor);
    wl_surface_set_input_region(d->surface, empty);
    wl_region_destroy(empty);
}

void display_present(Display *d, Buf *b, cairo_t *cr) {
    cairo_destroy(cr);
    cairo_surface_flush(b->img);
    cairo_surface_destroy(b->img);
    b->img = NULL;

    if (!d->surface) create_surface(d);
    if (d->hiding) {  /* something new arrived while fading out: bring it back */
        d->hiding = false;
        begin_fade(d, 1.0);
    }
    if (d->configured && b->w == d->w && b->h == d->h) {
        attach(d, b);  /* same size: no configure round trip needed */
        return;
    }
    /* New or resized: tell the compositor and attach once its configure is acked. */
    zwlr_layer_surface_v1_set_size(d->layer, b->w, b->h);
    d->w = b->w;
    d->h = b->h;
    buf_free(d->pending);
    d->pending = b;
    d->configured = false;
    wl_surface_commit(d->surface);
}

static void destroy_surface(Display *d) {
    if (d->frame) wl_callback_destroy(d->frame);
    if (d->alpha) wp_alpha_modifier_surface_v1_destroy(d->alpha);
    if (d->layer) zwlr_layer_surface_v1_destroy(d->layer);
    if (d->surface) wl_surface_destroy(d->surface);
    d->frame = NULL;
    d->alpha = NULL;
    d->layer = NULL;
    d->surface = NULL;
    buf_free(d->pending);
    d->pending = NULL;
    d->configured = false;
    d->animating = d->hiding = d->mapped = d->fade_in_pending = false;
    d->w = d->h = 0;
}

void display_hide(Display *d) {
    if (!d->surface) return;
    if (d->alpha && d->mapped && !d->hiding) {  /* fade out, then frame_done() tears it down */
        d->hiding = true;
        begin_fade(d, 0.0);
        wl_surface_commit(d->surface);
        return;
    }
    if (!d->hiding) destroy_surface(d);
}
