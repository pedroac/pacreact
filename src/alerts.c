#include "alerts.h"
#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_SOURCES 8
#define DEFAULT_GRACE_S 60
#define MAX_GRACE_S 3600
#define REPEAT_WINDOW_S (30 * 60)

typedef struct {
    char source[24];
    int level;
    long long when;  /* wall clock, so it means something to the next process */
} Shown;

static Shown shown[MAX_SOURCES];
static int count;
static long long start_mono;
static long grace_s = DEFAULT_GRACE_S;

static long long mono_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

static void load(void) {
    char path[512], line[128];
    state_file("alerts", path, sizeof path, false);
    FILE *f = fopen(path, "r");
    if (!f) return;
    while (count < MAX_SOURCES && fgets(line, sizeof line, f)) {
        Shown s;
        if (sscanf(line, "%23s %d %lld", s.source, &s.level, &s.when) == 3) shown[count++] = s;
    }
    fclose(f);
}

static void save(void) {
    char path[512], tmp[520];
    state_file("alerts", path, sizeof path, true);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    for (int i = 0; i < count; i++)
        fprintf(f, "%s %d %lld\n", shown[i].source, shown[i].level, shown[i].when);
    if (fclose(f) == 0) rename(tmp, path);
    else unlink(tmp);
}

void alerts_init(void) {
    const char *v = getenv("PACHUD_STARTUP_GRACE");
    grace_s = DEFAULT_GRACE_S;
    if (v && *v) {
        char *end;
        long g = strtol(v, &end, 10);
        if (*end == '\0' && g >= 0 && g <= MAX_GRACE_S) grace_s = g;
    }
    start_mono = mono_seconds();
    count = 0;
    load();
}

static Shown *find(const char *source) {
    for (int i = 0; i < count; i++)
        if (!strncmp(shown[i].source, source, sizeof shown[i].source - 1)) return &shown[i];
    return NULL;
}

/* The decision, with the clocks passed in. */
static bool decide(const char *source, int level, long long wall, long long mono) {
    Shown *last = find(source);

    bool in_grace = mono - start_mono < grace_s;
    bool recent = last && wall >= last->when && wall - last->when <= REPEAT_WINDOW_S;
    if (in_grace && recent && last->level >= level) return false;  /* nothing new since last time */

    if (!last && count < MAX_SOURCES) last = &shown[count++];
    if (last) {
        snprintf(last->source, sizeof last->source, "%s", source);
        last->level = level;
        last->when = wall;
        save();
    }
    return true;
}

bool alert_allowed(const char *source, int level) {
    return decide(source, level, (long long)time(NULL), mono_seconds());
}
