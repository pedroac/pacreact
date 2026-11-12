#include "display.h"
#include "alpha-modifier-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "scale.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "util.h"
#include <errno.h>
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define DEFAULT_MARGIN 16   /* from the screen's edge, in pixels; PACHUD_MARGIN */
#define MARGIN_MAX 200
#define DEFAULT_FADE_MS 120
#define MAX_OUTPUTS 8
#define DECK_GAP 16   /* the least space between the deck and the stack for them to count as apart */

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
    struct zxdg_output_v1 *xdg; /* what tells us the logical size, when the compositor has xdg-output */
    int logical_w;              /* in logical pixels, 0 until told */
    char name[64];              /* "DP-1", "HDMI-A-1", ... */
} Output;

/* A layer surface with what it takes to show buffers: the notification stack and the on-screen
 * display each have one, and differ only in the hooks. */
typedef struct Surface Surface;
struct Surface {
    Display *d;
    struct wl_surface *wl;
    struct zwlr_layer_surface_v1 *layer;
    struct wp_fractional_scale_v1 *frac;   /* optional: else whole-number scales */
    struct wp_viewport *viewport;
    Buf *pending;      /* waiting for the next configure */
    bool configured, mapped;
    int w, h;          /* logical size last sent to the compositor */
    ClickRect rects[CLICK_RECTS_MAX];      /* where it takes clicks: the rest is click-through */
    int nrects;
    void (*attach)(Surface *s, Buf *b);    /* shows a buffer; the stack also fades in on the first */
    void (*destroy)(Surface *s);           /* the compositor closed it */
};

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
    void (*click_cb)(int area, int x, int y, bool secondary);
    void (*hover_cb)(int area, int x, int y, bool over);
    uint32_t click_serial;        /* the input serial of the click being handled */
    struct zxdg_output_manager_v1 *xdg_manager;   /* optional: without it the screen's size is not known */
    int last_out;                 /* 1 + the index of the output the HUD last appeared on, 0 for none yet */
    struct xdg_activation_v1 *activation;   /* optional: without it clicks carry no token */
    struct xdg_activation_token_v1 *token;  /* a request waiting for the compositor's answer */
    void (*token_fn)(const char *token, void *ctx);
    void *token_ctx;
    int margin;                   /* the distance of the stack, and the deck's top, from the screen's edge */
    uint32_t anchor;              /* where the notification stack sits: PACHUD_ANCHOR */
    int align;                    /* 0 left, 1 centre, 2 right: how items line up in the stack */

    Surface main;      /* the notification stack */
    Surface deck;      /* the attention deck, at the top centre */
    Surface osd;       /* the on-screen display: volume, brightness, device changes */
    OsdAnchor osd_anchor;
    int scale120;        /* what new buffers are rendered at, in 120ths */
    bool scale_fixed;    /* PACHUD_SCALE: ignore what the compositor prefers */
    struct wp_fractional_scale_manager_v1 *frac_mgr;  /* optional: else whole-number scales */
    struct wp_viewporter *viewporter;
    void (*rescale_cb)(void);

    /* Fade: the compositor applies the opacity (no redraw), paced by frame callbacks, so
     * nothing runs while no HUD is on screen. */
    struct wp_alpha_modifier_surface_v1 *alpha;
    struct wl_callback *frame;
    int fade_ms;
    double cur_alpha, from_alpha, to_alpha;
    uint32_t t0;
    bool t0_set, animating, hiding, fade_in_pending;

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

/* xdg-output: the logical size of an output, which is what layer surfaces are measured in: the
 * physical size divided by the scale (fractional ones too) and turned for a rotated screen. */
static void xdg_position(void *data, struct zxdg_output_v1 *x, int32_t px, int32_t py) {
    (void)data; (void)x; (void)px; (void)py;
}
static void xdg_size(void *data, struct zxdg_output_v1 *x, int32_t w, int32_t h) {
    (void)x; (void)h;
    ((Output *)data)->logical_w = w;
}
static void xdg_done(void *data, struct zxdg_output_v1 *x) { (void)data; (void)x; }
static void xdg_name(void *data, struct zxdg_output_v1 *x, const char *name) { (void)data; (void)x; (void)name; }
static void xdg_description(void *data, struct zxdg_output_v1 *x, const char *d) { (void)data; (void)x; (void)d; }

static const struct zxdg_output_v1_listener xdg_output_listener = {
    .logical_position = xdg_position, .logical_size = xdg_size, .done = xdg_done,
    .name = xdg_name, .description = xdg_description,
};

/* Starts listening for the size of an output, once there is both the output and the manager */
static void watch_output(Display *d, Output *o) {
    if (!d->xdg_manager || !o->proxy || o->xdg) return;
    o->xdg = zxdg_output_manager_v1_get_xdg_output(d->xdg_manager, o->proxy);
    zxdg_output_v1_add_listener(o->xdg, &xdg_output_listener, o);
}

/* Which of the HUD's areas a surface is, or -1 if it is not ours (or has no input at all) */
static int area_of(Display *d, struct wl_surface *s) {
    if (!s) return -1;
    return s == d->deck.wl ? DISPLAY_AREA_DECK : s == d->main.wl ? DISPLAY_AREA_STACK : -1;
}

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
                      wl_fixed_t x, wl_fixed_t y) {
    Display *d = data;
    (void)p; (void)serial;
    d->pointer_surface = s;
    d->px = wl_fixed_to_double(x);
    d->py = wl_fixed_to_double(y);
    int area = area_of(d, s);
    if (area >= 0 && d->hover_cb) d->hover_cb(area, (int)d->px, (int)d->py, true);
}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    Display *d = data;
    (void)p; (void)serial; (void)s;
    int area = area_of(d, d->pointer_surface);
    d->pointer_surface = NULL;
    if (area >= 0 && d->hover_cb) d->hover_cb(area, 0, 0, false);
}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y) {
    Display *d = data;
    (void)p; (void)time;
    d->px = wl_fixed_to_double(x);
    d->py = wl_fixed_to_double(y);
    int area = area_of(d, d->pointer_surface);
    if (area >= 0 && d->hover_cb) d->hover_cb(area, (int)d->px, (int)d->py, true);
}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state) {
    Display *d = data;
    (void)p; (void)time;
    d->click_serial = serial;
    int area = area_of(d, d->pointer_surface);
    if ((button == BTN_LEFT || button == BTN_RIGHT) && state == WL_POINTER_BUTTON_STATE_PRESSED &&
        area >= 0 && d->click_cb)
        d->click_cb(area, (int)d->px, (int)d->py, button == BTN_RIGHT);
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
            d->outputs[i].logical_w = 0;
            wl_output_add_listener(d->outputs[i].proxy, &output_listener, &d->outputs[i]);
            watch_output(d, &d->outputs[i]);
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
    else if (!strcmp(iface, zxdg_output_manager_v1_interface.name)) {
        d->xdg_manager = wl_registry_bind(reg, name, &zxdg_output_manager_v1_interface, 1);
        for (int i = 0; i < MAX_OUTPUTS; i++) watch_output(d, &d->outputs[i]);   /* outputs that came first */
    } else if (!strcmp(iface, xdg_activation_v1_interface.name))
        d->activation = wl_registry_bind(reg, name, &xdg_activation_v1_interface, 1);
    else if (!strcmp(iface, wp_alpha_modifier_v1_interface.name))
        d->alpha_mgr = wl_registry_bind(reg, name, &wp_alpha_modifier_v1_interface, 1);
}

static void registry_remove(void *data, struct wl_registry *reg, uint32_t name) {
    Display *d = data;
    (void)reg;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (d->outputs[i].proxy && d->outputs[i].global == name) {  /* a monitor was unplugged */
            if (d->outputs[i].xdg) zxdg_output_v1_destroy(d->outputs[i].xdg);
            wl_output_release(d->outputs[i].proxy);
            d->outputs[i].proxy = NULL;
            d->outputs[i].xdg = NULL;
            d->outputs[i].logical_w = 0;
            d->outputs[i].name[0] = '\0';
            if (d->last_out == i + 1) d->last_out = 0;
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
    d->frame = wl_surface_frame(d->main.wl);
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
    if (!d->animating || !d->main.wl) return;

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
    wl_surface_commit(d->main.wl);
}

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
    d->margin = (int)env_int("PACHUD_MARGIN", DEFAULT_MARGIN, 0, MARGIN_MAX, "pixels");

    int fixed = parse_scale120(getenv("PACHUD_SCALE"));  /* e.g. 1.5, 2; unset follows the compositor */
    d->scale120 = fixed ? fixed : SCALE_UNIT;
    d->scale_fixed = fixed != 0;
    d->fade_ms = (int)env_int("PACHUD_FADE_MS", DEFAULT_FADE_MS, 0, 2000, "milliseconds");  /* 0 turns the fade off */
    if (d->fade_ms == 0 && d->alpha_mgr) {
        wp_alpha_modifier_v1_destroy(d->alpha_mgr);
        d->alpha_mgr = NULL;
    }
    return d;
}

static void surf_destroy(Surface *s);
static void deck_destroy(Surface *s);

void display_close(Display *d) {
    if (!d) return;
    destroy_surface(d);
    deck_destroy(&d->deck);
    surf_destroy(&d->osd);
    if (d->alpha_mgr) wp_alpha_modifier_v1_destroy(d->alpha_mgr);
    if (d->frac_mgr) wp_fractional_scale_manager_v1_destroy(d->frac_mgr);
    if (d->viewporter) wp_viewporter_destroy(d->viewporter);
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (d->outputs[i].xdg) zxdg_output_v1_destroy(d->outputs[i].xdg);
        if (d->outputs[i].proxy) wl_output_release(d->outputs[i].proxy);
    }
    if (d->xdg_manager) zxdg_output_manager_v1_destroy(d->xdg_manager);
    if (d->token) xdg_activation_token_v1_destroy(d->token);
    if (d->activation) xdg_activation_v1_destroy(d->activation);
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
    if ((d->main.wl || d->deck.wl) && d->rescale_cb) d->rescale_cb();  /* and the one on screen is drawn again */
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

/* An empty input region makes a surface click-through; clicks are taken only inside its rectangles. */
static void apply_click_region(Surface *s) {
    if (!s->wl) return;
    struct wl_region *region = wl_compositor_create_region(s->d->compositor);
    for (int i = 0; s->d->click && i < s->nrects; i++)
        wl_region_add(region, s->rects[i].x, s->rects[i].y, s->rects[i].w, s->rects[i].h);
    wl_surface_set_input_region(s->wl, region);
    wl_region_destroy(region);
}

void display_set_click_rects(Display *d, int area, const ClickRect *rects, int n) {
    Surface *s = area == DISPLAY_AREA_DECK ? &d->deck : &d->main;
    s->nrects = n > CLICK_RECTS_MAX ? CLICK_RECTS_MAX : n;
    for (int i = 0; i < s->nrects; i++) s->rects[i] = rects[i];
    apply_click_region(s);
}

static void token_done(void *data, struct xdg_activation_token_v1 *t, const char *token) {
    Display *d = data;
    void (*fn)(const char *, void *) = d->token_fn;
    void *ctx = d->token_ctx;
    xdg_activation_token_v1_destroy(t);
    d->token = NULL;
    d->token_fn = NULL;
    fn(token, ctx);
}
static const struct xdg_activation_token_v1_listener token_listener = { .done = token_done };

bool display_request_token(Display *d, void (*fn)(const char *token, void *ctx), void *ctx) {
    struct wl_surface *clicked = d->pointer_surface;   /* the surface of the click being handled */
    if (!d->activation || !d->seat || !clicked || d->token) return false;
    d->token = xdg_activation_v1_get_activation_token(d->activation);
    xdg_activation_token_v1_add_listener(d->token, &token_listener, d);
    xdg_activation_token_v1_set_serial(d->token, d->click_serial, d->seat);
    xdg_activation_token_v1_set_app_id(d->token, "pachud");
    xdg_activation_token_v1_set_surface(d->token, clicked);
    xdg_activation_token_v1_commit(d->token);
    d->token_fn = fn;
    d->token_ctx = ctx;
    return true;
}

void display_on_click(Display *d, void (*fn)(int area, int x, int y, bool secondary)) { d->click_cb = fn; }
void display_on_hover(Display *d, void (*fn)(int area, int x, int y, bool over)) { d->hover_cb = fn; }

static void buf_finish(Buf *b, cairo_t *cr) {
    cairo_destroy(cr);
    cairo_surface_flush(b->img);
    cairo_surface_destroy(b->img);
    b->img = NULL;
}

/* Hands a buffer to the compositor (the caller commits). */
static void surf_attach_buffer(Surface *s, Buf *b) {
    if (s->viewport) wp_viewport_set_destination(s->viewport, b->w, b->h);  /* logical size */
    else wl_surface_set_buffer_scale(s->wl, whole_scale(b->scale120));
    wl_surface_attach(s->wl, b->wl, 0, 0);
    wl_surface_damage_buffer(s->wl, 0, 0, INT32_MAX, INT32_MAX);
    s->mapped = true;
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                            uint32_t w, uint32_t h) {
    Surface *s = data;
    (void)w; (void)h;
    zwlr_layer_surface_v1_ack_configure(ls, serial);
    s->configured = true;
    if (s->pending) {
        s->attach(s, s->pending);
        s->pending = NULL;  /* now owned by the compositor until release */
    }
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
    Surface *s = data;
    (void)ls;
    s->destroy(s);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

/* The output a surface is on: where the HUD appears is what its width is judged by */
static void surface_enter(void *data, struct wl_surface *wl, struct wl_output *output) {
    Surface *s = data;
    (void)wl;
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (s->d->outputs[i].proxy == output) s->d->last_out = i + 1;
}
static void surface_leave(void *data, struct wl_surface *wl, struct wl_output *output) {
    (void)data; (void)wl; (void)output;
}
static const struct wl_surface_listener surface_listener = { .enter = surface_enter, .leave = surface_leave };

/* A click-through overlay surface; the caller adds what is particular to it. */
static void surf_open(Display *d, Surface *s, const char *name, uint32_t anchor,
                      void (*attach)(Surface *, Buf *), void (*destroy)(Surface *)) {
    s->d = d;
    s->attach = attach;
    s->destroy = destroy;
    s->wl = wl_compositor_create_surface(d->compositor);
    wl_surface_add_listener(s->wl, &surface_listener, s);
    s->layer = zwlr_layer_shell_v1_get_layer_surface(d->layer_shell, s->wl, pick_output(d),
                                                     ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, name);
    zwlr_layer_surface_v1_set_anchor(s->layer, anchor);
    zwlr_layer_surface_v1_set_exclusive_zone(s->layer, 0);  /* stay clear of bars and docks */
    zwlr_layer_surface_v1_set_keyboard_interactivity(s->layer,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    zwlr_layer_surface_v1_add_listener(s->layer, &layer_listener, s);

    if (d->frac_mgr && d->viewporter) {  /* fractional scales: render large, present at logical size */
        s->frac = wp_fractional_scale_manager_v1_get_fractional_scale(d->frac_mgr, s->wl);
        wp_fractional_scale_v1_add_listener(s->frac, &fractional_listener, d);
        s->viewport = wp_viewporter_get_viewport(d->viewporter, s->wl);
    }

    struct wl_region *empty = wl_compositor_create_region(d->compositor);  /* click-through */
    wl_surface_set_input_region(s->wl, empty);
    wl_region_destroy(empty);
}

static void surf_destroy(Surface *s) {
    if (s->viewport) wp_viewport_destroy(s->viewport);
    if (s->frac) wp_fractional_scale_v1_destroy(s->frac);
    if (s->layer) zwlr_layer_surface_v1_destroy(s->layer);
    if (s->wl) wl_surface_destroy(s->wl);
    buf_free(s->pending);
    s->viewport = NULL;
    s->frac = NULL;
    s->layer = NULL;
    s->wl = NULL;
    s->pending = NULL;
    s->configured = s->mapped = false;
    s->w = s->h = 0;
}

/* Shows b: at once if the surface already has this size, else once the compositor has configured
 * it for the new one (or when `resize` says to ask anyway, as after an anchor change). */
static void surf_show(Surface *s, Buf *b, bool resize) {
    if (!resize && s->configured && b->w == s->w && b->h == s->h) {
        s->attach(s, b);  /* same size: no configure round trip needed */
        return;
    }
    zwlr_layer_surface_v1_set_size(s->layer, b->w, b->h);
    s->w = b->w;
    s->h = b->h;
    buf_free(s->pending);
    s->pending = b;
    s->configured = false;
    wl_surface_commit(s->wl);
}

/* ---- the notification stack ---- */

static void stack_attach(Surface *s, Buf *b) {
    Display *d = s->d;
    surf_attach_buffer(s, b);
    if (d->fade_in_pending) {  /* first content of a new surface: fade it in */
        d->fade_in_pending = false;
        begin_fade(d, 1.0);
    }
    wl_surface_commit(s->wl);
}

static void stack_gone(Surface *s) { destroy_surface(s->d); }

static void create_surface(Display *d) {
    surf_open(d, &d->main, "pachud", d->anchor, stack_attach, stack_gone);
    zwlr_layer_surface_v1_set_margin(d->main.layer, d->margin, d->margin, d->margin, d->margin);

    if (d->alpha_mgr) {  /* start invisible; the first buffer fades in */
        d->alpha = wp_alpha_modifier_v1_get_surface(d->alpha_mgr, d->main.wl);
        set_alpha(d, 0);
        d->fade_in_pending = true;
    } else {
        d->cur_alpha = 1;
    }

    apply_click_region(&d->main);
}

void display_present(Display *d, Buf *b, cairo_t *cr) {
    buf_finish(b, cr);
    if (!d->main.wl) create_surface(d);
    if (d->hiding) {  /* something new arrived while fading out: bring it back */
        d->hiding = false;
        begin_fade(d, 1.0);
    }
    surf_show(&d->main, b, false);
}

static void destroy_surface(Display *d) {
    if (d->pointer_surface == d->main.wl) d->pointer_surface = NULL;
    if (d->frame) wl_callback_destroy(d->frame);
    if (d->alpha) wp_alpha_modifier_surface_v1_destroy(d->alpha);
    d->frame = NULL;
    d->alpha = NULL;
    surf_destroy(&d->main);
    d->animating = d->hiding = d->fade_in_pending = false;
}

void display_hide(Display *d) {
    if (!d->main.wl) return;
    if (d->alpha && d->main.mapped && !d->hiding) {  /* fade out, then frame_done() tears it down */
        d->hiding = true;
        begin_fade(d, 0.0);
        wl_surface_commit(d->main.wl);
        return;
    }
    if (!d->hiding) destroy_surface(d);
}

/* ---- the attention deck ---- */

static void deck_attach(Surface *s, Buf *b) {
    surf_attach_buffer(s, b);
    wl_surface_commit(s->wl);
}

/* Closed by the compositor or hidden by us: the pointer can no longer be over it */
static void deck_destroy(Surface *s) {
    if (!s->wl) return;   /* never opened (s->d is not set yet) */
    if (s->d->pointer_surface == s->wl) s->d->pointer_surface = NULL;
    surf_destroy(s);
}

void display_deck_present(Display *d, Buf *b, cairo_t *cr) {
    buf_finish(b, cr);
    if (!d->deck.wl) {
        surf_open(d, &d->deck, "pachud-attention", EDGE_T, deck_attach, deck_destroy);
        zwlr_layer_surface_v1_set_margin(d->deck.layer, d->margin, 0, 0, 0);
        apply_click_region(&d->deck);
    }
    surf_show(&d->deck, b, false);
}

void display_deck_hide(Display *d) { deck_destroy(&d->deck); }

int display_output_width(Display *d) {
    if (d->want_output) {
        for (int i = 0; i < MAX_OUTPUTS; i++)
            if (d->outputs[i].proxy && !strcmp(d->outputs[i].name, d->want_output)) return d->outputs[i].logical_w;
        return 0;
    }
    if (d->last_out && d->outputs[d->last_out - 1].proxy) return d->outputs[d->last_out - 1].logical_w;
    int narrowest = 0;   /* never shown yet, and no way to know where it will be: be careful */
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        int w = d->outputs[i].proxy ? d->outputs[i].logical_w : 0;
        if (w > 0 && (!narrowest || w < narrowest)) narrowest = w;
    }
    return narrowest;
}

bool display_deck_apart(Display *d, int item_width) {
    if (d->anchor == EDGE_T) return false;       /* the stack is where the deck would be */
    if (!(d->anchor & EDGE_T)) return true;      /* the stack is at the bottom: the top is free */
    int w = display_output_width(d);
    if (w <= 0) return true;                     /* not known: take it to be enough */
    /* The stack in a top corner, the deck in the middle: the deck's far edge, a gap and the stack's
     * near edge must fit, that is two margins, two gaps and three items across the screen */
    return w >= 3 * item_width + 2 * d->margin + 2 * DECK_GAP;
}

/* ---- the on-screen display ---- */

static void osd_attach(Surface *s, Buf *b) {
    surf_attach_buffer(s, b);
    wl_surface_commit(s->wl);
}

static uint32_t osd_edges(OsdAnchor a) {
    static const uint32_t edges[] = { [OSD_CENTER] = 0, [OSD_TOP] = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP,
                                      [OSD_BOTTOM] = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM };
    return edges[a];
}

void display_osd_present(Display *d, Buf *b, cairo_t *cr, OsdAnchor anchor) {
    buf_finish(b, cr);
    if (!d->osd.wl) {
        surf_open(d, &d->osd, "pachud-osd", osd_edges(anchor), osd_attach, surf_destroy);
        d->osd_anchor = anchor;
    }
    bool moved = d->osd_anchor != anchor;
    if (moved) {
        zwlr_layer_surface_v1_set_anchor(d->osd.layer, osd_edges(anchor));
        d->osd_anchor = anchor;
    }
    surf_show(&d->osd, b, moved);
}

void display_osd_hide(Display *d) {
    surf_destroy(&d->osd);
}
