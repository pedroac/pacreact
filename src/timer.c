#include "timer.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "state.h"
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
    bool paused;  /* timers only: the time left is kept in `left` and nothing is scheduled */
    long left;
    char label[LABEL_MAX];
} Entry;

static Entry entries[MAX_ENTRIES];
static int count, next_id = 1;
static int tfd = -1;

/* The last timer or alarm that went off, for `snooze`. Kept in memory only. */
static struct { bool valid, alarm; char label[LABEL_MAX]; } last_fired;

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

time_t timer_parse_datetime(const char *s, time_t now) {
    int y, mo, d, h, mi;
    char extra;
    if (sscanf(s, "%4d-%2d-%2dT%2d:%2d%c", &y, &mo, &d, &h, &mi, &extra) != 5 || y < 1970 ||
        mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
        return -1;
    struct tm tm = { .tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h,
                     .tm_min = mi, .tm_isdst = -1 };
    time_t t = mktime(&tm);
    /* mktime rolls February 30th into March: a date that moved did not exist */
    if (t == (time_t)-1 || tm.tm_year != y - 1900 || tm.tm_mon != mo - 1 || tm.tm_mday != d) return -1;
    return t > now ? t : -2;
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
    state_file("timers", out, n, make_dir);
}

static void save(void) {
    char path[512], tmp[520];
    state_path(path, sizeof path, true);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < count; i++) {
        const Entry *e = &entries[i];
        /* a paused timer stores its remaining seconds where the deadline would be */
        fprintf(f, "%lld %c %u %s\n", e->paused ? (long long)e->left : (long long)e->when,
                e->paused ? 'p' : e->alarm ? 'a' : 't', e->days, e->label);
    }
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
            (kind != 'a' && kind != 't' && kind != 'p') || days > ALL_DAYS)
            continue;
        entries[count++] = (Entry){ .id = next_id++, .alarm = kind == 'a', .days = days,
                                    .paused = kind == 'p', .left = kind == 'p' ? (long)when : 0,
                                    .when = kind == 'p' ? 0 : (time_t)when };
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
        if (!entries[i].paused && (!first || entries[i].when < first)) first = entries[i].when;
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
        if (entries[i].paused || entries[i].when > now) { i++; continue; }
        last_fired.valid = true;
        last_fired.alarm = entries[i].alarm;
        snprintf(last_fired.label, LABEL_MAX, "%s", entries[i].label);
        char msg[LABEL_MAX + 32];
        const char *what = entries[i].alarm ? _("Alarm") : _("Timer done");
        if (*entries[i].label) snprintf(msg, sizeof msg, "%s — %s", what, entries[i].label);
        else snprintf(msg, sizeof msg, "%s", what);
        hud_toast_icon(msg, LEVEL_WARN, FIRE_MS, "alarm,appointment-soon");
        sound_play(SOUND_ALARM);

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
    if (count == MAX_ENTRIES) { snprintf(reply, n, "%s", _("too many timers")); return; }
    Entry *e = &entries[count++];
    *e = (Entry){ .id = next_id++, .alarm = alarm, .days = days, .when = when };
    snprintf(e->label, LABEL_MAX, "%s", label);
    sanitize(e->label);
    save();
    rearm();
    snprintf(reply, n, "ok %d", e->id);
}

static Entry *find_entry(int id) {
    for (int i = 0; i < count; i++)
        if (entries[i].id == id) return &entries[i];
    return NULL;
}

static void list_entries(char *reply, size_t n) {
    int len = snprintf(reply, n, "ok");
    time_t now = time(NULL);
    for (int i = 0; i < count && len < (int)n - 200; i++) {
        char left[48], recur[40] = "";
        if (entries[i].paused) {
            char t[32];
            format_left(entries[i].left, t, sizeof t);
            snprintf(left, sizeof left, "paused %s", t);
        } else {
            format_left(entries[i].when - now, left, sizeof left);
        }
        if (entries[i].days) {
            char names[32];
            format_days(entries[i].days, names, sizeof names);
            snprintf(recur, sizeof recur, " [%s]", names);
        }
        len += snprintf(reply + len, n - len, "%s%d %s %s %s%s", i ? "\n" : " ", entries[i].id,
                        entries[i].alarm ? "alarm" : "timer", left, entries[i].label, recur);
    }
}

/* snooze [DURATION]: the timer or alarm that last went off, as a new one-shot. */
static void snooze(char *args, char *reply, size_t n) {
    if (!last_fired.valid) { snprintf(reply, n, "%s", _("nothing to snooze")); return; }
    char *save_ptr, *spec = strtok_r(args, "\t", &save_ptr);
    if (!spec) spec = (char *)"5m";
    long secs = timer_parse_duration(spec);
    if (secs < 0) { snprintf(reply, n, _("bad duration \"%s\": use e.g. 5m, 10m"), spec); return; }

    add(last_fired.alarm, 0, time(NULL) + secs, last_fired.label, reply, n);
    if (strncmp(reply, "ok", 2) != 0) return;
    char msg[LABEL_MAX + 64];
    int len = snprintf(msg, sizeof msg, _("Snoozed for %s"), spec);
    if (*last_fired.label) snprintf(msg + len, sizeof msg - len, " — %s", last_fired.label);
    hud_toast_icon(msg, LEVEL_INFO, 2500, "alarm,appointment-soon");
}

static void pause_resume_extend(const char *cmd, char *args, char *reply, size_t n) {
    char *save_ptr, *id_text = strtok_r(args, "\t", &save_ptr);
    Entry *e = id_text ? find_entry(atoi(id_text)) : NULL;
    if (!e) { snprintf(reply, n, "%s", _("no such timer")); return; }
    time_t now = time(NULL);

    if (!strcmp(cmd, "pause")) {
        if (e->alarm) { snprintf(reply, n, "%s", _("only timers can be paused")); return; }
        if (e->paused) { snprintf(reply, n, "%s", _("already paused")); return; }
        e->left = e->when - now > 1 ? e->when - now : 1;
        e->paused = true;
    } else if (!strcmp(cmd, "resume")) {
        if (!e->paused) { snprintf(reply, n, "%s", _("not paused")); return; }
        e->when = now + e->left;
        e->paused = false;
        e->left = 0;
    } else {  /* extend */
        long secs = save_ptr && *save_ptr ? timer_parse_duration(save_ptr) : -1;
        if (secs < 0) { snprintf(reply, n, "%s", _("bad duration: use e.g. 5m, 1h")); return; }
        long ahead = (e->paused ? e->left : e->when - now) + secs;
        if (ahead > MAX_SECS) { snprintf(reply, n, "%s", _("too far ahead")); return; }
        if (e->paused) e->left += secs;
        else e->when += secs;
    }
    save();
    rearm();
    snprintf(reply, n, "ok");
}

bool timer_command(const char *cmd, char *args, char *reply, size_t n) {
    static const char *const ours[] = { "timer", "alarm", "list", "cancel", "snooze", "pause",
                                        "resume", "extend" };
    bool mine = false;
    for (size_t i = 0; i < sizeof ours / sizeof *ours; i++)
        if (!strcmp(cmd, ours[i])) mine = true;
    if (!mine) return false;
    if (tfd < 0) {
        snprintf(reply, n, "%s", _("timers unavailable"));
        return true;
    }

    bool is_timer = !strcmp(cmd, "timer"), is_alarm = !strcmp(cmd, "alarm");
    if (is_timer || is_alarm) {
        char *save_ptr, *spec = strtok_r(args, "\t", &save_ptr);
        const char *label = save_ptr ? save_ptr : "";
        time_t now = time(NULL), when = -1;
        unsigned days = 0;
        if (!spec) { snprintf(reply, n, "%s", is_timer ? _("missing duration") : _("missing time")); return true; }
        if (is_timer) {
            long secs = timer_parse_duration(spec);
            if (secs > 0) when = now + secs;
        } else {
            char *at = strchr(spec, '@');  /* "HH:MM@weekdays" */
            if (at) {
                *at = '\0';
                int mask = timer_parse_recur(at + 1);
                if (mask < 0) {
                    snprintf(reply, n, _("bad recurrence \"%s\": use daily, weekdays, weekends "
                                         "or e.g. mon,wed,fri"), at + 1);
                    return true;
                }
                days = mask;
            }
            int h, m;
            if (strchr(spec, 'T')) {  /* "YYYY-MM-DDTHH:MM": one specific moment */
                if (days) { snprintf(reply, n, "%s", _("a dated alarm cannot repeat")); return true; }
                when = timer_parse_datetime(spec, now);
                if (when == -2) { snprintf(reply, n, "%s", _("that time has already passed")); return true; }
            } else if (parse_hm(spec, &h, &m)) {
                when = timer_next_alarm(h, m, days, now);
            }
        }
        if (when < 0) {
            snprintf(reply, n, is_timer ? _("bad duration \"%s\": use e.g. 90s, 5m, 1h30m")
                                        : _("bad time \"%s\": use HH:MM or YYYY-MM-DD HH:MM"), spec);
            return true;
        }
        add(is_alarm, days, when, label, reply, n);
        return true;
    }

    if (!strcmp(cmd, "list")) {
        list_entries(reply, n);
        return true;
    }

    if (!strcmp(cmd, "snooze")) {
        snooze(args, reply, n);
        return true;
    }

    if (!strcmp(cmd, "pause") || !strcmp(cmd, "resume") || !strcmp(cmd, "extend")) {
        pause_resume_extend(cmd, args, reply, n);
        return true;
    }

    /* cancel */
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
    if (!removed) { snprintf(reply, n, "%s", _("no such timer")); return true; }
    save();
    rearm();
    snprintf(reply, n, "ok");
    return true;
}
