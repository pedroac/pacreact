#include "clock.h"
#include "dnd.h"
#include "hud.h"
#include "loop.h"
#include "util.h"
#include <stdint.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include "tunables.h"

static int tfd = -1, every_min;

void clock_show(void) {
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[16], date[96];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    strftime(date, sizeof date, "%A %d %B", &tm);
    hud_osd_clock(hm, date);
}

time_t clock_next(time_t after, int every) {
    struct tm tm;
    localtime_r(&after, &tm);
    tm.tm_sec = 0;
    tm.tm_min = tm.tm_min / every * every + every;   /* tm_min may pass 59: mktime carries into the hour */
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    return t > after ? t : after + every * 60;
}

static void rearm(void) {
    struct itimerspec its = {0};
    its.it_value.tv_sec = clock_next(time(NULL), every_min);
    timerfd_settime(tfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
}

static void on_fire(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(tfd, &n, sizeof n) < 0) {   /* ECANCELED: the clock was set, so the next time is another */
        rearm();
        return;
    }
    if (!dnd_active()) clock_show();
    rearm();
}

void clock_init(void) {
    every_min = (int)setting_int("clock_every", CLOCK_EVERY_DEFAULT_MIN, 0, 60, "minutes");
    if (!every_min) return;
    tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0) return;
    loop_add(tfd, EPOLLIN, on_fire, NULL);
    rearm();
}
