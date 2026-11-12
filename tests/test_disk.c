/* Low disk space: thresholds with hysteresis, the mount table parser, the alert sequence, and
 * a real pass over this machine's own mounts. src_disk.c is included for its statics. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static char toasts[8][512];
static int ntoasts, registered, asked;
static Level last_level;
static bool allow = true;
static char last_key[64];

static char toast_icon[96];   /* the status icon of the last toast, for the tests that care */
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    extern void hud_toast(const char *, Level, int);
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    hud_toast(t, l, ms);
}
void hud_toast(const char *t, Level l, int ms) { (void)ms; snprintf(toasts[ntoasts++ % 8], sizeof toasts[0], "%s", t); last_level = l; }
void sound_play(SoundKind k) { (void)k; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
bool alert_allowed(const char *source, int level) { (void)level; asked++; snprintf(last_key, sizeof last_key, "%s", source); return allow; }

#include "../src/src_disk.c"

#define GB (1ULL << 30)
#define MB (1ULL << 20)

static void test_levels(void) {
    /* A big disk: percentages decide */
    assert(disk_level(50 * GB, 100 * GB, 0) == 0);
    assert(disk_level(10 * GB, 100 * GB, 0) == 0);          /* exactly 10% is not low */
    assert(disk_level(9 * GB, 100 * GB, 0) == 1);
    assert(disk_level(5 * GB, 100 * GB, 0) == 1);           /* exactly 5% is low, not critical */
    assert(disk_level(4 * GB, 100 * GB, 0) == 2);
    assert(disk_level(45 * GB, 500 * GB, 0) == 1);          /* 9% of a big disk is still 45 GB: not critical */

    /* A small partition: nearly empty in bytes is fine while it is nearly empty in use too */
    assert(disk_level(856 * MB, 1000 * MB, 0) == 0);        /* a boot partition that is 14% used */
    assert(disk_level(90 * MB, 1000 * MB, 0) == 2);         /* low in percent and in bytes: critical */
    assert(disk_level(900 * MB, 10 * GB, 0) == 2);          /* 8.8% of 10 GB, under 1 GB left */
    assert(disk_level(2 * GB, 10 * GB, 0) == 0);

    /* Hysteresis: a level is left only after recovering past a margin */
    assert(disk_level(11 * GB, 100 * GB, 1) == 1);          /* 11%: not yet recovered from "low" */
    assert(disk_level(13 * GB, 100 * GB, 1) == 0);
    assert(disk_level(6 * GB, 100 * GB, 2) == 2);           /* 6% after critical: still critical */
    assert(disk_level(7 * GB, 100 * GB, 2) == 2);
    assert(disk_level(9 * GB, 100 * GB, 2) == 1);           /* 9%: recovered from critical, still low */
    assert(disk_level(30 * GB, 100 * GB, 2) == 0);
    assert(disk_level(9 * GB, 100 * GB, 0) == 1);           /* ...but from fine, 9% is a fresh warning */

    /* Odd inputs */
    assert(disk_level(0, 0, 0) == 0);                       /* an empty filesystem reports nothing */
    assert(disk_level(0, 100 * GB, 0) == 2);
}

static void test_format(void) {
    char s[32];
    disk_format_size(5 * GB, s, sizeof s);              assert(!strcmp(s, "5.0 GB"));
    disk_format_size(1536 * MB, s, sizeof s);           assert(!strcmp(s, "1.5 GB"));
    disk_format_size(500 * MB, s, sizeof s);            assert(!strcmp(s, "500 MB"));
    disk_format_size(700 * 1024, s, sizeof s);          assert(!strcmp(s, "700 KB"));
    disk_format_size(0, s, sizeof s);                   assert(!strcmp(s, "0 KB"));
}

static void test_mountinfo(void) {
    const char *text =
        "22 1 259:2 / / rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "23 22 259:1 / /boot rw,relatime shared:2 - vfat /dev/nvme0n1p1 rw,fmask=0022\n"
        "24 22 0:21 / /run rw,nosuid shared:3 - tmpfs tmpfs rw\n"
        "25 22 7:0 / /var/lib/snapd/snap/core/1 ro,nodev - squashfs /dev/loop0 ro\n"
        "26 22 259:2 /home /home rw,relatime shared:1 - ext4 /dev/nvme0n1p2 rw\n"
        "27 22 8:17 / /mnt/my\\040drive rw,relatime - exfat /dev/sdb1 rw\n"
        "28 22 8:33 / /mnt/cd ro,relatime - iso9660 /dev/sr0 ro\n"
        "29 22 8:49 / /mnt/rodisk ro,relatime - ext4 /dev/sdd1 ro\n"
        "30 22 0:40 / /mnt/net rw - nfs4 server:/export rw\n"
        "31 22 0:41 / /sys rw - sysfs sysfs rw\n"
        "this line is not a mount\n"
        "32 22 8:65 / /mnt/optional rw,relatime shared:9 master:2 - xfs /dev/sde1 rw\n"
        "33 22 8:81 / /mnt/roword rw,errors=remount-ro - ext4 /dev/sdf1 rw\n";
    Mount m[16];
    int n = disk_parse_mountinfo(text, m, 16);
    assert(n == 5);
    assert(!strcmp(m[0].mount, "/") && !strcmp(m[0].dev, "259:2"));
    assert(!strcmp(m[1].mount, "/boot"));
    assert(!strcmp(m[2].mount, "/mnt/my drive"));            /* \040 is a space */
    assert(!strcmp(m[3].mount, "/mnt/optional") && !strcmp(m[3].dev, "8:65"));   /* optional fields skipped */
    assert(!strcmp(m[4].mount, "/mnt/roword"));              /* "remount-ro" in the options is not "ro" */

    assert(disk_parse_mountinfo(text, m, 2) == 2);           /* capped */
    assert(disk_parse_mountinfo("", m, 16) == 0);
    assert(disk_parse_mountinfo("garbage - \n- - -\n", m, 16) == 0);

    char esc[] = "a\\134b\\012c\\040d\\05x";
    unescape(esc);
    assert(!strcmp(esc, "a\\b\nc d\\05x"));                   /* backslash, newline, space; junk left alone */
}

static void test_alert_sequence(void) {
    memset(filesystems, 0, sizeof filesystems);
    ntoasts = asked = 0;
    allow = true;

    disk_evaluate("8:1", "/data", 50 * GB, 100 * GB, true);
    assert(ntoasts == 0 && asked == 0);

    disk_evaluate("8:1", "/data", 8 * GB, 100 * GB, true);   /* 8%: low */
    assert(ntoasts == 1 && last_level == LEVEL_WARN && !strcmp(toasts[0], "Disk space low — /data: 8% free (8.0 GB)"));
    assert(!strcmp(last_key, "disk:/data"));

    disk_evaluate("8:1", "/data", 7 * GB, 100 * GB, true);   /* no worse: nothing */
    assert(ntoasts == 1);

    disk_evaluate("8:1", "/data", 3 * GB, 100 * GB, true);   /* 3%: almost full */
    assert(ntoasts == 2 && last_level == LEVEL_CRIT && !strcmp(toasts[1], "Disk almost full — /data: 3% free (3.0 GB)"));
    disk_evaluate("8:1", "/data", 2 * GB, 100 * GB, true);
    assert(ntoasts == 2);

    disk_evaluate("8:1", "/data", 30 * GB, 100 * GB, true);  /* recovered: silent */
    assert(ntoasts == 2 && filesystems[0].level == 0);
    disk_evaluate("8:1", "/data", 8 * GB, 100 * GB, true);   /* and it can alert again */
    assert(ntoasts == 3);

    /* Another filesystem has its own state */
    disk_evaluate("8:2", "/home", 9 * GB, 100 * GB, true);
    assert(ntoasts == 4 && strstr(toasts[3], "/home"));

    /* Moved to another mount point: still the same filesystem, no repeat */
    disk_evaluate("8:2", "/mnt/home", 9 * GB, 100 * GB, true);
    assert(ntoasts == 4 && !strcmp(filesystems[1].mount, "/mnt/home"));

    /* Start-up readings that are not announced still set the level, so nothing repeats later */
    memset(filesystems, 0, sizeof filesystems);
    ntoasts = 0;
    disk_evaluate("8:3", "/quiet", 3 * GB, 100 * GB, false);
    assert(ntoasts == 0 && filesystems[0].level == 2);
    disk_evaluate("8:3", "/quiet", 3 * GB, 100 * GB, true);
    assert(ntoasts == 0);

    /* "Same as before the restart" (alert_allowed says no): not shown, but not repeated either */
    memset(filesystems, 0, sizeof filesystems);
    allow = false;
    disk_evaluate("8:4", "/held", 3 * GB, 100 * GB, true);
    assert(ntoasts == 0 && filesystems[0].level == 2);
    allow = true;
    disk_evaluate("8:4", "/held", 3 * GB, 100 * GB, true);
    assert(ntoasts == 0);

    /* More filesystems than slots: the extra ones are ignored, not a crash */
    memset(filesystems, 0, sizeof filesystems);
    char dev[24], mnt[32];
    for (int i = 0; i < MAX_FS + 4; i++) {
        snprintf(dev, sizeof dev, "9:%d", i);
        snprintf(mnt, sizeof mnt, "/m%d", i);
        disk_evaluate(dev, mnt, 50 * GB, 100 * GB, true);
    }
}

static void test_real_system(void) {
    /* The real mount table and statvfs: this machine's root filesystem must be found */
    memset(filesystems, 0, sizeof filesystems);
    setenv("PACHUD_DISK_INTERVAL", "0", 1);
    unsetenv("PACHUD_DISK");
    unsetenv("PACHUD_DISK_WARN");
    unsetenv("PACHUD_DISK_CRIT");
    registered = 0;
    disk_init();
    assert(registered == 1 && mounts_fd >= 0 && timer_fd < 0);   /* only the mount table: interval 0 */
    int tracked = 0;
    for (int i = 0; i < MAX_FS; i++) tracked += filesystems[i].dev[0] != '\0';
    assert(tracked >= 1);

    /* With an interval the timer is registered too */
    setenv("PACHUD_DISK_INTERVAL", "300", 1);
    registered = 0;
    disk_init();
    assert(registered == 2 && timer_fd >= 0);

    /* Switched off: nothing registered */
    setenv("PACHUD_DISK", "0", 1);
    registered = 0;
    disk_init();
    assert(registered == 0);
    unsetenv("PACHUD_DISK");

    /* Thresholds from the environment, and nonsense falls back */
    setenv("PACHUD_DISK_WARN", "20", 1);
    setenv("PACHUD_DISK_CRIT", "8", 1);
    disk_init();
    assert(warn_pct == 20 && crit_pct == 8);
    setenv("PACHUD_DISK_WARN", "abc", 1);
    setenv("PACHUD_DISK_CRIT", "99", 1);                          /* out of range: default, then kept below warn */
    disk_init();
    assert(warn_pct == 10 && crit_pct == 5);
    setenv("PACHUD_DISK_WARN", "6", 1);
    setenv("PACHUD_DISK_CRIT", "30", 1);                          /* in range but not below warn: pulled down */
    disk_init();
    assert(warn_pct == 6 && crit_pct == 5);
}

int main(void) {
    test_levels();
    test_format();
    test_mountinfo();
    test_alert_sequence();
    test_real_system();
    puts("disk: ok");
    return 0;
}
