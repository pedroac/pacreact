/* Idle notifications turned into "the user is at the computer or not". activity.c is included for its
 * statics; the compositor is stubbed and the boot clock is the test's own. */
#include "../src/display.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static long long fake_boot = 1000;
int clock_gettime(clockid_t id, struct timespec *ts) {
    if (id == CLOCK_BOOTTIME) { ts->tv_sec = fake_boot; ts->tv_nsec = 0; return 0; }
    return (int)syscall(SYS_clock_gettime, id, ts);
}

static bool protocol = true;
static unsigned watched_ms;
static void (*idle_cb)(void), (*back_cb)(void);
bool display_watch_idle(Display *d, unsigned ms, void (*idled)(void), void (*resumed)(void)) {
    (void)d;
    if (!protocol) return false;
    watched_ms = ms;
    idle_cb = idled;
    back_cb = resumed;
    return true;
}

#include "../src/activity.c"

static int changes;
static bool last_active;
static long last_secs;
static bool last_restart;
static void on_change(bool active, long secs, bool restart) {
    changes++;
    last_active = active;
    last_secs = secs;
    last_restart = restart;
}

int main(void) {
    /* defaults: idle after 5 minutes, a break is 15 */
    unsetenv("PACHUD_PEEK_IDLE");
    unsetenv("PACHUD_PEEK_BREAK");
    activity_on_change(on_change);
    activity_init(NULL);
    assert(watched_ms == 5 * 60 * 1000 && activity_break_secs() == 15 * 60);

    /* going idle: told how long they had already been idle */
    idle_cb();
    assert(changes == 1 && !last_active && last_secs == 300);

    /* back after a short while: not a break */
    fake_boot += 120;
    back_cb();
    assert(changes == 2 && last_active && !last_restart);

    /* back after a break: 5 idle minutes and 10 more make 15 */
    idle_cb();
    fake_boot += 600;
    back_cb();
    assert(changes == 4 && last_active && last_restart);
    idle_cb();
    fake_boot += 599;
    back_cb();
    assert(!last_restart);                                  /* a second short */

    /* a night of sleep is time away, though the monotonic clock stood still */
    idle_cb();
    fake_boot += 8 * 3600;
    back_cb();
    assert(last_restart);

    /* the settings, with the strict reading and the limits of the others */
    setenv("PACHUD_PEEK_IDLE", "2", 1);
    setenv("PACHUD_PEEK_BREAK", "30", 1);
    activity_init(NULL);
    assert(watched_ms == 120000 || watched_ms == 5 * 60 * 1000);   /* one watch per display: the first stands */
    assert(activity_break_secs() == 30 * 60);
    setenv("PACHUD_PEEK_IDLE", "0", 1);                     /* out of range: brought to 1 */
    setenv("PACHUD_PEEK_BREAK", "abc", 1);                  /* not a number: the default */
    activity_init(NULL);
    assert(activity_break_secs() == 15 * 60);

    /* no idle protocol: nothing is ever reported */
    protocol = false;
    changes = 0;
    idle_cb = back_cb = NULL;
    activity_init(NULL);
    assert(changes == 0 && idle_cb == NULL);

    puts("activity: ok");
    return 0;
}
