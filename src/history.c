#include "history.h"
#include "i18n.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "tunables.h"


typedef struct {
    uint32_t id;
    time_t when;
    HistReason reason;
    int urgency;
    char app[APP_MAX], summary[SUMMARY_MAX], body[BODY_MAX];
} Entry;

static Entry ring[HISTORY_MAX];
static int head, count;   /* head: the slot the next entry goes in */

static Entry *nth(int k) { return &ring[(head - 1 - k + HISTORY_MAX) % HISTORY_MAX]; }  /* 0: newest */

static Entry *find_open(uint32_t id) {
    for (int k = 0; id && k < count; k++) {
        Entry *e = nth(k);
        if (e->id == id && e->reason == HIST_OPEN) return e;
    }
    return NULL;
}

void history_record(uint32_t id, const char *app, const char *summary, const char *body,
                    int urgency, HistReason state) {
    Entry *e = find_open(id);
    if (!e) {
        e = &ring[head];
        head = (head + 1) % HISTORY_MAX;
        if (count < HISTORY_MAX) count++;
    }
    e->id = id;
    e->when = time(NULL);
    e->reason = state;
    e->urgency = urgency;
    snprintf(e->app, sizeof e->app, "%s", app);
    snprintf(e->summary, sizeof e->summary, "%s", summary);
    snprintf(e->body, sizeof e->body, "%s", body);
}

void history_close(uint32_t id, int reason) {
    Entry *e = find_open(id);
    if (!e) return;
    e->reason = reason >= HIST_EXPIRED && reason <= HIST_CLOSED ? (HistReason)reason : HIST_DROPPED;
}

void history_clear(void) {
    explicit_bzero(ring, sizeof ring);  /* bodies can be private: do not leave them behind */
    head = count = 0;
}

/* One line of plain text. The text comes from whatever app or message sent the notification and is
 * printed on a terminal, so every control character becomes a space: an escape sequence in it
 * could otherwise rewrite the screen, set the title or reach the clipboard. That is the C0
 * controls, DEL, and the C1 controls (U+0080 to U+009F, which some terminals act on too). What is
 * longer than max bytes is cut at a character boundary and ends in "…". dst needs room for max + 4. */
static void flatten(char *dst, const char *src, size_t max) {
    size_t len = strlen(src), cut = len > max ? max : len;
    if (len > max)
        while (cut > 0 && ((unsigned char)src[cut] & 0xC0) == 0x80) cut--;
    memcpy(dst, src, cut);
    dst[cut] = '\0';
    for (unsigned char *p = (unsigned char *)dst; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            *p = ' ';
        } else if (*p == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F) {
            *p++ = ' ';
            *p = ' ';
        }
    }
    if (len > max) strcpy(dst + cut, "…");
}

static const char *reason_label(HistReason r) {
    switch (r) {
    case HIST_OPEN: return _("on screen");
    case HIST_EXPIRED: return _("expired");
    case HIST_DISMISSED: return _("dismissed");
    case HIST_CLOSED: return _("closed by app");
    case HIST_SKIPPED: return _("skipped (do not disturb)");
    default: return _("not shown");
    }
}

static void format_entry(char *line, size_t n, const Entry *e) {
    char when[32], app[APP_MAX + 8] = "", summary[SUMMARY_SHOWN + 4], body[BODY_SHOWN + 4];
    struct tm t, today;
    time_t now = time(NULL);
    localtime_r(&e->when, &t);
    localtime_r(&now, &today);
    strftime(when, sizeof when, t.tm_year == today.tm_year && t.tm_yday == today.tm_yday ? "%H:%M" : "%b %d %H:%M", &t);
    if (e->app[0]) {
        char name[APP_MAX + 4];
        flatten(name, e->app, APP_MAX);
        snprintf(app, sizeof app, "[%s] ", name);
    }
    flatten(summary, e->summary, SUMMARY_SHOWN);
    flatten(body, e->body, BODY_SHOWN);
    snprintf(line, n, "%s  %s  %s%s%s%s%s", when, reason_label(e->reason), e->urgency >= 2 ? "! " : "",
             app, summary, body[0] ? " — " : "", body);
}

bool history_command(const char *cmd, char *args, char *reply, size_t n) {
    if (strcmp(cmd, "history") != 0) return false;
    while (*args == ' ' || *args == '\t') args++;

    if (!strcmp(args, "clear")) {
        history_clear();
        snprintf(reply, n, "ok");
        return true;
    }
    int want = SHOWN_DEFAULT;
    if (*args) {
        char *end;
        long v = strtol(args, &end, 10);
        if (*end || v < 1 || v > HISTORY_MAX) {
            snprintf(reply, n, _("history: use history [N] (1 to %d) or history clear"), HISTORY_MAX);
            return true;
        }
        want = (int)v;
    }
    if (!count) {
        snprintf(reply, n, "ok %s", _("no notifications yet"));
        return true;
    }

    int len = snprintf(reply, n, "ok"), printed = 0, limit = want < count ? want : count;
    for (int k = 0; k < limit; k++) {
        char line[512];
        format_entry(line, sizeof line, nth(k));
        if (len + (int)strlen(line) + 80 >= (int)n) break;  /* room for the footer */
        len += snprintf(reply + len, n - len, "%s%s", printed ? "\n" : " ", line);
        printed++;
    }
    if (printed < count && len < (int)n - 80)
        snprintf(reply + len, n - len, "%s%d %s", printed ? "\n" : " ", count - printed, _("older not shown"));
    return true;
}
