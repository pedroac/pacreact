#include "display.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "scale.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define MARGIN 16
#define DEFAULT_FADE_MS 120
#define MAX_OUTPUTS 8

struct Buf {
    struct wl_buffer *wl;
    cairo_surface_t *img;
    void *data;
    size_t size;
    int w, h;       /* logical */
    int scale120;   /* what it was rendered at */
};

typedef struct {
    uint32_t global;            /* registry name, to notice when it goes away */
    struct wl_output *proxy;    /* NULL: slot unused */
    char name[64];              /* "DP-1", "HDMI-A-1", ... */
} Output;

struct Display {
    struct wl_display *wl;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_shm *shm;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_alpha_modifier_v1 *alpha_mgr;  /* optional: without it HUDs just appear and vanish */

    Output outputs[MAX_OUTPUTS];  /* every monitor, so PACHUD_OUTPUT can pick one by name */
    const char *want_output;      /* PACHUD_OUTPUT, or NULL to let the compositor choose */
    bool warned_output;
    bool click;                   /* PACHUD_CLICK is not 0 */
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_surface *pointer_surface;  /* the surface the pointer is over, if it is ours */
    double px, py;                /* the pointer, in that surface's logical pixels */
    ClickRect rects[CLICK_RECTS_MAX];
    int nrects;
    void (*click_cb)(int x, int y);
    uint32_t anchor;              /* where the notification stack sits: PACHUD_ANCHOR */
    int align;                    /* 0 left, 1 centre, 2 right: how items line up in the stack */

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer;
    Buf *pending;      /* waiting for the next configure */
    bool configured;
    int w, h;          /* logical size last sent to the compositor */
    int scale120;        /* what new buffers are rendered at, in 120ths */
    bool scale_fixed;    /* PACHUD_SCALE: ignore what the compositor prefers */
    struct wp_fractional_scale_manager_v1 *frac_mgr;  /* optional: else whole-number scales */
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_v1 *frac;
    struct wp_viewport *viewport;
    void (*rescale_cb)(void);

    /* Fade: the compositor applies the opacity (no redraw), paced by frame callbacks, so
     * nothing runs while no HUD is on screen. */
    struct wp_alpha_modifier_surface_v1 *alpha;
    struct wl_callback *frame;
    int fade_ms;
    double cur_alpha, from_alpha, to_alpha;
    uint32_t t0;
    bool t0_set, animating, hiding, mapped, fade_in_pending;

    /* OSD (on-screen display): separate layer for user-triggered feedback at screen center.
     * Reuses the shared Wayland interfaces (compositor, shm, layer_shell, etc.). */
    struct wl_surface *osd_surface;
    struct zwlr_layer_surface_v1 *osd_layer;
    Buf *osd_pending;
    bool osd_configured;
    int osd_w, osd_h;
    struct wp_fractional_scale_v1 *osd_frac;
    struct wp_viewport *osd_viewport;
    bool osd_mapped;
    OsdAnchor osd_anchor;
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

/* Only the name matters; the other output events have no use here. */
static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                         int32_t subpixel, const char *make, const char *model, int32_t transform) {
    (void)data; (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel; (void)make;
    (void)model; (void)transform;
}
static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r) {
    (void)data; (void)o; (void)flags; (void)w; (void)h; (void)r;
}
static void out_done(void *data, struct wl_output *o) { (void)data; (void)o; }
static void out_scale(void *data, struct wl_output *o, int32_t f) { (void)data; (void)o; (void)f; }
static void out_name(void *data, struct wl_output *o, const char *name) {
    (void)o;
    snprintf(((Output *)data)->name, sizeof(((Output *)data)->name), "%s", name);
}
static void out_description(void *data, struct wl_output *o, const char *d) { (void)data; (void)o; (void)d; }

static const struct wl_output_listener output_listener = {
    .geometry = out_geometry, .mode = out_mode, .done = out_done, .scale = out_scale,
    .name = out_name, .description = out_description,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                      wl_fixed_t x, wl_fixed_t y) {
    Display *d = data;
    (void)p; (void)serial;
    d->pointer_surface = s;
    d->px = wl_fixed_to_double(x);
    d->py = wl_fixed_to_double(y);
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    Display *d = data;
    (void)p; (void)serial; (void)s;
    d->pointer_surface = NULL;
}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    Display *d = data;
    (void)p; (void)time;
    d->px = wl_fixed_to_double(x);
    d->py = wl_fixed_to_double(y);
}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state) {
    Display *d = data;
    (void)p; (void)serial; (void)time;
    if (button == BTN_LEFT && state == WL_POINTER_BUTTON_STATE_PRESSED && d->surface &&
        d->pointer_surface == d->surface && d->click_cb)
        d->click_cb((int)d->px, (int)d->py);
}
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v) {
    (void)data; (void)p; (void)time; (void)axis; (void)v;
}
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button, .axis = ptr_axis,
};

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps) {
    Display *d = data;
    bool has = caps & WL_SEAT_CAPABILITY_POINTER;
    if (has && !d->pointer) {
        d->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(d->pointer, &pointer_listener, d);
    } else if (!has && d->pointer) {
        wl_pointer_destroy(d->pointer);
        d->pointer = NULL;
        d->pointer_surface = NULL;
    }
}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps };

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    Display *d = data;
    if (!strcmp(iface, wl_output_interface.name) && version >= 4) {  /* names need version 4 */
        for (int i = 0; i < MAX_OUTPUTS; i++) {
            if (d->outputs[i].proxy) continue;
            d->outputs[i].global = name;
            d->outputs[i].name[0] = '\0';
            d->outputs[i].proxy = wl_registry_bind(reg, name, &wl_output_interface, 4);
            wl_output_add_listener(d->outputs[i].proxy, &output_listener, &d->outputs[i]);
            break;
        }
        return;
    }
    if (!strcmp(iface, wl_compositor_interface.name))
        d->compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wl_seat_interface.name) && d->click && !d->seat) {
        d->seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
        wl_seat_add_listener(d->seat, &seat_listener, d);
    } else if (!strcmp(iface, wl_shm_interface.name))
        d->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
        d->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface, version < 4 ? version : 4);
    else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name))
        d->frac_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
    else if (!strcmp(iface, wp_viewporter_interface.name))
        d->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
    else if (!strcmp(iface, wp_alpha_modifier_v1_interface.name))
        d->alpha_mgr = wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
    Display *d = data;
    (void)reg;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (d->outputs[i].proxy && d->outputs[i].global == name) {  /* a monitor was unplugged */
            wl_output_release(d->outputs[i].proxy);
            d->outputs[i].proxy = NULL;
            d->outputs[i].name[0] = '\0';
        }
    }
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
    if (d->viewport) wp_viewport_set_destination(d->viewport, b->w, b->h);  /* logical size */
    else wl_surface_set_buffer_scale(d->surface, whole_scale(b->scale120));
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

#define EDGE_T ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
#define EDGE_B ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
#define EDGE_L ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT
#define EDGE_R ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT

/* PACHUD_ANCHOR: top-right (the default), top-left, top, bottom-right, bottom-left or bottom. */
static void parse_anchor(Display *d, const char *name) {
    static const struct { const char *name; uint32_t edges; int align; } table[] = {
        { "top-right", EDGE_T | EDGE_R, 2 }, { "top-left", EDGE_T | EDGE_L, 0 }, { "top", EDGE_T, 1 },
        { "bottom-right", EDGE_B | EDGE_R, 2 }, { "bottom-left", EDGE_B | EDGE_L, 0 },
        { "bottom", EDGE_B, 1 },
    };
    d->anchor = table[0].edges;
    d->align = table[0].align;
    if (!name || !*name) return;
    for (size_t i = 0; i < sizeof table / sizeof *table; i++) {
        if (!strcmp(name, table[i].name)) {
            d->anchor = table[i].edges;
            d->align = table[i].align;
            return;
        }
    }
    fprintf(stderr, "pachud: PACHUD_ANCHOR \"%s\" is not one of top-right, top-left, top, "
                    "bottom-right, bottom-left, bottom\n", name);
}

Display *display_open(void) {
    Display *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    const char *click = getenv("PACHUD_CLICK");
    d->click = !(click && !strcmp(click, "0"));
    d->wl = wl_display_connect(NULL);
    if (!d->wl) { free(d); return NULL; }

    d->registry = wl_display_get_registry(d->wl);
    wl_registry_add_listener(d->registry, &registry_listener, d);
    wl_display_roundtrip(d->wl);
    if (!d->compositor || !d->shm || !d->layer_shell) {
        display_close(d);
        return NULL;
    }

    wl_display_roundtrip(d->wl);  /* the output names arrive after the outputs are bound */
    const char *want = getenv("PACHUD_OUTPUT");
    d->want_output = want && *want ? want : NULL;
    parse_anchor(d, getenv("PACHUD_ANCHOR"));

    int fixed = parse_scale120(getenv("PACHUD_SCALE"));  /* e.g. 1.5, 2; unset follows the compositor */
    d->scale120 = fixed ? fixed : SCALE_UNIT;
    d->scale_fixed = fixed != 0;
    const char *fade = getenv("PACHUD_FADE_MS");  /* 0 turns the fade off */
    d->fade_ms = fade && *fade ? atoi(fade) : DEFAULT_FADE_MS;
    if (d->fade_ms < 0 || d->fade_ms > 2000) d->fade_ms = DEFAULT_FADE_MS;
    if (d->fade_ms == 0 && d->alpha_mgr) {
        wp_alpha_modifier_v1_destroy(d->alpha_mgr);
        d->alpha_mgr = NULL;
    }
    return d;
}

static void destroy_osd_surface(Display *d);

void display_close(Display *d) {
    if (!d) return;
    destroy_surface(d);
    destroy_osd_surface(d);
    if (d->alpha_mgr) wp_alpha_modifier_v1_destroy(d->alpha_mgr);
    if (d->frac_mgr) wp_fractional_scale_manager_v1_destroy(d->frac_mgr);
    if (d->viewporter) wp_viewporter_destroy(d->viewporter);
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (d->outputs[i].proxy) wl_output_release(d->outputs[i].proxy);
    if (d->pointer) wl_pointer_destroy(d->pointer);
    if (d->seat) wl_seat_destroy(d->seat);
    if (d->layer_shell) zwlr_layer_shell_v1_destroy(d->layer_shell);
    if (d->shm) wl_shm_destroy(d->shm);
    if (d->compositor) wl_compositor_destroy(d->compositor);
    if (d->registry) wl_registry_destroy(d->registry);
    wl_display_flush(d->wl);
    wl_display_disconnect(d->wl);
    free(d);
}

int display_fd(Display *d) { return wl_display_get_fd(d->wl); }
int display_scale(Display *d) { return d->scale120; }
int display_align(Display *d) { return d->align; }
void display_on_rescale(Display *d, void (*fn)(void)) { d->rescale_cb = fn; }

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
    /* Without the viewport protocol only whole scales are possible */
    int scale120 = d->frac_mgr && d->viewporter ? d->scale120 : whole_scale(d->scale120) * SCALE_UNIT;
    int pw = scaled_px(w, scale120), ph = scaled_px(h, scale120);
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
    b->scale120 = scale120;

    struct wl_shm_pool *pool = wl_shm_create_pool(d->shm, fd, size);
    b->wl = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    wl_buffer_add_listener(b->wl, &buffer_listener, b);

    b->img = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_ARGB32, pw, ph, stride);
    *cr = cairo_create(b->img);
    cairo_scale(*cr, (double)pw / w, (double)ph / h);  /* logical units, whatever the scale */
    return b;
}

static void preferred_scale(void *data, struct wp_fractional_scale_v1 *f, uint32_t scale) {
    Display *d = data;
    (void)f;
    if (d->scale_fixed || scale < SCALE_MIN || scale > SCALE_MAX || (int)scale == d->scale120) return;
    d->scale120 = (int)scale;  /* later HUDs start at the right scale */
    if (d->surface && d->rescale_cb) d->rescale_cb();  /* and the one on screen is drawn again */
}

static const struct wp_fractional_scale_v1_listener fractional_listener = {
    .preferred_scale = preferred_scale,
};

/* The monitor named by PACHUD_OUTPUT, else NULL: the compositor then chooses (usually the
 * focused one). An unknown name is reported once, with the names that exist. */
static struct wl_output *pick_output(Display *d) {
    if (!d->want_output) return NULL;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (d->outputs[i].proxy && !strcmp(d->outputs[i].name, d->want_output)) return d->outputs[i].proxy;

    if (!d->warned_output) {
        d->warned_output = true;
        fprintf(stderr, "pachud: output \"%s\" not found; available:", d->want_output);
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (d->outputs[i].proxy && d->outputs[i].name[0]) fprintf(stderr, " %s", d->outputs[i].name);
        fputc('\n', stderr);
    }
    return NULL;
}

/* An empty input region makes the HUD click-through; clicks are taken only inside the rectangles. */
static void apply_click_region(Display *d) {
    if (!d->surface) return;
    struct wl_region *region = wl_compositor_create_region(d->compositor);
    for (int i = 0; d->click && i < d->nrects; i++)
        wl_region_add(region, d->rects[i].x, d->rects[i].y, d->rects[i].w, d->rects[i].h);
    wl_surface_set_input_region(d->surface, region);
    wl_region_destroy(region);
}

void display_set_click_rects(Display *d, const ClickRect *rects, int n) {
    d->nrects = n > CLICK_RECTS_MAX ? CLICK_RECTS_MAX : n;
    for (int i = 0; i < d->nrects; i++) d->rects[i] = rects[i];
    apply_click_region(d);
}

void display_on_click(Display *d, void (*fn)(int x, int y)) { d->click_cb = fn; }

static void create_surface(Display *d) {
    d->surface = wl_compositor_create_surface(d->compositor);
    d->layer = zwlr_layer_shell_v1_get_layer_surface(d->layer_shell, d->surface, pick_output(d),
                                                     ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "pachud");
    zwlr_layer_surface_v1_set_anchor(d->layer, d->anchor);
    zwlr_layer_surface_v1_set_margin(d->layer, MARGIN, MARGIN, MARGIN, MARGIN);
    zwlr_layer_surface_v1_set_exclusive_zone(d->layer, 0);  /* stay clear of bars and docks */
    zwlr_layer_surface_v1_set_keyboard_interactivity(d->layer,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(d->layer, &layer_listener, d);

    if (d->frac_mgr && d->viewporter) {  /* fractional scales: render large, present at logical size */
        d->frac = wp_fractional_scale_manager_v1_get_fractional_scale(d->frac_mgr, d->surface);
        wp_fractional_scale_v1_add_listener(d->frac, &fractional_listener, d);
        d->viewport = wp_viewporter_get_viewport(d->viewporter, d->surface);
    }

    if (d->alpha_mgr) {  /* start invisible; the first buffer fades in */
        d->alpha = wp_alpha_modifier_v1_get_surface(d->alpha_mgr, d->surface);
        set_alpha(d, 0);
        d->fade_in_pending = true;
    } else {
        d->cur_alpha = 1;
    }

    apply_click_region(d);
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
    if (d->pointer_surface == d->surface) d->pointer_surface = NULL;
    if (d->frame) wl_callback_destroy(d->frame);
    if (d->viewport) wp_viewport_destroy(d->viewport);
    if (d->frac) wp_fractional_scale_v1_destroy(d->frac);
    d->viewport = NULL;
    d->frac = NULL;
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

/* ---- OSD surface (on-screen display: volume, device changes) ---- */

static void osd_attach(Display *d, Buf *b) {
    if (d->osd_viewport) wp_viewport_set_destination(d->osd_viewport, b->w, b->h);
    else wl_surface_set_buffer_scale(d->osd_surface, whole_scale(b->scale120));
    wl_surface_attach(d->osd_surface, b->wl, 0, 0);
    wl_surface_damage_buffer(d->osd_surface, 0, 0, INT32_MAX, INT32_MAX);
    d->osd_mapped = true;
    wl_surface_commit(d->osd_surface);
}

static void osd_layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                                uint32_t w, uint32_t h) {
    Display *d = data;
    (void)w; (void)h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    d->osd_configured = true;
    if (d->osd_pending) {
        osd_attach(d, d->osd_pending);
        d->osd_pending = NULL;  /* owned by the compositor until release */
    }
}

static void osd_layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    (void)ls;
    destroy_osd_surface(data);
}

static const struct zwlr_layer_surface_v1_listener osd_layer_listener = {
    .configure = osd_layer_configure,
    .closed = osd_layer_closed,
};

static void destroy_osd_surface(Display *d) {
    if (d->osd_viewport) wp_viewport_destroy(d->osd_viewport);
    if (d->osd_frac) wp_fractional_scale_v1_destroy(d->osd_frac);
    if (d->osd_layer) zwlr_layer_surface_v1_destroy(d->osd_layer);
    if (d->osd_surface) wl_surface_destroy(d->osd_surface);
    d->osd_viewport = NULL;
    d->osd_frac = NULL;
    d->osd_layer = NULL;
    d->osd_surface = NULL;
    buf_free(d->osd_pending);
    d->osd_pending = NULL;
    d->osd_configured = d->osd_mapped = false;
    d->osd_w = d->osd_h = 0;
}

static void osd_set_anchor(Display *d, OsdAnchor a) {
    static const uint32_t edges[] = { [OSD_CENTER] = 0, [OSD_TOP] = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
                                      [OSD_BOTTOM] = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM };
    zwlr_layer_surface_v1_set_anchor(d->osd_layer, edges[a]);
    d->osd_anchor = a;
}

static void create_osd_surface(Display *d, OsdAnchor anchor) {
    d->osd_surface = wl_compositor_create_surface(d->compositor);
    d->osd_layer = zwlr_layer_shell_v1_get_layer_surface(d->layer_shell, d->osd_surface, pick_output(d),
                                                         ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "pachud-osd");
    osd_set_anchor(d, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(d->osd_layer, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(d->osd_layer,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(d->osd_layer, &osd_layer_listener, d);

    if (d->frac_mgr && d->viewporter) {
        d->osd_frac = wp_fractional_scale_manager_v1_get_fractional_scale(d->frac_mgr, d->osd_surface);
        wp_fractional_scale_v1_add_listener(d->osd_frac, &fractional_listener, d);
        d->osd_viewport = wp_viewporter_get_viewport(d->viewporter, d->osd_surface);
    }

    struct wl_region *empty = wl_compositor_create_region(d->compositor);
    wl_surface_set_input_region(d->osd_surface, empty);
    wl_region_destroy(empty);
}

Buf *display_osd_buf(Display *dpy, int w, int h, cairo_t **cr) {
    return display_buf(dpy, w, h, cr);
}

void display_osd_present(Display *d, Buf *b, cairo_t *cr, OsdAnchor anchor) {
    cairo_destroy(cr);
    cairo_surface_flush(b->img);
    cairo_surface_destroy(b->img);
    b->img = NULL;

    if (!d->osd_surface) create_osd_surface(d, anchor);
    bool moved = d->osd_anchor != anchor;
    if (moved) osd_set_anchor(d, anchor);
    if (!moved && d->osd_configured && b->w == d->osd_w && b->h == d->osd_h) {
        osd_attach(d, b);
        return;
    }
    zwlr_layer_surface_v1_set_size(d->osd_layer, b->w, b->h);
    d->osd_w = b->w;
    d->osd_h = b->h;
    buf_free(d->osd_pending);
    d->osd_pending = b;
    d->osd_configured = false;
    wl_surface_commit(d->osd_surface);
}

void display_osd_hide(Display *d) {
    destroy_osd_surface(d);
}
