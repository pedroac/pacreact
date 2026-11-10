/* The HUD manager (queue, preemption, replacement, overflow) and the card text helpers.
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

static void reset(void) {
    while (visible) hud_dismiss();
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
    assert(visible && hud_timeout_ms() == 500);
    fake_ms += 499;
    hud_tick();
    assert(visible && hud_timeout_ms() == 1);
    fake_ms += 1;
    hud_tick();
    assert(!visible && hud_timeout_ms() == -1 && hides == 1);

    hud_toast("a", LEVEL_INFO, 500);
    hud_toast("b", LEVEL_INFO, 500);  /* info replaces info */
    assert(!strcmp(cur.text, "b") && queued == 0);
}

static void test_alert_queue_and_dedupe(void) {
    reset();
    hud_toast("A", LEVEL_WARN, 1000);
    hud_toast("B", LEVEL_CRIT, 1000);
    assert(!strcmp(cur.text, "A") && queued == 1);

    hud_toast("B", LEVEL_CRIT, 1000);   /* already waiting */
    assert(queued == 1);

    fake_ms += 600;
    hud_toast("A", LEVEL_WARN, 1000);   /* the one on screen: refreshed, not queued */
    assert(queued == 1 && hud_timeout_ms() == 1000);

    advance_to_expiry();
    assert(!strcmp(cur.text, "B") && queued == 0 && hides == 0);
    advance_to_expiry();
    assert(!visible && hides == 1);
}

static void test_slider_interrupts_alert(void) {
    reset();
    hud_toast("A", LEVEL_WARN, 5000);
    fake_ms += 1000;
    hud_slider("Volume", 50, false);
    assert(cur.slider && queued == 1 && !strcmp(queue[0].text, "A"));
    assert(queue[0].ms == 4000);        /* what was left of it */

    advance_to_expiry();                /* the slider goes, the alert resumes */
    assert(!cur.slider && !strcmp(cur.text, "A") && queued == 0);

    /* An alert nearly done still gets a minimum of time back. */
    fake_ms += hud_timeout_ms() - 100;
    hud_slider("Volume", 60, false);
    assert(queue[0].ms == MIN_RESUME_MS);

    /* Sliders replace sliders. */
    hud_slider("Volume", 70, false);
    assert(cur.percent == 70 && queued == 1);
}

static void test_alert_preempts_ephemeral(void) {
    reset();
    hud_slider("Volume", 50, false);
    hud_toast("X", LEVEL_WARN, 1000);
    assert(!cur.slider && !strcmp(cur.text, "X") && queued == 0);

    reset();
    hud_toast("info", LEVEL_INFO, 1000);
    hud_toast("Y", LEVEL_CRIT, 1000);
    assert(!strcmp(cur.text, "Y") && queued == 0);
}

static void test_cards(void) {
    reset();
    hud_on_closed(on_closed);
    hud_card(1, "first", "body", "", LEVEL_INFO, 1000);
    hud_card(2, "second", "body", "", LEVEL_INFO, 1000);
    assert(cur.id == 1 && queued == 1);            /* cards queue, they do not replace */

    hud_card(2, "second v2", "", "", LEVEL_INFO, 1000);   /* replaced while waiting */
    assert(queued == 1 && !strcmp(queue[0].text, "second v2"));

    hud_card(1, "first v2", "", "", LEVEL_INFO, 1000);    /* replaced while shown */
    assert(cur.id == 1 && !strcmp(cur.text, "first v2") && queued == 1 && nevents == 0);

    advance_to_expiry();                           /* expiry is reported, with the reason */
    assert(nevents == 1 && events[0].id == 1 && events[0].reason == HUD_CLOSED_EXPIRED);
    assert(cur.id == 2);

    hud_dismiss();
    assert(nevents == 2 && events[1].id == 2 && events[1].reason == HUD_CLOSED_DISMISSED);
    assert(!visible);
    hud_dismiss();                                 /* nothing on screen: harmless */
    assert(nevents == 2);
}

static void test_remove(void) {
    reset();
    hud_on_closed(on_closed);
    hud_card(1, "a", "", "", LEVEL_INFO, 1000);
    hud_card(2, "b", "", "", LEVEL_INFO, 1000);
    hud_card(3, "c", "", "", LEVEL_INFO, 1000);

    assert(hud_remove(2) && queued == 1);          /* queued: gone, silently */
    assert(hud_remove(1) && cur.id == 3);          /* shown: the next one takes over */
    assert(nevents == 0);
    assert(!hud_remove(1) && !hud_remove(0) && !hud_remove(99));
    assert(hud_remove(3) && !visible);
}

static void test_queue_overflow(void) {
    reset();
    hud_on_closed(on_closed);
    hud_card(1, "shown", "", "", LEVEL_INFO, 1000);
    for (uint32_t id = 2; id <= 1 + QUEUE_MAX; id++) hud_card(id, "waiting", "", "", LEVEL_INFO, 1000);
    assert(queued == QUEUE_MAX && nevents == 0);

    hud_card(100, "one too many", "", "", LEVEL_INFO, 1000);
    assert(queued == QUEUE_MAX && nevents == 1);
    assert(events[0].id == 100 && events[0].reason == HUD_CLOSED_OTHER);

    /* A slider interrupting a full queue pushes the shown alert in front, dropping the last. */
    nevents = 0;
    hud_slider("Volume", 10, false);
    assert(queued == QUEUE_MAX && queue[0].id == 1);
    assert(nevents == 1 && events[0].id == 1 + QUEUE_MAX && events[0].reason == HUD_CLOSED_OTHER);
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

int main(void) {
    hud_init(NULL);
    test_toast_expiry();
    test_alert_queue_and_dedupe();
    test_slider_interrupts_alert();
    test_alert_preempts_ephemeral();
    test_cards();
    test_remove();
    test_queue_overflow();
    test_strip_markup();
    test_wrap();
    test_ellipsize();
    puts("hud: ok");
    return 0;
}
