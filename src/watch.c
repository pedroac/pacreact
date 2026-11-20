#include "watch.h"
#include "tunables.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static struct { char name[WATCH_NAME_LEN], value[WATCH_VALUE_LEN]; } values[MAX_WATCH_VALUES];
static int watchers[MAX_WATCHERS], n_watchers;
static void (*on_change)(const char *, const char *);

void watch_on_change(void (*fn)(const char *, const char *)) { on_change = fn; }

void watch_each(void (*fn)(const char *, const char *)) {
    for (int i = 0; i < MAX_WATCH_VALUES && values[i].name[0]; i++) fn(values[i].name, values[i].value);
}

/* One line to one watcher; false when it is gone. */
static bool send_line(int fd, const char *name, const char *value) {
    char line[WATCH_NAME_LEN + WATCH_VALUE_LEN + 2];
    int n = snprintf(line, sizeof line, "%s %s", name, value);
    return send(fd, line, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT) >= 0;
}

static void drop(int i) {
    close(watchers[i]);
    watchers[i] = watchers[--n_watchers];
}

void watch_set(const char *name, const char *value) {
    int i = 0;
    while (i < MAX_WATCH_VALUES && values[i].name[0] && strcmp(values[i].name, name)) i++;
    if (i == MAX_WATCH_VALUES) return;
    if (!strcmp(values[i].name, name) && !strcmp(values[i].value, value)) return;
    snprintf(values[i].name, sizeof values[i].name, "%s", name);
    snprintf(values[i].value, sizeof values[i].value, "%s", value);
    if (on_change) on_change(values[i].name, values[i].value);
    for (int w = 0; w < n_watchers;) {
        if (send_line(watchers[w], values[i].name, values[i].value)) w++;
        else drop(w);
    }
}

int watch_add(int cfd) {
    if (n_watchers == MAX_WATCHERS) {   /* find the ones that went away: a lone newline is skipped by the clients */
        for (int w = 0; w < n_watchers;) {
            if (send(watchers[w], "\n", 1, MSG_NOSIGNAL | MSG_DONTWAIT) >= 0) w++;
            else drop(w);
        }
    }
    if (n_watchers == MAX_WATCHERS) return -1;
    watchers[n_watchers++] = cfd;
    for (int i = 0; i < MAX_WATCH_VALUES && values[i].name[0]; i++)
        if (!send_line(cfd, values[i].name, values[i].value)) { drop(n_watchers - 1); return -1; }
    return 0;
}
