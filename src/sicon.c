#include "sicon.h"
#include "icon.h"
#include "pngload.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>


#define CACHE 32
#define SVG_WAIT_MS 1500          /* the longest a conversion may take */
#define SVG_OUT_MAX (256 * 1024)  /* the most of PNG taken from it: an icon is a few hundred bytes */

typedef struct {
    char names[96];
    cairo_surface_t *surf;   /* NULL: looked for and not found, remembered so as not to look again */
    bool symbolic;
} Entry;

static Entry cache[CACHE];
static int ncache, victim;
static bool enabled = true;
static int size = 24;
static char tool[1024];
static int tool_known;       /* 0 not looked for yet, 1 found, -1 not there */

bool sicon_enabled(void) { return enabled; }
int sicon_size(void) { return size; }

/* SVG is drawn by rsvg-convert, run for the purpose and gone again: librsvg and what it pulls in
 * (GLib, libxml2, ICU...) would otherwise sit in the daemon for good, several times its own size,
 * to draw a few icons once each. */
bool sicon_svg(void) {
    if (!tool_known) tool_known = find_in_path("rsvg-convert", tool, sizeof tool) ? 1 : -1;
    return tool_known > 0;
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
    tool_known = 0;   /* look for the tool again: it may have been installed since */
}

/* A picture fitted into px x px, centred, keeping its proportions. Takes img. */
static cairo_surface_t *fit_square(cairo_surface_t *img, int px) {
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

/* The PNG the tool writes on its standard output, read within a time limit and a size limit: a
 * tool that hangs or never stops writing is killed and the icon is not there. Without a shell, and
 * with an absolute path, so what is in the file name cannot be taken for an option. */
static cairo_surface_t *render_svg(const char *path, int px) {
    if (!sicon_svg() || path[0] != '/') return NULL;
    char dim[16];
    snprintf(dim, sizeof dim, "%d", px);
    char *argv[] = { "rsvg-convert", "-a", "-w", dim, "-h", dim, "-f", "png", (char *)path, NULL };

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) < 0) return NULL;
    /* Nothing of ours goes along. Without a session bus address the tool's GIO cannot ask D-Bus to
     * start a virtual file system daemon for a file it reads straight from disk. */
    static char *const clean_env[] = { (char *)"GIO_USE_VFS=local", NULL };
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    int err = posix_spawn(&pid, tool, &fa, NULL, argv, clean_env);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err) {
        close(fds[0]);
        return NULL;
    }

    unsigned char *buf = malloc(SVG_OUT_MAX);
    size_t len = 0;
    bool ok = buf != NULL;
    long long deadline = now_ms() + SVG_WAIT_MS;
    while (ok) {
        long long left = deadline - now_ms();
        struct pollfd p = { .fd = fds[0], .events = POLLIN };
        int r = left > 0 ? poll(&p, 1, (int)left) : 0;
        if (r < 0 && errno == EINTR) continue;
        ssize_t n = r > 0 ? read(fds[0], buf + len, SVG_OUT_MAX - len) : -1;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 || (n > 0 && (len += (size_t)n) == SVG_OUT_MAX)) ok = false;   /* too late, or too much */
        if (n <= 0) break;                                                       /* the end, or a failure */
    }
    close(fds[0]);
    /* Do not wait on what is not done: it has had its say. (With SIGCHLD ignored, as the sound player
     * code sets it, the kernel reaps it and waitpid just says so.) */
    if (!ok || waitpid(pid, NULL, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
    cairo_surface_t *img = ok ? png_decode(buf, len) : NULL;
    free(buf);
    return img ? fit_square(img, px) : NULL;
}

static cairo_surface_t *render(const char *path, int px) {
    size_t len = strlen(path);
    if (len > 4 && !strcmp(path + len - 4, ".svg")) return render_svg(path, px);
    cairo_surface_t *img = png_load(path);
    return img ? fit_square(img, px) : NULL;
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
