#include "dnd.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "timer.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define CRITICAL 2
#define NOTE_MS 2500

static bool active;
static int skipped;
static int tfd = -1;
static time_t until;  /* when it switches itself off; 0 = until told otherwise */

bool dnd_active(void) { return active; }

bool dnd_suppress(int urgency) {
    if (!active || urgency >= CRITICAL) return false;
    skipped++;
    return true;
}

static void arm(long secs) {
    struct itimerspec its = { .it_value.tv_sec = secs };  /* 0 disarms */
    if (tfd >= 0) timerfd_settime(tfd, 0, &its, NULL);
}

static void turn_on(long secs, const char *spec) {
    active = true;
    skipped = 0;
    until = secs > 0 ? time(NULL) + secs : 0;
    arm(secs);
    char msg[96];
    if (secs > 0) snprintf(msg, sizeof msg, _("Do not disturb on for %s"), spec);
    else snprintf(msg, sizeof msg, "%s", _("Do not disturb on"));
    hud_toast(msg, LEVEL_INFO, NOTE_MS);
}

static void turn_off(void) {
    int missed = skipped;
    active = false;
    skipped = 0;
    until = 0;
    arm(0);
    char msg[128];
    if (missed > 0)
        snprintf(msg, sizeof msg, ngettext("Do not disturb off — %d notification skipped",
                                           "Do not disturb off — %d notifications skipped", missed),
                 missed);
    else
        snprintf(msg, sizeof msg, "%s", _("Do not disturb off"));
    hud_toast(msg, LEVEL_INFO, NOTE_MS);
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(tfd, &n, sizeof n) > 0 && active) turn_off();
}

void dnd_init(void) {
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd >= 0) loop_add(tfd, EPOLLIN, on_timer, NULL);
}

static void status(char *reply, size_t n) {
    if (!active) { snprintf(reply, n, "ok off"); return; }
    int len = snprintf(reply, n, "ok on");
    if (until) len += snprintf(reply + len, n - len, ", %lds left", (long)(until - time(NULL)));
    snprintf(reply + len, n - len, ", %d skipped", skipped);
}

bool dnd_command(const char *cmd, char *args, char *reply, size_t n) {
    if (strcmp(cmd, "dnd") != 0) return false;

    char *save, *sub = strtok_r(args, "\t", &save);
    const char *spec = save ? save : "";
    if (!sub || !strcmp(sub, "status")) {
        status(reply, n);
    } else if (!strcmp(sub, "off")) {
        if (active) turn_off();
        snprintf(reply, n, "ok off");
    } else if (!strcmp(sub, "on") || !strcmp(sub, "toggle")) {
        if (!strcmp(sub, "toggle") && active) {
            turn_off();
            snprintf(reply, n, "ok off");
            return true;
        }
        long secs = 0;
        if (*spec && (secs = timer_parse_duration(spec)) < 0) {
            snprintf(reply, n, _("bad duration \"%s\": use e.g. 30m, 1h"), spec);
            return true;
        }
        if (active) {  /* already on: only the end time changes */
            until = secs > 0 ? time(NULL) + secs : 0;
            arm(secs);
        } else {
            turn_on(secs, spec);
        }
        status(reply, n);
    } else {
        snprintf(reply, n, "%s", _("dnd: use on [DURATION], off, toggle or status"));
    }
    return true;
}
