#include "alerts.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include "util.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* CPU and SSD temperature, from the kernel's hwmon sensors. The kernel only sends thermal events
 * when a trip point is crossed, which many machines never define, so those events are used as a
 * trigger to look at once, and a timer does the rest: slow while everything is cool (every 2
 * minutes), faster as a sensor approaches its warning level. PACHUD_TEMP=0 turns it off;
 * PACHUD_TEMP_INTERVAL sets the slowest interval (0: only react to kernel events). */

#define DEFAULT_INTERVAL_S 120
#define MIN_INTERVAL_S 10
#define CPU_DEFAULT_LIMIT 100
#define CPU_DEFAULT_WARN 85
#define CPU_DEFAULT_HOT 95
#define SSD_DEFAULT_LIMIT 80
#define HYSTERESIS_C 5
#define ALERT_MS 8000

typedef enum { CLASS_CPU, CLASS_SSD, CLASS_COUNT } SensorClass;

static const char *hwmon_root = "/sys/class/hwmon";
static long max_interval_s = DEFAULT_INTERVAL_S;
static int cpu_warn_c = CPU_DEFAULT_WARN, cpu_hot_c = CPU_DEFAULT_HOT;   /* PACHUD_TEMP_WARN / PACHUD_TEMP_CRIT */
static int timer_fd = -1;
static int levels[CLASS_COUNT];   /* 0 fine, 1 high, 2 critical */

/* ---- thresholds (no I/O) ---- */

typedef struct { int warn, hot; } Limits;

/* The temperatures (in °C) at which a sensor is "high" and "critical", from the limit the device
 * itself reports. A CPU is high at 85 and critical at 95 (PACHUD_TEMP_WARN and PACHUD_TEMP_CRIT),
 * or 15 and 5 degrees under its own limit if that is lower; an SSD, whose limit is much lower, gets 6 and 2 degrees. */
Limits temp_limits(SensorClass c, int limit) {
    Limits l;
    if (c == CLASS_CPU) {
        l.warn = limit - 15 < cpu_warn_c ? limit - 15 : cpu_warn_c;
        l.hot = limit - 5 < cpu_hot_c ? limit - 5 : cpu_hot_c;
    } else {
        l.warn = limit - 6;
        l.hot = limit - 2;
    }
    return l;
}

/* The level for a temperature, leaving a level only once it is HYSTERESIS_C below its threshold. */
int temp_level(int t, Limits l, int current) {
    int level = t >= l.hot ? 2 : t >= l.warn ? 1 : 0;
    if (current == 2 && level < 2 && t > l.hot - HYSTERESIS_C) level = 2;
    if (current >= 1 && level < 1 && t > l.warn - HYSTERESIS_C) level = 1;
    return level;
}

/* How long to wait before looking again, given how far the closest sensor is below its warning
 * level: relaxed when cool, close attention when near, and faster still once it is high. */
long temp_interval(int margin, int level, long max_interval) {
    long s = margin >= 25 ? max_interval : margin >= 10 ? 30 : MIN_INTERVAL_S;
    if (level >= 1) s = MIN_INTERVAL_S;
    if (s > max_interval) s = max_interval;
    return s < MIN_INTERVAL_S && max_interval >= MIN_INTERVAL_S ? MIN_INTERVAL_S : s;
}

/* ---- reading the sensors ---- */

static bool read_long(const char *path, long *out) {
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fscanf(f, "%ld", out) == 1;
    fclose(f);
    return ok;
}

static bool sensor_class(const char *name, SensorClass *c) {
    static const char *const cpu[] = { "coretemp", "k10temp", "zenpower", "cpu_thermal", "cpu-thermal" };
    for (size_t i = 0; i < sizeof cpu / sizeof *cpu; i++)
        if (!strcmp(name, cpu[i])) { *c = CLASS_CPU; return true; }
    if (!strcmp(name, "nvme")) { *c = CLASS_SSD; return true; }
    return false;
}

typedef struct {
    int level;          /* worst level among the class's sensors */
    int hottest;        /* °C of the hottest sensor */
    int margin;         /* °C below the warning level of the closest sensor (can be negative) */
    bool present;
} Reading;

/* Reads every temperature sensor of every class under hwmon_root. */
static void read_all(Reading out[CLASS_COUNT]) {
    memset(out, 0, sizeof(Reading) * CLASS_COUNT);
    for (int c = 0; c < CLASS_COUNT; c++) out[c].margin = 1000;

    DIR *d = opendir(hwmon_root);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "hwmon", 5)) continue;
        char path[512], name[64] = "";
        snprintf(path, sizeof path, "%s/%s/name", hwmon_root, e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        if (fgets(name, sizeof name, f)) name[strcspn(name, "\n")] = '\0';
        fclose(f);
        SensorClass c;
        if (!sensor_class(name, &c)) continue;

        for (int i = 1; i <= 16; i++) {
            long milli, limit_milli;
            snprintf(path, sizeof path, "%s/%s/temp%d_input", hwmon_root, e->d_name, i);
            if (!read_long(path, &milli)) continue;
            int limit = c == CLASS_CPU ? CPU_DEFAULT_LIMIT : SSD_DEFAULT_LIMIT;
            snprintf(path, sizeof path, "%s/%s/temp%d_crit", hwmon_root, e->d_name, i);
            if (read_long(path, &limit_milli) && limit_milli > 20000) limit = (int)(limit_milli / 1000);
            else {
                snprintf(path, sizeof path, "%s/%s/temp%d_max", hwmon_root, e->d_name, i);
                if (read_long(path, &limit_milli) && limit_milli > 20000) limit = (int)(limit_milli / 1000);
            }

            int t = (int)(milli / 1000);
            Limits l = temp_limits(c, limit);
            int level = temp_level(t, l, levels[c]);
            Reading *r = &out[c];
            r->present = true;
            if (level > r->level) r->level = level;
            if (t > r->hottest) r->hottest = t;
            if (l.warn - t < r->margin) r->margin = l.warn - t;
        }
    }
    closedir(d);
}

/* ---- the check ---- */

static void alert(SensorClass c, int level, int temp) {
    char key[16], msg[96];
    snprintf(key, sizeof key, "temp-%s", c == CLASS_CPU ? "cpu" : "ssd");
    if (!alert_allowed(key, level == 2 ? LEVEL_CRIT : LEVEL_WARN)) return;
    if (c == CLASS_CPU)
        snprintf(msg, sizeof msg, level == 2 ? _("CPU temperature critical — %d °C") : _("CPU temperature high — %d °C"), temp);
    else
        snprintf(msg, sizeof msg, level == 2 ? _("SSD temperature critical — %d °C") : _("SSD temperature high — %d °C"), temp);
    hud_toast_icon(msg, level == 2 ? LEVEL_CRIT : LEVEL_WARN, ALERT_MS,
                   level == 2 ? "dialog-error,dialog-warning" : "dialog-warning");
    sound_play(SOUND_ALERT);
}

/* Returns the seconds to wait for the next look, 0 if no timer is wanted. */
static long check(bool announce) {
    Reading r[CLASS_COUNT];
    read_all(r);

    int margin = 1000, level = 0;
    for (int c = 0; c < CLASS_COUNT; c++) {
        if (!r[c].present) continue;
        if (r[c].level > levels[c] && announce) alert((SensorClass)c, r[c].level, r[c].hottest);
        levels[c] = r[c].level;
        if (r[c].margin < margin) margin = r[c].margin;
        if (r[c].level > level) level = r[c].level;
    }
    if (margin == 1000) return max_interval_s;     /* no sensors: look rarely in case some appear */
    return max_interval_s ? temp_interval(margin, level, max_interval_s) : 0;
}

static void arm(long secs) {
    struct itimerspec its = { .it_value.tv_sec = secs };   /* 0 disarms */
    if (timer_fd >= 0) timerfd_settime(timer_fd, 0, &its, NULL);
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) > 0) arm(check(true));
}

/* A kernel thermal event: look at once. */
void temp_poke(void) {
    if (timer_fd >= 0) arm(check(true));
}

void temp_init(void) {
    const char *off = getenv("PACHUD_TEMP");
    if (off && !strcmp(off, "0")) return;
    max_interval_s = env_int("PACHUD_TEMP_INTERVAL", DEFAULT_INTERVAL_S, 0, 3600, "seconds");
    cpu_warn_c = (int)env_int("PACHUD_TEMP_WARN", CPU_DEFAULT_WARN, 20, 150, "degrees Celsius");
    cpu_hot_c = (int)env_int("PACHUD_TEMP_CRIT", CPU_DEFAULT_HOT, 20, 150, "degrees Celsius");
    if (cpu_hot_c <= cpu_warn_c) cpu_hot_c = cpu_warn_c + 1;
    timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (timer_fd < 0) return;
    loop_add(timer_fd, EPOLLIN, on_timer, NULL);
    arm(check(true));   /* start-up: an alert for a sensor that is already hot goes through the grace period */
}
