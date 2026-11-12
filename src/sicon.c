#include "sicon.h"
#include "icon.h"
#include "pngload.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef HAVE_RSVG
#include <librsvg/rsvg.h>
#endif

#define CACHE 32
#define SVG_MAX (1 << 20)   /* the largest SVG file taken */

typedef struct {
    char names[96];
    cairo_surface_t *surf;   /* NULL: looked for and not found, remembered so as not to look again */
    bool symbolic;
} Entry;

static Entry cache[CACHE];
static int ncache, victim;
static bool enabled = true;
static int size = 24;

bool sicon_enabled(void) { return enabled; }
int sicon_size(void) { return size; }

bool sicon_svg(void) {
#ifdef HAVE_RSVG
    return true;
#else
    return false;
#endif
}

static void forget(void) {
    for (int i = 0; i < ncache; i++)
        if (cache[i].surf) cairo_surface_destroy(cache[i].surf);
    memset(cache, 0, sizeof cache);
    ncache = victim = 0;
}

void sicon_configure(bool on, int size_px) {
    if (size_px != size) forget();
    enabled = on;
    size = size_px > 0 ? size_px : 24;
}

#ifdef HAVE_RSVG
/* The file, if it is a regular file of a sane size: the path comes from a theme directory, but a pipe
 * there would still stop the daemon, so the same care as for any other file. */
static char *read_regular(const char *path, size_t max, size_t *len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return NULL;
    struct stat st;
    char *data = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && (size_t)st.st_size <= max) {
        data = malloc((size_t)st.st_size);
        size_t got = 0;
        while (data && got < (size_t)st.st_size) {
            ssize_t n = read(fd, data + got, (size_t)st.st_size - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
        if (data && got != (size_t)st.st_size) { free(data); data = NULL; }
        if (data) *len = got;
    }
    close(fd);
    return data;
}

static cairo_surface_t *render_svg(const char *path, int px) {
    size_t len;
    char *data = read_regular(path, SVG_MAX, &len);
    if (!data) return NULL;
    GError *err = NULL;
    RsvgHandle *h = rsvg_handle_new_from_data((const guint8 *)data, len, &err);
    free(data);
    if (!h) {
        if (err) g_error_free(err);
        return NULL;
    }
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    RsvgRectangle viewport = { 0, 0, px, px };   /* the whole picture, fitted, keeping its proportions */
    gboolean ok = rsvg_handle_render_document(h, cr, &viewport, &err);
    if (err) g_error_free(err);
    cairo_destroy(cr);
    g_object_unref(h);
    if (!ok) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    return surf;
}
#endif

/* A PNG scaled to fit px x px, centred */
static cairo_surface_t *render_png(const char *path, int px) {
    cairo_surface_t *img = png_load(path);
    if (!img) return NULL;
    int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    double k = (double)px / (w > h ? w : h);
    cairo_translate(cr, (px - w * k) / 2, (px - h * k) / 2);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(img);
    return surf;
}

static cairo_surface_t *render(const char *path, int px) {
    size_t len = strlen(path);
    if (len > 4 && !strcmp(path + len - 4, ".svg")) {
#ifdef HAVE_RSVG
        return render_svg(path, px);
#else
        return NULL;
#endif
    }
    return render_png(path, px);
}

/* The entry for these names, found and drawn the first time they are asked for */
static Entry *entry_for(const char *names) {
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].names, names)) return &cache[i];

    Entry *e;
    if (ncache < CACHE) {
        e = &cache[ncache++];
    } else {
        e = &cache[victim++ % CACHE];
        if (e->surf) cairo_surface_destroy(e->surf);
    }
    memset(e, 0, sizeof *e);
    snprintf(e->names, sizeof e->names, "%s", names);

    char path[1024];
    bool symbolic = false;
    if (strlen(names) < sizeof e->names && icon_find_status(names, sicon_svg(), path, sizeof path, &symbolic)) {
        e->surf = render(path, size);
        e->symbolic = symbolic;
    }
    return e;
}

bool sicon_has(const char *names) {
    return enabled && names && *names && entry_for(names)->surf != NULL;
}

void sicon_paint(cairo_t *cr, const char *names, double x, double y, const double rgb[3]) {
    if (!sicon_has(names)) return;
    Entry *e = entry_for(names);
    if (e->symbolic) {
        cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
        cairo_mask_surface(cr, e->surf, x, y);
    } else {
        cairo_set_source_surface(cr, e->surf, x, y);
        cairo_paint(cr);
    }
}
