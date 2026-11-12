#include "paint.h"
#include "sicon.h"
#include "pngload.h"
#include "text.h"
#include "util.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Drawing of the HUD's items: toasts, sliders and notification cards. Each function draws one item
 * with its top-left at the origin of cr, or only measures it when cr is NULL; either way it
 * reports the item's size. What is on screen, and when, is hud.c's business. */

/* The lengths and font sizes are those of a 16 px font, passed through paint_px() and paint_font()
 * so they follow PACHUD_FONT_SIZE; the numbers of lines are not lengths and stay as they are. */
#define FONT_SIZE paint_font(16)
#define PAD paint_px(16)
#define BAR paint_px(5)
#define RADIUS paint_px(12)
#define LINE_GAP paint_px(4)
#define TOAST_LINES 3
#define STRIP_SIZE paint_font(14)
#define CARD_ICON paint_px(40)
#define ICON_GAP paint_px(12)
#define CARD_BODY_LINES 4
#define CARD_BODY_LINES_EXPANDED 20
#define CARD_BODY_SIZE paint_font(13)
#define CARD_SUMMARY_SIZE paint_font(15)
#define ACTION_SIZE paint_font(12)
#define ACTION_GAP paint_px(6)
#define SLIDER_H paint_px(64)
#define SLIDER_TOP paint_px(12)
#define TRACK_H paint_px(6)

static double zoom = 1.0;
static int width_set;   /* PACHUD_WIDTH, or 0 to follow the zoom */

static int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static bool icons_on = true;
static int icon_set;    /* PACHUD_ICON_SIZE, or 0 to follow the zoom */

static void apply_icons(void) {
    sicon_configure(icons_on, icon_set ? clamp(icon_set, PAINT_ICON_MIN, PAINT_ICON_MAX) : paint_px(24));
}

void paint_configure(int font_px, int width_px) {
    zoom = font_px > 0 ? clamp(font_px, PAINT_FONT_MIN, PAINT_FONT_MAX) / 16.0 : 1.0;
    width_set = width_px > 0 ? clamp(width_px, PAINT_WIDTH_MIN, PAINT_WIDTH_MAX) : 0;
    apply_icons();   /* the default size of an icon follows the font */
}

void paint_configure_icons(bool enabled, int size_px) {
    icons_on = enabled;
    icon_set = size_px > 0 ? size_px : 0;
    apply_icons();
}

void paint_init(void) {
    const char *icons = getenv("PACHUD_ICONS");
    paint_configure_icons(!(icons && !strcmp(icons, "0")), (int)env_int("PACHUD_ICON_SIZE", 0, PAINT_ICON_MIN, PAINT_ICON_MAX, "pixels"));
    paint_configure((int)env_int("PACHUD_FONT_SIZE", 0, PAINT_FONT_MIN, PAINT_FONT_MAX, "pixels"),
                    (int)env_int("PACHUD_WIDTH", 0, PAINT_WIDTH_MIN, PAINT_WIDTH_MAX, "pixels"));
}

int paint_px(int design_px) {
    int px = (int)lround(design_px * zoom);
    return px < 1 ? 1 : px;
}
double paint_font(double design_px) { return design_px * zoom; }
int paint_item_w(void) { return width_set ? width_set : paint_px(380); }
int paint_osd_w(void) { return paint_px(300); }

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
    round_rect(cr, 0, 0, w, h, RADIUS);
    cairo_set_source_rgba(cr, 0.09, 0.09, 0.11, 0.94);
    cairo_fill_preserve(cr);
    cairo_clip(cr);
}

static void ellipsize(const char *in, double w, double size, bool bold, char *out, size_t n);

void paint_slider(cairo_t *cr, const char *label, int percent, bool dim, const char *icon, int width,
                  int *ow, int *oh) {
    double ascent, height;
    text_extents(label, FONT_SIZE, false, &ascent, &height);
    bool has_icon = sicon_has(icon);
    int isz = has_icon ? sicon_size() : 0;
    int extra = isz > (int)ceil(height) ? isz - (int)ceil(height) : 0;   /* an icon taller than the text */
    int h = SLIDER_H + extra;
    *ow = width;
    *oh = h;
    if (!cr) return;
    if (percent < 0) percent = 0;
    int fill = percent > 100 ? 100 : percent;

    paint_bg(cr, width, h);
    double lx = PAD + (has_icon ? isz + ICON_GAP : 0), row = SLIDER_TOP + (isz > height ? (isz - height) / 2 : 0);
    if (has_icon) {
        static const double text[3] = { 0.93, 0.93, 0.95 };
        sicon_paint(cr, icon, mirror(PAD, isz, width), SLIDER_TOP + (height > isz ? (height - isz) / 2 : 0), text);
    }
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.95);
    char pct[16];
    snprintf(pct, sizeof pct, "%d%%", percent);
    double pw = text_width(pct, (int)strlen(pct), FONT_SIZE, false);
    text_draw(cr, pct, mirror(width - PAD - pw, pw, width), row + ascent, FONT_SIZE, false);

    /* A device name can be long ("Audio output: TU116 High Definition Audio Controller ..."): it gives
     * way to the percentage rather than running into it */
    char shown[256];
    ellipsize(label, width - PAD - pw - ICON_GAP - lx, FONT_SIZE, false, shown, sizeof shown);
    double lw = text_width(shown, (int)strlen(shown), FONT_SIZE, false);
    text_draw(cr, shown, mirror(lx, lw, width), row + ascent, FONT_SIZE, false);

    double tw = width - 2 * PAD, ty = h - PAD - TRACK_H + paint_px(2);
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

/* A PNG file as the small raw icon a card draws (IMAGE_BYTES of premultiplied ARGB32, the same as
 * an image-data hint), or false. Done once when the card arrives, not on every redraw. */
bool load_icon_pixels(const char *path, uint8_t *out) {
    if (!strncmp(path, "file://", 7)) path += 7;
    cairo_surface_t *img = png_load(path);   /* which is careful about what it is given */
    if (!img) return false;

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

/* The paint_* functions draw one item with its top-left at the origin of cr, or only measure it
 * when cr is NULL; either way they report the item's size. Items in the stack all have the same
 * width, so a toast wraps onto a few lines instead of growing wider with its text. */
void paint_toast(cairo_t *cr, const char *text, Level level, const char *icon, int *ow, int *oh) {
    char flat[512];
    snprintf(flat, sizeof flat, "%s", text);
    for (char *p = flat; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';   /* one paragraph, wrapped as a whole */

    bool has_icon = sicon_has(icon);
    int isz = has_icon ? sicon_size() : 0;
    double x0 = BAR + PAD + (has_icon ? isz + ICON_GAP : 0), tw = PAINT_ITEM_W - x0 - PAD;
    Line lines[TOAST_LINES];
    bool truncated;
    int nl = wrap(flat, tw, FONT_SIZE, lines, TOAST_LINES, &truncated);
    double ascent, lh;
    text_extents(*flat ? flat : "x", FONT_SIZE, false, &ascent, &lh);
    double block = (nl ? nl : 1) * lh;   /* the text, which the icon is centred on or centres */
    double content = block > isz ? block : isz;
    int w = PAINT_ITEM_W, h = (int)ceil(content) + 2 * PAD;
    *ow = w;
    *oh = h;
    if (!cr) return;

    double rgb[3];
    accent(level, rgb);

    paint_bg(cr, w, h);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_rectangle(cr, mirror(0, BAR, w), 0, BAR, h);
    cairo_fill(cr);

    cairo_reset_clip(cr);
    if (has_icon) {   /* the level's colour where there is one to speak of, else the text's */
        static const double text[3] = { 0.93, 0.93, 0.95 };
        sicon_paint(cr, icon, mirror(BAR + PAD, isz, w), PAD + (content - isz) / 2, level == LEVEL_INFO ? text : rgb);
    }
    cairo_set_source_rgb(cr, 0.93, 0.93, 0.95);
    for (int i = 0; i < nl; i++) {
        char tmp[520];
        snprintf(tmp, sizeof tmp, "%.*s%s", lines[i].len, lines[i].p, truncated && i == nl - 1 ? "…" : "");
        double lw = text_width(tmp, (int)strlen(tmp), FONT_SIZE, false);
        text_draw(cr, tmp, mirror(x0, lw, w), PAD + (content - block) / 2 + ascent + i * lh, FONT_SIZE, false);
    }
}

void paint_strip(cairo_t *cr, const char *text, Level level, int width, int *ow, int *oh) {
    *ow = width;
    *oh = PAINT_STRIP_H;
    if (!cr) return;

    char flat[256], line[300];
    snprintf(flat, sizeof flat, "%s", text);
    for (char *p = flat; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    double tw = width - BAR - 2 * PAD;
    ellipsize(flat, tw, STRIP_SIZE, false, line, sizeof line);

    double rgb[3];
    accent(level, rgb);
    paint_bg(cr, width, PAINT_STRIP_H);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_rectangle(cr, mirror(0, BAR, width), 0, BAR, PAINT_STRIP_H);
    cairo_fill(cr);

    /* centred in what shows, below the part tucked under the card in front */
    double ascent, height;
    text_extents(line, STRIP_SIZE, false, &ascent, &height);
    double visible = PAINT_STRIP_H - PAINT_STRIP_TUCK;
    double lw = text_width(line, (int)strlen(line), STRIP_SIZE, false);
    cairo_reset_clip(cr);
    cairo_set_source_rgb(cr, 0.80, 0.80, 0.84);
    text_draw(cr, line, mirror(BAR + PAD, lw, width), PAINT_STRIP_TUCK + (visible - height) / 2 + ascent,
              STRIP_SIZE, false);
}

void paint_card(cairo_t *cr, const CardPaint *card, int *ow, int *oh) {
    char body[PAINT_BODY_MAX], summary[256];
    strip_markup(card->body, body, sizeof body);
    snprintf(summary, sizeof summary, "%s", card->summary);
    for (char *p = summary; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    cairo_surface_t *icon = card->image
        ? cairo_image_surface_create_for_data((unsigned char *)card->image, CAIRO_FORMAT_ARGB32,
                                              IMAGE_PX, IMAGE_PX, IMAGE_PX * 4)
        : NULL;

    double x0 = BAR + PAD + (icon ? CARD_ICON + ICON_GAP : 0), tw = PAINT_ITEM_W - x0 - PAD;

    char title[300];
    ellipsize(summary, tw, CARD_SUMMARY_SIZE, true, title, sizeof title);
    double sasc, sheight, basc, bheight;
    text_extents(title, CARD_SUMMARY_SIZE, true, &sasc, &sheight);
    text_extents(*body ? body : "x", CARD_BODY_SIZE, false, &basc, &bheight);

    Line lines[CARD_BODY_LINES_EXPANDED];
    bool truncated;
    int nl = wrap(body, tw, CARD_BODY_SIZE, lines, card->expanded ? CARD_BODY_LINES_EXPANDED : CARD_BODY_LINES,
                  &truncated);

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

    double content = sheight + (nl ? LINE_GAP + nl * bheight : 0) + (card->nact ? ACTION_GAP + aheight : 0);
    if (icon && content < CARD_ICON) content = CARD_ICON;
    int h = (int)ceil(content) + 2 * PAD;

    *ow = PAINT_ITEM_W;
    *oh = h;
    if (!cr) { if (icon) cairo_surface_destroy(icon); return; }

    double rgb[3];
    accent(card->level, rgb);
    paint_bg(cr, PAINT_ITEM_W, h);
    cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
    cairo_rectangle(cr, mirror(0, BAR, PAINT_ITEM_W), 0, BAR, h);
    cairo_fill(cr);

    if (icon) {
        cairo_save(cr);
        cairo_translate(cr, mirror(BAR + PAD, CARD_ICON, PAINT_ITEM_W), PAD);
        cairo_scale(cr, (double)CARD_ICON / cairo_image_surface_get_width(icon),
                    (double)CARD_ICON / cairo_image_surface_get_height(icon));
        cairo_set_source_surface(cr, icon, 0, 0);
        cairo_paint(cr);
        cairo_restore(cr);
        cairo_surface_destroy(icon);
    }

    cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
    double title_w = text_width(title, (int)strlen(title), CARD_SUMMARY_SIZE, true);
    text_draw(cr, title, mirror(x0, title_w, PAINT_ITEM_W), PAD + sasc, CARD_SUMMARY_SIZE, true);

    cairo_set_source_rgb(cr, 0.75, 0.75, 0.80);
    for (int i = 0; i < nl; i++) {
        char tmp[520];
        snprintf(tmp, sizeof tmp, "%.*s%s", lines[i].len, lines[i].p,
                 truncated && i == nl - 1 ? "…" : "");
        double lw = text_width(tmp, (int)strlen(tmp), CARD_BODY_SIZE, false);
        text_draw(cr, tmp, mirror(x0, lw, PAINT_ITEM_W), PAD + sheight + LINE_GAP + i * bheight + basc,
                  CARD_BODY_SIZE, false);
    }
    if (card->nact) {
        cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
        double y = PAD + sheight + (nl ? LINE_GAP + nl * bheight : 0) + ACTION_GAP + aasc;
        text_draw(cr, actions, mirror(x0, actions_w, PAINT_ITEM_W), y, ACTION_SIZE, false);
    }
}

