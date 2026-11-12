#include "activity.h"
#include "util.h"
#include <time.h>

#define IDLE_DEFAULT_MIN 5
#define BREAK_DEFAULT_MIN 15

static void (*change_fn)(bool active, long secs, bool restart);
static long idle_secs = IDLE_DEFAULT_MIN * 60, break_secs = BREAK_DEFAULT_MIN * 60;
static long long idled_at;   /* the boot clock, in seconds, when the user was told to be idle */

/* The boot clock keeps running while the machine sleeps, so a night's sleep counts as time away. */
static long long boot_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec;
}

static void idled(void) {
    idled_at = boot_s();
    if (change_fn) change_fn(false, idle_secs, false);
}

static void resumed(void) {
    long away = idle_secs + (idled_at ? (long)(boot_s() - idled_at) : 0);
    idled_at = 0;
    if (change_fn) change_fn(true, 0, away >= break_secs);
}

void activity_on_change(void (*fn)(bool active, long secs, bool restart)) { change_fn = fn; }
long activity_break_secs(void) { return break_secs; }

void activity_init(Display *dpy) {
    idle_secs = env_int("PACHUD_PEEK_IDLE", IDLE_DEFAULT_MIN, 1, 120, "minutes") * 60;
    break_secs = env_int("PACHUD_PEEK_BREAK", BREAK_DEFAULT_MIN, 1, 720, "minutes") * 60;
    display_watch_idle(dpy, (unsigned)(idle_secs * 1000), idled, resumed);
}
