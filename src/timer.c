#include "timer.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define MAX_ENTRIES 32
#define MAX_SECS (30L * 86400)
#define LABEL_MAX 128
#define FIRE_MS 15000

typedef struct {
    int id;
    bool alarm;
    unsigned days;  /* recurring alarms: weekday bitmask (bit 0 = Sunday); 0 = once */
    time_t when;  /* wall clock: survives suspend and daemon restarts */
    char label[LABEL_MAX];
} Entry;

static Entry entries[MAX_ENTRIES];
static int count, next_id = 1;
static int tfd = -1;

long timer_parse_duration(const char *s) {
    long total = 0;
    if (!*s) return -1;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return -1;
        long v = 0;
        while (isdigit((unsigned char)*s)) {
            v = v * 10 + (*s++ - '0');
            if (v > MAX_SECS) return -1;
        }
        long unit;
        switch (*s) {
        case 's': unit = 1; break;
        case 'm': unit = 60; break;
        case 'h': unit = 3600; break;
        case 'd': unit = 86400; break;
        default: return -1;
        }
        s++;
        total += v * unit;
        if (total > MAX_SECS) return -1;
    }
    return total > 0 ? total : -1;
}

static bool parse_hm(const char *s, int *h, int *m) {
    char extra;
    return sscanf(s, "%d:%d%c", h, m, &extra) == 2 && *h >= 0 && *h <= 23 && *m >= 0 && *m <= 59;
}

time_t timer_next_alarm(int hour, int min, unsigned days, time_t after) {
    struct tm base;
    localtime_r(&after, &base);
    for (int d = 0; d <= 7; d++) {
        struct tm tm = base;
        tm.tm_mday += d;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = 0;
        tm.tm_isdst = -1;
        time_t t = mktime(&tm);  /* normalizes the date and fills in tm_wday */
        if (t > after && (!days || (days >> tm.tm_wday & 1))) return t;
    }
    return -1;
}

time_t timer_parse_alarm(const char *s, time_t now) {
    int h, m;
    if (!parse_hm(s, &h, &m)) return -1;
    return timer_next_alarm(h, m, 0, now);
}

#define WEEKDAYS 0x3E  /* Mon..Fri */
#define WEEKENDS 0x41  /* Sun, Sat */
#define ALL_DAYS 0x7F

static const char *const day_names[7] = { "sun", "mon", "tue", "wed", "thu", "fri", "sat" };

int timer_parse_recur(const char *s) {
    if (!strcmp(s, "daily")) return ALL_DAYS;
    if (!strcmp(s, "weekdays")) return WEEKDAYS;
    if (!strcmp(s, "weekends")) return WEEKENDS;

    int mask = 0;
    while (*s) {
        int d;
        for (d = 0; d < 7; d++)
            if (!strncasecmp(s, day_names[d], 3)) break;
        if (d == 7) return -1;
        mask |= 1 << d;
        s += 3;
        while (isalpha((unsigned char)*s)) s++;  /* accept "monday" for "mon" */
        if (*s == ',') s++;
        else if (*s) return -1;
    }
    return mask ? mask : -1;
}

static void format_days(unsigned days, char *out, size_t n) {
    if (days == ALL_DAYS) { snprintf(out, n, "daily"); return; }
    if (days == WEEKDAYS) { snprintf(out, n, "weekdays"); return; }
    if (days == WEEKENDS) { snprintf(out, n, "weekends"); return; }
    size_t len = 0;
    out[0] = '\0';
    for (int d = 0; d < 7; d++)
        if (days >> d & 1) len += snprintf(out + len, n - len, "%s%s", len ? "," : "", day_names[d]);
}

static void state_path(char *out, size_t n, bool make_dir) {
    const char *base = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[400];
    if (base && *base) snprintf(dir, sizeof dir, "%s", base);
    else snprintf(dir, sizeof dir, "%s/.local/state", home ? home : "/tmp");
    if (make_dir) {
        mkdir(dir, 0700);
        strncat(dir, "/pachud", sizeof dir - strlen(dir) - 1);
        mkdir(dir, 0700);
    } else {
        strncat(dir, "/pachud", sizeof dir - strlen(dir) - 1);
    }
    snprintf(out, n, "%s/timers", dir);
}

static void save(void) {
    char path[512], tmp[520];
    state_path(path, sizeof path, true);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < count; i++)
        fprintf(f, "%lld %c %u %s\n", (long long)entries[i].when, entries[i].alarm ? 'a' : 't',
                entries[i].days, entries[i].label);
    if (fclose(f) == 0) rename(tmp, path);
    else unlink(tmp);
}

static void load(void) {
    char path[512], line[256 + LABEL_MAX];
    state_path(path, sizeof path, false);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (count < MAX_ENTRIES && fgets(line, sizeof line, f)) {
        long long when;
        char kind, label[LABEL_MAX] = "";
        unsigned days;
        line[strcspn(line, "\n")] = '\0';
        if (sscanf(line, "%lld %c %u %127[^\n]", &when, &kind, &days, label) < 3 ||
            (kind != 'a' && kind != 't') || days > ALL_DAYS)
            continue;
        entries[count++] = (Entry){ .id = next_id++, .alarm = kind == 'a', .days = days,
                                    .when = (time_t)when };
        snprintf(entries[count - 1].label, LABEL_MAX, "%s", label);
    }
    fclose(f);
}

/* Arms the timerfd for the earliest entry. CANCEL_ON_SET makes a wall-clock change (NTP step,
 * manual set) wake us so deadlines are re-evaluated instead of drifting. */
static void rearm(void) {
    struct itimerspec its = {0};
    time_t first = 0;
    for (int i = 0; i < count; i++)
        if (!first || entries[i].when < first) first = entries[i].when;
    its.it_value.tv_sec = first;  /* zero disarms */
    timerfd_settime(tfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
}

static void on_fire(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(tfd, &n, sizeof n) < 0) {
        if (errno == ECANCELED) rearm();  /* clock was set */
        return;
    }

    time_t now = time(NULL);
    for (int i = 0; i < count;) {
        if (entries[i].when > now) { i++; continue; }
        char msg[LABEL_MAX + 32];
        const char *what = entries[i].alarm ? _("Alarm") : _("Timer done");
        if (*entries[i].label) snprintf(msg, sizeof msg, "%s — %s", what, entries[i].label);
        else snprintf(msg, sizeof msg, "%s", what);
        hud_toast(msg, LEVEL_WARN, FIRE_MS);

        if (entries[i].days) {  /* recurring: schedule the next occurrence, skipping missed ones */
            struct tm tm;
            localtime_r(&entries[i].when, &tm);
            time_t next = timer_next_alarm(tm.tm_hour, tm.tm_min, entries[i].days, now);
            if (next > 0) { entries[i++].when = next; continue; }
        }
        memmove(entries + i, entries + i + 1, (--count - i) * sizeof *entries);
    }
    save();
    rearm();
}

void timer_init(void) {
    tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0) return;
    load();
    loop_add(tfd, EPOLLIN, on_fire, NULL);
    rearm();
}

static void format_left(long secs, char *out, size_t n) {
    if (secs >= 3600) snprintf(out, n, "%ldh%02ldm", secs / 3600, secs % 3600 / 60);
    else if (secs >= 60) snprintf(out, n, "%ldm%02lds", secs / 60, secs % 60);
    else snprintf(out, n, "%lds", secs < 0 ? 0 : secs);
}

static void sanitize(char *s) {
    for (; *s; s++)
        if (*s == '\n' || *s == '\t') *s = ' ';
}

static void add(bool alarm, unsigned days, time_t when, const char *label, char *reply, size_t n) {
    if (count == MAX_ENTRIES) { snprintf(reply, n, "too many timers"); return; }
    Entry *e = &entries[count++];
    *e = (Entry){ .id = next_id++, .alarm = alarm, .days = days, .when = when };
    snprintf(e->label, LABEL_MAX, "%s", label);
    sanitize(e->label);
    save();
    rearm();
    snprintf(reply, n, "ok %d", e->id);
}

bool timer_command(const char *cmd, char *args, char *reply, size_t n) {
    bool is_timer = !strcmp(cmd, "timer"), is_alarm = !strcmp(cmd, "alarm");
    if (tfd < 0 && (is_timer || is_alarm || !strcmp(cmd, "list") || !strcmp(cmd, "cancel"))) {
        snprintf(reply, n, "timers unavailable");
        return true;
    }

    if (is_timer || is_alarm) {
        char *save_ptr, *spec = strtok_r(args, "\t", &save_ptr);
        const char *label = save_ptr ? save_ptr : "";
        time_t now = time(NULL), when = -1;
        unsigned days = 0;
        if (!spec) { snprintf(reply, n, "missing %s", is_timer ? "duration" : "time"); return true; }
        if (is_timer) {
            long secs = timer_parse_duration(spec);
            if (secs > 0) when = now + secs;
        } else {
            char *at = strchr(spec, '@');  /* "HH:MM@weekdays" */
            if (at) {
                *at = '\0';
                int mask = timer_parse_recur(at + 1);
                if (mask < 0) {
                    snprintf(reply, n, "bad recurrence \"%s\": use daily, weekdays, weekends "
                                       "or e.g. mon,wed,fri", at + 1);
                    return true;
                }
                days = mask;
            }
            int h, m;
            if (parse_hm(spec, &h, &m)) when = timer_next_alarm(h, m, days, now);
        }
        if (when < 0) {
            snprintf(reply, n, is_timer ? "bad duration \"%s\": use e.g. 90s, 5m, 1h30m"
                                        : "bad time \"%s\": use HH:MM", spec);
            return true;
        }
        add(is_alarm, days, when, label, reply, n);
        return true;
    }

    if (!strcmp(cmd, "list")) {
        int len = snprintf(reply, n, "ok");
        time_t now = time(NULL);
        for (int i = 0; i < count && len < (int)n - 200; i++) {
            char left[32], recur[40] = "";
            format_left(entries[i].when - now, left, sizeof left);
            if (entries[i].days) {
                char names[32];
                format_days(entries[i].days, names, sizeof names);
                snprintf(recur, sizeof recur, " [%s]", names);
            }
            len += snprintf(reply + len, n - len, "%s%d %s %s %s%s", i ? "\n" : " ", entries[i].id,
                            entries[i].alarm ? "alarm" : "timer", left, entries[i].label, recur);
        }
        return true;
    }

    if (!strcmp(cmd, "cancel")) {
        bool all = !strcmp(args, "all");
        int id = atoi(args), removed = 0;
        for (int i = 0; i < count;) {
            if (all || entries[i].id == id) {
                memmove(entries + i, entries + i + 1, (--count - i) * sizeof *entries);
                removed++;
            } else {
                i++;
            }
        }
        if (!removed) { snprintf(reply, n, "no such timer"); return true; }
        save();
        rearm();
        snprintf(reply, n, "ok");
        return true;
    }
    return false;
}
