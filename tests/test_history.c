/* The notification history: a RAM-only ring. history.c is included so its statics are reachable. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/history.c"

static char reply[4096];

static void ask(const char *args) {
    char a[64];
    snprintf(a, sizeof a, "%s", args);
    assert(history_command("history", a, reply, sizeof reply));
}

static int lines(const char *text) {
    int n = 1;
    for (; *text; text++) n += *text == '\n';
    return n;
}

static void add(uint32_t id, const char *summary, HistReason st) {
    history_record(id, "app", summary, "body", 1, st);
}

static void test_basics(void) {
    history_clear();
    assert(!history_command("dnd", (char *)"", reply, sizeof reply));
    ask("");
    assert(!strncmp(reply, "ok ", 3) && strstr(reply, "no notifications yet"));

    add(1, "first", HIST_OPEN);
    add(2, "second", HIST_OPEN);
    ask("");
    assert(!strncmp(reply, "ok ", 3) && lines(reply) == 2);
    /* newest first, with its state */
    assert(strstr(reply, "second") < strstr(reply, "first"));
    assert(strstr(reply, "on screen") && strstr(reply, "[app] second — body"));

    history_close(1, 1);
    history_close(2, 2);
    ask("");
    assert(strstr(reply, "dismissed") && strstr(reply, "expired"));

    /* closed ones stay closed, unknown ids and a repeated close change nothing */
    history_close(1, 3);
    history_close(99, 1);
    history_close(0, 1);
    ask("");
    assert(strstr(reply, "expired") && !strstr(reply, "closed by app"));

    add(3, "third", HIST_OPEN);
    history_close(3, 3);
    add(4, "fourth", HIST_OPEN);
    history_close(4, 4);
    add(5, "fifth", HIST_SKIPPED);
    history_close(5, 4);                 /* the signal for a skipped one: already closed, left alone */
    ask("");
    assert(strstr(reply, "closed by app") && strstr(reply, "not shown") && strstr(reply, "skipped (do not disturb)"));
}

static void test_replacement(void) {
    history_clear();
    add(7, "download 10%", HIST_OPEN);
    add(7, "download 60%", HIST_OPEN);   /* an app replacing its card: one entry, updated */
    history_record(7, "app", "download done", "all files", 1, HIST_OPEN);
    ask("");
    assert(count == 1 && lines(reply) == 1 && strstr(reply, "download done") && !strstr(reply, "10%"));

    history_close(7, 1);
    add(7, "again", HIST_OPEN);          /* the id came back after it was closed: a new entry */
    ask("");
    assert(count == 2 && strstr(reply, "again") && strstr(reply, "download done"));

    /* do-not-disturb suppressing the replacement of a card on screen updates that entry */
    history_clear();
    add(8, "shown", HIST_OPEN);
    add(8, "replacement", HIST_SKIPPED);
    ask("");
    assert(count == 1 && strstr(reply, "skipped") && strstr(reply, "replacement"));
}

static void test_ring(void) {
    history_clear();
    char name[16];
    for (uint32_t i = 1; i <= HISTORY_MAX + 6; i++) {
        snprintf(name, sizeof name, "n%u", i);
        add(i, name, HIST_OPEN);
        history_close(i, 1);
    }
    assert(count == HISTORY_MAX);
    ask("3");
    assert(lines(reply) == 4 && strstr(reply, "61 older not shown"));   /* three entries and a footer */
    char want[16];
    snprintf(want, sizeof want, "n%d", HISTORY_MAX + 6);
    assert(strstr(reply, want));
    assert(!strstr(reply, "n1 ") && !strstr(reply, "n6 "));   /* the oldest were pushed out */

    /* the oldest one still held is the 7th */
    history_clear();
    for (uint32_t i = 1; i <= HISTORY_MAX + 6; i++) { snprintf(name, sizeof name, "m%u", i); add(i, name, HIST_OPEN); }
    assert(strcmp(nth(HISTORY_MAX - 1)->summary, "m7") == 0 && strcmp(nth(0)->summary, "m70") == 0);
}

static void test_listing_limits(void) {
    history_clear();
    for (uint32_t i = 1; i <= 30; i++) add(i, "x", HIST_OPEN);
    ask("");
    assert(lines(reply) == SHOWN_DEFAULT + 1 && strstr(reply, "20 older not shown"));
    ask("30");
    assert(lines(reply) == 30 && !strstr(reply, "older"));   /* everything: no footer */

    /* long entries: the reply stops before it would overflow and says how many are left out */
    history_clear();
    char big[SUMMARY_MAX], body[BODY_MAX];
    memset(big, 'a', sizeof big - 1); big[sizeof big - 1] = '\0';
    memset(body, 'b', sizeof body - 1); body[sizeof body - 1] = '\0';
    for (uint32_t i = 1; i <= HISTORY_MAX; i++) history_record(i, "application", big, body, 2, HIST_OPEN);
    ask("64");
    assert(strlen(reply) < sizeof reply && strstr(reply, "older not shown"));
    assert(lines(reply) < 64 && strstr(reply, "! "));          /* critical ones are marked */
    assert(!strstr(reply, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));

    ask("0"); assert(strstr(reply, "history: use"));
    ask("65"); assert(strstr(reply, "history: use"));
    ask("abc"); assert(strstr(reply, "history: use"));
    ask("5x"); assert(strstr(reply, "history: use"));
}

static void test_text(void) {
    char out[128];
    flatten(out, "line one\nline\ttwo\r", 100);
    assert(!strcmp(out, "line one line two "));
    flatten(out, "abcdefghij", 5);
    assert(!strcmp(out, "abcde…"));
    flatten(out, "ééééé", 5);                 /* 2-byte characters: never cut in the middle of one */
    assert(!strcmp(out, "éé…"));
    flatten(out, "short", 5);
    assert(!strcmp(out, "short"));

    history_clear();
    history_record(1, "", "multi\nline summary", "first\nsecond", 1, HIST_OPEN);
    ask("");
    assert(lines(reply) == 1 && strstr(reply, "multi line summary — first second") && !strstr(reply, "[]"));
}

static int has_control(const char *s, bool allow_newline) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if ((c < 0x20 && !(allow_newline && c == '\n')) || c == 0x7F) return 1;
        if (c == 0xC2 && (unsigned char)s[1] >= 0x80 && (unsigned char)s[1] <= 0x9F) return 1;
    }
    return 0;
}

/* The listing is printed on a terminal and the text comes from whoever sent the notification. */
static void test_terminal_safety(void) {
    char out[256];
    flatten(out, "a\x1b[31mred\x1b]0;title\x07 b\x7f c\x01", 200);
    assert(!has_control(out, false) && strstr(out, "red") && strstr(out, "title"));
    flatten(out, "x\xC2\x9B[2Jy\xC2\x85z \xC2\xA0 \xC3\xA9", 200);   /* CSI and NEL as C1 controls... */
    assert(!has_control(out, false));
    assert(strstr(out, "\xC2\xA0") && strstr(out, "\xC3\xA9"));       /* ...but NBSP and é are left alone */
    flatten(out, "ends in a\xC2", 200);                                 /* a lone lead byte at the end */
    assert(!has_control(out, false));

    /* every field of a listed entry, the app name included */
    history_clear();
    history_record(1, "ap\x1b]52;c;ZXZpbA==\x07p", "sum\x1b[2Jmary", "bo\ndy\x1b[8m", 2, HIST_OPEN);
    history_record(2, "app\n", "second", "", 1, HIST_OPEN);
    ask("");
    assert(!has_control(reply, true) && lines(reply) == 2);
    assert(strstr(reply, "[ap") && strstr(reply, "mary"));
}

static void test_clear(void) {
    history_clear();
    add(1, "secret", HIST_OPEN);
    history_record(2, "app", "also", "my password is hunter2", 1, HIST_OPEN);
    ask("clear");
    assert(!strcmp(reply, "ok") && count == 0 && head == 0);
    /* wiped, not just forgotten */
    for (size_t i = 0; i < sizeof ring; i++) assert(((unsigned char *)ring)[i] == 0);
    ask("");
    assert(strstr(reply, "no notifications yet"));
}

int main(void) {
    test_basics();
    test_replacement();
    test_ring();
    test_listing_limits();
    test_text();
    test_terminal_safety();
    test_clear();
    puts("history: ok");
    return 0;
}
