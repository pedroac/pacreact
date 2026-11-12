#include "timer.h"
#include "dnd.h"
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
#define PEEK_MS 4000          /* how long a peek at the time stays */
#define PEEK_STALE_S 120      /* a peek this late is not shown: the time it was for has gone */
#define MIN_EVERY_S 60

/* A timer and an alarm are meant to be noticed. A peek is the opposite: the time, shown quietly, as if
 * glancing at a clock: at a time of day (K_PEEK), or after this much time of use (K_USAGE). */
typedef enum { K_TIMER, K_ALARM, K_PEEK, K_USAGE } Kind;

typedef struct {
    int id;
    Kind kind;
    unsigned days;  /* recurring alarms and peeks: weekday bitmask (bit 0 = Sunday); 0 = once */
    time_t when;  /* wall clock: survives suspend and daemon restarts */
    bool paused;  /* timers only: the time left is kept in `left` and nothing is scheduled */
    long left;
    long every;         /* K_USAGE: seconds of use between peeks */
    long used;          /* K_USAGE: seconds of use counted since the last one */
    long long since;    /* K_USAGE: the second (monotonic) the present stretch of use began, -1 while away */
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
        long long field = e->paused ? (long long)e->left : e->kind == K_USAGE ? e->every : (long long)e->when;
        fprintf(f, "%lld %c %u %s\n", field,
                e->paused ? 'p' : e->kind == K_ALARM ? 'a' : e->kind == K_PEEK ? 'k' : e->kind == K_USAGE ? 'u' : 't',
                e->days, e->label);
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
            (kind != 'a' && kind != 't' && kind != 'p' && kind != 'k' && kind != 'u') || days > ALL_DAYS)
            continue;
        if (kind == 'u' && (when < MIN_EVERY_S || when > MAX_SECS)) continue;
        entries[count++] = (Entry){ .id = next_id++, .days = days, .paused = kind == 'p',
                                    .kind = kind == 'a' ? K_ALARM : kind == 'k' ? K_PEEK : kind == 'u' ? K_USAGE : K_TIMER,
                                    .left = kind == 'p' ? (long)when : 0,
                                    .when = kind == 'p' || kind == 'u' ? 0 : (time_t)when,
                                    .every = kind == 'u' ? (long)when : 0, .since = -1 };
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
        if (!entries[i].paused && entries[i].kind != K_USAGE && (!first || entries[i].when < first))
            first = entries[i].when;
    its.it_value.tv_sec = first;  /* zero disarms */
    timerfd_settime(tfd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &its, NULL);
}

/* The time as a peek shows it: "14:32", then a label if there is one (else `fallback`, if any). Quiet:
 * no sound, no card, not in the history, and not at all while do-not-disturb is on. */
static void show_peek(const char *label, const char *fallback) {
    if (dnd_active()) return;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char hm[16], msg[LABEL_MAX + 48];
    strftime(hm, sizeof hm, "%H:%M", &tm);
    const char *extra = *label ? label : fallback;
    if (extra && *extra) snprintf(msg, sizeof msg, "%s — %s", hm, extra);
    else snprintf(msg, sizeof msg, "%s", hm);
    hud_toast_icon(msg, LEVEL_INFO, PEEK_MS, "clock,preferences-system-time,appointment-soon");
}

/* ---- peeks after a stretch of use ----
 * Time of use is counted while the user is at the computer (see activity.c; without an idle protocol it
 * is always, so it is time since start-up). It uses its own timerfd on the monotonic clock, which does not
 * run while the machine sleeps, so sleep is not counted; a sleep long enough to be a break starts the count
 * over (a monotonic clock cannot see that on its own, but its difference from the boot clock can). */

static int ufd = -1;
static bool usage_active = true;
static long break_s = 15 * 60;
static long long sleep_off;     /* boot clock minus monotonic clock at the last look: grows while asleep */

static long long clock_s(clockid_t id) {
    struct timespec ts;
    clock_gettime(id, &ts);
    return ts.tv_sec;
}

void timer_usage_config(long break_secs) { if (break_secs > 0) break_s = break_secs; }

static long usage_left(const Entry *e, long long now) {
    long used = e->used + (e->since >= 0 ? (long)(now - e->since) : 0);
    return e->every - used;
}

/* A sleep that lasted as long as a break ends the stretch of use, whatever the idle protocol said. */
static void usage_check_sleep(long long mono) {
    long long off = clock_s(CLOCK_BOOTTIME) - mono;
    bool slept = off - sleep_off >= break_s;
    sleep_off = off;
    if (!slept) return;
    for (int i = 0; i < count; i++) {
        if (entries[i].kind != K_USAGE) continue;
        entries[i].used = 0;
        entries[i].since = usage_active ? mono : -1;
    }
}

static void usage_rearm(void) {
    if (ufd < 0) return;
    long long now = clock_s(CLOCK_MONOTONIC), first = 0;
    for (int i = 0; i < count; i++) {
        if (entries[i].kind != K_USAGE || entries[i].since < 0) continue;
        long long due = now + (usage_left(&entries[i], now) > 0 ? usage_left(&entries[i], now) : 0);
        if (!first || due < first) first = due;
    }
    struct itimerspec its = {0};
    its.it_value.tv_sec = first;   /* zero disarms; while away nothing counts, so nothing is due */
    timerfd_settime(ufd, TFD_TIMER_ABSTIME, &its, NULL);
}

/* "1h of use", "30m of use", "1h30m of use" */
static void format_of_use(long every, char *out, size_t n) {
    char d[24];
    if (every % 3600 == 0) snprintf(d, sizeof d, "%ldh", every / 3600);
    else if (every < 3600 && every % 60 == 0) snprintf(d, sizeof d, "%ldm", every / 60);
    else snprintf(d, sizeof d, "%ldh%02ldm", every / 3600, every % 3600 / 60);
    snprintf(out, n, _("%s of use"), d);
}

/* Shows the peeks whose use is up, and counts on. */
static void usage_due(void) {
    long long now = clock_s(CLOCK_MONOTONIC);
    usage_check_sleep(now);
    for (int i = 0; i < count; i++) {
        Entry *e = &entries[i];
        if (e->kind != K_USAGE || e->since < 0 || usage_left(e, now) > 0) continue;
        char of_use[48];
        format_of_use(e->every, of_use, sizeof of_use);
        show_peek(e->label, of_use);
        e->used = 0;
        e->since = now;
    }
    usage_rearm();
}

static void on_usage_fire(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(ufd, &n, sizeof n) < 0) return;
    usage_due();
}

void timer_activity(bool active, long secs, bool restart) {
    long long now = clock_s(CLOCK_MONOTONIC);
    usage_check_sleep(now);
    usage_active = active;
    for (int i = 0; i < count; i++) {
        Entry *e = &entries[i];
        if (e->kind != K_USAGE) continue;
        if (!active) {
            /* the user stopped `secs` ago, which is when the stretch of use ended */
            long run = e->since >= 0 ? (long)(now - e->since) - secs : 0;
            if (run > 0) e->used += run;
            e->since = -1;
        } else {
            if (restart) e->used = 0;
            e->since = now;
        }
    }
    usage_rearm();
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
        if (entries[i].paused || entries[i].kind == K_USAGE || entries[i].when > now) { i++; continue; }
        if (entries[i].kind == K_PEEK) {
            /* A peek that is late (the daemon was down, the machine asleep) would show the wrong time
             * under a label that says what it is for: it is skipped, and a recurring one goes on. */
            if (now - entries[i].when <= PEEK_STALE_S) show_peek(entries[i].label, NULL);
        } else {
            last_fired.valid = true;
            last_fired.alarm = entries[i].kind == K_ALARM;
            snprintf(last_fired.label, LABEL_MAX, "%s", entries[i].label);
            char msg[LABEL_MAX + 32];
            const char *what = entries[i].kind == K_ALARM ? _("Alarm") : _("Timer done");
            if (*entries[i].label) snprintf(msg, sizeof msg, "%s — %s", what, entries[i].label);
            else snprintf(msg, sizeof msg, "%s", what);
            hud_toast_icon(msg, LEVEL_WARN, FIRE_MS, "alarm,appointment-soon");
            sound_play(SOUND_ALARM);
        }

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

    ufd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (ufd < 0) return;
    long long now = clock_s(CLOCK_MONOTONIC);
    sleep_off = clock_s(CLOCK_BOOTTIME) - now;
    for (int i = 0; i < count; i++)
        if (entries[i].kind == K_USAGE) entries[i].since = usage_active ? now : -1;
    loop_add(ufd, EPOLLIN, on_usage_fire, NULL);
    usage_rearm();
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

static void add(Kind kind, unsigned days, time_t when, const char *label, char *reply, size_t n) {
    if (count == MAX_ENTRIES) { snprintf(reply, n, "%s", _("too many timers")); return; }
    Entry *e = &entries[count++];
    *e = (Entry){ .id = next_id++, .kind = kind, .days = days, .when = when, .since = -1 };
    snprintf(e->label, LABEL_MAX, "%s", label);
    sanitize(e->label);
    save();
    rearm();
    snprintf(reply, n, "ok %d", e->id);
}

/* A peek after `every` seconds of use. It starts counting now. */
static void add_usage(long every, const char *label, char *reply, size_t n) {
    if (count == MAX_ENTRIES) { snprintf(reply, n, "%s", _("too many timers")); return; }
    Entry *e = &entries[count++];
    *e = (Entry){ .id = next_id++, .kind = K_USAGE, .every = every,
                  .since = usage_active ? clock_s(CLOCK_MONOTONIC) : -1 };
    snprintf(e->label, LABEL_MAX, "%s", label);
    sanitize(e->label);
    save();
    usage_rearm();
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
        char left[48], recur[64] = "";
        if (entries[i].paused) {
            char t[32];
            format_left(entries[i].left, t, sizeof t);
            snprintf(left, sizeof left, "paused %s", t);
        } else if (entries[i].kind == K_USAGE) {   /* the use that is left before it shows */
            long u = usage_left(&entries[i], clock_s(CLOCK_MONOTONIC));
            format_left(u, left, sizeof left);
            char every[32];
            format_left(entries[i].every, every, sizeof every);
            snprintf(recur, sizeof recur, " [every %s of use]", every);
        } else {
            format_left(entries[i].when - now, left, sizeof left);
        }
        if (entries[i].days) {
            char names[32];
            format_days(entries[i].days, names, sizeof names);
            snprintf(recur, sizeof recur, " [%s]", names);
        }
        static const char *const kind_names[] = { "timer", "alarm", "peek", "peek" };
        len += snprintf(reply + len, n - len, "%s%d %s %s %s%s", i ? "\n" : " ", entries[i].id,
                        kind_names[entries[i].kind], left, entries[i].label, recur);
    }
}

/* snooze [DURATION]: the timer or alarm that last went off, as a new one-shot. */
static void snooze(char *args, char *reply, size_t n) {
    if (!last_fired.valid) { snprintf(reply, n, "%s", _("nothing to snooze")); return; }
    char *save_ptr, *spec = strtok_r(args, "\t", &save_ptr);
    if (!spec) spec = (char *)"5m";
    long secs = timer_parse_duration(spec);
    if (secs < 0) { snprintf(reply, n, _("bad duration \"%s\": use e.g. 5m, 10m"), spec); return; }

    add(last_fired.alarm ? K_ALARM : K_TIMER, 0, time(NULL) + secs, last_fired.label, reply, n);
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
        if (e->kind != K_TIMER) { snprintf(reply, n, "%s", _("only timers can be paused")); return; }
        if (e->paused) { snprintf(reply, n, "%s", _("already paused")); return; }
        e->left = e->when - now > 1 ? e->when - now : 1;
        e->paused = true;
    } else if (!strcmp(cmd, "resume")) {
        if (!e->paused) { snprintf(reply, n, "%s", _("not paused")); return; }
        e->when = now + e->left;
        e->paused = false;
        e->left = 0;
    } else {  /* extend */
        if (e->kind == K_USAGE) { snprintf(reply, n, "%s", _("a peek after use cannot be extended")); return; }
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
                                        "resume", "extend", "peek" };
    bool mine = false;
    for (size_t i = 0; i < sizeof ours / sizeof *ours; i++)
        if (!strcmp(cmd, ours[i])) mine = true;
    if (!mine) return false;
    if (tfd < 0) {
        snprintf(reply, n, "%s", _("timers unavailable"));
        return true;
    }

    bool is_timer = !strcmp(cmd, "timer"), is_alarm = !strcmp(cmd, "alarm"), is_peek = !strcmp(cmd, "peek");
    if (is_timer || is_alarm || is_peek) {
        char *save_ptr, *spec = strtok_r(args, "\t", &save_ptr);
        const char *label = save_ptr ? save_ptr : "";
        time_t now = time(NULL), when = -1;
        unsigned days = 0;
        if (!spec) { snprintf(reply, n, "%s", is_timer ? _("missing duration") : _("missing time")); return true; }
        if (is_peek && !strncmp(spec, "every@", 6)) {   /* after a stretch of use, not at a time of day */
            long secs = timer_parse_duration(spec + 6);
            if (secs < MIN_EVERY_S) {
                snprintf(reply, n, _("bad interval \"%s\": use e.g. 30m or 1h, at least 1m"), spec + 6);
                return true;
            }
            add_usage(secs, label, reply, n);
            return true;
        }
        if (is_peek && strchr(spec, 'T')) { snprintf(reply, n, "%s", _("a peek takes HH:MM, not a date")); return true; }
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
        add(is_timer ? K_TIMER : is_alarm ? K_ALARM : K_PEEK, days, when, label, reply, n);
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
    usage_rearm();
    snprintf(reply, n, "ok");
    return true;
}
