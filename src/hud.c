#include "hud.h"
#include "i18n.h"
#include "text.h"
#include <limits.h>
#include <math.h>
#include <stddef.h>
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
#define ACTION_SIZE 12
#define ACTION_GAP 6
#define OSD_TIMEOUT_MS 1000

static long long osd_deadline_ms;
static bool osd_visible;
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

static void on_rescale(void);
static void on_click(int x, int y, bool secondary);

void hud_init(Display *d) {
    dpy = d;
    if (d) {
        display_on_rescale(d, on_rescale);
        display_on_click(d, on_click);
    }
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
static void paint_toast(cairo_t *cr, const char *text, Level level, int *ow, int *oh) {
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

static void paint_slider(cairo_t *cr, const char *label, int percent, bool dim, int *ow, int *oh) {
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

/* What is on screen, and what is waiting. Items stack in one column, newest on top, each with its
 * own timeout. Alerts (warn/crit toasts and every card) are never lost: when the stack is full
 * they wait in a queue for room. Ephemeral items (slider, info toast) never wait: a full stack
 * makes room by dropping the oldest ephemeral one, or the new one is dropped if there is none. */
typedef struct {
    bool slider, dim, card, has_default, resident;
    Level level;
    int ms, percent;
    long long deadline;   /* when it leaves the screen, on the monotonic clock */
    uint32_t id;
    char text[256];   /* toast text, slider label or card summary */
    char body[512];
    char icon[256];
    int nact;
    char act_key[HUD_MAX_ACTIONS][64];
    char act_label[HUD_MAX_ACTIONS][64];
    bool has_image;
    uint8_t image[IMAGE_BYTES];  /* last: copies stop before it unless has_image */
} Item;

#define STACK_MAX 4
#define STACK_GAP 8
#define QUEUE_MAX 8
#define SLIDER_MS 1500

static Item stack[STACK_MAX], queue[QUEUE_MAX];
static int nstack, queued;

static long long deadline_for(int ms) { return ms == HUD_NEVER ? LLONG_MAX : now_ms() + ms; }

/* Items carry a 16 KB icon buffer that most of them do not use, so copies leave it out. */
static void item_copy(Item *dst, const Item *src) {
    size_t n = offsetof(Item, image);
    if (src->has_image) n = sizeof *src;
    memcpy(dst, src, n);
}

static void queue_remove_at(int i) {
    for (; i + 1 < queued; i++) item_copy(&queue[i], &queue[i + 1]);
    queued--;
}

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
        char t[560];
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

static void paint_card(cairo_t *cr, const Item *it, int *ow, int *oh) {
    char body[512], summary[256];
    strip_markup(it->body, body, sizeof body);
    snprintf(summary, sizeof summary, "%s", it->text);
    for (char *p = summary; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    cairo_surface_t *icon = it->has_image
        ? cairo_image_surface_create_for_data((unsigned char *)it->image, CAIRO_FORMAT_ARGB32,
                                              IMAGE_PX, IMAGE_PX, IMAGE_PX * 4)
        : load_icon(it->icon);

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
    if (it->nact) {
        int len = 0;
        for (int i = 0; i < it->nact && len < (int)sizeof actions; i++)
            len += snprintf(actions + len, sizeof actions - len, "%s[%d] %s", i ? "   " : "", i + 1,
                            it->act_label[i]);
        char fit[256];
        ellipsize(actions, tw, ACTION_SIZE, false, fit, sizeof fit);
        snprintf(actions, sizeof actions, "%s", fit);
        text_extents(actions, ACTION_SIZE, false, &aasc, &aheight);
        actions_w = text_width(actions, (int)strlen(actions), ACTION_SIZE, false);
    }

    double content = sheight + (nl ? 4 + nl * bheight : 0) + (it->nact ? ACTION_GAP + aheight : 0);
    if (icon && content < CARD_ICON) content = CARD_ICON;
    int h = (int)ceil(content) + 2 * PAD;

    *ow = CARD_W;
    *oh = h;
    if (!cr) { if (icon) cairo_surface_destroy(icon); return; }

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
    if (it->nact) {
        cairo_set_source_rgb(cr, rgb[0], rgb[1], rgb[2]);
        double y = PAD + sheight + (nl ? 4 + nl * bheight : 0) + ACTION_GAP + aasc;
        text_draw(cr, actions, mirror(x0, actions_w, CARD_W), y, ACTION_SIZE, false);
    }
}

static bool is_alert(const Item *it) { return it->card || (!it->slider && it->level != LEVEL_INFO); }

static void closed(const Item *it, int reason) {
    if (it->id && closed_cb && reason) closed_cb(it->id, reason);
}

static void paint_item(cairo_t *cr, const Item *it, int *w, int *h) {
    if (it->slider) paint_slider(cr, it->text, it->percent, it->dim, w, h);
    else if (it->card) paint_card(cr, it, w, h);
    else paint_toast(cr, it->text, it->level, w, h);
}

/* Where each card is in the surface, from the last redraw: what a click is matched against. */
static ClickRect hit[STACK_MAX];
static uint32_t hit_id[STACK_MAX];
static int nhit;

/* Draws the whole stack into one buffer, so a change in any item is one redraw. */
static void render(void) {
    nhit = 0;
    if (!nstack) {
        display_set_click_rects(dpy, NULL, 0);
        display_hide(dpy);
        return;
    }
    int w[STACK_MAX], h[STACK_MAX], x[STACK_MAX], y[STACK_MAX], cw = 0, ch = 0;
    for (int i = 0; i < nstack; i++) {
        paint_item(NULL, &stack[i], &w[i], &h[i]);
        if (w[i] > cw) cw = w[i];
    }
    for (int i = 0; i < nstack; i++) {
        x[i] = (int)((cw - w[i]) * display_align(dpy) / 2.0);
        y[i] = ch;
        ch += h[i] + STACK_GAP;
        if (!stack[i].card) continue;
        hit[nhit] = (ClickRect){ x[i], y[i], w[i], h[i] };
        hit_id[nhit] = stack[i].id;
        nhit++;
    }
    ch -= STACK_GAP;
    display_set_click_rects(dpy, hit, nhit);

    cairo_t *cr;
    Buf *buf = display_buf(dpy, cw, ch, &cr);
    if (!buf) return;
    for (int i = 0; i < nstack; i++) {
        int dw, dh;
        cairo_save(cr);
        cairo_translate(cr, x[i], y[i]);
        paint_item(cr, &stack[i], &dw, &dh);
        cairo_restore(cr);
    }
    display_present(dpy, buf, cr);
}

/* The compositor wants a different scale: draw what is on screen again, timers untouched. */
static void on_rescale(void) { render(); }

static void stack_insert(int at, const Item *it) {
    for (int i = nstack; i > at; i--) item_copy(&stack[i], &stack[i - 1]);
    item_copy(&stack[at], it);
    nstack++;
}

/* Leaves the stack (reason 0: silently). */
static void stack_remove_at(int i, int reason) {
    closed(&stack[i], reason);
    for (; i + 1 < nstack; i++) item_copy(&stack[i], &stack[i + 1]);
    nstack--;
}

/* Room appeared: the oldest waiting alerts take it, below what is already shown. */
static void refill(void) {
    while (nstack < STACK_MAX && queued) {
        static Item next;
        item_copy(&next, &queue[0]);
        queue_remove_at(0);
        next.deadline = deadline_for(next.ms);
        stack_insert(nstack, &next);
    }
}

static bool queued_text(const char *text) {
    for (int i = 0; i < queued; i++)
        if (!queue[i].card && !strcmp(queue[i].text, text)) return true;
    return false;
}

static void enqueue(const Item *it) {
    if (queued == QUEUE_MAX) closed(it, HUD_CLOSED_OTHER);
    else item_copy(&queue[queued++], it);
}

/* The stack position of what `it` replaces: the card with the same id, or for a toast or slider
 * the same text. */
static int find_same(const Item *it) {
    for (int i = 0; i < nstack; i++) {
        if (it->id) {
            if (stack[i].id == it->id) return i;
        } else if (!stack[i].card && !stack[i].id && stack[i].slider == it->slider &&
                   !strcmp(stack[i].text, it->text)) {
            return i;
        }
    }
    return -1;
}

static int oldest_ephemeral(void) {
    for (int i = nstack - 1; i >= 0; i--)
        if (!is_alert(&stack[i])) return i;
    return -1;
}

static void submit(const Item *in) {
    static Item add;
    item_copy(&add, in);
    add.deadline = deadline_for(add.ms);

    int same = find_same(&add);
    if (same >= 0) {  /* refreshed where it is, with a new timeout */
        item_copy(&stack[same], &add);
        render();
        return;
    }
    if (add.id) {
        for (int i = 0; i < queued; i++)
            if (queue[i].id == add.id) { item_copy(&queue[i], &add); return; }
    }

    if (nstack == STACK_MAX) {
        int old = oldest_ephemeral();
        if (old >= 0) {
            stack_remove_at(old, 0);
        } else {
            if (is_alert(&add) && (add.card || !queued_text(add.text))) enqueue(&add);
            return;
        }
    }
    stack_insert(0, &add);
    render();
}

void hud_toast(const char *text, Level level, int timeout_ms) {
    static Item it;
    memset(&it, 0, offsetof(Item, image));
    it.level = level;
    it.ms = timeout_ms;
    snprintf(it.text, sizeof it.text, "%s", text);
    submit(&it);
}

void hud_slider(const char *label, int percent, bool dim) {
    static Item it;
    memset(&it, 0, offsetof(Item, image));
    it.slider = true;
    it.dim = dim;
    it.ms = SLIDER_MS;
    it.percent = percent;
    snprintf(it.text, sizeof it.text, "%s", label);
    submit(&it);
}

static void add_action(Item *it, const char *key, const char *label) {
    snprintf(it->act_key[it->nact], sizeof it->act_key[0], "%s", key);
    snprintf(it->act_label[it->nact], sizeof it->act_label[0], "%s", label);
    for (char *p = it->act_label[it->nact]; *p; p++)
        if (*p == '\n' || *p == '\t') *p = ' ';
    it->nact++;
}

/* Which of a notification's actions the card offers: all but "default" (which apps use for
 * "the user clicked the notification"), unless that is all there is. */
static void set_actions(Item *it, const char *const *a, int n) {
    it->nact = 0;
    it->has_default = false;
    for (int i = 0; a && i + 1 < n; i += 2)
        if (!strcmp(a[i], "default")) it->has_default = true;
    for (int i = 0; a && i + 1 < n && it->nact < HUD_MAX_ACTIONS; i += 2)
        if (strcmp(a[i], "default") != 0) add_action(it, a[i], a[i + 1]);
    if (it->nact) return;
    for (int i = 0; a && i + 1 < n; i += 2) {
        if (!strcmp(a[i], "default")) {
            add_action(it, a[i], *a[i + 1] ? a[i + 1] : _("Open"));
            return;
        }
    }
}

void hud_card(const HudCard *c) {
    static Item it;  /* reused for every card instead of living on the stack */
    memset(&it, 0, offsetof(Item, image));
    it.has_image = false;
    it.card = true;
    it.id = c->id;
    it.level = c->level;
    it.ms = c->timeout_ms;
    it.resident = c->resident;
    snprintf(it.text, sizeof it.text, "%s", c->summary);
    snprintf(it.body, sizeof it.body, "%s", c->body);
    snprintf(it.icon, sizeof it.icon, "%s", c->icon_path ? c->icon_path : "");
    if (c->image) {
        it.has_image = true;
        memcpy(it.image, c->image, IMAGE_BYTES);
    }
    set_actions(&it, c->actions, c->n_actions);
    submit(&it);
}

const char *hud_action_key(int n, uint32_t *id) {
    for (int i = 0; i < nstack; i++) {
        if (!stack[i].card) continue;
        if (n < 1 || n > stack[i].nact) return NULL;
        *id = stack[i].id;
        return stack[i].act_key[n - 1];
    }
    return NULL;
}

void hud_on_closed(HudClosedFn fn) { closed_cb = fn; }

static HudClickFn click_cb;
void hud_on_click(HudClickFn fn) { click_cb = fn; }

static const Item *by_id(uint32_t id) {
    for (int i = 0; id && i < nstack; i++)
        if (stack[i].id == id) return &stack[i];
    return NULL;
}

void hud_action_invoked(uint32_t id) {
    const Item *it = by_id(id);
    if (it && !it->resident) hud_dismiss_id(id);
}

static void on_click(int px, int py, bool secondary) {
    for (int k = 0; k < nhit; k++) {
        if (px < hit[k].x || py < hit[k].y || px >= hit[k].x + hit[k].w || py >= hit[k].y + hit[k].h)
            continue;
        uint32_t id = hit_id[k];  /* the redraw that closing causes replaces the table */
        const Item *it = by_id(id);
        if (!secondary && it && it->has_default) {
            if (click_cb) click_cb(id);
            hud_action_invoked(id);
        } else {
            hud_dismiss_id(id);
        }
        return;
    }
}

static void left_the_stack(void) {
    refill();
    render();
}

bool hud_remove(uint32_t id) {
    if (!id) return false;
    for (int i = 0; i < nstack; i++) {
        if (stack[i].id == id) {
            stack_remove_at(i, 0);
            left_the_stack();
            return true;
        }
    }
    for (int i = 0; i < queued; i++) {
        if (queue[i].id == id) {
            queue_remove_at(i);
            return true;
        }
    }
    return false;
}

bool hud_dismiss_id(uint32_t id) {
    for (int i = 0; id && i < nstack; i++) {
        if (stack[i].id == id) {
            stack_remove_at(i, HUD_CLOSED_DISMISSED);
            left_the_stack();
            return true;
        }
    }
    return false;
}

int hud_card_count(void) {
    int n = 0;
    for (int i = 0; i < nstack; i++) n += stack[i].card;
    for (int i = 0; i < queued; i++) n += queue[i].card;
    return n;
}

void hud_dismiss(void) {
    if (!nstack) return;
    int i = 0;
    for (int k = 0; k < nstack; k++) {
        if (is_alert(&stack[k])) { i = k; break; }
    }
    stack_remove_at(i, HUD_CLOSED_DISMISSED);
    left_the_stack();
}

int hud_timeout_ms(void) {
    long long soonest = -1;
    for (int i = 0; i < nstack; i++)
        if (stack[i].deadline != LLONG_MAX && (soonest < 0 || stack[i].deadline < soonest))
            soonest = stack[i].deadline;
    if (osd_visible && (soonest < 0 || osd_deadline_ms < soonest)) soonest = osd_deadline_ms;
    if (soonest < 0) return -1;
    long long left = soonest - now_ms();
    return left < 0 ? 0 : (int)left;
}

void hud_tick(void) {
    long long now = now_ms();
    bool changed = false;
    for (int i = nstack - 1; i >= 0; i--) {
        if (now >= stack[i].deadline) {
            stack_remove_at(i, HUD_CLOSED_EXPIRED);
            changed = true;
        }
    }
    if (changed) left_the_stack();
    if (osd_visible && now >= osd_deadline_ms) hud_osd_hide();
}

/* ---- OSD (on-screen display): center-positioned user feedback ---- */


void hud_osd_slider(const char *label, int percent, bool dim, OsdAnchor anchor) {
    int w, h;
    paint_slider(NULL, label, percent, dim, &w, &h);
    cairo_t *cr;
    Buf *buf = display_osd_buf(dpy, w, h, &cr);
    if (!buf) return;
    paint_slider(cr, label, percent, dim, &w, &h);
    display_osd_present(dpy, buf, cr, anchor);
    osd_visible = true;
    osd_deadline_ms = now_ms() + OSD_TIMEOUT_MS;
}

void hud_osd_hide(void) {
    osd_visible = false;
    display_osd_hide(dpy);
}
