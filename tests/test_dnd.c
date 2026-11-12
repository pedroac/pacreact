/* Do-not-disturb: what it suppresses, the commands, the toasts it shows, and the timed
 * switch-off. dnd.c is included so its statics are reachable; timer.c is linked only for its
 * duration parser. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>

static char toasts[8][160];
static int ntoasts;

static char toast_icon[96];   /* the status icon of the last toast, for the tests that care */
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    extern void hud_toast(const char *, Level, int);
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    hud_toast(t, l, ms);
}
void hud_toast(const char *t, Level l, int ms) {
    (void)l; (void)ms;
    snprintf(toasts[ntoasts++ % 8], sizeof toasts[0], "%s", t);
}
void sound_play(SoundKind k) { (void)k; }
static bool quiet_now;
void sound_set_quiet(bool q) { quiet_now = q; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

#include "../src/dnd.c"

static char reply[256];

static bool run(const char *args) {
    char buf[128];
    snprintf(buf, sizeof buf, "%s", args);
    return dnd_command("dnd", buf, reply, sizeof reply);
}

int main(void) {
    dnd_init();
    assert(tfd >= 0);

    /* Off by default: nothing is suppressed */
    assert(!dnd_active() && !dnd_suppress(0) && !dnd_suppress(1) && skipped == 0);
    char buf[8] = "";
    assert(!dnd_command("timer", buf, reply, sizeof reply));          /* not ours */
    assert(run("") && !strcmp(reply, "ok off"));
    assert(run("status") && !strcmp(reply, "ok off"));

    /* Turning it on */
    ntoasts = 0;
    assert(!quiet_now);
    assert(run("on") && dnd_active() && !strncmp(reply, "ok on", 5));
    assert(ntoasts == 1 && !strcmp(toasts[0], "Do not disturb on"));
    assert(!strcmp(toast_icon, "notifications-disabled"));
    assert(quiet_now);                                                /* the opt-in event sounds are held back too */

    /* Normal and low notifications are held and counted; critical ones come through */
    assert(dnd_suppress(0) && dnd_suppress(1) && dnd_suppress(1));
    assert(!dnd_suppress(2));
    assert(skipped == 3);
    assert(run("status") && !strcmp(reply, "ok on, 3 skipped"));

    /* Turning it off says how many were missed */
    ntoasts = 0;
    assert(run("off") && !dnd_active() && !strcmp(reply, "ok off"));
    assert(ntoasts == 1 && !strcmp(toasts[0], "Do not disturb off — 3 notifications skipped"));
    assert(!quiet_now);
    assert(!dnd_suppress(1));

    /* One is singular, none says nothing about counts */
    run("on"); dnd_suppress(1);
    ntoasts = 0; run("off");
    assert(!strcmp(toasts[0], "Do not disturb off — 1 notification skipped"));
    run("on");
    ntoasts = 0; run("off");
    assert(!strcmp(toasts[0], "Do not disturb off"));
    assert(run("off") && ntoasts == 1);                               /* off while off: no toast */

    /* toggle */
    run("toggle"); assert(dnd_active() && quiet_now);
    run("toggle"); assert(!dnd_active() && !quiet_now);

    /* With a duration */
    ntoasts = 0;
    assert(run("on\t30m") && dnd_active() && strstr(reply, "ok on, 18") != NULL);
    assert(!strcmp(toasts[0], "Do not disturb on for 30m"));
    assert(until > 0 && until - time(NULL) <= 1800 && until - time(NULL) >= 1799);

    /* Changing the end while on: no new toast, the skipped count is kept */
    dnd_suppress(1);
    ntoasts = 0;
    assert(run("on\t1h") && ntoasts == 0 && skipped == 1);
    assert(until - time(NULL) > 3500);
    assert(run("on") && until == 0);                                  /* no end any more */
    run("off");

    /* Bad input changes nothing */
    assert(run("on\t5") && strstr(reply, "bad duration") && !dnd_active());
    assert(run("on\tbogus") && strstr(reply, "bad duration") && !dnd_active());
    assert(run("sideways") && strstr(reply, "use on") && !dnd_active());

    /* It switches itself off when the time is up */
    assert(run("on\t1s") && dnd_active());
    dnd_suppress(1); dnd_suppress(0);
    struct pollfd p = { .fd = tfd, .events = POLLIN };
    assert(poll(&p, 1, 3000) == 1);
    ntoasts = 0;
    on_timer(NULL, 0);
    assert(!dnd_active() && !dnd_suppress(1));
    assert(ntoasts == 1 && !strcmp(toasts[0], "Do not disturb off — 2 notifications skipped"));

    /* A stale timer firing after a manual off does nothing */
    run("on\t1h"); run("off");
    ntoasts = 0;
    on_timer(NULL, 0);
    assert(ntoasts == 0 && !dnd_active());

    puts("dnd: ok");
    return 0;
}
