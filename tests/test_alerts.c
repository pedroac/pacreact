/* The start-up grace period for repeated alerts. alerts.c is included so the decision can be
 * driven with explicit clocks. */
#include <assert.h>
#include "setting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../src/alerts.c"

#define WARN 1
#define CRIT 2

static char dir[64];

static void read_file(char *out, size_t n) {
    char path[512];
    state_file("alerts", path, sizeof path, false);
    FILE *f = fopen(path, "r");
    size_t len = f ? fread(out, 1, n - 1, f) : 0;
    out[len] = '\0';
    if (f) fclose(f);
}

static void restart(void) { alerts_init(); }   /* a new process: reads what the last one saved */

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/pacreact-alerts-XXXXXX");
    assert(mkdtemp(dir));
    setenv("XDG_STATE_HOME", dir, 1);
    pac_setting_set("startup_grace", NULL);

    char file[512];
    restart();
    assert(grace_s == 60 && count == 0);
    long long t0 = start_mono, wall = 1000000;

    /* The first alert of a kind is always shown, and remembered on disk */
    assert(decide("memory", WARN, wall, t0 + 5));
    read_file(file, sizeof file);
    assert(!strcmp(file, "memory 1 1000000\n"));

    /* Shortly after, within the grace period: the same alert is left out... */
    assert(!decide("memory", WARN, wall + 10, t0 + 15));
    read_file(file, sizeof file);
    assert(!strcmp(file, "memory 1 1000000\n"));            /* and the record is not refreshed */

    /* ...but a worse one is not, and replaces the record */
    assert(decide("memory", CRIT, wall + 20, t0 + 25));
    read_file(file, sizeof file);
    assert(!strcmp(file, "memory 2 1000020\n"));
    assert(!decide("memory", CRIT, wall + 30, t0 + 35));
    assert(!decide("memory", WARN, wall + 40, t0 + 45));    /* a milder one is covered by the critical one */
    read_file(file, sizeof file);
    assert(!strcmp(file, "memory 2 1000020\n"));            /* and does not replace its record */

    /* Sources do not affect each other */
    assert(decide("battery", WARN, wall + 50, t0 + 55));
    assert(!decide("battery", WARN, wall + 51, t0 + 56));
    assert(!decide("memory", WARN, wall + 52, t0 + 57));

    /* After the grace period the same alert shows again */
    assert(decide("memory", WARN, wall + 100, t0 + 61));
    assert(decide("battery", WARN, wall + 200, t0 + 3000));

    /* A restart: the grace period starts over and the last shown alerts are remembered */
    restart();
    assert(count == 2);
    t0 = start_mono;
    assert(!decide("memory", WARN, wall + 205, t0 + 3));    /* the same as before the restart */
    assert(!decide("battery", WARN, wall + 206, t0 + 4));
    assert(decide("memory", CRIT, wall + 207, t0 + 5));
    assert(decide("memory", WARN, wall + 208, t0 + 61));    /* grace is over */

    /* "Shortly before" means within 30 minutes of the last one shown */
    restart();
    t0 = start_mono;
    assert(!decide("memory", WARN, wall + 208 + 1800, t0 + 1));      /* exactly 30 minutes: still recent */
    assert(decide("memory", WARN, wall + 208 + 1801 + 1800, t0 + 2)); /* 30 minutes and a second after the refreshed record */
    restart();
    t0 = start_mono;
    assert(decide("memory", WARN, wall + 10000000, t0 + 1));          /* long ago: not the same event */

    /* A clock that went backwards (or a record from the future) is not "recent" */
    restart();
    t0 = start_mono;
    assert(decide("battery", WARN, wall - 5000000, t0 + 1));

    /* startup_grace */
    pac_setting_set("startup_grace", "0");
    restart();
    assert(grace_s == 0);
    t0 = start_mono;
    assert(decide("memory", WARN, wall + 10000100, t0));
    assert(decide("memory", WARN, wall + 10000101, t0));    /* never left out */
    pac_setting_set("startup_grace", "30");
    restart();
    assert(grace_s == 30);
    t0 = start_mono;
    assert(!decide("memory", WARN, wall + 10000102, t0 + 29));
    assert(decide("memory", WARN, wall + 10000103, t0 + 30));
    const char *bad[] = { "abc", "1.5", "", "10s" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        pac_setting_set("startup_grace", bad[i]);
        restart();
        assert(grace_s == 60);                               /* not a number: the default */
    }
    pac_setting_set("startup_grace", "-5");
    restart();
    assert(grace_s == 0);                                    /* out of range: the nearest end of it */
    pac_setting_set("startup_grace", "99999");
    restart();
    assert(grace_s == MAX_GRACE_S);
    pac_setting_set("startup_grace", NULL);

    /* A damaged state file does no harm */
    char path[512];
    state_file("alerts", path, sizeof path, false);
    FILE *f = fopen(path, "w");
    fputs("garbage\n"
          "memory\n"
          "memory notanumber 5\n"
          "battery 1 1000000\n", f);
    for (int i = 0; i < 20; i++) fprintf(f, "source%d 1 1000000\n", i);   /* more than we keep */
    fclose(f);
    restart();
    assert(count == MAX_SOURCES);
    t0 = start_mono;
    assert(!decide("battery", WARN, wall + 1, t0 + 1));
    assert(decide("never-recorded", WARN, wall, t0 + 1));    /* a ninth source: shown, just not remembered */
    assert(!find("never-recorded"));

    /* Long source names are cut consistently, not mixed up */
    restart();
    assert(decide("a-source-name-that-is-far-too-long-to-keep", WARN, wall, start_mono + 1) || count == MAX_SOURCES);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
    puts("alerts: ok");
    return 0;
}
