#include "sicon.h"
#include "i18n.h"
#include "config.h"
#include "fontfind.h"
#include "icon.h"
#include "pngload.h"
#include "util.h"
#ifndef NO_I18N
#include <cairo/cairo-ft.h>
#include <dirent.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include <sys/stat.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "tunables.h"



typedef struct {
    char names[96];
    cairo_surface_t *surf;   /* NULL: looked for and not found, remembered so as not to look again */
    bool symbolic;
} Entry;

static Entry cache[SICON_CACHE];
static int ncache, victim;
static bool enabled = true;
static int size = 24;
static char tool[1024];
static int tool_known;       /* 0 not looked for yet, 1 found, -1 not there */

static bool glyphs_on;
static char glyph_font[512];   /* icon_font: a font file; empty looks for the Symbols Nerd Font */

/* Nerd Fonts (version 3) Material Design glyphs, by the freedesktop name a message asks for. The "-off"
 * names are not theme names: they say what the glyph for a thing that went away is, and a theme
 * lookup simply passes over them to the next name in the list. */
#ifndef NO_I18N
static const struct { const char *name; uint32_t cp; } glyph_table[] = {
    { "camera-web", 0xF05A0 },                   /* nf-md-webcam */
    { "camera-web-off", 0xF1737 },               /* nf-md-webcam_off */
    { "printer", 0xF042A },                      /* nf-md-printer */
    { "printer-warning", 0xF042C },              /* nf-md-printer_alert */
    { "printer-error", 0xF042C },
    { "printer-off", 0xF0E5D },                  /* nf-md-printer_off */
    { "audio-headphones", 0xF02CB },             /* nf-md-headphones */
    { "audio-headphones-off", 0xF07CE },         /* nf-md-headphones_off */
    { "audio-headset", 0xF02CE },                /* nf-md-headset */
    { "audio-headset-off", 0xF02D0 },            /* nf-md-headset_off */
    { "audio-input-microphone", 0xF036C },       /* nf-md-microphone */
    { "audio-input-microphone-off", 0xF036D },   /* nf-md-microphone_off */
    { "microphone-sensitivity-high", 0xF036C },
    { "microphone-sensitivity-medium", 0xF036C },
    { "microphone-sensitivity-low", 0xF036C },
    { "microphone-sensitivity-muted", 0xF036D }, /* nf-md-microphone_off */
    { "audio-volume-muted", 0xF075F },           /* nf-md-volume_mute */
    { "audio-volume-low", 0xF057F },             /* nf-md-volume_low */
    { "audio-volume-medium", 0xF0580 },          /* nf-md-volume_medium */
    { "audio-volume-high", 0xF057E },            /* nf-md-volume_high */
    { "audio-volume-overamplified", 0xF057E },
};
#endif

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

#ifndef NO_I18N
static FT_Face gft;                /* the font, opened once: a face opened per icon stays alive in cairo's cache, each with its own pages of the file */
static cairo_font_face_t *gface;
#endif
#ifndef NO_I18N
static char found_font[512];
#endif
static int font_known;   /* 0 not looked for yet, 1 found, -1 not there */

void sicon_set_glyphs(bool on, const char *font) {
    forget();
    glyphs_on = on;
    snprintf(glyph_font, sizeof glyph_font, "%s", font ? font : "");
    font_known = 0;
#ifndef NO_I18N
    if (gface) cairo_font_face_destroy(gface);   /* the font may have changed */
    gface = NULL;
    gft = NULL;
#endif
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

#ifndef NO_I18N
/* Where the Symbols Nerd Font is, found by walking the font directories for its file name rather than
 * asking fontconfig: fontconfig maps its cache of every installed font (a few MB of shared pages and
 * some private ones, on a machine with thousands) for the sake of a handful of icons. */
static bool font_file(char *out, size_t n) {
    if (font_known == 0) {
        font_known = -1;
        struct stat st;
        if (glyph_font[0]) {
            if (stat(glyph_font, &st) == 0 && S_ISREG(st.st_mode)) {
                snprintf(found_font, sizeof found_font, "%s", glyph_font);
                font_known = 1;
            } else {
                fprintf(stderr, _("pacreact: icon_font \"%s\" is not a font file that can be read; using the theme's icons\n"), glyph_font);
            }
        } else {
            FontDirs dirs;
            font_dirs_init(&dirs);
            static const char *const names[] = { "SymbolsNerdFont-Regular.ttf", "SymbolsNerdFontMono-Regular.ttf", NULL };
            if (font_find(dirs.list, names, found_font, sizeof found_font)) font_known = 1;
        }
    }
    if (font_known < 0) return false;
    snprintf(out, n, "%s", found_font);
    return true;
}

static FT_Library ft;
static bool ft_ready;
static const cairo_user_data_key_t ft_face_key;
static void done_face(void *face) { FT_Done_Face(face); }

/* The font, opened the first time and kept (one mapping of the file, not one per icon). */
static bool open_font(void) {
    if (gface) return true;
    char path[512];
    if (!font_file(path, sizeof path)) return false;
    if (!ft_ready) {
        if (FT_Init_FreeType(&ft) != 0) return false;
        ft_ready = true;
    }
    FT_Face face;
    if (FT_New_Face(ft, path, 0, &face) != 0) return false;
    gft = face;
    gface = cairo_ft_font_face_create_for_ft_face(face, 0);
    cairo_font_face_set_user_data(gface, &ft_face_key, face, done_face);   /* the face goes with the last use of the font face */
    return true;
}

#define VOLUME_BOX_CP 0xF057E   /* nf-md-volume_high: the widest of the volume glyphs */

/* The glyph for the first of these names, when it has one and the font has it: drawn as a mask, like a
 * symbolic icon, centred and fitted in a square. */
static cairo_surface_t *render_glyph(const char *names, int px) {
    size_t len = strcspn(names, ",");
    uint32_t cp = 0;
    for (size_t i = 0; i < sizeof glyph_table / sizeof *glyph_table; i++)
        if (strlen(glyph_table[i].name) == len && !strncmp(glyph_table[i].name, names, len)) cp = glyph_table[i].cp;
    if (!cp || !open_font()) return NULL;
    cairo_glyph_t g = { .index = FT_Get_Char_Index(gft, cp) };
    if (!g.index) return NULL;

    cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, px, px);
    cairo_t *cr = cairo_create(surf);
    cairo_set_font_face(cr, gface);
    cairo_set_font_size(cr, px);
    cairo_text_extents_t ext;
    cairo_glyph_extents(cr, &g, 1, &ext);
    /* The volume glyphs share one imaginary rectangle, the size of the widest, and are aligned to its
     * left: the speaker stays where it is while the waves come and go. */
    cairo_text_extents_t box = ext;
    if (!strncmp(names, "audio-volume-", 13)) {
        cairo_glyph_t r = { .index = FT_Get_Char_Index(gft, VOLUME_BOX_CP) };
        if (r.index) cairo_glyph_extents(cr, &r, 1, &box);
    }
    double big = box.width > box.height ? box.width : box.height;
    bool have = big > 0 && ext.width > 0;
    if (have) {
        if (big > px) {   /* never larger than its square */
            cairo_set_font_size(cr, px * px / big);
            cairo_glyph_extents(cr, &g, 1, &ext);
            if (box.width != ext.width || box.height != ext.height) {
                cairo_glyph_t r = { .index = FT_Get_Char_Index(gft, VOLUME_BOX_CP) };
                if (r.index && !strncmp(names, "audio-volume-", 13)) cairo_glyph_extents(cr, &r, 1, &box);
                else box = ext;
            }
        }
        g.x = (px - box.width) / 2 - ext.x_bearing;
        g.y = (px - box.height) / 2 - box.y_bearing;
        cairo_set_source_rgb(cr, 1, 1, 1);
        cairo_show_glyphs(cr, &g, 1);
    }
    cairo_destroy(cr);
    if (!have) {
        cairo_surface_destroy(surf);
        return NULL;
    }
    return surf;
}
#else
static cairo_surface_t *render_glyph(const char *names, int px) { (void)names; (void)px; return NULL; }
#endif

/* The entry for these names, found and drawn the first time they are asked for */
static Entry *entry_for(const char *names) {
    for (int i = 0; i < ncache; i++)
        if (!strcmp(cache[i].names, names)) return &cache[i];

    Entry *e;
    if (ncache < SICON_CACHE) {
        e = &cache[ncache++];
    } else {
        e = &cache[victim++ % SICON_CACHE];
        if (e->surf) cairo_surface_destroy(e->surf);
    }
    memset(e, 0, sizeof *e);
    snprintf(e->names, sizeof e->names, "%s", names);

    char path[1024];
    bool symbolic = false;
    if (glyphs_on && strlen(names) < sizeof e->names && (e->surf = render_glyph(names, size))) {
        e->symbolic = true;
    } else if (strlen(names) < sizeof e->names && icon_find_status(names, sicon_svg(), path, sizeof path, &symbolic)) {
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
