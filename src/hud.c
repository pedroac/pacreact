#include "hud.h"
#include "i18n.h"
#include "paint.h"
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define OSD_TIMEOUT_MS 1000

static long long osd_deadline_ms;
static bool osd_visible;
static Display *dpy;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
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

static bool is_alert(const Item *it) { return it->card || (!it->slider && it->level != LEVEL_INFO); }

static void closed(const Item *it, int reason) {
    if (it->id && closed_cb && reason) closed_cb(it->id, reason);
}

static void paint_item(cairo_t *cr, const Item *it, int *w, int *h) {
    if (it->slider) paint_slider(cr, it->text, it->percent, it->dim, w, h);
    else if (it->card) {
        CardPaint c = { .summary = it->text, .body = it->body, .level = it->level,
                        .image = it->has_image ? it->image : NULL, .nact = it->nact };
        for (int i = 0; i < it->nact; i++) c.act_label[i] = it->act_label[i];
        paint_card(cr, &c, w, h);
    }
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
    if (c->image) {
        it.has_image = true;
        memcpy(it.image, c->image, IMAGE_BYTES);
    } else if (c->icon_path && *c->icon_path) {
        it.has_image = load_icon_pixels(c->icon_path, it.image);   /* once, not on every redraw */
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

#define TOKEN_WAIT_MS 2000   /* the compositor always answers; one that does not must not block clicks */
static bool token_pending;   /* a click is waiting for its activation token */
static long long token_since;

/* The click on a card's default action, with the token the compositor gave for it (or NULL). */
static void click_default(uint32_t id, const char *token) {
    token_pending = false;
    if (!by_id(id)) return;   /* it left the screen while the compositor was answering */
    if (click_cb) click_cb(id, token);
    hud_action_invoked(id);
}

static void token_ready(const char *token, void *ctx) { click_default((uint32_t)(uintptr_t)ctx, token); }

static void on_click(int px, int py, bool secondary) {
    for (int k = 0; k < nhit; k++) {
        if (px < hit[k].x || py < hit[k].y || px >= hit[k].x + hit[k].w || py >= hit[k].y + hit[k].h)
            continue;
        uint32_t id = hit_id[k];  /* the redraw that closing causes replaces the table */
        const Item *it = by_id(id);
        if (!secondary && it && it->has_default) {
            if (token_pending && now_ms() - token_since < TOKEN_WAIT_MS) return;
            token_pending = true;
            token_since = now_ms();
            if (!display_request_token(dpy, token_ready, (void *)(uintptr_t)id)) click_default(id, NULL);
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
