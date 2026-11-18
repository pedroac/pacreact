/* The chime: when it is next due, and what it does when it fires. clock.c is included to reach its statics;
 * the HUD, the loop and do-not-disturb are stubbed. */
#include "../src/hud.h"
#include "../src/loop.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static int shown;
static bool dnd_on;
bool dnd_active(void) { return dnd_on; }
void hud_osd_clock(const char *t, const char *d) { (void)d; assert(strlen(t) == 5); shown++; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

#include "../src/clock.c"
#include "../src/setting.h"

static time_t at(int h, int m, int s) {
    struct tm tm = { .tm_year = 126, .tm_mon = 5, .tm_mday = 15, .tm_hour = h, .tm_min = m, .tm_sec = s, .tm_isdst = -1 };
    return mktime(&tm);
}

int main(void) {
    /* every 30 minutes: :00 and :30, strictly after now */
    assert(clock_next(at(10, 0, 0), 30) == at(10, 30, 0));
    assert(clock_next(at(10, 12, 41), 30) == at(10, 30, 0));
    assert(clock_next(at(10, 30, 0), 30) == at(11, 0, 0));
    assert(clock_next(at(23, 45, 5), 30) == at(24, 0, 0));
    assert(clock_next(at(10, 59, 59), 60) == at(11, 0, 0));
    assert(clock_next(at(10, 7, 0), 15) == at(10, 15, 0));
    assert(clock_next(at(10, 7, 0), 1) == at(10, 8, 0));

    /* shows the clock, but not in do-not-disturb */
    clock_show();
    assert(shown == 1);
    unsetenv("TZ");
    return 0;
}
