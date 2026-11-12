#include "alerts.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/statvfs.h>
#include <sys/timerfd.h>
#include <unistd.h>

/* Low disk space. The kernel has no "this filesystem is filling up" event, so this is the one
 * source that cannot be fully event-driven: a slow timer (default every 5 minutes) checks the
 * filesystems that sit on a block device, and a change in the mount table (a drive plugged in)
 * checks at once. PACHUD_DISK=0 turns it off. */

#define DEFAULT_WARN_PCT 10
#define DEFAULT_CRIT_PCT 5
#define CRIT_BYTES (1ULL << 30)     /* with little room left in percent, under 1 GiB is critical */
#define REARM_MARGIN_PCT 3          /* it must recover this far before it can alert again */
#define DEFAULT_INTERVAL_S 300
#define MAX_FS 16
#define ALERT_MS 8000

static int warn_pct = DEFAULT_WARN_PCT, crit_pct = DEFAULT_CRIT_PCT;

typedef struct {
    char mount[256];
    char dev[24];       /* "major:minor": the same device mounted twice is one filesystem */
    int level;          /* 0 fine, 1 low, 2 almost full */
    bool seen;
} Fs;

static Fs filesystems[MAX_FS];
static int mounts_fd = -1, timer_fd = -1;

/* ---- decisions (no I/O) ---- */

static double free_pct(uint64_t avail, uint64_t total) {
    return total ? (double)avail * 100.0 / (double)total : 100.0;
}

/* The level for the space left, with hysteresis: a level is only left once the filesystem has
 * recovered past a margin, so a value hovering at a threshold does not flap. `total` is what
 * `df` calls used plus available. */
int disk_level(uint64_t avail, uint64_t total, int current) {
    double pct = free_pct(avail, total);

    /* Small filesystems have little room in bytes even when they are nearly empty in percent:
     * an almost-empty 1 GB boot partition is not critical, so the byte limit only counts when
     * the percentage is low too. */
    int level = 0;
    if (pct < crit_pct || (pct < warn_pct && avail < CRIT_BYTES)) level = 2;
    else if (pct < warn_pct) level = 1;

    if (current == 2 && level < 2 && !(pct >= crit_pct + REARM_MARGIN_PCT && avail >= 2 * CRIT_BYTES))
        level = 2;
    if (current >= 1 && level < 1 && pct < warn_pct + REARM_MARGIN_PCT) level = 1;
    return level;
}

void disk_format_size(uint64_t bytes, char *out, size_t n) {
    if (bytes >= (1ULL << 30)) snprintf(out, n, "%.1f GB", (double)bytes / (double)(1ULL << 30));
    else if (bytes >= (1ULL << 20)) snprintf(out, n, "%llu MB", (unsigned long long)(bytes >> 20));
    else snprintf(out, n, "%llu KB", (unsigned long long)(bytes >> 10));
}

/* ---- the mount table ---- */

typedef struct {
    char mount[256];
    char dev[24];
} Mount;

/* "\040" and friends: mountinfo escapes spaces, tabs, newlines and backslashes in octal. */
static void unescape(char *s) {
    char *w = s;
    for (const char *r = s; *r; r++) {
        if (r[0] == '\\' && r[1] >= '0' && r[1] <= '3' && r[2] >= '0' && r[2] <= '7' && r[3] >= '0' && r[3] <= '7') {
            *w++ = (char)((r[1] - '0') * 64 + (r[2] - '0') * 8 + (r[3] - '0'));
            r += 3;
        } else {
            *w++ = *r;
        }
    }
    *w = '\0';
}

static bool has_option(const char *options, const char *want) {
    size_t len = strlen(want);
    for (const char *p = options; p && *p;) {
        if (!strncmp(p, want, len) && (p[len] == ',' || p[len] == '\0')) return true;
        p = strchr(p, ',');
        if (p) p++;
    }
    return false;
}

/* The filesystems worth watching in /proc/self/mountinfo text: on a block device, writable, not
 * a read-only image format, once per device. Returns how many were found. */
int disk_parse_mountinfo(const char *text, Mount *out, int max) {
    int n = 0;
    char copy[8192];
    snprintf(copy, sizeof copy, "%s", text);

    char *save_line, *line = strtok_r(copy, "\n", &save_line);
    for (; line && n < max; line = strtok_r(NULL, "\n", &save_line)) {
        /* id parent maj:min root mountpoint options [optional fields...] - fstype source superopts */
        char *sep = strstr(line, " - ");
        if (!sep) continue;
        char fstype[32] = "", source[256] = "";
        if (sscanf(sep + 3, "%31s %255s", fstype, source) != 2) continue;

        char head[1024];
        snprintf(head, sizeof head, "%.*s", (int)(sep - line), line);
        char *tok[7] = {0}, *save;
        int t = 0;
        for (char *p = strtok_r(head, " ", &save); p && t < 7; p = strtok_r(NULL, " ", &save)) tok[t++] = p;
        if (t < 6) continue;

        if (strncmp(source, "/dev/", 5) != 0) continue;                 /* tmpfs, proc, overlay, ... */
        if (!strcmp(fstype, "squashfs") || !strcmp(fstype, "iso9660") || !strcmp(fstype, "udf") ||
            !strcmp(fstype, "erofs"))
            continue;                                                    /* images: always full */
        if (has_option(tok[5], "ro")) continue;                          /* cannot fill up */

        Mount m;
        snprintf(m.dev, sizeof m.dev, "%s", tok[2]);
        snprintf(m.mount, sizeof m.mount, "%s", tok[4]);
        unescape(m.mount);

        bool dup = false;
        for (int i = 0; i < n; i++)
            if (!strcmp(out[i].dev, m.dev)) dup = true;                  /* bind mounts, subvolumes */
        if (!dup) out[n++] = m;
    }
    return n;
}

/* ---- checking ---- */

static Fs *fs_for(const char *dev, const char *mount) {
    Fs *free_slot = NULL;
    for (int i = 0; i < MAX_FS; i++) {
        if (filesystems[i].dev[0] && !strcmp(filesystems[i].dev, dev)) {
            snprintf(filesystems[i].mount, sizeof filesystems[i].mount, "%s", mount);
            return &filesystems[i];
        }
        if (!filesystems[i].dev[0] && !free_slot) free_slot = &filesystems[i];
    }
    if (free_slot) {
        snprintf(free_slot->dev, sizeof free_slot->dev, "%s", dev);
        snprintf(free_slot->mount, sizeof free_slot->mount, "%s", mount);
        free_slot->level = 0;
    }
    return free_slot;
}

/* One filesystem's numbers in; at most one alert out, and only when it got worse. */
void disk_evaluate(const char *dev, const char *mount, uint64_t avail, uint64_t total, bool announce) {
    Fs *fs = fs_for(dev, mount);
    if (!fs) return;
    fs->seen = true;
    int level = disk_level(avail, total, fs->level);
    bool worse = level > fs->level;
    fs->level = level;
    if (!worse || !announce) return;

    char key[32], size[32], msg[512];
    snprintf(key, sizeof key, "disk:%s", mount);
    if (!alert_allowed(key, level == 2 ? LEVEL_CRIT : LEVEL_WARN)) return;
    disk_format_size(avail, size, sizeof size);
    int pct = (int)free_pct(avail, total);
    if (level == 2) snprintf(msg, sizeof msg, _("Disk almost full — %s: %d%% free (%s)"), mount, pct, size);
    else snprintf(msg, sizeof msg, _("Disk space low — %s: %d%% free (%s)"), mount, pct, size);
    hud_toast_icon(msg, level == 2 ? LEVEL_CRIT : LEVEL_WARN, ALERT_MS,
                   level == 2 ? "drive-harddisk,dialog-error" : "drive-harddisk,dialog-warning");
    sound_play(SOUND_ALERT);
}

static void check_all(bool announce) {
    char text[8192];
    ssize_t len = 0;
    if (mounts_fd >= 0) {  /* re-reading from the start also re-arms the change notification */
        if (lseek(mounts_fd, 0, SEEK_SET) < 0) return;
        ssize_t r;
        while (len < (ssize_t)sizeof text - 1 && (r = read(mounts_fd, text + len, sizeof text - 1 - len)) > 0) len += r;
    }
    text[len > 0 ? len : 0] = '\0';

    Mount mounts[MAX_FS];
    int n = disk_parse_mountinfo(text, mounts, MAX_FS);

    for (int i = 0; i < MAX_FS; i++) filesystems[i].seen = false;
    for (int i = 0; i < n; i++) {
        struct statvfs sv;
        if (statvfs(mounts[i].mount, &sv) != 0 || sv.f_blocks == 0) continue;
        uint64_t unit = sv.f_frsize ? sv.f_frsize : sv.f_bsize;
        uint64_t avail = (uint64_t)sv.f_bavail * unit;
        uint64_t used = (uint64_t)(sv.f_blocks - sv.f_bfree) * unit;
        disk_evaluate(mounts[i].dev, mounts[i].mount, avail, used + avail, announce);
    }
    for (int i = 0; i < MAX_FS; i++)             /* unmounted: forget it, so it can alert anew */
        if (filesystems[i].dev[0] && !filesystems[i].seen) memset(&filesystems[i], 0, sizeof filesystems[i]);
}

static void on_mounts(void *ctx, uint32_t ev) { (void)ctx; (void)ev; check_all(true); }

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(timer_fd, &n, sizeof n) > 0) check_all(true);
}

static long env_number(const char *name, long def, long lo, long hi) {
    const char *v = getenv(name);
    if (!v || !*v) return def;
    char *end;
    long x = strtol(v, &end, 10);
    return *end == '\0' && x >= lo && x <= hi ? x : def;
}

void disk_init(void) {
    const char *off = getenv("PACHUD_DISK");
    if (off && !strcmp(off, "0")) return;

    warn_pct = (int)env_number("PACHUD_DISK_WARN", DEFAULT_WARN_PCT, 2, 95);
    crit_pct = (int)env_number("PACHUD_DISK_CRIT", DEFAULT_CRIT_PCT, 1, 94);
    if (crit_pct >= warn_pct) crit_pct = warn_pct - 1;
    long interval = env_number("PACHUD_DISK_INTERVAL", DEFAULT_INTERVAL_S, 0, 86400);  /* 0: mount changes only */

    mounts_fd = open("/proc/self/mountinfo", O_RDONLY | O_CLOEXEC);
    if (mounts_fd >= 0) loop_add(mounts_fd, EPOLLPRI | EPOLLERR, on_mounts, NULL);
    if (interval > 0) {
        timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
        if (timer_fd >= 0) {
            struct itimerspec its = { .it_value.tv_sec = interval, .it_interval.tv_sec = interval };
            timerfd_settime(timer_fd, 0, &its, NULL);
            loop_add(timer_fd, EPOLLIN, on_timer, NULL);
        }
    }
    check_all(true);  /* start-up: an alert for a disk that is already full goes through the grace period */
}
