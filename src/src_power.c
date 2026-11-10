#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include <dirent.h>
#include <linux/netlink.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define WARN_PCT 15
#define CRIT_PCT 5
#define REARM_PCT 20
#define POLL_SECS 60  /* fallback for drivers that don't emit uevents on capacity changes */
#define ALERT_MS 8000

static int ac = -1;        /* last known charger state, -1 unknown */
static bool warned, critical;
static int timer_fd = -1;
static char bat_dir[300];  /* sysfs dir of the first battery, if any */

/* Pure logic, kept free of I/O so it can be tested. */
void battery_update(int capacity, bool discharging) {
    if (!discharging || capacity > REARM_PCT) {
        warned = critical = false;
        return;
    }
    char msg[128];
    if (capacity <= CRIT_PCT && !critical) {
        critical = warned = true;
        snprintf(msg, sizeof msg, _("Battery critical — %d%%"), capacity);
        hud_toast(msg, LEVEL_CRIT, ALERT_MS);
        sound_play(SOUND_ALERT);
    } else if (capacity <= WARN_PCT && !warned) {
        warned = true;
        snprintf(msg, sizeof msg, _("Battery low — %d%%"), capacity);
        hud_toast(msg, LEVEL_WARN, ALERT_MS);
        sound_play(SOUND_ALERT);
    }
}

void charger_update(bool online) {
    int prev = ac;
    ac = online;
    if (prev >= 0 && prev != ac)
        hud_toast(online ? _("Charger connected") : _("Charger disconnected"), LEVEL_INFO, 2500);
}

/* A kernel uevent is "action@path\0KEY=VALUE\0KEY=VALUE\0...". */
void power_uevent(const char *msg, size_t len) {
    const char *name_end = msg + len;
    const char *subsystem = NULL, *type = NULL, *online = NULL, *cap = NULL, *status = NULL;

    for (const char *p = msg + strlen(msg) + 1; p < name_end; p += strlen(p) + 1) {
        if (!strncmp(p, "SUBSYSTEM=", 10)) subsystem = p + 10;
        else if (!strncmp(p, "POWER_SUPPLY_TYPE=", 18)) type = p + 18;
        else if (!strncmp(p, "POWER_SUPPLY_ONLINE=", 20)) online = p + 20;
        else if (!strncmp(p, "POWER_SUPPLY_CAPACITY=", 22)) cap = p + 22;
        else if (!strncmp(p, "POWER_SUPPLY_STATUS=", 20)) status = p + 20;
    }
    if (!subsystem || strcmp(subsystem, "power_supply") || !type) return;

    if (!strcmp(type, "Mains") || !strncmp(type, "USB", 3)) {
        if (online) charger_update(*online == '1');
    } else if (!strcmp(type, "Battery") && cap && status) {
        battery_update(atoi(cap), !strcmp(status, "Discharging"));
    }
}

static bool read_attr(const char *dir, const char *attr, char *out, size_t n) {
    char path[360];
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, n, f) != NULL;
    fclose(f);
    if (ok) out[strcspn(out, "\n")] = '\0';
    return ok;
}

/* Reads the battery from sysfs and keeps the fallback timer armed only while discharging. */
static void check_battery(void) {
    char cap[16], status[32];
    bool discharging = false;
    if (read_attr(bat_dir, "capacity", cap, sizeof cap) &&
        read_attr(bat_dir, "status", status, sizeof status)) {
        discharging = !strcmp(status, "Discharging");
        battery_update(atoi(cap), discharging);
    }
    struct itimerspec its = { .it_value.tv_sec = discharging ? POLL_SECS : 0 };
    timerfd_settime(timer_fd, 0, &its, NULL);
}

static void on_uevent(void *ctx, uint32_t ev) {
    (void)ev;
    char buf[8192];
    ssize_t n;
    while ((n = recv(*(int *)ctx, buf, sizeof buf - 1, MSG_DONTWAIT)) > 0) {
        buf[n] = '\0';
        power_uevent(buf, n);
        if (*bat_dir) check_battery();
    }
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t expirations;
    if (read(timer_fd, &expirations, sizeof expirations) > 0) check_battery();
}

static bool find_battery(void) {
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return false;
    struct dirent *e;
    bool found = false;
    while (!found && (e = readdir(d))) {
        char dir[300], type[16];
        if (e->d_name[0] == '.') continue;
        snprintf(dir, sizeof dir, "/sys/class/power_supply/%s", e->d_name);
        if (read_attr(dir, "type", type, sizeof type) && !strcmp(type, "Battery") &&
            read_attr(dir, "capacity", type, sizeof type)) {  /* e.g. input devices have none */
            snprintf(bat_dir, sizeof bat_dir, "%s", dir);
            found = true;
        }
    }
    closedir(d);
    return found;
}

void power_init(void) {
    static int fd;
    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) return;
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK, .nl_groups = 1 };  /* kernel events */
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return; }
    loop_add(fd, EPOLLIN, on_uevent, &fd);

    if (find_battery()) {
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        loop_add(timer_fd, EPOLLIN, on_timer, NULL);
        check_battery();
    }
}
