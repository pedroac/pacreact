/* The HUD manager (stacking, replacement, overflow) and the card text helpers.
 * hud.c is included so its statics are reachable; drawing and time are stubbed: no surface
 * is ever created, and the clock only moves when the test says so. */
#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long long fake_ms = 1000;
static int fake_clock_gettime(clockid_t c, struct timespec *ts) {
    (void)c;
    ts->tv_sec = fake_ms / 1000;
    ts->tv_nsec = (fake_ms % 1000) * 1000000L;
    return 0;
}
#define clock_gettime fake_clock_gettime

#include "../src/display.h"
#include "../src/sicon.h"
#include "../src/text.h"

static int hides;
Buf *display_buf(Display *d, int w, int h, cairo_t **cr) { (void)d; (void)w; (void)h; (void)cr; return NULL; }
void display_present(Display *d, Buf *b, cairo_t *cr) { (void)d; (void)b; (void)cr; }
void display_hide(Display *d) { (void)d; hides++; }
void display_osd_present(Display *d, Buf *b, cairo_t *cr, OsdAnchor a) { (void)d; (void)b; (void)cr; (void)a; }
void display_osd_hide(Display *d) { (void)d; }
void display_on_rescale(Display *d, void (*fn)(void)) { (void)d; (void)fn; }
int display_align(Display *d) { (void)d; return 2; }
static int click_rects_set = -1, deck_rects_set = -1;
void display_set_click_rects(Display *d, int area, const ClickRect *r, int n) {
    (void)d; (void)r;
    if (area == DISPLAY_AREA_DECK) deck_rects_set = n;
    else click_rects_set = n;
}
void display_on_click(Display *d, void (*fn)(int, int, int, bool)) { (void)d; (void)fn; }
void display_on_hover(Display *d, void (*fn)(int, int, int, bool)) { (void)d; (void)fn; }
static int deck_hides, deck_presents;
static bool deck_is_apart = true;
void display_deck_present(Display *d, Buf *b, cairo_t *cr) { (void)d; (void)b; (void)cr; deck_presents++; }
void display_deck_hide(Display *d) { (void)d; deck_hides++; }
bool display_deck_apart(Display *d, int item_width) { (void)d; (void)item_width; return deck_is_apart; }
/* The compositor's answer comes later: a test makes tokens available, then delivers one. */
static bool tokens_available;
static int token_requests;
static void (*token_fn)(const char *, void *);
static void *token_ctx;
bool display_request_token(Display *d, void (*fn)(const char *, void *), void *ctx) {
    (void)d;
    if (!tokens_available) return false;
    token_requests++;
    token_fn = fn;
    token_ctx = ctx;
    return true;
}

/* Ten units per byte: easy to reason about when wrapping. */
bool text_rtl(void) { return false; }
double text_width(const char *u, int len, double size, bool bold) { (void)u; (void)size; (void)bold; return len * 10.0; }
void text_extents(const char *u, double size, bool bold, double *asc, double *h) {
    (void)u; (void)size; (void)bold;
    *asc = 12;
    *h = 16;
}
void text_draw(cairo_t *cr, const char *u, double x, double y, double size, bool bold) {
    (void)cr; (void)u; (void)x; (void)y; (void)size; (void)bold;
}

#include "../src/paint.c"
#include "../src/hud.c"

/* ---- helpers ---- */

static struct { uint32_t id; int reason; } events[32];
static int nevents;

static void on_closed(uint32_t id, int reason) {
    events[nevents].id = id;
    events[nevents].reason = reason;
    nevents++;
}

static void card(uint32_t id, const char *summary, const char *body, Level level, int ms) {
    HudCard c = { .id = id, .summary = summary, .body = body, .icon_path = "", .level = level,
                  .timeout_ms = ms };
    hud_card(&c);
}

static void reset(void) {
    while (nstack) hud_dismiss();
    queued = 0;
    nevents = 0;
    hides = 0;
    fake_ms += 100000;
}

static void advance_to_expiry(void) {
    fake_ms += hud_timeout_ms();
    hud_tick();
}

/* ---- manager tests ---- */

static void test_toast_expiry(void) {
    reset();
    hud_toast("a", LEVEL_INFO, 500);
    assert(nstack == 1 && hud_timeout_ms() == 500);
    fake_ms += 499;
    hud_tick();
    assert(nstack == 1 && hud_timeout_ms() == 1);
    fake_ms += 1;
    hud_tick();
    assert(!nstack && hud_timeout_ms() == -1 && hides == 1);
}

static void test_stacking(void) {
    reset();
    hud_toast("a", LEVEL_INFO, 500);
    hud_toast("b", LEVEL_INFO, 500);   /* the last one does not hide the one before */
    assert(nstack == 2 && !strcmp(stack[0].text, "b") && !strcmp(stack[1].text, "a"));

    fake_ms += 300;
    hud_toast("a", LEVEL_INFO, 500);   /* the same text: refreshed where it is, not stacked again */
    assert(nstack == 2 && !strcmp(stack[1].text, "a") && hud_timeout_ms() == 200);

    fake_ms += 200;                    /* each leaves on its own timeout */
    hud_tick();
    assert(nstack == 1 && !strcmp(stack[0].text, "a") && hud_timeout_ms() == 300);
}

static void test_alert_dedupe(void) {
    reset();
    hud_toast("A", LEVEL_WARN, 1000);
    hud_toast("B", LEVEL_WARN, 1000);
    assert(nstack == 2 && !strcmp(stack[0].text, "B") && !strcmp(stack[1].text, "A") && queued == 0);

    fake_ms += 600;
    hud_toast("A", LEVEL_WARN, 1000);  /* refreshed, not shown twice */
    assert(nstack == 2 && hud_timeout_ms() == 400);

    advance_to_expiry();
    assert(nstack == 1 && !strcmp(stack[0].text, "A") && hides == 0);
    advance_to_expiry();
    assert(!nstack && hides == 1);
}

static void test_slider_beside_alert(void) {
    reset();
    hud_toast("A", LEVEL_WARN, 5000);
    fake_ms += 1000;
    hud_slider("Volume", 50, false, NULL);   /* no longer pushes the alert out of sight */
    assert(nstack == 2 && stack[0].slider && !strcmp(stack[1].text, "A"));
    assert(hud_timeout_ms() == SLIDER_MS);

    advance_to_expiry();               /* the slider goes; the alert kept its own clock */
    assert(nstack == 1 && !strcmp(stack[0].text, "A") && hud_timeout_ms() == 5000 - 1000 - SLIDER_MS);

    hud_slider("Volume", 60, false, NULL);
    hud_slider("Volume", 70, false, NULL);   /* sliders refresh in place */
    assert(nstack == 2 && stack[0].percent == 70);
}

static void test_stack_full(void) {
    /* Ephemeral items make room by displacing the oldest ephemeral one */
    reset();
    char name[8];
    for (int i = 0; i < STACK_MAX + 1; i++) {
        snprintf(name, sizeof name, "t%d", i);
        hud_toast(name, LEVEL_INFO, 1000);
    }
    assert(nstack == STACK_MAX && !strcmp(stack[0].text, "t4") && !strcmp(stack[STACK_MAX - 1].text, "t1"));

    /* An alert displaces an ephemeral item, never the other way round */
    reset();
    card(1, "a", "", LEVEL_INFO, 1000);
    card(2, "b", "", LEVEL_INFO, 1000);
    card(3, "c", "", LEVEL_INFO, 1000);
    hud_slider("Volume", 10, false, NULL);
    assert(nstack == STACK_MAX && stack[0].slider);
    card(4, "d", "", LEVEL_INFO, 1000);
    assert(nstack == STACK_MAX && queued == 0 && stack[0].id == 4 && !stack[1].slider);
    hud_slider("Volume", 20, false, NULL);   /* nothing ephemeral to displace: the slider is dropped */
    assert(nstack == STACK_MAX && queued == 0 && !stack[0].slider);
    hud_toast("note", LEVEL_INFO, 1000);
    assert(!stack[0].slider && strcmp(stack[0].text, "note") != 0);
}

static void test_cards(void) {
    reset();
    hud_on_closed(on_closed);
    card(1, "first", "body", LEVEL_INFO, 1000);
    card(2, "second", "body", LEVEL_INFO, 1000);
    assert(nstack == 2 && stack[0].id == 2 && stack[1].id == 1);   /* newest on top */

    card(2, "second v2", "", LEVEL_INFO, 1000);   /* replaced in place */
    assert(nstack == 2 && stack[0].id == 2 && !strcmp(stack[0].text, "second v2") && nevents == 0);

    fake_ms += 100;
    card(1, "first v2", "", LEVEL_INFO, 1000);
    assert(stack[1].id == 1 && !strcmp(stack[1].text, "first v2") && nevents == 0);

    advance_to_expiry();                           /* expiry is reported, with the reason */
    assert(nevents == 1 && events[0].id == 2 && events[0].reason == HUD_CLOSED_EXPIRED);
    assert(nstack == 1 && stack[0].id == 1);

    hud_dismiss();
    assert(nevents == 2 && events[1].id == 1 && events[1].reason == HUD_CLOSED_DISMISSED);
    assert(!nstack);
    hud_dismiss();                                 /* nothing on screen: harmless */
    assert(nevents == 2);
}

static void test_remove(void) {
    reset();
    hud_on_closed(on_closed);
    card(1, "a", "", LEVEL_INFO, 1000);
    card(2, "b", "", LEVEL_INFO, 1000);
    card(3, "c", "", LEVEL_INFO, 1000);

    assert(hud_remove(2) && nstack == 2);          /* gone, silently */
    assert(hud_remove(1) && stack[0].id == 3 && nstack == 1);
    assert(nevents == 0);
    assert(!hud_remove(1) && !hud_remove(0) && !hud_remove(99));
    assert(hud_remove(3) && !nstack);

    /* A waiting card is removed silently too, and a leaving one makes room for it */
    reset();
    for (uint32_t id = 1; id <= STACK_MAX + 2; id++) card(id, "x", "", LEVEL_INFO, 1000);
    assert(nstack == STACK_MAX && queued == 2);
    assert(hud_remove(STACK_MAX + 2) && queued == 1);
    assert(hud_remove(2) && nstack == STACK_MAX && queued == 0 && stack[STACK_MAX - 1].id == STACK_MAX + 1);
    assert(nevents == 0);

    /* Dismissing by id takes that card, whatever is on top of it, and says why */
    reset();
    card(20, "card", "", LEVEL_INFO, 1000);
    hud_toast("on top", LEVEL_INFO, 1000);
    assert(hud_dismiss_id(20) && nstack == 1 && !strcmp(stack[0].text, "on top"));
    assert(nevents == 1 && events[0].id == 20 && events[0].reason == HUD_CLOSED_DISMISSED);
    assert(!hud_dismiss_id(20) && !hud_dismiss_id(0));
}

static void test_never_expires(void) {
    reset();
    hud_on_closed(on_closed);
    card(1, "sticky", "", LEVEL_INFO, HUD_NEVER);
    assert(hud_timeout_ms() == -1);                /* nothing to wake up for */
    fake_ms += 10 * 60 * 1000;
    hud_tick();
    assert(nstack == 1 && nevents == 0);

    hud_toast("brief", LEVEL_INFO, 500);           /* others still expire around it */
    assert(hud_timeout_ms() == 500);
    advance_to_expiry();
    assert(nstack == 1 && stack[0].id == 1 && hud_timeout_ms() == -1);

    card(1, "sticky v2", "", LEVEL_INFO, HUD_NEVER);   /* a replacement stays too */
    assert(nstack == 1 && hud_timeout_ms() == -1);
    hud_dismiss();                                 /* until it is dismissed */
    assert(!nstack && nevents == 1 && events[0].reason == HUD_CLOSED_DISMISSED);

    /* A waiting never-expiring card keeps that when it comes in */
    reset();
    for (uint32_t id = 1; id <= STACK_MAX; id++) card(id, "x", "", LEVEL_INFO, 1000);
    card(50, "waits", "", LEVEL_INFO, HUD_NEVER);
    assert(queued == 1);
    advance_to_expiry();
    assert(nstack == 1 && stack[0].id == 50 && hud_timeout_ms() == -1);
}

static void test_card_count(void) {
    reset();
    assert(hud_card_count() == 0);
    hud_toast("toast", LEVEL_WARN, 1000);
    hud_slider("Volume", 40, false, NULL);
    assert(hud_card_count() == 0);                 /* only notification cards count */
    card(1, "a", "", LEVEL_INFO, 1000);
    card(2, "b", "", LEVEL_INFO, 1000);
    assert(hud_card_count() == 2);
    card(2, "b again", "", LEVEL_INFO, 1000);      /* a replacement is the same notification */
    assert(hud_card_count() == 2);

    /* those waiting for room count as well */
    for (uint32_t id = 3; id <= STACK_MAX + 3; id++) card(id, "x", "", LEVEL_INFO, 1000);
    assert(nstack == STACK_MAX && queued > 0 && hud_card_count() == STACK_MAX + 3);

    assert(hud_remove(1) && hud_card_count() == STACK_MAX + 2);
    hud_dismiss();
    assert(hud_card_count() == STACK_MAX + 1);
    while (nstack) hud_dismiss();
    assert(hud_card_count() == 0);
}

static void test_dismiss_prefers_alerts(void) {
    reset();
    hud_on_closed(on_closed);
    card(1, "card", "", LEVEL_INFO, 1000);
    hud_slider("Volume", 30, false, NULL);
    hud_dismiss();                                 /* the card, not the slider on top of it */
    assert(nstack == 1 && stack[0].slider && nevents == 1 && events[0].id == 1);
}

static void test_queue_overflow(void) {
    reset();
    hud_on_closed(on_closed);
    for (uint32_t id = 1; id <= STACK_MAX; id++) card(id, "shown", "", LEVEL_INFO, 1000);
    for (uint32_t id = STACK_MAX + 1; id <= STACK_MAX + QUEUE_MAX; id++) card(id, "waiting", "", LEVEL_INFO, 1000);
    assert(nstack == STACK_MAX && queued == QUEUE_MAX && nevents == 0);

    card(100, "one too many", "", LEVEL_INFO, 1000);
    assert(queued == QUEUE_MAX && nevents == 1);
    assert(events[0].id == 100 && events[0].reason == HUD_CLOSED_OTHER);

    /* The waiting ones come in, oldest first, as the shown ones leave */
    nevents = 0;
    advance_to_expiry();
    assert(nstack == STACK_MAX && queued == QUEUE_MAX - STACK_MAX);
    assert(stack[0].id == STACK_MAX + 1 && stack[STACK_MAX - 1].id == 2 * STACK_MAX);
}

static void card_with_actions(uint32_t id, const char *const *actions, int n) {
    HudCard c = { .id = id, .summary = "mail", .body = "from bob", .icon_path = "", .level = LEVEL_INFO,
                  .timeout_ms = 1000, .actions = actions, .n_actions = n };
    hud_card(&c);
}

static uint32_t clicks[8];
static char click_tokens[8][32];
static int nclicks;
static void on_card_click(uint32_t id, const char *token) {
    snprintf(click_tokens[nclicks], sizeof click_tokens[0], "%s", token ? token : "(none)");
    clicks[nclicks++] = id;
}

static void test_click(void) {
    reset();
    hud_on_closed(on_closed);
    hud_on_click(on_card_click);
    nclicks = 0;

    card(1, "plain", "", LEVEL_INFO, 1000);
    card_with_actions(2, (const char *[]){ "default", "", "reply", "Reply" }, 4);
    hud_toast("note", LEVEL_INFO, 1000);
    assert(nstack == 3 && click_rects_set == 2 && nhit == 2);   /* only cards take clicks */
    assert(stack[0].has_default == false && stack[1].has_default == true);

    on_click(0, -5, -5, false);                       /* outside every card: nothing */
    assert(nclicks == 0 && nstack == 3);

    assert(hit_id[0] == 2);                        /* the card with `default` is on top */
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(nclicks == 1 && clicks[0] == 2);
    assert(nstack == 2 && nevents == 1 && events[0].id == 2 && events[0].reason == HUD_CLOSED_DISMISSED);

    on_click(0, hit[0].x + 1, hit[0].y + 1, false);   /* the plain card: dismissed, nothing to invoke */
    assert(nclicks == 1 && nstack == 1 && nevents == 2 && events[1].id == 1);
    assert(click_rects_set == 0);                  /* only the toast left: click-through */

    on_click(0, 1, 1, false);                         /* a stale click is harmless */
    assert(nstack == 1);
    hud_on_click(NULL);
}

static void resident_card(uint32_t id, const char *const *actions, int n) {
    HudCard c = { .id = id, .summary = "resident", .body = "", .icon_path = "", .level = LEVEL_INFO,
                  .timeout_ms = 1000, .actions = actions, .n_actions = n, .resident = true };
    hud_card(&c);
}

static void test_resident(void) {
    reset();
    hud_on_closed(on_closed);
    hud_on_click(on_card_click);
    nclicks = 0;

    hud_action_invoked(7);                         /* unknown ids and 0: harmless */
    hud_action_invoked(0);
    card(1, "plain", "", LEVEL_INFO, 1000);
    resident_card(2, (const char *[]){ "default", "", "reply", "Reply" }, 4);
    assert(nstack == 2 && nevents == 0);

    /* An invoked action closes an ordinary card, and leaves a resident one */
    hud_action_invoked(2);
    assert(nstack == 2 && nevents == 0);
    card(9, "temporary", "", LEVEL_INFO, 1000);
    hud_action_invoked(9);
    assert(nstack == 2 && nevents == 1 && events[0].id == 9 && events[0].reason == HUD_CLOSED_DISMISSED);
    nevents = 0;

    /* A click invokes `default` but the card stays; it is not dismissed by it */
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(nclicks == 1 && clicks[0] == 2);
    assert(nstack == 2 && stack[0].id == 2 && nevents == 0);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);   /* and it can be invoked again */
    assert(nclicks == 2 && nstack == 2 && nevents == 0);

    /* A right click removes it without invoking anything */
    on_click(0, hit[0].x + 1, hit[0].y + 1, true);
    assert(nclicks == 2 && nstack == 1 && nevents == 1);
    assert(events[0].id == 2 && events[0].reason == HUD_CLOSED_DISMISSED);

    /* A right click on an ordinary card dismisses it too, again without invoking */
    on_click(0, hit[0].x + 1, hit[0].y + 1, true);
    assert(nclicks == 2 && !nstack && nevents == 2 && events[1].id == 1);

    /* Resident with nothing to invoke: a left click is the only thing it can mean, so it dismisses */
    reset();
    hud_on_closed(on_closed);
    resident_card(3, (const char *[]){ "reply", "Reply" }, 2);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(!nstack && nevents == 1 && events[0].id == 3);

    /* A replacement can change whether it is resident */
    reset();
    resident_card(4, NULL, 0);
    hud_action_invoked(4);
    assert(nstack == 1);
    card(4, "again", "", LEVEL_INFO, 1000);
    hud_action_invoked(4);
    assert(!nstack);
    hud_on_click(NULL);
}

static void test_click_token(void) {
    reset();
    hud_on_closed(on_closed);
    hud_on_click(on_card_click);
    nclicks = 0;
    token_pending = false;

    /* Without the protocol the click is acted on at once, with no token */
    tokens_available = false;
    card_with_actions(1, (const char *[]){ "default", "" }, 2);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(nclicks == 1 && !strcmp(click_tokens[0], "(none)") && !nstack && !token_pending);
    nevents = 0;

    /* With it, the app is told when the compositor answers, with the token; until then nothing moves */
    tokens_available = true;
    token_requests = 0;
    card_with_actions(2, (const char *[]){ "default", "" }, 2);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(token_requests == 1 && nclicks == 1 && nstack == 1 && token_pending);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);           /* a second click meanwhile is ignored */
    assert(token_requests == 1 && nclicks == 1);
    token_fn("tok-123", token_ctx);
    assert(nclicks == 2 && clicks[1] == 2 && !strcmp(click_tokens[1], "tok-123"));
    assert(!nstack && !token_pending && nevents == 1 && events[0].reason == HUD_CLOSED_DISMISSED);

    /* A compositor that never answers must not make cards unclickable: after a while a click goes
     * ahead without a token */
    reset();
    hud_on_closed(on_closed);
    card_with_actions(7, (const char *[]){ "default", "" }, 2);
    tokens_available = true;
    int before = nclicks;
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(token_pending && nclicks == before && nstack == 1);
    fake_ms += TOKEN_WAIT_MS - 1;
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);           /* still waiting: ignored */
    assert(nstack == 1);
    fake_ms += 1;
    tokens_available = false;                              /* the display is stuck on the old request */
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(nstack == 0 && !token_pending && nclicks == before + 1 && !strcmp(click_tokens[before], "(none)"));
    tokens_available = true;
    token_pending = false;

    /* A resident card stays after the action, and can be clicked again */
    reset();
    hud_on_closed(on_closed);
    int base = nclicks;
    resident_card(3, (const char *[]){ "default", "" }, 2);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    token_fn("tok-a", token_ctx);
    assert(nstack == 1 && nevents == 0 && !token_pending && nclicks == base + 1);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    token_fn("tok-b", token_ctx);
    assert(nclicks == base + 2 && !strcmp(click_tokens[base + 1], "tok-b"));

    /* The card goes away before the compositor answers: nobody is told about a card that is gone */
    hud_remove(3);
    card_with_actions(4, (const char *[]){ "default", "" }, 2);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(token_pending);
    hud_remove(4);
    token_fn("late", token_ctx);
    assert(nclicks == base + 2 && !token_pending);

    /* A right click, or a card with nothing to invoke, never asks for a token */
    token_requests = 0;
    card_with_actions(5, (const char *[]){ "default", "" }, 2);
    on_click(0, hit[0].x + 1, hit[0].y + 1, true);
    card(6, "no default", "", LEVEL_INFO, 1000);
    on_click(0, hit[0].x + 1, hit[0].y + 1, false);
    assert(token_requests == 0 && !nstack);

    tokens_available = false;
    hud_on_click(NULL);
}

/* ---- icons from files ---- */

#define ICON_FILE_MAX (4 << 20)   /* the size limit of pngload.c, which is linked in */
static char icondir[64];

static void write_png(const char *name, int w, int h, double r, double g, double b) {
    char path[128];
    snprintf(path, sizeof path, "%s/%s", icondir, name);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, r, g, b);
    cairo_paint(cr);
    cairo_destroy(cr);
    assert(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);
}

static void ipath(char *out, size_t n, const char *name) { snprintf(out, n, "%s/%s", icondir, name); }

static uint32_t pixel(const uint8_t *img, int x, int y) { return ((const uint32_t *)img)[y * IMAGE_PX + x]; }

static void test_icon_files(void) {
    alarm(20);   /* a blocking read on a pipe must fail the test, not hang it */
    snprintf(icondir, sizeof icondir, "/tmp/pachud-icons-XXXXXX");
    assert(mkdtemp(icondir));
    static uint8_t img[IMAGE_BYTES];
    char p[160];

    /* A real PNG becomes the 64x64 raw icon, fitted and centred: a wide one leaves bands above and below */
    write_png("wide.png", 100, 50, 1, 0, 0);
    ipath(p, sizeof p, "wide.png");
    assert(load_icon_pixels(p, img));
    assert(pixel(img, 32, 32) == 0xFFFF0000u);
    assert(pixel(img, 32, 2) == 0 && pixel(img, 32, 61) == 0);
    char url[200];
    snprintf(url, sizeof url, "file://%s", p);
    assert(load_icon_pixels(url, img));                    /* the file:// spelling too */

    write_png("tiny.png", 8, 8, 0, 0, 1);                   /* small ones are scaled up to fill it */
    ipath(p, sizeof p, "tiny.png");
    assert(load_icon_pixels(p, img) && pixel(img, 2, 2) == 0xFF0000FFu && pixel(img, 61, 61) == 0xFF0000FFu);

    /* Not a path to a PNG */
    assert(!load_icon_pixels("relative.png", img) && !load_icon_pixels("", img));
    ipath(p, sizeof p, "missing.png");
    assert(!load_icon_pixels(p, img));
    char wrong[160];
    ipath(wrong, sizeof wrong, "wide.png");
    wrong[strlen(wrong) - 1] = 'x';                        /* ".pnx": a different suffix */
    assert(!load_icon_pixels(wrong, img));

    /* Not a regular file: none of these may block or be read */
    ipath(p, sizeof p, "pipe.png");
    assert(mkfifo(p, 0600) == 0);
    assert(!load_icon_pixels(p, img));
    ipath(p, sizeof p, "zero.png");
    assert(symlink("/dev/zero", p) == 0 && !load_icon_pixels(p, img));
    ipath(p, sizeof p, "dir.png");
    assert(mkdir(p, 0700) == 0 && !load_icon_pixels(p, img));

    /* Contents that are not a sane PNG */
    FILE *f;
    ipath(p, sizeof p, "text.png");
    f = fopen(p, "w");
    assert(f && fputs("this is not a PNG, whatever the name says", f) >= 0 && !fclose(f));
    assert(!load_icon_pixels(p, img));
    ipath(p, sizeof p, "empty.png");
    f = fopen(p, "w");
    assert(f && !fclose(f) && !load_icon_pixels(p, img));

    /* A header that claims a huge picture is refused before anything is decoded or allocated: this
     * file is 33 bytes and would otherwise mean about 3.6 GB */
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    unsigned char bomb[33] = { 0 };
    memcpy(bomb, sig, 8);
    bomb[11] = 13;
    memcpy(bomb + 12, "IHDR", 4);
    bomb[16] = bomb[17] = 0x00; bomb[18] = 0x75; bomb[19] = 0x30;   /* 30000 */
    bomb[20] = bomb[21] = 0x00; bomb[22] = 0x75; bomb[23] = 0x30;
    bomb[24] = 8; bomb[25] = 6;
    ipath(p, sizeof p, "bomb.png");
    f = fopen(p, "wb");
    assert(f && fwrite(bomb, 1, sizeof bomb, f) == sizeof bomb && !fclose(f));
    assert(!load_icon_pixels(p, img));
    bomb[18] = 0x08; bomb[19] = 0x01;                      /* 2049 wide: one over the limit */
    bomb[22] = 0x00; bomb[23] = 0x10;
    f = fopen(p, "wb");
    assert(f && fwrite(bomb, 1, sizeof bomb, f) == sizeof bomb && !fclose(f));
    assert(!load_icon_pixels(p, img));
    bomb[22] = 0x00; bomb[23] = 0x00;                      /* a zero height */
    f = fopen(p, "wb");
    assert(f && fwrite(bomb, 1, sizeof bomb, f) == sizeof bomb && !fclose(f));
    assert(!load_icon_pixels(p, img));

    /* A valid header with the rest missing, and a valid PNG that is too large a file */
    ipath(p, sizeof p, "wide.png");
    char cut[160];
    ipath(cut, sizeof cut, "cut.png");
    f = fopen(p, "rb");
    FILE *g = fopen(cut, "wb");
    assert(f && g);
    unsigned char buf[40];
    assert(fread(buf, 1, sizeof buf, f) == sizeof buf && fwrite(buf, 1, sizeof buf, g) == sizeof buf);
    fclose(f);
    fclose(g);
    assert(!load_icon_pixels(cut, img));
    ipath(p, sizeof p, "wide.png");
    ipath(cut, sizeof cut, "big.png");
    f = fopen(p, "rb");
    g = fopen(cut, "wb");
    assert(f && g);
    size_t got;
    while ((got = fread(buf, 1, sizeof buf, f)) > 0) assert(fwrite(buf, 1, got, g) == got);
    fclose(f);
    fclose(g);
    assert(load_icon_pixels(cut, img));                    /* as it is, it loads */
    assert(truncate(cut, ICON_FILE_MAX + 1) == 0);         /* padded past the size limit */
    assert(!load_icon_pixels(cut, img));

    /* A card picks its icon up when it arrives, once: the stored item carries the pixels */
    reset();
    ipath(p, sizeof p, "wide.png");
    HudCard c = { .id = 1, .summary = "with icon file", .body = "", .icon_path = p, .level = LEVEL_INFO, .timeout_ms = 1000 };
    hud_card(&c);
    assert(stack[0].has_image && pixel(stack[0].image, 32, 32) == 0xFFFF0000u);
    ipath(p, sizeof p, "pipe.png");
    HudCard bad = { .id = 2, .summary = "bad icon file", .body = "", .icon_path = p, .level = LEVEL_INFO, .timeout_ms = 1000 };
    hud_card(&bad);                                         /* still shown, just without the icon */
    assert(nstack == 2 && !stack[0].has_image);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", icondir);
    assert(system(cmd) == 0);
    alarm(0);
}

static int card_height(int index) {
    int w, h;
    paint_item(NULL, &stack[index], &w, &h);
    return h;
}

static void long_card(uint32_t id) {
    char body[PAINT_BODY_MAX] = "";
    for (int i = 0; i < 60; i++) strcat(body, "abcdefgh ");   /* far more than four lines at 34 bytes each */
    card(id, "a long one", body, LEVEL_INFO, 1000);
}

static void test_expand(void) {
    reset();
    hud_on_closed(on_closed);
    assert(hud_toggle_expand() == -1);                      /* nothing on screen */
    hud_toast("only a toast", LEVEL_WARN, 1000);
    assert(hud_toggle_expand() == -1);                      /* toasts have nothing more to show */

    reset();
    long_card(1);
    int collapsed = card_height(0);
    assert(collapsed == 16 + 4 + 4 * 16 + 2 * PAD);          /* the summary and four lines of body */

    /* From the keyboard: open, and then it does not expire however long it takes to read */
    assert(hud_toggle_expand() == 1 && stack[0].expanded);
    int open = card_height(0);
    assert(open > collapsed && open <= 16 + 4 + 20 * 16 + 2 * PAD);   /* more lines, but not unbounded */
    assert(hud_timeout_ms() == -1);
    fake_ms += 60000;
    hud_tick();
    assert(nstack == 1 && nevents == 0);

    /* Closed again: it is not to vanish at that moment, but it goes soon */
    assert(hud_toggle_expand() == 0 && card_height(0) == collapsed);
    assert(nstack == 1 && hud_timeout_ms() == HOLD_GRACE_MS);
    fake_ms += HOLD_GRACE_MS;
    hud_tick();
    assert(!nstack && nevents == 1 && events[0].reason == HUD_CLOSED_EXPIRED);

    /* An app updating the card (a replacement) leaves it as open as the reader made it */
    reset();
    long_card(2);
    assert(hud_toggle_expand() == 1);
    long_card(2);
    assert(nstack == 1 && stack[0].expanded && card_height(0) > collapsed);

    /* The newest card is the one it applies to */
    reset();
    long_card(3);
    long_card(4);
    assert(hud_toggle_expand() == 1 && stack[0].id == 4 && stack[0].expanded && !stack[1].expanded);
}

static void test_hover(void) {
    reset();
    hud_on_closed(on_closed);
    long_card(1);
    int collapsed = card_height(0);
    ptr_over = false;

    /* The pointer arrives: the card is held at once, and opens after a moment, not before */
    on_hover(0, hit[0].x + 5, hit[0].y + 5, true);
    assert(hover_id == 1 && hover_open == 0 && card_height(0) == collapsed);
    assert(hud_timeout_ms() == HOVER_DELAY_MS);              /* the loop wakes for it, and for nothing else */
    fake_ms += HOVER_DELAY_MS - 1;
    on_hover(0, hit[0].x + 9, hit[0].y + 9, true);              /* moving about on it does not start over */
    hud_tick();
    assert(hover_open == 0);
    fake_ms += 1;
    hud_tick();
    assert(hover_open == 1 && card_height(0) > collapsed);

    /* held: it does not expire while the pointer is there */
    assert(hud_timeout_ms() == -1);
    fake_ms += 600000;
    hud_tick();
    assert(nstack == 1 && nevents == 0);

    /* The pointer leaves: it closes, and has a little time left */
    on_hover(0, 0, 0, false);
    assert(hover_id == 0 && hover_open == 0 && card_height(0) == collapsed);
    assert(hud_timeout_ms() == HOLD_GRACE_MS);
    fake_ms += HOLD_GRACE_MS;
    hud_tick();
    assert(!nstack);

    /* Passing over without lingering never opens it */
    reset();
    long_card(2);
    on_hover(0, hit[0].x + 5, hit[0].y + 5, true);
    fake_ms += HOVER_DELAY_MS - 50;
    on_hover(0, 0, 0, false);
    fake_ms += 5 * HOVER_DELAY_MS;
    hud_tick();
    assert(hover_open == 0 && nstack == 1);

    /* Toasts and sliders take no part: the pointer is not even over them as far as the HUD knows */
    reset();
    long_card(3);
    hud_toast("a toast", LEVEL_WARN, 1000);                  /* newest, on top */
    assert(nhit == 1);
    on_hover(0, hit[0].x + 5, hit[0].y + 5, true);
    assert(hover_id == 3);
    on_hover(0, 5, 5, true);                                    /* over the toast, which has no rectangle */
    assert(hover_id == 0);

    /* Between cards: leaving one for the next closes the first and starts the delay for the second */
    reset();
    long_card(4);
    long_card(5);                                            /* on top */
    on_hover(0, hit[0].x + 5, hit[0].y + 5, true);
    assert(hover_id == 5);
    fake_ms += HOVER_DELAY_MS;
    hud_tick();
    assert(hover_open == 5);
    on_hover(0, hit[1].x + 5, hit[1].y + 5, true);             /* onto the card below, where it was */
    assert(hover_open == 0 && card_height(0) == card_height(1));   /* the first one closed... */
    assert(hover_id == 0);                                   /* ...and the lower one slid up from under the pointer */
    on_hover(0, hit[1].x + 5, hit[1].y + 5, true);             /* one more movement, where it is now */
    assert(hover_id == 4 && hover_open == 0);

    /* The layout moves under a pointer that does not: whatever is there now is what it is over */
    reset();
    long_card(6);
    long_card(7);
    on_hover(0, hit[0].x + 5, hit[0].y + 5, true);              /* over 7, the top one */
    assert(hover_id == 7);
    hud_dismiss_id(7);                                       /* 6 slides up to where the pointer is */
    assert(nstack == 1 && hover_id == 6 && hover_open == 0);

    /* A card that disappears takes the hover with it */
    reset();
    long_card(8);
    on_hover(0, hit[0].x + 5, hit[0].y + 5, true);
    assert(hover_id == 8);
    hud_remove(8);
    assert(hover_id == 0 && hover_open == 0);
    ptr_over = false;
}

static void test_actions(void) {
    uint32_t id = 0;
    reset();
    hud_on_closed(on_closed);

    /* Nothing on screen, or a card without actions: no action to invoke */
    assert(hud_action_key(1, &id) == NULL);
    card(1, "plain", "", LEVEL_INFO, 1000);
    assert(stack[0].nact == 0 && hud_action_key(1, &id) == NULL);

    /* Actions are offered by number, in the order the app sent them */
    reset();
    card_with_actions(2, (const char *[]){ "reply", "Reply", "archive", "Archive" }, 4);
    assert(stack[0].nact == 2);
    assert(!strcmp(hud_action_key(1, &id), "reply") && id == 2);
    assert(!strcmp(hud_action_key(2, &id), "archive"));
    assert(hud_action_key(0, &id) == NULL && hud_action_key(3, &id) == NULL && hud_action_key(-1, &id) == NULL);
    assert(!strcmp(stack[0].act_label[1], "Archive"));

    /* "default" is how apps say "the user clicked the notification": not shown as a button when
     * there are real ones... */
    reset();
    card_with_actions(3, (const char *[]){ "default", "", "open", "Open it", "mute", "Mute" }, 6);
    assert(stack[0].nact == 2 && !strcmp(hud_action_key(1, &id), "open") && !strcmp(hud_action_key(2, &id), "mute"));

    /* ...but it is offered when it is all the card has (an empty label gets a generic one) */
    reset();
    card_with_actions(4, (const char *[]){ "default", "" }, 2);
    assert(stack[0].nact == 1 && !strcmp(hud_action_key(1, &id), "default") && !strcmp(stack[0].act_label[0], "Open"));
    reset();
    card_with_actions(5, (const char *[]){ "default", "Go" }, 2);
    assert(!strcmp(stack[0].act_label[0], "Go"));

    /* No more than three are shown; an odd trailing string is ignored; labels stay on one line */
    reset();
    card_with_actions(6, (const char *[]){ "a", "A", "b", "B", "c", "C", "d", "D", "dangling" }, 9);
    assert(stack[0].nact == HUD_MAX_ACTIONS && hud_action_key(4, &id) == NULL);
    reset();
    card_with_actions(7, (const char *[]){ "k", "two\nlines\tand tab" }, 2);
    assert(!strcmp(stack[0].act_label[0], "two lines and tab"));

    /* With several cards on screen the newest one's actions are the ones offered */
    reset();
    card_with_actions(8, (const char *[]){ "first", "First" }, 2);
    card_with_actions(9, (const char *[]){ "second", "Second" }, 2);
    assert(!strcmp(hud_action_key(1, &id), "second") && id == 9);
    hud_dismiss();
    assert(!strcmp(hud_action_key(1, &id), "first") && id == 8);

    /* Raw icon pixels are kept only for the card that has them, and survive the stack shifting */
    reset();
    static uint8_t pixels[IMAGE_BYTES];
    memset(pixels, 0x7F, sizeof pixels);
    HudCard with = { .id = 10, .summary = "pic", .body = "", .icon_path = "", .image = pixels,
                     .level = LEVEL_INFO, .timeout_ms = 1000 };
    hud_card(&with);
    assert(stack[0].has_image && stack[0].image[0] == 0x7F && stack[0].image[IMAGE_BYTES - 1] == 0x7F);
    card(11, "no pic", "", LEVEL_INFO, 1000);
    assert(nstack == 2 && !stack[0].has_image && stack[1].has_image && stack[1].image[IMAGE_BYTES - 1] == 0x7F);
    hud_toast("ephemeral", LEVEL_INFO, 100);
    assert(nstack == 3 && stack[2].has_image && stack[2].image[100] == 0x7F);
}

/* ---- card text helpers ---- */

static void test_strip_markup(void) {
    char out[128];
    strip_markup("<b>bold</b> &amp; &lt;x&gt;<br>next", out, sizeof out);
    assert(!strcmp(out, "bold & <x>\nnext"));
    strip_markup("a < b and c > d", out, sizeof out);       /* not a tag */
    assert(!strcmp(out, "a < b and c > d"));
    strip_markup("5 &unknown; ok", out, sizeof out);        /* unknown entities stay */
    assert(!strcmp(out, "5 &unknown; ok"));
    strip_markup("<a href=\"x\">link</a> &quot;q&quot; &apos;s&apos;", out, sizeof out);
    assert(!strcmp(out, "link \"q\" 's'"));
    strip_markup("<broken", out, sizeof out);               /* unterminated: kept as text */
    assert(!strcmp(out, "<broken"));
    strip_markup("long long long long", out, 8);            /* never overruns the buffer */
    assert(strlen(out) == 7);
}

static void check_lines(const char *text, double width, int maxlines, int n_expected,
                        const char *const *expected, bool trunc_expected) {
    Line lines[8];
    bool truncated;
    int n = wrap(text, width, 13, lines, maxlines, &truncated);
    assert(n == n_expected);
    for (int i = 0; i < n; i++) {
        assert((int)strlen(expected[i]) == lines[i].len);
        assert(!strncmp(expected[i], lines[i].p, lines[i].len));
    }
    assert(truncated == trunc_expected);
}

static void test_wrap(void) {
    /* 100 units = 10 characters per line */
    check_lines("hello world foo bar", 100, 4, 3, (const char *[]){ "hello", "world foo", "bar" }, false);
    check_lines("ab\ncd", 100, 4, 2, (const char *[]){ "ab", "cd" }, false);
    check_lines("", 100, 4, 0, NULL, false);
    check_lines("short", 100, 4, 1, (const char *[]){ "short" }, false);

    /* A word wider than a line is broken inside; the last line leaves 3 chars for the ellipsis */
    check_lines("abcdefghijklmnopqrstuvwxyz", 100, 2, 2, (const char *[]){ "abcdefghij", "klmnopq" }, true);
    check_lines("abcdefghijklmnopqrstuvwxyz", 100, 3, 3,
                (const char *[]){ "abcdefghij", "klmnopqrst", "uvwxyz" }, false);

    /* Too much text for the lines allowed */
    check_lines("one two three four five six seven", 100, 2, 2,
                (const char *[]){ "one two", "three" }, true);

    /* Leading spaces after a break are dropped */
    check_lines("aaaaaaaaaa    bbb", 100, 4, 2, (const char *[]){ "aaaaaaaaaa", "bbb" }, false);
}

static void test_ellipsize(void) {
    char out[64];
    ellipsize("hello world", 60, 13, false, out, sizeof out);   /* "…" is 3 bytes = 30 units */
    assert(!strcmp(out, "hel…"));
    ellipsize("fits", 100, 13, false, out, sizeof out);
    assert(!strcmp(out, "fits"));
    ellipsize("anything", 10, 13, false, out, sizeof out);      /* not even the ellipsis fits */
    assert(!strcmp(out, ""));
    ellipsize("héllo wörld", 80, 13, false, out, sizeof out);   /* never cuts a UTF-8 sequence */
    for (const unsigned char *p = (const unsigned char *)out; *p; p++)
        assert(!((*p & 0xC0) == 0x80 && (p == (const unsigned char *)out || p[-1] < 0x80)));
}

/* Everything in the stack has one width; a toast wraps instead of growing. The stubs make every byte
 * 10 units wide and every line 16 high, so 343 units of text room hold 34 bytes per line. */
static void test_item_widths(void) {
    int w, h;
    paint_toast(NULL, "short", LEVEL_INFO, NULL, &w, &h);
    assert(w == PAINT_ITEM_W && h == 16 + 2 * PAD);
    paint_toast(NULL, "", LEVEL_INFO, NULL, &w, &h);              /* nothing to say still has a normal size */
    assert(w == PAINT_ITEM_W && h == 16 + 2 * PAD);

    char text[512];
    memset(text, 'a', 34);
    text[34] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);            /* exactly one line */
    assert(w == PAINT_ITEM_W && h == 16 + 2 * PAD);
    memset(text, 'a', 35);
    text[35] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);            /* one more: a second line, not a wider toast */
    assert(w == PAINT_ITEM_W && h == 2 * 16 + 2 * PAD);

    paint_toast(NULL, "two\nlines\tand tab", LEVEL_INFO, NULL, &w, &h);   /* newlines are not line breaks */
    assert(w == PAINT_ITEM_W && h == 16 + 2 * PAD);

    /* words wrap at spaces, and no toast is ever more than three lines */
    paint_toast(NULL, "aaaaaaaaaaaaaaaaaaaa bbbbbbbbbbbbbbbbbbbb cccccccccccccccccccc", LEVEL_INFO, NULL, &w, &h);
    assert(h == 3 * 16 + 2 * PAD);
    memset(text, 'x', 400);
    text[400] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);
    assert(w == PAINT_ITEM_W && h == 3 * 16 + 2 * PAD);
    char wide[512] = "";                                      /* multi-byte text wraps without cutting a character */
    for (int i = 0; i < 200; i++) strcat(wide, "é");
    paint_toast(NULL, wide, LEVEL_INFO, NULL, &w, &h);
    assert(w == PAINT_ITEM_W && h == 3 * 16 + 2 * PAD);

    /* a strip of the deck: the width it is given, and a fixed height */
    paint_strip(NULL, "a strip", LEVEL_CRIT, 300, &w, &h);
    assert(w == 300 && h == PAINT_STRIP_H);

    /* sliders: the stack's width, or the on-screen display's own */
    paint_slider(NULL, "Brightness", 50, false, NULL, PAINT_ITEM_W, &w, &h);
    assert(w == PAINT_ITEM_W);
    paint_slider(NULL, "Volume", 50, false, NULL, PAINT_OSD_W, &w, &h);
    assert(w == PAINT_OSD_W);

    /* a toast, a slider and a card side by side: one width */
    reset();
    hud_toast("a fairly short one", LEVEL_WARN, 1000);
    hud_toast("and a much longer one, that would have made the others look narrow", LEVEL_INFO, 1000);
    hud_slider("Brightness", 40, false, NULL);
    card(1, "a card", "with a body", LEVEL_INFO, 1000);
    assert(nstack == 4);
    for (int i = 0; i < nstack; i++) {
        paint_item(NULL, &stack[i], &w, &h);
        assert(w == PAINT_ITEM_W);
    }
}

/* ---- the attention deck ---- */

static void attention_card(uint32_t id, const char *summary, const char *const *actions, int n) {
    HudCard c = { .id = id, .summary = summary, .body = "", .icon_path = "", .level = LEVEL_CRIT,
                  .timeout_ms = 1000, .actions = actions, .n_actions = n, .attention = true };
    hud_card(&c);
}

static void test_deck_lanes(void) {
    reset();
    hud_on_closed(on_closed);
    /* Warnings and information stay in the stack, however urgent they look */
    hud_toast("amber", LEVEL_WARN, 1000);
    hud_toast("blue", LEVEL_INFO, 1000);
    card(1, "an ordinary card", "", LEVEL_INFO, 1000);
    assert(ndeck == 0 && NSTACK == 3);

    /* A critical toast, which is what the system's own critical alerts are, goes to the deck */
    hud_toast("red", LEVEL_CRIT, 1000);
    assert(ndeck == 1 && NSTACK == 3 && nstack == 4);
    assert(!strcmp(stack[DECK_FRONT].text, "red") && stack[DECK_FRONT].attention);
    assert(!strcmp(stack[0].text, "an ordinary card"));            /* the stack is as it was, newest first */

    /* So does a card that says it must not be ignored; the card counts as a notification */
    attention_card(2, "incoming call", NULL, 0);
    assert(ndeck == 2 && hud_card_count() == 2);
    assert(stack[DECK_FRONT].id == 0 && stack[DECK_FRONT + 1].id == 2);   /* in the order they arrived */

    /* The same text again refreshes where it is */
    hud_toast("red", LEVEL_CRIT, 1000);
    assert(ndeck == 2 && nstack == 5);
}

static void test_deck_order_and_overflow(void) {
    reset();
    hud_on_closed(on_closed);
    for (uint32_t id = 1; id <= DECK_MAX; id++) attention_card(id, "call", NULL, 0);
    assert(ndeck == DECK_MAX && queued == 0);
    attention_card(10, "one too many", NULL, 0);
    assert(ndeck == DECK_MAX && queued == 1);                       /* waits: nothing in the deck makes way */
    attention_card(11, "and another", NULL, 0);
    assert(ndeck == DECK_MAX && queued == 2);

    /* The stack is another matter: ordinary things come and go without touching the deck */
    for (int i = 0; i < 2 * STACK_MAX; i++) {
        char t[16];
        snprintf(t, sizeof t, "n%d", i);
        hud_toast(t, LEVEL_INFO, 1000);
    }
    assert(ndeck == DECK_MAX && NSTACK == STACK_MAX && stack[DECK_FRONT].id == 1);

    /* Handling the front one brings the next forward, and the one that waited joins at the back */
    hud_dismiss();
    assert(stack[DECK_FRONT].id == 2 && stack[DECK_FRONT + 1].id == 3 && stack[DECK_FRONT + 2].id == 10);
    assert(ndeck == DECK_MAX && queued == 1 && nevents == 1 && events[0].id == 1);
    assert(events[0].reason == HUD_CLOSED_DISMISSED);
    hud_dismiss();
    assert(stack[DECK_FRONT].id == 3 && stack[DECK_FRONT + 2].id == 11 && queued == 0);

    /* An attention toast of the same text waiting is not queued twice */
    reset();
    for (int i = 0; i < DECK_MAX; i++) {
        char t[16];
        snprintf(t, sizeof t, "red%d", i);
        hud_toast(t, LEVEL_CRIT, 1000);
    }
    hud_toast("waiting", LEVEL_CRIT, 1000);
    hud_toast("waiting", LEVEL_CRIT, 1000);
    assert(queued == 1);
}

static void test_deck_targets(void) {
    uint32_t id = 0;
    reset();
    hud_on_closed(on_closed);
    card_with_actions(1, (const char *[]){ "reply", "Reply" }, 2);
    assert(!strcmp(hud_action_key(1, &id), "reply") && id == 1);   /* only the stack has a card */

    /* The deck's front card is what actions apply to, though the stack has a newer card */
    const char *call[] = { "answer", "Answer", "decline", "Decline" };
    attention_card(2, "incoming call", call, 4);
    card_with_actions(3, (const char *[]){ "open", "Open" }, 2);
    assert(!strcmp(hud_action_key(1, &id), "answer") && id == 2);
    assert(!strcmp(hud_action_key(2, &id), "decline"));
    assert(hud_toggle_expand() == 1 && stack[DECK_FRONT].expanded && !stack[0].expanded);
    assert(hud_toggle_expand() == 0);

    /* A toast in front has no actions: they fall to the newest card of the stack */
    reset();
    card_with_actions(4, (const char *[]){ "open", "Open" }, 2);
    hud_toast("red", LEVEL_CRIT, 1000);
    assert(!strcmp(hud_action_key(1, &id), "open") && id == 4);
    assert(hud_toggle_expand() == 1 && stack[0].expanded);

    /* Dismissal goes to the deck first, whatever the stack holds */
    reset();
    hud_on_closed(on_closed);
    card(5, "stack card", "", LEVEL_INFO, 1000);
    attention_card(6, "deck card", NULL, 0);
    hud_dismiss();
    assert(nevents == 1 && events[0].id == 6 && ndeck == 0 && NSTACK == 1);
    hud_dismiss();
    assert(nevents == 2 && events[1].id == 5);
}

static void test_deck_clicks(void) {
    reset();
    hud_on_closed(on_closed);
    hud_on_click(on_card_click);
    nclicks = 0;
    attention_card(1, "first", (const char *[]){ "default", "" }, 2);
    attention_card(2, "second", NULL, 0);
    attention_card(3, "third", NULL, 0);
    assert(ndeck == 3 && ndhit == 3);

    /* The front card takes clicks as a card; the others only show their strips, narrower each time */
    assert(!dhit_strip[0] && dhit_id[0] == 1 && dhit[0].w == PAINT_ITEM_W);
    assert(dhit_strip[1] && dhit_id[1] == 2 && dhit_strip[2] && dhit_id[2] == 3);
    assert(dhit[1].w == PAINT_ITEM_W - 2 * PAINT_STRIP_INSET && dhit[2].w == PAINT_ITEM_W - 4 * PAINT_STRIP_INSET);
    assert(dhit[1].y == dhit[0].h);                                 /* what shows starts below the front card */
    assert(dhit[1].h == PAINT_STRIP_H - PAINT_STRIP_TUCK);
    assert(dhit[2].y == dhit[1].y + dhit[1].h);

    /* A left click on a strip brings that card to the front: nothing is invoked or closed */
    on_click(DISPLAY_AREA_DECK, dhit[2].x + 3, dhit[2].y + 3, false);
    assert(stack[DECK_FRONT].id == 3 && stack[DECK_FRONT + 1].id == 1 && stack[DECK_FRONT + 2].id == 2);
    assert(nclicks == 0 && nevents == 0 && ndeck == 3);
    assert(dhit_id[0] == 3 && dhit_id[1] == 1 && dhit_id[2] == 2);   /* the tables follow */

    /* A right click on a strip gets rid of that card without touching the front */
    on_click(DISPLAY_AREA_DECK, dhit[1].x + 3, dhit[1].y + 3, true);
    assert(nevents == 1 && events[0].id == 1 && events[0].reason == HUD_CLOSED_DISMISSED);
    assert(ndeck == 2 && stack[DECK_FRONT].id == 3 && stack[DECK_FRONT + 1].id == 2);

    /* The front card is clicked like any other: dismissed, and the next one comes forward */
    on_click(DISPLAY_AREA_DECK, dhit[0].x + 3, dhit[0].y + 3, false);
    assert(nevents == 2 && events[1].id == 3 && ndeck == 1 && stack[DECK_FRONT].id == 2);
    on_click(DISPLAY_AREA_DECK, 5000, 5000, false);                 /* nowhere: nothing */
    assert(ndeck == 1);

    /* Not the stack's table: the same point in the stack's area is a different place */
    card(9, "stack card", "", LEVEL_INFO, 1000);
    on_click(DISPLAY_AREA_STACK, dhit[0].x + 3, dhit[0].y + 3, false);   /* a stack card is at that point too */
    assert(nevents == 3 && events[2].id == 9);
    hud_on_click(NULL);
}

static void test_deck_time(void) {
    reset();
    hud_on_closed(on_closed);
    attention_card(1, "first", NULL, 0);
    fake_ms += 400;
    attention_card(2, "second", NULL, 0);
    assert(hud_timeout_ms() == 600);                                /* the deck's own timeouts count */
    fake_ms += 600;
    hud_tick();                                                     /* the front one is due: the next comes forward */
    assert(ndeck == 1 && stack[DECK_FRONT].id == 2 && nevents == 1 && events[0].reason == HUD_CLOSED_EXPIRED);

    /* A card that was waiting starts its time when it gets in, not before */
    reset();
    for (uint32_t id = 1; id <= DECK_MAX; id++) attention_card(id, "x", NULL, 0);
    attention_card(20, "waits", NULL, 0);
    fake_ms += 900;
    hud_dismiss();
    assert(ndeck == DECK_MAX && hud_timeout_ms() <= 100);           /* the others are nearly due... */
    long long entered = 0;
    for (int i = 0; i < nstack; i++)
        if (stack[i].id == 20) entered = stack[i].deadline;
    assert(entered == fake_ms + 1000);                              /* ...this one has all of its time */
}

static void test_deck_hover(void) {
    reset();
    hud_on_closed(on_closed);
    char body[PAINT_BODY_MAX] = "";
    for (int i = 0; i < 60; i++) strcat(body, "abcdefgh ");
    HudCard c = { .id = 1, .summary = "a long call", .body = body, .icon_path = "", .level = LEVEL_CRIT,
                  .timeout_ms = 1000, .attention = true };
    hud_card(&c);
    attention_card(2, "behind", NULL, 0);
    int strip_y = dhit[1].y;

    /* The pointer rests on the front card of the deck: it is held, and opens like any card */
    on_hover(DISPLAY_AREA_DECK, dhit[0].x + 5, dhit[0].y + 5, true);
    assert(hover_id == 1 && ptr_area == DISPLAY_AREA_DECK);
    fake_ms += HOVER_DELAY_MS;
    hud_tick();
    assert(hover_open == 1);
    assert(dhit[1].y > strip_y);                                    /* the strips move down with it */

    /* A strip is not hoverable: nothing opens behind the front card */
    on_hover(DISPLAY_AREA_DECK, dhit[1].x + 3, dhit[1].y + 3, true);
    assert(hover_id == 0 && hover_open == 0);
    on_hover(DISPLAY_AREA_DECK, 0, 0, false);
}

static void test_deck_moves(void) {
    /* A replacement that becomes urgent moves from the stack to the deck, and back */
    reset();
    hud_on_closed(on_closed);
    card(5, "a download", "", LEVEL_INFO, 1000);
    assert(NSTACK == 1 && ndeck == 0);
    attention_card(5, "the download failed", NULL, 0);
    assert(NSTACK == 0 && ndeck == 1 && nevents == 0);              /* moved, not closed */
    card(5, "retrying", "", LEVEL_INFO, 1000);
    assert(NSTACK == 1 && ndeck == 0 && nevents == 0);

    /* A card moved to the deck while waiting there takes its place in the queue */
    reset();
    for (uint32_t id = 1; id <= DECK_MAX; id++) attention_card(id, "x", NULL, 0);
    attention_card(8, "waits", NULL, 0);
    attention_card(8, "waits, updated", NULL, 0);
    assert(queued == 1 && !strcmp(queue[0].text, "waits, updated"));

    /* Removal by id works across both */
    reset();
    card(1, "stack", "", LEVEL_INFO, 1000);
    attention_card(2, "deck", NULL, 0);
    assert(hud_remove(2) && ndeck == 0 && NSTACK == 1);
    assert(hud_remove(1) && !nstack);

    /* With the stack at the top centre there is no place apart: everything goes in the stack */
    reset();
    deck_is_apart = false;
    hud_toast("red", LEVEL_CRIT, 1000);
    attention_card(3, "call", NULL, 0);
    assert(ndeck == 0 && NSTACK == 2);
    deck_is_apart = true;
}

/* ---- sizes: PACHUD_FONT_SIZE and PACHUD_WIDTH ---- */

static void test_sizes(void) {
    int w, h;
    paint_configure(0, 0);
    assert(paint_item_w() == 380 && paint_osd_w() == 300 && paint_px(16) == 16 && paint_font(16) == 16.0);

    /* A 20 px font is a factor of 1.25 on everything: lengths, fonts, the width of the items */
    paint_configure(20, 0);
    assert(paint_px(16) == 20 && paint_px(40) == 50 && paint_font(13) == 16.25);
    assert(paint_item_w() == 475 && paint_osd_w() == 375);
    paint_toast(NULL, "short", LEVEL_INFO, NULL, &w, &h);
    assert(w == 475 && h == 16 + 2 * 20);                        /* the padding grew (the stub's line does not) */
    char text[200];
    memset(text, 'a', 42);                                       /* 475 - 6 - 40 = 429: 42 bytes a line */
    text[42] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);
    assert(h == 16 + 2 * 20);
    text[42] = 'a';
    text[43] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);
    assert(h == 2 * 16 + 2 * 20);
    paint_slider(NULL, "Brightness", 50, false, NULL, paint_item_w(), &w, &h);
    assert(w == 475 && h == 80);
    paint_slider(NULL, "Volume", 50, false, NULL, PAINT_OSD_W, &w, &h);
    assert(w == 375 && h == 80);
    paint_strip(NULL, "a strip", LEVEL_CRIT, 300, &w, &h);
    assert(h == PAINT_STRIP_H && h == 40 && PAINT_STRIP_TUCK == 13 && PAINT_STRIP_INSET == 18);

    /* The width alone changes the width, not the sizes of things */
    paint_configure(0, 600);
    assert(paint_item_w() == 600 && paint_px(16) == 16 && paint_osd_w() == 300);
    paint_toast(NULL, "short", LEVEL_INFO, NULL, &w, &h);
    assert(w == 600 && h == 16 + 2 * 16);
    memset(text, 'a', 56);                                       /* 600 - 5 - 32 = 563: 56 bytes a line */
    text[56] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);
    assert(h == 16 + 2 * 16);

    /* Both: the width is the user's, the rest follows the font */
    paint_configure(32, 700);
    assert(paint_item_w() == 700 && paint_px(16) == 32 && paint_osd_w() == 600);

    /* Out of range values are brought in, not trusted */
    paint_configure(5, 50);
    assert(paint_px(16) == 10 && paint_item_w() == PAINT_WIDTH_MIN);
    paint_configure(500, 5000);
    assert(paint_px(16) == 48 && paint_item_w() == PAINT_WIDTH_MAX);
    paint_configure(-3, -3);                                     /* nonsense: the defaults */
    assert(paint_px(16) == 16 && paint_item_w() == 380);

    /* A lengths never rounds to nothing, however small the font */
    paint_configure(10, 0);
    assert(paint_px(1) == 1 && paint_px(5) == 3);

    /* A stack of different things keeps one width at any size, and the deck's strips follow it */
    paint_configure(24, 0);
    reset();
    hud_toast("a toast", LEVEL_WARN, 1000);
    hud_slider("Brightness", 40, false, NULL);
    card(1, "a card", "with a body", LEVEL_INFO, 1000);
    for (int i = 0; i < nstack; i++) {
        paint_item(NULL, &stack[i], &w, &h);
        assert(w == paint_item_w() && w == 570);
    }
    attention_card(2, "first", NULL, 0);
    attention_card(3, "second", NULL, 0);
    assert(dhit[0].w == 570 && dhit[1].w == 570 - 2 * PAINT_STRIP_INSET && PAINT_STRIP_INSET == 21);
    assert(dhit[1].h == PAINT_STRIP_H - PAINT_STRIP_TUCK);
    reset();
    paint_configure(0, 0);
}

/* The environment: PACHUD_FONT_SIZE and PACHUD_WIDTH, whole numbers of pixels */
static void test_size_environment(void) {
    unsetenv("PACHUD_FONT_SIZE");
    unsetenv("PACHUD_WIDTH");
    paint_init();
    assert(paint_px(16) == 16 && paint_item_w() == 380);

    setenv("PACHUD_FONT_SIZE", "24", 1);
    setenv("PACHUD_WIDTH", "500", 1);
    paint_init();
    assert(paint_px(16) == 24 && paint_item_w() == 500);

    setenv("PACHUD_FONT_SIZE", "99", 1);                         /* too large: the largest */
    setenv("PACHUD_WIDTH", "10", 1);                             /* too small: the smallest */
    paint_init();
    assert(paint_px(16) == 48 && paint_item_w() == PAINT_WIDTH_MIN);

    /* Not whole numbers of pixels: said so, and the default stays */
    const char *bad[] = { "abc", "20px", "1.5", "20 ", "" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        setenv("PACHUD_FONT_SIZE", bad[i], 1);
        setenv("PACHUD_WIDTH", bad[i], 1);
        paint_init();
        assert(paint_px(16) == 16 && paint_item_w() == 380);
    }

    setenv("PACHUD_FONT_SIZE", "0", 1);                          /* zero and negatives are out of range: the smallest */
    setenv("PACHUD_WIDTH", "-4", 1);
    paint_init();
    assert(paint_px(16) == PAINT_FONT_MIN && paint_item_w() == PAINT_WIDTH_MIN);

    setenv("PACHUD_FONT_SIZE", " 20", 1);                       /* a stray space in front is forgiven */
    unsetenv("PACHUD_WIDTH");
    paint_init();
    assert(paint_px(16) == 20 && paint_item_w() == 475);

    unsetenv("PACHUD_FONT_SIZE");
    unsetenv("PACHUD_WIDTH");
    paint_init();
    assert(paint_px(16) == 16 && paint_item_w() == 380);
}

/* ---- status icons ---- */

static void test_status_icons(void) {
    /* A theme of our own, so what is on this machine does not matter */
    char root[64], path[256], cmd[320];
    snprintf(root, sizeof root, "/tmp/pachud-hudicons-XXXXXX");
    assert(mkdtemp(root));
    snprintf(path, sizeof path, "%s/data", root);
    setenv("XDG_DATA_DIRS", path, 1);
    snprintf(path, sizeof path, "%s/home", root);
    setenv("XDG_DATA_HOME", path, 1);
    setenv("XDG_CONFIG_HOME", root, 1);
    snprintf(path, sizeof path, "%s/data/icons/hicolor/22x22/status", root);
    snprintf(cmd, sizeof cmd, "mkdir -p %s", path);
    assert(system(cmd) == 0);
    snprintf(path, sizeof path, "%s/data/icons/hicolor/22x22/status/test-icon.png", root);
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 16, 16);
    cairo_t *cr = cairo_create(s);
    cairo_set_source_rgb(cr, 1, 0, 0);
    cairo_paint(cr);
    cairo_destroy(cr);
    assert(cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(s);

    int w, h;
    paint_configure(0, 0);
    paint_configure_icons(true, 0);                         /* on, at the default size: 24 */
    assert(sicon_size() == 24);

    /* Without an icon, or with one the theme does not have, a toast is what it always was */
    paint_toast(NULL, "short", LEVEL_INFO, NULL, &w, &h);
    assert(w == 380 && h == 16 + 2 * 16);
    paint_toast(NULL, "short", LEVEL_INFO, "no-such-icon", &w, &h);
    assert(w == 380 && h == 16 + 2 * 16);

    /* With one: as tall as the icon if the text is shorter, and the text has less room beside it */
    paint_toast(NULL, "short", LEVEL_INFO, "test-icon", &w, &h);
    assert(w == 380 && h == 24 + 2 * 16);
    char text[200];
    memset(text, 'a', 30);                                  /* 380 - (5 + 16 + 24 + 12) - 16 = 307: 30 bytes */
    text[30] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, "test-icon", &w, &h);
    assert(h == 24 + 2 * 16);
    text[30] = 'a';
    text[31] = '\0';
    paint_toast(NULL, text, LEVEL_INFO, "test-icon", &w, &h);
    assert(h == 2 * 16 + 2 * 16);                           /* a second line: the text is taller than the icon */
    paint_toast(NULL, text, LEVEL_INFO, NULL, &w, &h);      /* the same without it: still one line (34 fit) */
    assert(h == 16 + 2 * 16);

    /* The first name that exists, from a list */
    paint_toast(NULL, "short", LEVEL_INFO, "no-such-icon,test-icon", &w, &h);
    assert(h == 24 + 2 * 16);

    /* The icon is drawn where the layout says: inside the card, beside the accent bar, centred */
    s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 380, 56);
    cr = cairo_create(s);
    paint_toast(cr, "short", LEVEL_WARN, "test-icon", &w, &h);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s) / 4;
    assert(px[28 * stride + 33] == 0xFFFF0000u);            /* the icon's middle: x 21..45, y 16..40 */
    assert(px[28 * stride + 70] != 0xFFFF0000u);            /* and not out where the text is */
    cairo_surface_destroy(s);

    /* A slider: the icon beside the label, and the bar lower when the icon is taller than the text */
    paint_slider(NULL, "Volume", 50, false, NULL, PAINT_ITEM_W, &w, &h);
    assert(w == 380 && h == 64);
    paint_slider(NULL, "Volume", 50, false, "test-icon", PAINT_ITEM_W, &w, &h);
    assert(w == 380 && h == 64 + (24 - 16));
    paint_slider(NULL, "Volume", 50, false, "test-icon", PAINT_OSD_W, &w, &h);
    assert(w == 300 && h == 72);

    /* The size: its own option, clamped, and not following the font when it is set */
    paint_configure_icons(true, 40);
    paint_toast(NULL, "short", LEVEL_INFO, "test-icon", &w, &h);
    assert(sicon_size() == 40 && h == 40 + 2 * 16);
    paint_slider(NULL, "Volume", 50, false, "test-icon", PAINT_ITEM_W, &w, &h);
    assert(h == 64 + (40 - 16));
    paint_configure_icons(true, 5);
    assert(sicon_size() == PAINT_ICON_MIN);
    paint_configure_icons(true, 500);
    assert(sicon_size() == PAINT_ICON_MAX);

    /* By default it follows the font, with the rest; when set it does not */
    paint_configure_icons(true, 0);
    paint_configure(20, 0);
    assert(sicon_size() == 30);                             /* 24 * 1.25 */
    paint_configure(32, 0);
    assert(sicon_size() == 48);
    paint_configure_icons(true, 24);
    paint_configure(32, 0);
    assert(sicon_size() == 24);                             /* asked for 24, so 24 */
    paint_configure(0, 0);
    paint_configure_icons(true, 0);

    /* Off: the text has the room, and the card is as it was */
    paint_configure_icons(false, 0);
    paint_toast(NULL, "short", LEVEL_INFO, "test-icon", &w, &h);
    assert(h == 16 + 2 * 16);
    paint_slider(NULL, "Volume", 50, false, "test-icon", PAINT_ITEM_W, &w, &h);
    assert(h == 64);
    paint_configure_icons(true, 0);

    /* The environment: PACHUD_ICONS=0 turns them off, PACHUD_ICON_SIZE is a size in pixels */
    unsetenv("PACHUD_ICONS");
    setenv("PACHUD_ICON_SIZE", "-3", 1);
    paint_init();
    assert(sicon_size() == PAINT_ICON_MIN);                 /* out of range: the nearest end */
    unsetenv("PACHUD_ICON_SIZE");
    paint_init();
    assert(sicon_enabled() && sicon_size() == 24);
    setenv("PACHUD_ICONS", "0", 1);
    paint_init();
    assert(!sicon_enabled());
    setenv("PACHUD_ICONS", "1", 1);                         /* anything but 0 leaves them on */
    paint_init();
    assert(sicon_enabled());
    unsetenv("PACHUD_ICONS");
    setenv("PACHUD_ICON_SIZE", "32", 1);
    paint_init();
    assert(sicon_enabled() && sicon_size() == 32);
    setenv("PACHUD_ICON_SIZE", "999", 1);
    paint_init();
    assert(sicon_size() == PAINT_ICON_MAX);
    const char *bad[] = { "big", "20px", "1.5" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        setenv("PACHUD_ICON_SIZE", bad[i], 1);
        paint_init();
        assert(sicon_size() == 24);                         /* said so, and the default stays */
    }
    unsetenv("PACHUD_ICON_SIZE");
    paint_init();

    /* In the HUD: an item keeps its icon, and a refresh brings the new one */
    reset();
    hud_toast_icon("battery", LEVEL_INFO, 1000, "test-icon");
    assert(nstack == 1 && !strcmp(stack[0].icon, "test-icon"));
    hud_toast_icon("battery", LEVEL_INFO, 1000, "other-icon");
    assert(nstack == 1 && !strcmp(stack[0].icon, "other-icon"));
    hud_toast("battery", LEVEL_INFO, 1000);                 /* and none, when none is given */
    assert(nstack == 1 && stack[0].icon[0] == '\0');
    hud_slider("Volume", 30, false, "test-icon");
    assert(nstack == 2 && !strcmp(stack[0].icon, "test-icon") && stack[0].slider);
    int sw, sh;
    paint_item(NULL, &stack[0], &sw, &sh);
    assert(sw == 380 && sh == 72);                           /* the stack lays out what the icon asks for */
    reset();

    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

/* The clock the on-screen display shows for `pachud time` */
static void test_clock_paint(void) {
    int w, h, plain;
    paint_clock(NULL, "14:32", "", PAINT_OSD_W, &w, &h);
    plain = h;
    assert(w == PAINT_OSD_W && h > 0);
    paint_clock(NULL, "14:32", "Thursday 12 November", PAINT_OSD_W, &w, &h);
    assert(w == PAINT_OSD_W && h > plain);                  /* a line under it adds height */

    /* drawn into a surface of the size it says, a long line is cut with an ellipsis, not widened */
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(s);
    paint_clock(cr, "14:32", "an extremely long line of text that cannot possibly fit in the display at all", PAINT_OSD_W, &w, &h);
    assert(cairo_status(cr) == CAIRO_STATUS_SUCCESS && w == PAINT_OSD_W);
    cairo_destroy(cr);
    cairo_surface_flush(s);
    uint32_t *px = (uint32_t *)cairo_image_surface_get_data(s);
    assert((px[(h / 2) * (cairo_image_surface_get_stride(s) / 4) + w / 2] >> 24) != 0);   /* the card is there */
    cairo_surface_destroy(s);

    /* it grows with the font like everything else */
    paint_configure(32, 0);
    int big;
    paint_clock(NULL, "14:32", "Thursday 12 November", PAINT_OSD_W, &w, &big);
    assert(big > h && w == 2 * 300);
    paint_configure(0, 0);

    hud_osd_clock("14:32", "Thursday");                      /* no buffer from the stub: nothing to show, no crash */
}

int main(void) {
    hud_init(NULL);
    test_clock_paint();
    test_item_widths();
    test_toast_expiry();
    test_stacking();
    test_alert_dedupe();
    test_slider_beside_alert();
    test_stack_full();
    test_cards();
    test_remove();
    test_never_expires();
    test_card_count();
    test_click();
    test_resident();
    test_click_token();
    test_expand();
    test_hover();
    test_deck_lanes();
    test_deck_order_and_overflow();
    test_deck_targets();
    test_deck_clicks();
    test_deck_time();
    test_deck_hover();
    test_deck_moves();
    test_sizes();
    test_size_environment();
    test_status_icons();
    test_icon_files();
    test_dismiss_prefers_alerts();
    test_queue_overflow();
    test_actions();
    test_strip_markup();
    test_wrap();
    test_ellipsize();
    puts("hud: ok");
    return 0;
}
