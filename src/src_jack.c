#include "evdev.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sources.h"
#include "sound.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* Audio jacks: the kernel reports a plugged connector as a switch on an input device (the
 * "HDA Intel PCH Front Headphone" device, ...). Events arrive only when something is plugged in
 * or out, so there is nothing to poll. HDMI and DisplayPort audio also show up as jacks but
 * mean "the monitor woke up", so they are left alone. PACHUD_JACKS=0 turns this off. */

#define MAX_JACKS 16
#define DEBOUNCE_MS 150   /* a headset reports its headphone and microphone contacts separately */
#define TOAST_MS 2500

typedef enum { K_NONE, K_HEADPHONES, K_MIC, K_LINEOUT, K_COUNT } JackKind;

typedef struct {
    int fd;               /* -1: slot unused */
    char path[64];
    JackKind kind;
    bool plugged;
} Jack;

static Jack jacks[MAX_JACKS];
static int pending[K_COUNT];    /* per kind: +1 plugged in, -1 plugged out, since the last toast */
static int debounce_fd = -1;
static bool started;

/* What a jack device is, from its name. */
JackKind jack_kind(const char *name) {
    if (!name || !*name) return K_NONE;
    if (strcasestr(name, "HDMI") || strcasestr(name, "DisplayPort") || strstr(name, "/DP")) return K_NONE;
    if (strcasestr(name, "Mic")) return K_MIC;                       /* "Headset Mic", "Front Mic" */
    if (strcasestr(name, "Headphone") || strcasestr(name, "Headset")) return K_HEADPHONES;
    if (strcasestr(name, "Line Out") || strcasestr(name, "Lineout")) return K_LINEOUT;
    return K_NONE;                                                   /* line in, speakers, ... */
}

static int switch_code(JackKind k) {
    return k == K_MIC ? SW_MICROPHONE_INSERT : k == K_LINEOUT ? SW_LINEOUT_INSERT : SW_HEADPHONE_INSERT;
}

static void arm(long ms) {
    struct itimerspec its = { .it_value = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000L } };
    if (debounce_fd >= 0) timerfd_settime(debounce_fd, 0, &its, NULL);
}

static void note(Jack *j, bool plugged) {
    if (plugged == j->plugged) return;
    j->plugged = plugged;
    pending[j->kind] = plugged ? 1 : -1;
    arm(DEBOUNCE_MS);
}

/* One toast for what changed: a headset (headphones and microphone together) is one thing. */
void jack_flush(void) {
    const char *msg = NULL;
    int dir = 0;   /* +1 connected, -1 disconnected */
    if (pending[K_HEADPHONES] && pending[K_HEADPHONES] == pending[K_MIC]) {
        dir = pending[K_HEADPHONES];
        msg = dir > 0 ? _("Headset connected") : _("Headset disconnected");
    } else if (pending[K_HEADPHONES]) {
        dir = pending[K_HEADPHONES];
        msg = dir > 0 ? _("Headphones connected") : _("Headphones disconnected");
    } else if (pending[K_LINEOUT]) {
        dir = pending[K_LINEOUT];
        msg = dir > 0 ? _("Line out connected") : _("Line out disconnected");
    } else if (pending[K_MIC]) {
        dir = pending[K_MIC];
        msg = dir > 0 ? _("Microphone connected") : _("Microphone disconnected");
    }
    memset(pending, 0, sizeof pending);
    if (!msg) return;
    hud_toast(msg, LEVEL_INFO, TOAST_MS);
    sound_play(dir > 0 ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED);
}

static void on_debounce(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(debounce_fd, &n, sizeof n) > 0) jack_flush();
}

static void drop(Jack *j) {
    if (j->fd >= 0) {
        loop_remove(j->fd);
        close(j->fd);
    }
    memset(j, 0, sizeof *j);
    j->fd = -1;
}

static void on_events(void *ctx, uint32_t ev) {
    (void)ev;
    Jack *j = ctx;
    struct input_event events[16];
    int n = evdev_read(j->fd, events, 16);
    if (n < 0) { drop(j); return; }               /* unplugged for good: the sound card went away */
    for (int i = 0; i < n; i++)
        if (events[i].type == EV_SW && events[i].code == switch_code(j->kind))
            note(j, events[i].value != 0);
}

static void consider(const char *path, void *ctx) {
    (void)ctx;
    for (int i = 0; i < MAX_JACKS; i++)
        if (jacks[i].fd >= 0 && !strcmp(jacks[i].path, path)) return;       /* already watching it */
    Jack *slot = NULL;
    for (int i = 0; i < MAX_JACKS && !slot; i++)
        if (jacks[i].fd < 0) slot = &jacks[i];
    if (!slot) return;

    char name[96];
    int fd = evdev_open(path, name, sizeof name);
    if (fd < 0) return;
    JackKind kind = jack_kind(name);
    if (kind == K_NONE || !evdev_has_code(fd, EV_SW, switch_code(kind))) { close(fd); return; }

    slot->fd = fd;
    snprintf(slot->path, sizeof slot->path, "%s", path);
    slot->kind = kind;
    slot->plugged = evdev_switch_state(fd, switch_code(kind)) == 1;       /* the state now: no toast for it */
    loop_add(fd, EPOLLIN, on_events, slot);
}

static void rescan(void) { evdev_scan(consider, NULL); }

void jack_init(void) {
    const char *off = getenv("PACHUD_JACKS");
    if (off && !strcmp(off, "0")) return;
    if (!started) {
        for (int i = 0; i < MAX_JACKS; i++) jacks[i].fd = -1;
        started = true;
    }
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (debounce_fd >= 0) loop_add(debounce_fd, EPOLLIN, on_debounce, NULL);
    rescan();
    evdev_on_change(rescan);   /* a sound card that appears later */
}
