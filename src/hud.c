#include "hud.h"
#include "text.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define FONT_SIZE 16
#define PAD 16
#define BAR 5
#define MAX_TEXT_W 720
#define CARD_W 380
#define CARD_ICON 40
#define CARD_BODY_LINES 4
#define CARD_BODY_SIZE 13
#define CARD_SUMMARY_SIZE 15

static long long deadline_ms;
static bool visible;
static Display *dpy;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

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

void hud_init(Display *d) { dpy = d; }

static void draw_toast(const char *text, Level level) {
    char line[512];
    snprintf(line, sizeof line, "%s", text);
    for (char *p = line; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';

    /* The buffer must be sized up front, so measure first. */
    double ascent, height;
    text_extents(line, FONT_SIZE, false, &ascent, &height);
    double tw = text_width(line, (int)strlen(line), FONT_SIZE, false);
    if (tw > MAX_TEXT_W) tw = MAX_TEXT_W;
    int w = (int)ceil(tw) + 2 * PAD + BAR, h = (int)ceil(height) + 2 * PAD;

    cairo_t *cr;
    Buf *buf = display_buf(dpy, w, h, &cr);
    if (!buf) return;

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

    display_present(dpy, buf, cr);
}

#define SLIDER_W 300
#define SLIDER_H 64
#define TRACK_H 6

static void draw_slider(const char *label, int percent, bool dim) {
    if (percent < 0) percent = 0;
    int fill = percent > 100 ? 100 : percent;

    cairo_t *cr;
    Buf *buf = display_buf(dpy, SLIDER_W, SLIDER_H, &cr);
    if (!buf) return;

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
    display_present(dpy, buf, cr);
}

/* What is on screen, and what is waiting. Alerts (warn/crit toasts and every card) are never
 * lost: they queue behind each other. An ephemeral HUD (slider, info toast) takes the screen
 * only briefly, pushing the alert it interrupted back to the front of the queue. */
typedef struct {
    bool slider, dim, card;
    Level level;
    int ms, percent;
    uint32_t id;
    char text[256];   /* toast text, slider label or card summary */
    char body[512];
    char icon[256];
} Item;

#define QUEUE_MAX 8
#define SLIDER_MS 1500
#define MIN_RESUME_MS 2000

static Item cur, queue[QUEUE_MAX];
static int queued;
static HudClosedFn closed_cb;

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
        char t[300];
        snprintf(t, sizeof t, "%.*s…", (int)len, out);
        if (text_width(t, (int)strlen(t), size, bold) <= w) { snprintf(out, n, "%s", t); return; }
    }
    out[0] = '\0';
}

static cairo_surface_t *load_icon(const char *path) {
    if (!strncmp(path, "file://", 7)) path += 7;
    size_t len = strlen(path);
    if (path[0] != '/' || len < 5 || strcmp(path + len - 4, ".png")) return NULL;
    cairo_surface_t *img = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS ||
        cairo_image_surface_get_width(img) > 2048 || cairo_image_surface_get_height(img) > 2048) {
        cairo_surface_destroy(img);
        return NULL;
    }
    return img;
}

static void draw_card(const Item *it) {
    char body[512], summary[256];
    strip_markup(it->body, body, sizeof body);
    snprintf(summary, sizeof summary, "%s", it->text);
    for (char *p = summary; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    cairo_surface_t *icon = load_icon(it->icon);

    double x0 = BAR + PAD + (icon ? CARD_ICON + 12 : 0), tw = CARD_W - x0 - PAD;

    char title[300];
    ellipsize(summary, tw, CARD_SUMMARY_SIZE, true, title, sizeof title);
    double sasc, sheight, basc, bheight;
    text_extents(title, CARD_SUMMARY_SIZE, true, &sasc, &sheight);
    text_extents(*body ? body : "x", CARD_BODY_SIZE, false, &basc, &bheight);

    Line lines[CARD_BODY_LINES];
    bool truncated;
    int nl = wrap(body, tw, CARD_BODY_SIZE, lines, CARD_BODY_LINES, &truncated);

    double content = sheight + (nl ? 4 + nl * bheight : 0);
    if (icon && content < CARD_ICON) content = CARD_ICON;
    int h = (int)ceil(content) + 2 * PAD;

    cairo_t *cr;
    Buf *buf = display_buf(dpy, CARD_W, h, &cr);
    if (!buf) { if (icon) cairo_surface_destroy(icon); return; }

    double rgb[3];
    accent(it->level, rgb);
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
    display_present(dpy, buf, cr);
}

static bool is_alert(const Item *it) { return it->card || (!it->slider && it->level != LEVEL_INFO); }

static void closed(const Item *it, int reason) {
    if (it->id && closed_cb && reason) closed_cb(it->id, reason);
}

static void start(const Item *it) {
    cur = *it;
    if (it->slider) draw_slider(it->text, it->percent, it->dim);
    else if (it->card) draw_card(it);
    else draw_toast(it->text, it->level);
    visible = true;
    deadline_ms = now_ms() + it->ms;
}

/* Leaves the current item (reason 0: silently) and shows the next one, if any. */
static void advance(int reason) {
    closed(&cur, reason);
    if (queued) {
        Item next = queue[0];
        memmove(queue, queue + 1, --queued * sizeof *queue);
        start(&next);
    } else {
        display_hide(dpy);
        visible = false;
    }
}

static bool queued_text(const char *text) {
    for (int i = 0; i < queued; i++)
        if (!queue[i].card && !strcmp(queue[i].text, text)) return true;
    return false;
}

static void push_front(const Item *it) {
    if (queued == QUEUE_MAX) closed(&queue[--queued], HUD_CLOSED_OTHER);
    memmove(queue + 1, queue, queued * sizeof *queue);
    queue[0] = *it;
    queued++;
}

static void enqueue(const Item *it) {
    if (queued == QUEUE_MAX) closed(it, HUD_CLOSED_OTHER);
    else queue[queued++] = *it;
}

static void submit(const Item *it) {
    if (it->id) {  /* a replacement for a card we already hold */
        if (visible && cur.id == it->id) { start(it); return; }
        for (int i = 0; i < queued; i++)
            if (queue[i].id == it->id) { queue[i] = *it; return; }
    }

    if (!visible) {
        start(it);
    } else if (is_alert(it)) {
        if (!is_alert(&cur)) start(it);
        else if (!it->card && !cur.card && !strcmp(cur.text, it->text)) start(it);  /* refresh */
        else if (it->card || !queued_text(it->text)) enqueue(it);
    } else {
        if (is_alert(&cur)) {
            Item resumed = cur;
            long long left = deadline_ms - now_ms();
            resumed.ms = left > MIN_RESUME_MS ? (int)left : MIN_RESUME_MS;
            push_front(&resumed);
        }
        start(it);
    }
}

void hud_toast(const char *text, Level level, int timeout_ms) {
    Item it = { .level = level, .ms = timeout_ms };
    snprintf(it.text, sizeof it.text, "%s", text);
    submit(&it);
}

void hud_slider(const char *label, int percent, bool dim) {
    Item it = { .slider = true, .dim = dim, .ms = SLIDER_MS, .percent = percent };
    snprintf(it.text, sizeof it.text, "%s", label);
    submit(&it);
}

void hud_card(uint32_t id, const char *summary, const char *body, const char *icon_path,
              Level level, int timeout_ms) {
    Item it = { .card = true, .id = id, .level = level, .ms = timeout_ms };
    snprintf(it.text, sizeof it.text, "%s", summary);
    snprintf(it.body, sizeof it.body, "%s", body);
    snprintf(it.icon, sizeof it.icon, "%s", icon_path);
    submit(&it);
}

void hud_on_closed(HudClosedFn fn) { closed_cb = fn; }

bool hud_remove(uint32_t id) {
    if (!id) return false;
    if (visible && cur.id == id) { advance(0); return true; }
    for (int i = 0; i < queued; i++) {
        if (queue[i].id == id) {
            memmove(queue + i, queue + i + 1, (--queued - i) * sizeof *queue);
            return true;
        }
    }
    return false;
}

void hud_dismiss(void) {
    if (visible) advance(HUD_CLOSED_DISMISSED);
}

int hud_timeout_ms(void) {
    if (!visible) return -1;
    long long left = deadline_ms - now_ms();
    return left < 0 ? 0 : (int)left;
}

void hud_tick(void) {
    if (visible && now_ms() >= deadline_ms) advance(HUD_CLOSED_EXPIRED);
}
