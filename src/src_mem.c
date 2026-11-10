#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sources.h"
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define COOLDOWN_SECS 30
#define LOW_PCT 10      /* fallback poll: alert below this share of MemAvailable... */
#define REARM_PCT 15    /* ...and re-arm above this one */
#define POLL_SECS 10

static long mem_available_mb(long *total_mb) {
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    long avail = -1, total = -1;
    while (fgets(line, sizeof line, f)) {
        sscanf(line, "MemAvailable: %ld kB", &avail);
        sscanf(line, "MemTotal: %ld kB", &total);
    }
    fclose(f);
    if (total_mb) *total_mb = total / 1024;
    return avail < 0 ? -1 : avail / 1024;
}

static void alert(Level level) {
    static time_t last;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if (last && ts.tv_sec - last < COOLDOWN_SECS) return;
    last = ts.tv_sec;

    char msg[128];
    snprintf(msg, sizeof msg, _("Memory pressure — %ld MB available"), mem_available_mb(NULL));
    hud_toast(msg, level, 6000);
}

static void on_pressure(void *ctx, uint32_t ev) {
    if (ev & EPOLLPRI) alert(*(Level *)ctx);
}

static bool add_trigger(const char *spec, Level *level) {
    int fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return false;
    if (write(fd, spec, strlen(spec) + 1) < 0) { close(fd); return false; }
    loop_add(fd, EPOLLPRI, on_pressure, level);
    return true;
}

static void on_poll(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    static bool low;
    uint64_t n;
    if (read(*(int *)ctx, &n, sizeof n) < 0) return;

    long total, avail = mem_available_mb(&total);
    if (avail < 0 || total <= 0) return;
    if (!low && avail * 100 < total * LOW_PCT) {
        low = true;
        alert(LEVEL_CRIT);
    } else if (low && avail * 100 > total * REARM_PCT) {
        low = false;
    }
}

void mem_init(void) {
    /* Unprivileged triggers need a window that is a multiple of 2 s. Stalling for 500 ms of
     * 2 s means tasks are waiting on memory; 250 ms with everything stalled is thrashing. */
    static Level warn = LEVEL_WARN, crit = LEVEL_CRIT;
    bool w = add_trigger("some 500000 2000000", &warn);
    bool c = add_trigger("full 250000 2000000", &crit);
    if (w || c) return;

    /* No PSI (kernel without it, or denied): poll MemAvailable at a low rate instead. */
    static int fd;
    fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (fd < 0) return;
    struct itimerspec its = { .it_value.tv_sec = POLL_SECS, .it_interval.tv_sec = POLL_SECS };
    timerfd_settime(fd, 0, &its, NULL);
    loop_add(fd, EPOLLIN, on_poll, &fd);
}
