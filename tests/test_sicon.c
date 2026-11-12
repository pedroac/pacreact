/* Status icons: found by name in a theme, drawn at one size (PNG or, with librsvg, SVG), remembered,
 * switched off, and safe against what a theme directory can hold. Needs no real icon theme: it
 * makes its own, in a temporary XDG_DATA_HOME. */
#include "../src/sicon.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64];

static void path_of(char *out, size_t n, const char *rel) { snprintf(out, n, "%s/%s", root, rel); }

static void make_dir_of(const char *path) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname %s)\"", path);
    assert(system(cmd) == 0);
}

static void write_file(const char *rel, const char *text) {
    char path[512];
    path_of(path, sizeof path, rel);
    make_dir_of(path);
    FILE *f = fopen(path, "w");
    assert(f && fputs(text, f) >= 0 && !fclose(f));
}

static void write_png(const char *rel, int w, int h, double r, double g, double b) {
    char path[512];
    path_of(path, sizeof path, rel);
    make_dir_of(path);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);
    cairo_destroy(cr);
    assert(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);
}

static uint32_t pixel(cairo_surface_t *s, int x, int y) {
    cairo_surface_flush(s);
    return ((uint32_t *)(cairo_image_surface_get_data(s) + y * cairo_image_surface_get_stride(s)))[x];
}

/* Draws the icon at (4, 4) of a fresh 48 x 48 surface */
static cairo_surface_t *drawn(const char *names, const double rgb[3]) {
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 48, 48);
    cairo_t *cr = cairo_create(s);
    sicon_paint(cr, names, 4, 4, rgb);
    cairo_destroy(cr);
    return s;
}

static const double GREEN[3] = { 0, 1, 0 };

int main(void) {
    alarm(30);   /* a pipe in a theme directory must fail the test, not hang it */
    snprintf(root, sizeof root, "/tmp/pachud-sicon-XXXXXX");
    assert(mkdtemp(root));
    char data[128], home[128];
    path_of(data, sizeof data, "data");
    path_of(home, sizeof home, "home");
    setenv("XDG_DATA_DIRS", data, 1);
    setenv("XDG_DATA_HOME", home, 1);
    setenv("XDG_CONFIG_HOME", root, 1);

    write_png("data/icons/hicolor/22x22/status/red-png.png", 32, 16, 1, 0, 0);   /* wider than tall */
    sicon_configure(true, 24);
    assert(sicon_enabled() && sicon_size() == 24);

    /* A PNG: scaled to fit the square and centred, so a wide one leaves bands above and below */
    assert(sicon_has("red-png"));
    cairo_surface_t *s = drawn("red-png", GREEN);
    assert(pixel(s, 16, 16) == 0xFFFF0000u);            /* the middle: red, whatever the colour asked for */
    assert(pixel(s, 16, 4 + 2) == 0 && pixel(s, 16, 4 + 21) == 0);   /* the bands */
    assert(pixel(s, 4 + 2, 16) == 0xFFFF0000u && pixel(s, 4 + 21, 16) == 0xFFFF0000u);
    cairo_surface_destroy(s);

    /* What is not there is not drawn, and nothing is said */
    assert(!sicon_has("no-such-icon") && !sicon_has("") && !sicon_has(NULL));
    s = drawn("no-such-icon", GREEN);
    assert(pixel(s, 16, 16) == 0);
    cairo_surface_destroy(s);

    /* Several names: the first that exists */
    assert(sicon_has("no-such-icon,red-png"));
    s = drawn("no-such-icon,red-png", GREEN);
    assert(pixel(s, 16, 16) == 0xFFFF0000u);
    cairo_surface_destroy(s);

    /* Misses are remembered, so a theme is not searched on every redraw; changing the size or
     * turning things off and on forgets them */
    assert(!sicon_has("installed-later"));
    write_png("data/icons/hicolor/22x22/status/installed-later.png", 16, 16, 0, 0, 1);
    assert(!sicon_has("installed-later"));
    sicon_configure(true, 25);
    assert(sicon_size() == 25 && sicon_has("installed-later"));
    sicon_configure(true, 24);

    /* Off: nothing, even for what exists, and on again brings it back */
    sicon_configure(false, 24);
    assert(!sicon_enabled() && !sicon_has("red-png"));
    s = drawn("red-png", GREEN);
    assert(pixel(s, 16, 16) == 0);
    cairo_surface_destroy(s);
    sicon_configure(true, 24);
    assert(sicon_has("red-png"));

    /* Another size: drawn again at that size */
    sicon_configure(true, 40);
    s = drawn("red-png", GREEN);
    assert(pixel(s, 4 + 36, 4 + 20) == 0xFFFF0000u && pixel(s, 4 + 20, 4 + 4) == 0);   /* 40 wide, 20 tall, centred */
    cairo_surface_destroy(s);
    sicon_configure(true, 24);

    /* Files a theme directory should not hold: a pipe must not block, nor a device be read */
    char fifo[512];
    path_of(fifo, sizeof fifo, "data/icons/hicolor/22x22/status/pipe.png");
    assert(mkfifo(fifo, 0600) == 0);
    assert(!sicon_has("pipe"));
    write_file("data/icons/hicolor/22x22/status/garbage.png", "this is not a PNG");
    assert(!sicon_has("garbage"));

    /* The cache holds a few dozen: more names than that just push the oldest out */
    for (int i = 0; i < 100; i++) {
        char name[32];
        snprintf(name, sizeof name, "missing-%d", i);
        assert(!sicon_has(name));
    }
    assert(sicon_has("red-png"));                   /* looked up and drawn again, after being pushed out */

    if (sicon_svg()) {
        /* A symbolic SVG has no colour of its own: it is the HUD's, through the icon's shape */
        write_file("data/icons/hicolor/symbolic/status/dot-symbolic.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 16 16'>"
                   "<rect x='4' y='4' width='8' height='8' fill='#2e3436'/></svg>");
        write_file("data/icons/hicolor/scalable/status/blue-square.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16' viewBox='0 0 16 16'>"
                   "<rect width='16' height='16' fill='#0000ff'/></svg>");
        assert(sicon_has("dot"));
        s = drawn("dot", GREEN);
        assert(pixel(s, 4 + 12, 4 + 12) == 0xFF00FF00u);     /* the middle, in the colour given */
        assert(pixel(s, 4 + 1, 4 + 1) == 0);                 /* outside the square: nothing */
        cairo_surface_destroy(s);
        static const double RED[3] = { 1, 0, 0 };
        s = drawn("dot", RED);
        assert(pixel(s, 4 + 12, 4 + 12) == 0xFFFF0000u);
        cairo_surface_destroy(s);

        /* A coloured one keeps its own, whatever the colour asked for */
        assert(sicon_has("blue-square"));
        s = drawn("blue-square", GREEN);
        assert(pixel(s, 4 + 12, 4 + 12) == 0xFF0000FFu && pixel(s, 4 + 1, 4 + 1) == 0xFF0000FFu);
        cairo_surface_destroy(s);

        /* A coloured one is a PNG's equal; a symbolic one beats a coloured one of the same name */
        write_file("data/icons/hicolor/scalable/status/both.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='#0000ff'/></svg>");
        write_file("data/icons/hicolor/symbolic/status/both-symbolic.svg",
                   "<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'><rect width='16' height='16' fill='#000000'/></svg>");
        s = drawn("both", GREEN);
        assert(pixel(s, 4 + 12, 4 + 12) == 0xFF00FF00u);     /* the symbolic one, so the HUD's colour */
        cairo_surface_destroy(s);

        /* Not drawable: not XML, a pipe, a file too large to be an icon */
        write_file("data/icons/hicolor/scalable/status/bad-xml.svg", "this is not an SVG");
        assert(!sicon_has("bad-xml"));
        path_of(fifo, sizeof fifo, "data/icons/hicolor/scalable/status/pipe-svg.svg");
        assert(mkfifo(fifo, 0600) == 0);
        assert(!sicon_has("pipe-svg"));
        char big[512];
        path_of(big, sizeof big, "data/icons/hicolor/scalable/status/huge.svg");
        FILE *f = fopen(big, "w");
        assert(f && fputs("<svg xmlns='http://www.w3.org/2000/svg' width='16' height='16'>", f) >= 0);
        assert(truncate(big, 2 << 20) == 0 && !fclose(f));
        assert(!sicon_has("huge"));
    } else {
        puts("sicon: SVG not tested (built without librsvg)");
        /* Without a renderer an SVG-only icon is simply not there */
        write_file("data/icons/hicolor/scalable/status/svg-only.svg", "<svg/>");
        assert(!sicon_has("svg-only"));
    }

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    puts("sicon: ok");
    return 0;
}
