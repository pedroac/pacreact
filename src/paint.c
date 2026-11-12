#include "paint.h"
#include "text.h"
#include <fcntl.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* Drawing of the HUD's items: toasts, sliders and notification cards. Each function draws one item
 * with its top-left at the origin of cr, or only measures it when cr is NULL; either way it
 * reports the item's size. What is on screen, and when, is hud.c's business. */

#define FONT_SIZE 16
#define PAD 16
#define BAR 5
#define MAX_TEXT_W 720
#define CARD_W 380
#define CARD_ICON 40
#define CARD_BODY_LINES 4
#define CARD_BODY_SIZE 13
#define CARD_SUMMARY_SIZE 15
#define ACTION_SIZE 12
#define ACTION_GAP 6

/* Layouts are written left to right; in a right-to-left UI language every element is
 * mirrored inside the window: this maps the left edge of an element x wide w. */
static double mirror(double x, double w, int total) {
    return text_rtl() ? total - x - w : x;
}

static void round_rect(cairo_t *cr, double x, double y, double w, double h, double r) {
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void accent(Level level, double rgb[3]) {
    static const double c[][3] = {
        [LEVEL_INFO] = { 0.40, 0.65, 1.00 },
        [LEVEL_WARN] = { 1.00, 0.75, 0.25 },
        [LEVEL_CRIT] = { 1.00, 0.35, 0.35 },
    };
    memcpy(rgb, c[level], sizeof c[level]);
}

static void paint_bg(cairo_t *cr, int w, int h) {
    round_rect(cr, 0, 0, w, h, 12);
    cairo_set_source_rgba(cr, 0.09, 0.09, 0.11, 0.94);
    cairo_fill_preserve(cr);
    cairo_clip(cr);
}

static void ellipsize(const char *in, double w, double size, bool bold, char *out, size_t n);

/* One line of at most MAX_TEXT_W: newlines become spaces and what does not fit ends in "…". */
static void toast_line(const char *text, char *out, size_t n) {
    char line[512];
    snprintf(line, sizeof line, "%s", text);
    for (char *p = line; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    ellipsize(line, MAX_TEXT_W, FONT_SIZE, false, out, n);
}

/* The paint_* functions draw one item with its top-left at the origin of cr, or only measure it
 * when cr is NULL; either way they report the item's size. */
void paint_toast(cairo_t *cr, const char *text, Level level, int *ow, int *oh) {
    char line[512];
    toast_line(text, line, sizeof line);

    double ascent, height;
    text_extents(line, FONT_SIZE, false, &ascent, &height);
    double tw = text_width(line, (int)strlen(line), FONT_SIZE, false);
    int w = (int)ceil(tw) + 2 * PAD + BAR, h = (int)ceil(height) + 2 * PAD;
    *ow = w;
    *oh = h;
    if (!cr) return;

    double rgb[3];
    accent(level, rgb);

    paint_bg(cr, w, h);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_rectangle(cr, mirror(0, BAR, w), 0, BAR, h);
    cairo_fill(cr);

    double x = mirror(BAR + PAD, tw, w);
    cairo_reset_clip(cr);
    cairo_rectangle(cr, x, 0, tw, h);
    cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.95);
    text_draw(cr, line, x, PAD + ascent, FONT_SIZE, false);
}

#define SLIDER_W 300
#define SLIDER_H 64
#define TRACK_H 6

void paint_slider(cairo_t *cr, const char *label, int percent, bool dim, int *ow, int *oh) {
    *ow = SLIDER_W;
    *oh = SLIDER_H;
    if (!cr) return;
    if (percent < 0) percent = 0;
    int fill = percent > 100 ? 100 : percent;

    paint_bg(cr, SLIDER_W, SLIDER_H);
    double ascent, height;
    text_extents(label, FONT_SIZE, false, &ascent, &height);
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.95);
    double lw = text_width(label, (int)strlen(label), FONT_SIZE, false);
    text_draw(cr, label, mirror(PAD, lw, SLIDER_W), 12 + ascent, FONT_SIZE, false);

    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", percent);
    double pw = text_width(pct, (int)strlen(pct), FONT_SIZE, false);
    text_draw(cr, pct, mirror(SLIDER_W - PAD - pw, pw, SLIDER_W), 12 + ascent, FONT_SIZE, false);

    double tw = SLIDER_W - 2 * PAD, ty = SLIDER_H - PAD - TRACK_H + 2;
    cairo_reset_clip(cr);
    round_rect(cr, PAD, ty, tw, TRACK_H, TRACK_H / 2.0);
    cairo_set_source_rgba(cr, 1, 1, 1, 0.15);
    cairo_fill(cr);
    if (fill > 0) {
        double fw = tw * fill / 100;
        if (fw < TRACK_H) fw = TRACK_H;
        round_rect(cr, text_rtl() ? PAD + tw - fw : PAD, ty, fw, TRACK_H, TRACK_H / 2.0);
        if (dim) cairo_set_source_rgb(cr, 0.55, 0.55, 0.60);
        else if (percent > 100) cairo_set_source_rgb(cr, 1.00, 0.75, 0.25);
        else cairo_set_source_rgb(cr, 0.40, 0.65, 1.00);
        cairo_fill(cr);
    }
}

/* ---- notification cards ---- */

typedef struct { const char *p; int len; } Line;

static int utf8_step(unsigned char c) {
    return c < 0xC0 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
}

/* Notification bodies may carry a small HTML subset. Drop tags (<br> breaks the line) and
 * decode the basic entities; nothing is rendered as markup. */
static void strip_markup(const char *in, char *out, size_t n) {
    static const struct { const char *ent; char ch; } ents[] = {
        { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
    };
    size_t o = 0;
    while (*in && o < n - 1) {
        if (*in == '<' && (in[1] == '/' || in[1] == '!' || (in[1] >= 'a' && in[1] <= 'z') ||
                           (in[1] >= 'A' && in[1] <= 'Z'))) {
            const char *end = strchr(in, '>');
            if (end) {
                if (!strncasecmp(in + 1, "br", 2)) out[o++] = '\n';
                in = end + 1;
                continue;
            }
        }
        bool done = false;
        for (size_t e = 0; e < sizeof ents / sizeof *ents && !done; e++) {
            size_t l = strlen(ents[e].ent);
            if (!strncmp(in, ents[e].ent, l)) { out[o++] = ents[e].ch; in += l; done = true; }
        }
        if (!done) out[o++] = *in++;
    }
    out[o] = '\0';
}

/* Greedy word wrap into at most maxlines lines, breaking inside a word only when it alone is
 * too wide. The last line leaves room for an ellipsis, drawn if text remains. */
static int wrap(const char *text, double maxw, double size, Line *out, int maxlines,
                bool *truncated) {
    double ell = text_width("…", 3, size, false);
    int n = 0;
    const char *start = text;

    while (*start && n < maxlines) {
        double w = n == maxlines - 1 ? maxw - ell : maxw;
        const char *i = start, *last_space = NULL, *next;
        while (*i && *i != '\n') {
            int step = utf8_step((unsigned char)*i);
            if (text_width(start, (int)(i + step - start), size, false) > w) break;
            if (*i == ' ') last_space = i;
            i += step;
        }
        if (!*i || *i == '\n') {
            out[n++] = (Line){ start, (int)(i - start) };
            next = *i ? i + 1 : i;
        } else if (last_space && last_space > start) {
            out[n++] = (Line){ start, (int)(last_space - start) };
            next = last_space + 1;
        } else {
            if (i == start) i += utf8_step((unsigned char)*i);  /* always make progress */
            out[n++] = (Line){ start, (int)(i - start) };
            next = i;
        }
        while (*next == ' ') next++;
        start = next;
    }
    *truncated = *start != '\0';
    return n;
}

static void ellipsize(const char *in, double w, double size, bool bold, char *out, size_t n) {
    snprintf(out, n, "%s", in);
    if (text_width(out, (int)strlen(out), size, bold) <= w) return;
    size_t len = strlen(out);
    while (len > 0) {
        len--;
        while (len > 0 && ((unsigned char)out[len] & 0xC0) == 0x80) len--;
        char t[560];
        snprintf(t, sizeof t, "%.*s…", (int)len, out);
        if (text_width(t, (int)strlen(t), size, bold) <= w) { snprintf(out, n, "%s", t); return; }
    }
    out[0] = '\0';
}

/* An icon path comes from the app that sent the notification, and a file can be anything: a pipe
 * that never delivers (which would stop the whole daemon), a device, or a few kilobytes of PNG that
 * claim to be gigapixels (which would decode into gigabytes before anything could object). So the
 * file is opened without blocking, must be a regular file of a sane size, and its header must
 * declare a sane picture before any decoding starts. */
#define ICON_FILE_MAX (4 << 20)
#define ICON_DIM_MAX 2048

typedef struct { int fd; size_t left; } PngSource;

static cairo_status_t png_read(void *closure, unsigned char *data, unsigned int length) {
    PngSource *src = closure;
    while (length) {
        if (!src->left) return CAIRO_STATUS_READ_ERROR;
        ssize_t n = read(src->fd, data, length < src->left ? length : src->left);
        if (n <= 0) return CAIRO_STATUS_READ_ERROR;
        data += n;
        length -= (unsigned)n;
        src->left -= (size_t)n;
    }
    return CAIRO_STATUS_SUCCESS;
}

/* The signature and the IHDR chunk, which comes first and holds the size. */
static bool png_header_ok(int fd) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    unsigned char h[24];
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || memcmp(h, sig, sizeof sig) || memcmp(h + 12, "IHDR", 4))
        return false;
    uint32_t w = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 | (uint32_t)h[18] << 8 | h[19];
    uint32_t ht = (uint32_t)h[20] << 24 | (uint32_t)h[21] << 16 | (uint32_t)h[22] << 8 | h[23];
    return w >= 1 && ht >= 1 && w <= ICON_DIM_MAX && ht <= ICON_DIM_MAX;
}

/* A PNG file as the small raw icon a card draws (IMAGE_BYTES of premultiplied ARGB32, the same as
 * an image-data hint), or false. Done once when the card arrives, not on every redraw. */
bool load_icon_pixels(const char *path, uint8_t *out) {
    if (!strncmp(path, "file://", 7)) path += 7;
    size_t len = strlen(path);
    if (path[0] != '/' || len < 5 || strcmp(path + len - 4, ".png")) return false;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return false;
    struct stat st;
    cairo_surface_t *img = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= ICON_FILE_MAX &&
        png_header_ok(fd)) {
        PngSource src = { fd, (size_t)st.st_size };
        img = cairo_image_surface_create_from_png_stream(png_read, &src);
    }
    close(fd);
    if (!img) return false;
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        return false;
    }

    int w = cairo_image_surface_get_width(img), h = cairo_image_surface_get_height(img);
    memset(out, 0, IMAGE_BYTES);
    cairo_surface_t *dst = cairo_image_surface_create_for_data(out, CAIRO_FORMAT_ARGB32, IMAGE_PX,
                                                               IMAGE_PX, IMAGE_PX * 4);
    cairo_t *cr = cairo_create(dst);
    double k = fmin((double)IMAGE_PX / w, (double)IMAGE_PX / h);   /* fit, keeping the proportions */
    cairo_translate(cr, (IMAGE_PX - w * k) / 2, (IMAGE_PX - h * k) / 2);
    cairo_scale(cr, k, k);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BEST);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(dst);
    cairo_surface_destroy(img);
    return true;
}

void paint_card(cairo_t *cr, const CardPaint *card, int *ow, int *oh) {
    char body[512], summary[256];
    strip_markup(card->body, body, sizeof body);
    snprintf(summary, sizeof summary, "%s", card->summary);
    for (char *p = summary; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    cairo_surface_t *icon = card->image
        ? cairo_image_surface_create_for_data((unsigned char *)card->image, CAIRO_FORMAT_ARGB32,
                                              IMAGE_PX, IMAGE_PX, IMAGE_PX * 4)
        : NULL;

    double x0 = BAR + PAD + (icon ? CARD_ICON + 12 : 0), tw = CARD_W - x0 - PAD;

    char title[300];
    ellipsize(summary, tw, CARD_SUMMARY_SIZE, true, title, sizeof title);
    double sasc, sheight, basc, bheight;
    text_extents(title, CARD_SUMMARY_SIZE, true, &sasc, &sheight);
    text_extents(*body ? body : "x", CARD_BODY_SIZE, false, &basc, &bheight);

    Line lines[CARD_BODY_LINES];
    bool truncated;
    int nl = wrap(body, tw, CARD_BODY_SIZE, lines, CARD_BODY_LINES, &truncated);

    /* A hint line for the actions: pachud cannot be clicked, so each is invoked by number */
    char actions[256] = "";
    double aasc = 0, aheight = 0, actions_w = 0;
    if (card->nact) {
        int len = 0;
        for (int i = 0; i < card->nact && len < (int)sizeof actions; i++)
            len += snprintf(actions + len, sizeof actions - len, "%s[%d] %s", i ? "   " : "", i + 1,
                            card->act_label[i]);
        char fit[256];
        ellipsize(actions, tw, ACTION_SIZE, false, fit, sizeof fit);
        snprintf(actions, sizeof actions, "%s", fit);
        text_extents(actions, ACTION_SIZE, false, &aasc, &aheight);
        actions_w = text_width(actions, (int)strlen(actions), ACTION_SIZE, false);
    }

    double content = sheight + (nl ? 4 + nl * bheight : 0) + (card->nact ? ACTION_GAP + aheight : 0);
    if (icon && content < CARD_ICON) content = CARD_ICON;
    int h = (int)ceil(content) + 2 * PAD;

    *ow = CARD_W;
    *oh = h;
    if (!cr) { if (icon) cairo_surface_destroy(icon); return; }

    double rgb[3];
    accent(card->level, rgb);
    paint_bg(cr, CARD_W, h);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_rectangle(cr, mirror(0, BAR, CARD_W), 0, BAR, h);
    cairo_fill(cr);

    if (icon) {
        cairo_save(cr);
        cairo_translate(cr, mirror(BAR + PAD, CARD_ICON, CARD_W), PAD);
        cairo_scale(cr, (double)CARD_ICON / cairo_image_surface_get_width(icon),
                    (double)CARD_ICON / cairo_image_surface_get_height(icon));
        cairo_set_source_surface(cr, icon, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(icon);
    }

    cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
    double title_w = text_width(title, (int)strlen(title), CARD_SUMMARY_SIZE, true);
    text_draw(cr, title, mirror(x0, title_w, CARD_W), PAD + sasc, CARD_SUMMARY_SIZE, true);

    cairo_set_source_rgb(cr, 0.75, 0.75, 0.80);
    for (int i = 0; i < nl; i++) {
        char tmp[520];
        snprintf(tmp, sizeof tmp, "%.*s%s", lines[i].len, lines[i].p,
                 truncated && i == nl - 1 ? "…" : "");
        double lw = text_width(tmp, (int)strlen(tmp), CARD_BODY_SIZE, false);
        text_draw(cr, tmp, mirror(x0, lw, CARD_W), PAD + sheight + 4 + i * bheight + basc,
                  CARD_BODY_SIZE, false);
    }
    if (card->nact) {
        cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
        double y = PAD + sheight + (nl ? 4 + nl * bheight : 0) + ACTION_GAP + aasc;
        text_draw(cr, actions, mirror(x0, actions_w, CARD_W), y, ACTION_SIZE, false);
    }
}

