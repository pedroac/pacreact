/* The HUD manager (stacking, replacement, overflow) and the card text helpers.
 * hud.c is included so its statics are reachable; drawing and time are stubbed: no surface
 * is ever created, and the clock only moves when the test says so. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static long long fake_ms = 1000;
static int fake_clock_gettime(clockid_t c, struct timespec *ts) {
    (void)c;
    ts->tv_sec = fake_ms / 1000;
    ts->tv_nsec = (fake_ms % 1000) * 1000000L;
    return 0;
}
#define clock_gettime fake_clock_gettime

#include "../src/display.h"
#include "../src/text.h"

static int hides;
Buf *display_buf(Display *d, int w, int h, cairo_t **cr) { (void)d; (void)w; (void)h; (void)cr; return NULL; }
void display_present(Display *d, Buf *b, cairo_t *cr) { (void)d; (void)b; (void)cr; }
void display_hide(Display *d) { (void)d; hides++; }
Buf *display_osd_buf(Display *d, int w, int h, cairo_t **cr) { (void)d; (void)w; (void)h; (void)cr; return NULL; }
void display_osd_present(Display *d, Buf *b, cairo_t *cr, OsdAnchor a) { (void)d; (void)b; (void)cr; (void)a; }
void display_osd_hide(Display *d) { (void)d; }
void display_on_rescale(Display *d, void (*fn)(void)) { (void)d; (void)fn; }
int display_align(Display *d) { (void)d; return 2; }
static int click_rects_set = -1;
void display_set_click_rects(Display *d, const ClickRect *r, int n) { (void)d; (void)r; click_rects_set = n; }
void display_on_click(Display *d, void (*fn)(int, int, bool)) { (void)d; (void)fn; }

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
    hud_toast("B", LEVEL_CRIT, 1000);
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
    hud_slider("Volume", 50, false);   /* no longer pushes the alert out of sight */
    assert(nstack == 2 && stack[0].slider && !strcmp(stack[1].text, "A"));
    assert(hud_timeout_ms() == SLIDER_MS);

    advance_to_expiry();               /* the slider goes; the alert kept its own clock */
    assert(nstack == 1 && !strcmp(stack[0].text, "A") && hud_timeout_ms() == 5000 - 1000 - SLIDER_MS);

    hud_slider("Volume", 60, false);
    hud_slider("Volume", 70, false);   /* sliders refresh in place */
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
    hud_slider("Volume", 10, false);
    assert(nstack == STACK_MAX && stack[0].slider);
    card(4, "d", "", LEVEL_INFO, 1000);
    assert(nstack == STACK_MAX && queued == 0 && stack[0].id == 4 && !stack[1].slider);
    hud_slider("Volume", 20, false);   /* nothing ephemeral to displace: the slider is dropped */
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
    hud_slider("Volume", 40, false);
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
    hud_slider("Volume", 30, false);
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
static int nclicks;
static void on_card_click(uint32_t id) { clicks[nclicks++] = id; }

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

    on_click(-5, -5, false);                       /* outside every card: nothing */
    assert(nclicks == 0 && nstack == 3);

    assert(hit_id[0] == 2);                        /* the card with `default` is on top */
    on_click(hit[0].x + 1, hit[0].y + 1, false);
    assert(nclicks == 1 && clicks[0] == 2);
    assert(nstack == 2 && nevents == 1 && events[0].id == 2 && events[0].reason == HUD_CLOSED_DISMISSED);

    on_click(hit[0].x + 1, hit[0].y + 1, false);   /* the plain card: dismissed, nothing to invoke */
    assert(nclicks == 1 && nstack == 1 && nevents == 2 && events[1].id == 1);
    assert(click_rects_set == 0);                  /* only the toast left: click-through */

    on_click(1, 1, false);                         /* a stale click is harmless */
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
    on_click(hit[0].x + 1, hit[0].y + 1, false);
    assert(nclicks == 1 && clicks[0] == 2);
    assert(nstack == 2 && stack[0].id == 2 && nevents == 0);
    on_click(hit[0].x + 1, hit[0].y + 1, false);   /* and it can be invoked again */
    assert(nclicks == 2 && nstack == 2 && nevents == 0);

    /* A right click removes it without invoking anything */
    on_click(hit[0].x + 1, hit[0].y + 1, true);
    assert(nclicks == 2 && nstack == 1 && nevents == 1);
    assert(events[0].id == 2 && events[0].reason == HUD_CLOSED_DISMISSED);

    /* A right click on an ordinary card dismisses it too, again without invoking */
    on_click(hit[0].x + 1, hit[0].y + 1, true);
    assert(nclicks == 2 && !nstack && nevents == 2 && events[1].id == 1);

    /* Resident with nothing to invoke: a left click is the only thing it can mean, so it dismisses */
    reset();
    hud_on_closed(on_closed);
    resident_card(3, (const char *[]){ "reply", "Reply" }, 2);
    on_click(hit[0].x + 1, hit[0].y + 1, false);
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

static void test_toast_line(void) {
    char out[512], text[512];
    toast_line("short", out, sizeof out);
    assert(!strcmp(out, "short"));
    toast_line("two\nlines\tand tab", out, sizeof out);
    assert(!strcmp(out, "two lines and tab"));

    /* 10 units per byte, 720 available: 69 bytes plus the 3-byte ellipsis fit */
    memset(text, 'a', 100);
    text[100] = '\0';
    toast_line(text, out, sizeof out);
    assert(strlen(out) == 72 && !strcmp(out + 69, "…") && out[68] == 'a');
    assert(text_width(out, (int)strlen(out), FONT_SIZE, false) <= MAX_TEXT_W);

    /* exactly at the limit is left alone */
    memset(text, 'b', 72);
    text[72] = '\0';
    toast_line(text, out, sizeof out);
    assert(!strcmp(out, text));

    /* multi-byte text is never cut inside a character, even from a very long string */
    char wide[512] = "";
    for (int i = 0; i < 120; i++) strcat(wide, "é");
    toast_line(wide, out, sizeof out);
    assert(strlen(out) % 2 == 1 && !strcmp(out + strlen(out) - 3, "…"));   /* é is 2 bytes, … is 3 */
    for (const unsigned char *p = (const unsigned char *)out; *p; p++)
        assert(!((*p & 0xC0) == 0x80 && p[-1] < 0x80));
}

int main(void) {
    hud_init(NULL);
    test_toast_line();
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
    test_dismiss_prefers_alerts();
    test_queue_overflow();
    test_actions();
    test_strip_markup();
    test_wrap();
    test_ellipsize();
    puts("hud: ok");
    return 0;
}
