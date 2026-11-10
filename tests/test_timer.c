/* Timers and alarms: parsing, scheduling, the IPC commands, the state file, and what happens
 * when entries come due. timer.c is included so its statics are reachable; the HUD and the
 * loop are stubbed. */
#include "../src/hud.h"
#include "../src/loop.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char toasts[8][160];
static int ntoasts;

void hud_toast(const char *t, Level l, int ms) {
    (void)l;
    assert(ms > 0);
    snprintf(toasts[ntoasts++ % 8], sizeof toasts[0], "%s", t);
}
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

#include "../src/timer.c"

/* ---- parsing and scheduling (pure) ---- */

static void test_parsing(void) {
    assert(timer_parse_duration("90s") == 90);
    assert(timer_parse_duration("5m") == 300);
    assert(timer_parse_duration("1h30m") == 5400);
    assert(timer_parse_duration("2d") == 172800);
    assert(timer_parse_duration("5") == -1);      /* unit required */
    assert(timer_parse_duration("") == -1);
    assert(timer_parse_duration("m5") == -1);
    assert(timer_parse_duration("0s") == -1);
    assert(timer_parse_duration("31d") == -1);
    assert(timer_parse_duration("99999999999s") == -1);

    setenv("TZ", "UTC", 1);
    tzset();
    time_t noon = 1700049600;  /* 2023-11-15 12:00:00 UTC, a Wednesday */
    assert(timer_parse_alarm("13:30", noon) == noon + 5400);
    assert(timer_parse_alarm("12:00", noon) == noon + 86400);   /* not in the future: tomorrow */
    assert(timer_parse_alarm("07:30", noon) == noon + 19 * 3600 + 30 * 60);
    assert(timer_parse_alarm("24:00", noon) == -1);
    assert(timer_parse_alarm("7", noon) == -1);
    assert(timer_parse_alarm("07:30pm", noon) == -1);

    assert(timer_parse_recur("daily") == 0x7F);
    assert(timer_parse_recur("weekdays") == 0x3E);
    assert(timer_parse_recur("weekends") == 0x41);
    assert(timer_parse_recur("mon,wed,fri") == ((1 << 1) | (1 << 3) | (1 << 5)));
    assert(timer_parse_recur("Sat,SUNDAY") == 0x41);
    assert(timer_parse_recur("") == -1);
    assert(timer_parse_recur("funday") == -1);
    assert(timer_parse_recur("mon,") == 0x02);          /* trailing comma is harmless */
    assert(timer_parse_recur("mon wed") == -1);

    const unsigned weekdays = 0x3E, weekends = 0x41;
    assert(timer_next_alarm(13, 0, weekdays, noon) == noon + 3600);              /* later today */
    assert(timer_next_alarm(7, 30, weekdays, noon) == noon + 19 * 3600 + 1800);  /* Thu */
    assert(timer_next_alarm(13, 0, weekends, noon) == noon + 3 * 86400 + 3600);  /* Sat */
    assert(timer_next_alarm(12, 0, 1u << 3, noon) == noon + 7 * 86400);          /* Wed only: next week */
    assert(timer_next_alarm(9, 0, 0, noon) == noon + 21 * 3600);                 /* any day: tomorrow */
    time_t fri = noon + 2 * 86400 + 8 * 3600;  /* Fri 20:00 */
    assert(timer_next_alarm(7, 0, weekdays, fri) == fri + 59 * 3600);            /* Mon 07:00 */

    char names[32];
    format_days(0x7F, names, sizeof names);  assert(!strcmp(names, "daily"));
    format_days(0x3E, names, sizeof names);  assert(!strcmp(names, "weekdays"));
    format_days(0x41, names, sizeof names);  assert(!strcmp(names, "weekends"));
    format_days(0x2A, names, sizeof names);  assert(!strcmp(names, "mon,wed,fri"));

    char left[32];
    format_left(5, left, sizeof left);       assert(!strcmp(left, "5s"));
    format_left(125, left, sizeof left);     assert(!strcmp(left, "2m05s"));
    format_left(3725, left, sizeof left);    assert(!strcmp(left, "1h02m"));
    format_left(-4, left, sizeof left);      assert(!strcmp(left, "0s"));
}

/* ---- commands, persistence and firing ---- */

static char state_dir[64];

static bool command(const char *cmd, const char *args, char *reply, size_t n) {
    char buf[256];
    snprintf(buf, sizeof buf, "%s", args);
    return timer_command(cmd, buf, reply, n);
}

static void read_state(char *out, size_t n) {
    char path[512];
    state_path(path, sizeof path, false);
    FILE *f = fopen(path, "r");
    size_t len = f ? fread(out, 1, n - 1, f) : 0;
    out[len] = '\0';
    if (f) fclose(f);
}

static void wait_readable(int fd) {
    struct pollfd p = { .fd = fd, .events = POLLIN };
    assert(poll(&p, 1, 1000) == 1);
}

static void test_commands(void) {
    char reply[1024], state[1024];
    snprintf(state_dir, sizeof state_dir, "/tmp/pachud-timer-XXXXXX");
    assert(mkdtemp(state_dir));
    setenv("XDG_STATE_HOME", state_dir, 1);

    /* Before init there is no timerfd: the commands say so instead of crashing. */
    assert(command("timer", "5m", reply, sizeof reply) && !strcmp(reply, "timers unavailable"));
    timer_init();
    assert(tfd >= 0 && count == 0);

    assert(!command("bogus", "", reply, sizeof reply));      /* not ours */

    assert(command("timer", "5m\ttea", reply, sizeof reply) && !strcmp(reply, "ok 1"));
    assert(count == 1 && !entries[0].alarm && entries[0].days == 0);
    assert(entries[0].when - time(NULL) >= 299 && entries[0].when - time(NULL) <= 300);

    assert(command("list", "", reply, sizeof reply));
    assert(!strncmp(reply, "ok 1 timer ", 11) && !strcmp(reply + strlen(reply) - 4, " tea"));

    read_state(state, sizeof state);
    long long when;
    char kind, label[64];
    unsigned days;
    assert(sscanf(state, "%lld %c %u %63[^\n]", &when, &kind, &days, label) == 4);
    assert(when == entries[0].when && kind == 't' && days == 0 && !strcmp(label, "tea"));

    /* Bad input is explained, and nothing is added */
    assert(command("timer", "5\tx", reply, sizeof reply) && strstr(reply, "bad duration"));
    assert(command("timer", "", reply, sizeof reply) && strstr(reply, "missing duration"));
    assert(command("alarm", "25:99\tx", reply, sizeof reply) && strstr(reply, "bad time"));
    assert(command("alarm", "07:30@someday\tx", reply, sizeof reply) && strstr(reply, "bad recurrence"));
    assert(count == 1);

    /* Labels cannot break the line-based state file */
    assert(command("timer", "10m\tline1\nline2\tx", reply, sizeof reply) && !strcmp(reply, "ok 2"));
    assert(!strchr(entries[1].label, '\n') && !strchr(entries[1].label, '\t'));

    /* A recurring alarm */
    assert(command("alarm", "07:30@weekdays\tgym", reply, sizeof reply) && !strcmp(reply, "ok 3"));
    assert(entries[2].alarm && entries[2].days == 0x3E);
    assert(command("list", "", reply, sizeof reply) && strstr(reply, "gym [weekdays]"));

    /* Cancelling */
    assert(command("cancel", "99", reply, sizeof reply) && !strcmp(reply, "no such timer"));
    assert(command("cancel", "2", reply, sizeof reply) && !strcmp(reply, "ok") && count == 2);

    /* Entries coming due: the one-shot is removed, the recurring one is rescheduled */
    time_t now = time(NULL);
    entries[0].when = now - 5;  /* tea */
    entries[1].when = now - 5;  /* gym, recurring */
    ntoasts = 0;
    rearm();                    /* a deadline in the past makes the timerfd fire at once */
    wait_readable(tfd);
    on_fire(NULL, 0);

    assert(ntoasts == 2);
    assert(!strcmp(toasts[0], "Timer done — tea"));
    assert(!strcmp(toasts[1], "Alarm — gym"));
    assert(count == 1 && !strcmp(entries[0].label, "gym") && entries[0].when > now);
    read_state(state, sizeof state);
    assert(!strstr(state, "tea") && strstr(state, "gym"));

    /* Without a label the toast is just the kind */
    assert(command("alarm", "06:00", reply, sizeof reply));
    entries[count - 1].when = now - 1;
    rearm();
    wait_readable(tfd);
    ntoasts = 0;
    on_fire(NULL, 0);
    assert(ntoasts == 1 && !strcmp(toasts[0], "Alarm"));

    /* A recurring alarm missed for days fires once, then waits for its next slot */
    entries[0].when = now - 5 * 86400;
    ntoasts = 0;
    rearm();
    wait_readable(tfd);
    on_fire(NULL, 0);
    assert(ntoasts == 1 && entries[0].when > now && entries[0].when < now + 8 * 86400);

    /* The table is bounded */
    assert(command("cancel", "all", reply, sizeof reply) && count == 0);
    read_state(state, sizeof state);
    assert(state[0] == '\0');
    for (int i = 0; i < MAX_ENTRIES; i++)
        assert(command("timer", "1h\tx", reply, sizeof reply) && !strncmp(reply, "ok ", 3));
    assert(command("timer", "1h\tx", reply, sizeof reply) && !strcmp(reply, "too many timers"));
    assert(command("cancel", "all", reply, sizeof reply));
}

static void test_state_file(void) {
    char path[512];
    state_path(path, sizeof path, true);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs("garbage\n"
          "1700000000 x 0 unknown kind\n"
          "1700000000 a 200 days out of range\n"
          "1700000000\n"
          "1700000100 a 62 wake up now\n"
          "1700000200 t 0 \n"
          "1700000300 t 0 tea\n", f);
    fclose(f);

    count = 0;
    next_id = 1;
    load();
    assert(count == 3);                                    /* the good lines only */
    assert(entries[0].alarm && entries[0].days == 62 && entries[0].when == 1700000100);
    assert(!strcmp(entries[0].label, "wake up now"));      /* spaces survive */
    assert(!entries[1].alarm && entries[1].label[0] == '\0');
    assert(!strcmp(entries[2].label, "tea"));
    assert(entries[0].id == 1 && entries[2].id == 3);

    count = 0;
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", state_dir);
    assert(system(cmd) == 0);
}

int main(void) {
    test_parsing();
    test_commands();
    test_state_file();
    puts("timer: ok");
    return 0;
}
