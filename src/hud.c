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
static void on_click(int area, int x, int y, bool secondary);
static void on_hover(int area, int x, int y, bool over);

void hud_init(Display *d) {
    dpy = d;
    if (d) {
        display_on_rescale(d, on_rescale);
        display_on_click(d, on_click);
        display_on_hover(d, on_hover);
    }
}

/* What is on screen, and what is waiting. Items stack in one column, newest on top, each with its
 * own timeout. Alerts (warn/crit toasts and every card) are never lost: when the stack is full
 * they wait in a queue for room. Ephemeral items (slider, info toast) never wait: a full stack
 * makes room by dropping the oldest ephemeral one, or the new one is dropped if there is none. */
typedef struct {
    bool slider, dim, card, has_default, resident, expanded, attention;
    Level level;
    int ms, percent;
    long long deadline;   /* when it leaves the screen, on the monotonic clock */
    uint32_t id;
    char text[256];   /* toast text, slider label or card summary */
    char icon[64];    /* a toast's or slider's status icon: a name, or a few, comma separated */
    char body[PAINT_BODY_MAX];
    int nact;
    char act_key[HUD_MAX_ACTIONS][64];
    char act_label[HUD_MAX_ACTIONS][64];
    bool has_image;
    uint8_t image[IMAGE_BYTES];  /* last: copies stop before it unless has_image */
} Item;

#define STACK_MAX 4
#define DECK_MAX 3
#define ITEMS_MAX (STACK_MAX + DECK_MAX)
#define STACK_GAP paint_px(8)
#define QUEUE_MAX 8
#define SLIDER_MS 1500

/* Everything on screen, in one array: first the stack's items, newest first, then the deck's, the
 * front one first (the oldest of them). `ndeck` is how many of the last ones are the deck's. */
static Item stack[ITEMS_MAX], queue[QUEUE_MAX];
static int nstack, ndeck, queued;

#define NSTACK (nstack - ndeck)
#define DECK_FRONT (nstack - ndeck)   /* the index of the deck's front item, when there is one */

static bool apart(void) { return display_deck_apart(dpy, PAINT_ITEM_W); }

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

/* Reading a long notification. The pointer over a card holds it (its timeout waits) and, after a
 * moment, opens it to show more of its body; `pachud expand` does the same from the keyboard, for
 * as long as it is not toggled again. When a card stops being held it stays at least this long. */
#define HOVER_DELAY_MS 250
#define HOLD_GRACE_MS 2000

static uint32_t hover_id;      /* the card the pointer is over, or 0 */
static uint32_t hover_open;    /* the card opened by the hover: the same one, once the delay has passed */
static long long hover_since;
static bool ptr_over;          /* the pointer is on the HUD, here, in this area */
static int ptr_area, ptr_x, ptr_y;

static bool held(const Item *it) { return it->card && (it->expanded || it->id == hover_id); }

static void paint_item(cairo_t *cr, const Item *it, int *w, int *h) {
    if (it->slider) paint_slider(cr, it->text, it->percent, it->dim, it->icon, PAINT_ITEM_W, w, h);
    else if (it->card) {
        CardPaint c = { .summary = it->text, .body = it->body, .level = it->level,
                        .image = it->has_image ? it->image : NULL, .nact = it->nact,
                        .expanded = it->expanded || it->id == hover_open };
        for (int i = 0; i < it->nact; i++) c.act_label[i] = it->act_label[i];
        paint_card(cr, &c, w, h);
    }
    else paint_toast(cr, it->text, it->level, it->icon, w, h);
}

/* Where each card is in its surface, from the last redraw: what a click is matched against. The
 * deck has its own table, which also holds its strips. */
static ClickRect hit[STACK_MAX];
static uint32_t hit_id[STACK_MAX];
static int nhit;
static ClickRect dhit[DECK_MAX];
static uint32_t dhit_id[DECK_MAX];
static bool dhit_strip[DECK_MAX];
static int ndhit;

static void rehover(void);
static const Item *by_id(uint32_t id);

/* The stack: one buffer, so a change in any item is one redraw. */
static void render_stack(void) {
    int n = NSTACK;
    nhit = 0;
    if (!n) {
        display_set_click_rects(dpy, DISPLAY_AREA_STACK, NULL, 0);
        display_hide(dpy);
        return;
    }
    int w[STACK_MAX], h[STACK_MAX], x[STACK_MAX], y[STACK_MAX], cw = 0, ch = 0;
    for (int i = 0; i < n; i++) {
        paint_item(NULL, &stack[i], &w[i], &h[i]);
        if (w[i] > cw) cw = w[i];
    }
    for (int i = 0; i < n; i++) {
        x[i] = (int)((cw - w[i]) * display_align(dpy) / 2.0);
        y[i] = ch;
        ch += h[i] + STACK_GAP;
        if (!stack[i].card) continue;
        hit[nhit] = (ClickRect){ x[i], y[i], w[i], h[i] };
        hit_id[nhit] = stack[i].id;
        nhit++;
    }
    ch -= STACK_GAP;
    display_set_click_rects(dpy, DISPLAY_AREA_STACK, hit, nhit);

    cairo_t *cr;
    Buf *buf = display_buf(dpy, cw, ch, &cr);
    if (buf) {
        for (int i = 0; i < n; i++) {
            int dw, dh;
            cairo_save(cr);
            cairo_translate(cr, x[i], y[i]);
            paint_item(cr, &stack[i], &dw, &dh);
            cairo_restore(cr);
        }
        display_present(dpy, buf, cr);
    }
}

/* The deck: the front item in full, and the others as strips tucked under it, each a little
 * narrower, so the pile reads as cards behind one another. */
static void render_deck(void) {
    ndhit = 0;
    if (!ndeck) {
        display_set_click_rects(dpy, DISPLAY_AREA_DECK, NULL, 0);
        display_deck_hide(dpy);
        return;
    }
    const Item *front = &stack[DECK_FRONT];
    int fw, fh, behind = ndeck - 1;
    paint_item(NULL, front, &fw, &fh);

    int sx[DECK_MAX], sy[DECK_MAX], sw[DECK_MAX], total = fh;
    for (int k = 1; k <= behind; k++) {
        sw[k] = fw - 2 * PAINT_STRIP_INSET * k;
        sx[k] = (fw - sw[k]) / 2;
        sy[k] = fh - PAINT_STRIP_TUCK + (k - 1) * (PAINT_STRIP_H - PAINT_STRIP_TUCK);
        total = sy[k] + PAINT_STRIP_H;
    }

    if (front->card) {
        dhit[ndhit] = (ClickRect){ 0, 0, fw, fh };
        dhit_id[ndhit] = front->id;
        dhit_strip[ndhit++] = false;
    }
    for (int k = 1; k <= behind; k++) {   /* only what shows of a strip takes clicks */
        dhit[ndhit] = (ClickRect){ sx[k], sy[k] + PAINT_STRIP_TUCK, sw[k], PAINT_STRIP_H - PAINT_STRIP_TUCK };
        dhit_id[ndhit] = stack[DECK_FRONT + k].id;
        dhit_strip[ndhit++] = true;
    }
    display_set_click_rects(dpy, DISPLAY_AREA_DECK, dhit, ndhit);

    cairo_t *cr;
    Buf *buf = display_buf(dpy, fw, total, &cr);
    if (buf) {
        for (int k = behind; k >= 1; k--) {   /* the back first, the front over them */
            const Item *it = &stack[DECK_FRONT + k];
            int dw, dh;
            cairo_save(cr);
            cairo_translate(cr, sx[k], sy[k]);
            paint_strip(cr, it->text, it->level, sw[k], &dw, &dh);
            cairo_restore(cr);
        }
        int dw, dh;
        paint_item(cr, front, &dw, &dh);
        display_deck_present(dpy, buf, cr);
    }
}

static void render(void) {
    if (hover_id && !by_id(hover_id)) hover_id = hover_open = 0;   /* the card is gone */
    render_stack();
    render_deck();
    rehover();   /* the layout moved: the pointer may be over something else now */
}

/* The compositor wants a different scale: draw what is on screen again, timers untouched. */
static void on_rescale(void) { render(); }

static void stack_insert(int at, const Item *it) {
    for (int i = nstack; i > at; i--) item_copy(&stack[i], &stack[i - 1]);
    item_copy(&stack[at], it);
    nstack++;
    if (it->attention) ndeck++;
}

/* Leaves the stack (reason 0: silently). */
static void stack_remove_at(int i, int reason) {
    closed(&stack[i], reason);
    if (stack[i].attention) ndeck--;
    for (; i + 1 < nstack; i++) item_copy(&stack[i], &stack[i + 1]);
    nstack--;
}

/* Room appeared: the oldest waiting alerts take it, below what is already shown in their place. */
static void refill(void) {
    for (int i = 0; i < queued;) {
        bool room = queue[i].attention ? ndeck < DECK_MAX : NSTACK < STACK_MAX;
        if (!room) { i++; continue; }
        static Item next;
        item_copy(&next, &queue[i]);
        queue_remove_at(i);
        next.deadline = deadline_for(next.ms);
        stack_insert(next.attention ? nstack : NSTACK, &next);
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
    if (same >= 0 && stack[same].attention == add.attention) {
        /* refreshed where it is, with a new timeout, and as open as it was */
        bool expanded = stack[same].expanded;
        item_copy(&stack[same], &add);
        stack[same].expanded = expanded;
        render();
        return;
    }
    if (same >= 0) stack_remove_at(same, 0);   /* it changes places: leaves one, arrives in the other */
    if (add.id) {
        for (int i = 0; i < queued; i++)
            if (queue[i].id == add.id) { item_copy(&queue[i], &add); return; }
    }

    if (add.attention) {
        if (ndeck == DECK_MAX) {   /* what is in the deck is not made to leave: this waits */
            if (add.card || !queued_text(add.text)) enqueue(&add);
            return;
        }
    } else if (NSTACK == STACK_MAX) {
        int old = oldest_ephemeral();
        if (old >= 0) {
            stack_remove_at(old, 0);
        } else {
            if (is_alert(&add) && (add.card || !queued_text(add.text))) enqueue(&add);
            return;
        }
    }
    stack_insert(add.attention ? nstack : 0, &add);
    render();
}

void hud_toast_icon(const char *text, Level level, int timeout_ms, const char *icon) {
    static Item it;
    memset(&it, 0, offsetof(Item, image));
    it.level = level;
    it.ms = timeout_ms;
    it.attention = level == LEVEL_CRIT && apart();   /* the system's critical alerts */
    snprintf(it.text, sizeof it.text, "%s", text);
    snprintf(it.icon, sizeof it.icon, "%s", icon ? icon : "");
    submit(&it);
}

void hud_toast(const char *text, Level level, int timeout_ms) { hud_toast_icon(text, level, timeout_ms, NULL); }

void hud_slider(const char *label, int percent, bool dim, const char *icon) {
    static Item it;
    memset(&it, 0, offsetof(Item, image));
    it.slider = true;
    it.dim = dim;
    it.ms = SLIDER_MS;
    it.percent = percent;
    snprintf(it.text, sizeof it.text, "%s", label);
    snprintf(it.icon, sizeof it.icon, "%s", icon ? icon : "");
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
    it.attention = c->attention && apart();
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

/* The card that actions, expansion and the like apply to: the deck's front one, which is what is
 * asking for attention, else the newest in the stack. */
static Item *front_card(void) {
    if (ndeck && stack[DECK_FRONT].card) return &stack[DECK_FRONT];
    for (int i = 0; i < NSTACK; i++)
        if (stack[i].card) return &stack[i];
    return NULL;
}

const char *hud_action_key(int n, uint32_t *id) {
    const Item *it = front_card();
    if (!it || n < 1 || n > it->nact) return NULL;
    *id = it->id;
    return it->act_key[n - 1];
}

void hud_on_closed(HudClosedFn fn) { closed_cb = fn; }

static HudClickFn click_cb;
void hud_on_click(HudClickFn fn) { click_cb = fn; }

static const Item *by_id(uint32_t id) {
    for (int i = 0; id && i < nstack; i++)
        if (stack[i].id == id) return &stack[i];
    return NULL;
}

static Item *find_card(uint32_t id) {
    for (int i = 0; id && i < nstack; i++)
        if (stack[i].id == id && stack[i].card) return &stack[i];
    return NULL;
}

/* A card that is no longer held (the pointer left, or it was collapsed) is not to vanish at once:
 * what was being read gets a little more time. */
static void release(uint32_t id) {
    Item *it = find_card(id);
    if (!it || held(it) || it->deadline == LLONG_MAX) return;
    long long least = now_ms() + HOLD_GRACE_MS;
    if (it->deadline < least) it->deadline = least;
}

static bool inside(const ClickRect *r, int px, int py) {
    return px >= r->x && py >= r->y && px < r->x + r->w && py < r->y + r->h;
}

/* What is at a point of an area: a card (then `strip` is false) or a strip of the deck, or nothing */
static uint32_t hit_at(int area, int px, int py, bool *strip) {
    *strip = false;
    if (area == DISPLAY_AREA_DECK) {
        for (int k = 0; k < ndhit; k++)
            if (inside(&dhit[k], px, py)) { *strip = dhit_strip[k]; return dhit_id[k]; }
        return 0;
    }
    for (int k = 0; k < nhit; k++)
        if (inside(&hit[k], px, py)) return hit_id[k];
    return 0;
}

static uint32_t card_at(int area, int px, int py) {
    bool strip;
    uint32_t id = hit_at(area, px, py, &strip);
    return strip ? 0 : id;
}

static void set_hover(uint32_t id) {
    if (id == hover_id) return;
    uint32_t old = hover_id;
    bool was_open = hover_open != 0;
    hover_id = id;
    hover_open = 0;
    hover_since = now_ms();
    if (old) release(old);
    if (was_open) render();   /* it closes */
}

static void rehover(void) { set_hover(ptr_over ? card_at(ptr_area, ptr_x, ptr_y) : 0); }

static void on_hover(int area, int x, int y, bool over) {
    ptr_over = over;
    ptr_area = area;
    ptr_x = x;
    ptr_y = y;
    rehover();
}

int hud_toggle_expand(void) {
    Item *it = front_card();
    if (!it) return -1;
    it->expanded = !it->expanded;
    if (!it->expanded) release(it->id);
    render();
    return it->expanded;
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

/* Brings a card of the deck to the front: the user's own choice, never done on its own. */
static void raise_in_deck(uint32_t id) {
    int at = -1;
    for (int i = DECK_FRONT; ndeck && i < nstack; i++)
        if (stack[i].id == id) at = i;
    if (at <= DECK_FRONT) return;
    static Item moved;
    item_copy(&moved, &stack[at]);
    for (int i = at; i > DECK_FRONT; i--) item_copy(&stack[i], &stack[i - 1]);
    item_copy(&stack[DECK_FRONT], &moved);
    render();
}

static void on_click(int area, int px, int py, bool secondary) {
    bool strip;
    uint32_t id = hit_at(area, px, py, &strip);   /* the redraw that closing causes replaces the tables */
    if (!id) return;
    if (strip) {   /* a card behind the front one: bring it forward, or with the right button, get rid of it */
        if (secondary) hud_dismiss_id(id);
        else raise_in_deck(id);
        return;
    }
    const Item *it = by_id(id);
    if (!secondary && it && it->has_default) {
        if (token_pending && now_ms() - token_since < TOKEN_WAIT_MS) return;
        token_pending = true;
        token_since = now_ms();
        if (!display_request_token(dpy, token_ready, (void *)(uintptr_t)id)) click_default(id, NULL);
    } else {
        hud_dismiss_id(id);
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
    int i = ndeck ? DECK_FRONT : 0;   /* what asks for attention first */
    for (int k = 0; !ndeck && k < NSTACK; k++) {
        if (is_alert(&stack[k])) { i = k; break; }
    }
    stack_remove_at(i, HUD_CLOSED_DISMISSED);
    left_the_stack();
}

int hud_timeout_ms(void) {
    long long soonest = -1;
    for (int i = 0; i < nstack; i++)
        if (!held(&stack[i]) && stack[i].deadline != LLONG_MAX && (soonest < 0 || stack[i].deadline < soonest))
            soonest = stack[i].deadline;
    long long open_at = hover_since + HOVER_DELAY_MS;   /* the pointer has lingered: open the card then */
    if (hover_id && !hover_open && (soonest < 0 || open_at < soonest)) soonest = open_at;
    if (osd_visible && (soonest < 0 || osd_deadline_ms < soonest)) soonest = osd_deadline_ms;
    if (soonest < 0) return -1;
    long long left = soonest - now_ms();
    return left < 0 ? 0 : (int)left;
}

void hud_tick(void) {
    long long now = now_ms();
    bool changed = false;
    for (int i = nstack - 1; i >= 0; i--) {
        if (!held(&stack[i]) && now >= stack[i].deadline) {
            stack_remove_at(i, HUD_CLOSED_EXPIRED);
            changed = true;
        }
    }
    if (changed) left_the_stack();
    if (hover_id && !hover_open && now >= hover_since + HOVER_DELAY_MS) {
        hover_open = hover_id;
        render();
    }
    if (osd_visible && now >= osd_deadline_ms) hud_osd_hide();
}

/* ---- OSD (on-screen display): center-positioned user feedback ---- */


void hud_osd_slider(const char *label, int percent, bool dim, OsdAnchor anchor, const char *icon) {
    int w, h;
    paint_slider(NULL, label, percent, dim, icon, PAINT_OSD_W, &w, &h);
    cairo_t *cr;
    Buf *buf = display_osd_buf(dpy, w, h, &cr);
    if (!buf) return;
    paint_slider(cr, label, percent, dim, icon, PAINT_OSD_W, &w, &h);
    display_osd_present(dpy, buf, cr, anchor);
    osd_visible = true;
    osd_deadline_ms = now_ms() + OSD_TIMEOUT_MS;
}

void hud_osd_hide(void) {
    osd_visible = false;
    display_osd_hide(dpy);
}
