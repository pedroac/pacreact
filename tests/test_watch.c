/* What `pacreact watch` is told: a change goes to every watcher as one line, a repeat goes to nobody, a new watcher
 * gets what is already known, and one that has gone away is let go. */
#include "../src/watch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int pair[8][2];

static int watcher(int i) {
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, pair[i]) == 0);
    return pair[i][0];   /* the daemon's end; the client reads pair[i][1] */
}

static const char *next(int i) {
    static char line[128];
    ssize_t n = recv(pair[i][1], line, sizeof line - 1, MSG_DONTWAIT);
    if (n < 0) return NULL;
    line[n] = '\0';
    return line;
}

static char heard_name[32][16], heard_value[32][16];
static int heard_n;
static void hear(const char *name, const char *value) {
    snprintf(heard_name[heard_n % 32], 16, "%s", name);
    snprintf(heard_value[heard_n % 32], 16, "%s", value);
    heard_n++;
}

/* Whoever asks to be told of changes, and a late one asking for what is already set. */
static void test_hooks(void) {
    int before = heard_n;
    watch_on_change(hear);
    watch_set("displays", "2");
    assert(heard_n == before + 1 && !strcmp(heard_name[before], "displays") && !strcmp(heard_value[before], "2"));
    watch_set("displays", "2");
    assert(heard_n == before + 1);                        /* the same again is not a change */
    before = heard_n;
    watch_each(hear);                                     /* everything there is, for one that came late */
    int found = 0;
    for (int i = before; i < heard_n; i++) found += !strcmp(heard_name[i % 32], "displays") && !strcmp(heard_value[i % 32], "2");
    assert(found == 1 && heard_n - before >= 2);
    watch_on_change(NULL);
    before = heard_n;
    watch_set("displays", "1");
    assert(heard_n == before);
}

int main(void) {
    watch_set("camera", "off");                           /* nobody is watching yet */
    assert(watch_add(watcher(0)) == 0);
    assert(!strcmp(next(0), "camera off"));               /* told what is known */
    assert(next(0) == NULL);

    watch_set("camera", "off");
    assert(next(0) == NULL);                              /* nothing changed: nothing sent */
    watch_set("camera", "on");
    assert(!strcmp(next(0), "camera on"));
    watch_set("printer", "busy");
    assert(!strcmp(next(0), "printer busy"));

    assert(watch_add(watcher(1)) == 0);
    const char *a = next(1);
    assert(a && !strcmp(a, "camera on"));
    assert(!strcmp(next(1), "printer busy"));

    close(pair[0][1]);                                    /* the first one goes away */
    watch_set("camera", "off");
    assert(!strcmp(next(1), "camera off"));
    for (int i = 2; i < 5; i++) assert(watch_add(watcher(i)) == 0);   /* the freed place is used: 4 fit */
    assert(watch_add(watcher(5)) == -1);                  /* a fifth does not */
    close(pair[5][0]);
    close(pair[5][1]);

    watch_set("a-name-that-is-much-too-long-to-fit", "x");   /* cut, never overrun */
    test_hooks();
    puts("watch: ok");
    return 0;
}
