/* CPU and SSD temperature: the thresholds, hysteresis, polling interval, and the whole check
 * against a mock hwmon tree. src_temp.c is included for its statics. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include "setting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char toasts[8][128];
static int ntoasts, registered;
static Level last_level;
static bool allow = true;
static char last_key[32];

static char toast_icon[96];   /* the status icon of the last toast, for the tests that care */
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    extern void hud_toast(const char *, Level, int);
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    hud_toast(t, l, ms);
}
void hud_toast(const char *t, Level l, int ms) { (void)ms; snprintf(toasts[ntoasts++ % 8], sizeof toasts[0], "%s", t); last_level = l; }
void sound_play(SoundKind k) { (void)k; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
bool alert_allowed(const char *s, int level) { (void)level; snprintf(last_key, sizeof last_key, "%s", s); return allow; }

#include "../src/src_temp.c"

static char root[64];

static void put(const char *rel, const char *content) {
    char path[512], cmd[600];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    snprintf(cmd, sizeof cmd, "mkdir -p \"$(dirname %s)\"", path);
    assert(system(cmd) == 0);
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void set_temp(const char *rel, int celsius) {
    char v[16];
    snprintf(v, sizeof v, "%d\n", celsius * 1000);
    put(rel, v);
}

static void test_limits(void) {
    Limits l = temp_limits(CLASS_CPU, 100);   assert(l.warn == 85 && l.hot == 95);
    l = temp_limits(CLASS_CPU, 110);          assert(l.warn == 85 && l.hot == 95);    /* never above the defaults */
    l = temp_limits(CLASS_CPU, 90);           assert(l.warn == 75 && l.hot == 85);    /* a lower own limit wins */
    l = temp_limits(CLASS_CPU, 75);           assert(l.warn == 60 && l.hot == 70);
    l = temp_limits(CLASS_SSD, 79);           assert(l.warn == 73 && l.hot == 77);
    l = temp_limits(CLASS_SSD, 80);           assert(l.warn == 74 && l.hot == 78);

    Limits c = { 85, 95 };
    assert(temp_level(60, c, 0) == 0 && temp_level(84, c, 0) == 0);
    assert(temp_level(85, c, 0) == 1 && temp_level(94, c, 0) == 1);
    assert(temp_level(95, c, 0) == 2 && temp_level(120, c, 0) == 2);
    /* hysteresis: down only once 5 degrees under the threshold */
    assert(temp_level(81, c, 1) == 1 && temp_level(80, c, 1) == 0);
    assert(temp_level(91, c, 2) == 2 && temp_level(90, c, 2) == 1);
    assert(temp_level(80, c, 2) == 0 && temp_level(81, c, 2) == 1);
    assert(temp_level(84, c, 0) == 0);                          /* but from fine, 84 is fine */

    /* the polling interval follows the margin to the warning level */
    assert(temp_interval(40, 0, 120) == 120 && temp_interval(25, 0, 120) == 120);
    assert(temp_interval(24, 0, 120) == 30 && temp_interval(10, 0, 120) == 30);
    assert(temp_interval(9, 0, 120) == 10 && temp_interval(-3, 0, 120) == 10);
    assert(temp_interval(40, 1, 120) == 10);                    /* already high: keep watching closely */
    assert(temp_interval(40, 0, 60) == 60 && temp_interval(15, 0, 20) == 20);   /* never slower than asked */
    assert(temp_interval(5, 0, 5) == 5);                         /* a very short maximum is honoured */
}

static void build_tree(void) {
    snprintf(root, sizeof root, "/tmp/pacreact-temp-XXXXXX");
    assert(mkdtemp(root));
    put("hwmon0/name", "coretemp\n");
    set_temp("hwmon0/temp1_input", 46);  set_temp("hwmon0/temp1_crit", 100);
    set_temp("hwmon0/temp2_input", 45);  set_temp("hwmon0/temp2_crit", 100);
    put("hwmon1/name", "nvme\n");
    set_temp("hwmon1/temp1_input", 27);  set_temp("hwmon1/temp1_crit", 79);
    put("hwmon2/name", "iwlwifi_1\n");  set_temp("hwmon2/temp1_input", 99);   /* not watched */
    put("hwmon3/name", "acpitz\n");     set_temp("hwmon3/temp1_input", 99);   /* not watched */
    put("hwmon4/name", "k10temp\n");    set_temp("hwmon4/temp1_input", 40);   /* no limit: the default */
    hwmon_root = root;
}

static void test_check(void) {
    build_tree();
    memset(levels, 0, sizeof levels);
    ntoasts = 0;

    /* Everything cool: nothing, and a relaxed timer (closest margin: 85 - 46) */
    assert(check(true) == 120 && ntoasts == 0);

    /* A hot CPU: high, then critical, each once */
    set_temp("hwmon0/temp1_input", 86);
    assert(check(true) == 10);
    assert(ntoasts == 1 && last_level == LEVEL_WARN && !strcmp(toasts[0], "CPU temperature high — 86 °C"));
    assert(!strcmp(last_key, "temp-cpu"));
    check(true);
    assert(ntoasts == 1);
    set_temp("hwmon0/temp1_input", 96);
    check(true);
    assert(ntoasts == 2 && last_level == LEVEL_CRIT && !strcmp(toasts[1], "CPU temperature critical — 96 °C"));
    check(true);
    assert(ntoasts == 2);

    /* Cooling down: nothing is said, and the alert can come back only after real recovery */
    set_temp("hwmon0/temp1_input", 91);
    check(true);
    assert(ntoasts == 2 && levels[CLASS_CPU] == 2);              /* 91 is still "critical" (95 - 5 = 90) */
    set_temp("hwmon0/temp1_input", 70);
    assert(check(true) == 30);                                   /* 15 degrees below the warning level */
    assert(levels[CLASS_CPU] == 0);
    set_temp("hwmon0/temp1_input", 86);
    check(true);
    assert(ntoasts == 3);

    /* The hottest of a class's sensors is what is reported (a core, not the package) */
    set_temp("hwmon0/temp1_input", 50);
    check(true);
    set_temp("hwmon0/temp2_input", 97);
    check(true);
    assert(ntoasts == 4 && strstr(toasts[3], "critical — 97"));
    set_temp("hwmon0/temp2_input", 45);
    check(true);

    /* An SSD has its own wording and its own, lower, limits (79: high at 73, critical at 77) */
    set_temp("hwmon1/temp1_input", 74);
    check(true);
    assert(ntoasts == 5 && !strcmp(toasts[4], "SSD temperature high — 74 °C") && !strcmp(last_key, "temp-ssd"));
    set_temp("hwmon1/temp1_input", 78);
    check(true);
    assert(ntoasts == 6 && !strcmp(toasts[5], "SSD temperature critical — 78 °C"));
    set_temp("hwmon1/temp1_input", 30);
    set_temp("hwmon0/temp1_input", 40);
    check(true);
    assert(levels[CLASS_SSD] == 0 && levels[CLASS_CPU] == 0);

    /* Sensors that are not watched, and unreadable files, change nothing */
    set_temp("hwmon2/temp1_input", 120);
    put("hwmon4/temp2_input", "not a number\n");
    put("hwmon4/temp3_input", "");
    int before = ntoasts;
    check(true);
    assert(ntoasts == before);

    /* A device's own limit counts: 90 makes this CPU high at 75 */
    set_temp("hwmon4/temp1_crit", 90);
    set_temp("hwmon4/temp1_input", 76);
    check(true);
    assert(ntoasts == before + 1 && strstr(toasts[(ntoasts - 1) % 8], "CPU temperature high — 76"));
    set_temp("hwmon4/temp1_input", 40);
    check(true);
    /* a bogus limit (under 20 degrees) is ignored in favour of the default; "max" is the fallback */
    set_temp("hwmon4/temp1_crit", 5);
    set_temp("hwmon4/temp1_input", 80);
    int n2 = ntoasts;
    check(true);
    assert(ntoasts == n2);                                       /* 80 is fine under the default 100 */
    put("hwmon4/temp1_crit", "");
    set_temp("hwmon4/temp1_max", 85);                            /* an unreadable crit falls back to max */
    check(true);
    assert(ntoasts == n2 + 1);
    set_temp("hwmon4/temp1_input", 40);
    check(true);

    /* Silent start-up readings still set the level: nothing repeats later */
    memset(levels, 0, sizeof levels);
    set_temp("hwmon0/temp1_input", 97);
    int n3 = ntoasts;
    check(false);
    assert(ntoasts == n3 && levels[CLASS_CPU] == 2);
    check(true);
    assert(ntoasts == n3);

    /* "The same as before the restart": not shown, and not repeated */
    memset(levels, 0, sizeof levels);
    allow = false;
    check(true);
    assert(ntoasts == n3 && levels[CLASS_CPU] == 2);
    allow = true;
    check(true);
    assert(ntoasts == n3);

    /* No sensors at all (or no hwmon directory): a rare look, no crash */
    hwmon_root = "/nonexistent-hwmon";
    memset(levels, 0, sizeof levels);
    assert(check(true) == max_interval_s);

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
}

static void test_init(void) {
    hwmon_root = "/sys/class/hwmon";
    pac_setting_set("temp", NULL);
    pac_setting_set("temp_interval", NULL);
    registered = 0;
    temp_poke();                                            /* before init: harmless */
    temp_init();
    assert(registered == 1 && timer_fd >= 0 && max_interval_s == 120);

    pac_setting_set("temp_interval", "30");       temp_init();   assert(max_interval_s == 30);
    pac_setting_set("temp_interval", "0");        temp_init();   assert(max_interval_s == 0);   /* kernel events only */
    const char *bad[] = { "abc", "1.5", "5s" };
    for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) {
        pac_setting_set("temp_interval", bad[i]);
        max_interval_s = 77;
        temp_init();
        assert(max_interval_s == SRC_TEMP_DEFAULT_INTERVAL_S);       /* not a number: the default */
    }
    pac_setting_set("temp_interval", "-1");   temp_init();   assert(max_interval_s == 0);      /* out of range: */
    pac_setting_set("temp_interval", "99999"); temp_init();  assert(max_interval_s == 3600);   /* the nearest end */
    pac_setting_set("temp_interval", NULL);
    max_interval_s = SRC_TEMP_DEFAULT_INTERVAL_S;

    pac_setting_set("temp", "0");
    registered = 0;
    temp_init();
    assert(registered == 0);
    pac_setting_set("temp", NULL);

    /* CPU thresholds from the environment; nonsense falls back, and critical stays above high */
    pac_setting_set("temp_warn", "70");
    pac_setting_set("temp_crit", "80");
    temp_init();
    assert(cpu_warn_c == 70 && cpu_hot_c == 80);
    Limits l = temp_limits(CLASS_CPU, 100);
    assert(l.warn == 70 && l.hot == 80);
    pac_setting_set("temp_warn", "abc");
    pac_setting_set("temp_crit", "abc");
    temp_init();
    assert(cpu_warn_c == 85 && cpu_hot_c == 95);
    pac_setting_set("temp_crit", "5");                             /* below the range: its lowest, then pushed above high */
    temp_init();
    assert(cpu_warn_c == 85 && cpu_hot_c == 86);
    pac_setting_set("temp_warn", "90");
    pac_setting_set("temp_crit", "60");                            /* critical below high: pushed above it */
    temp_init();
    assert(cpu_warn_c == 90 && cpu_hot_c == 91);
    pac_setting_set("temp_warn", NULL);
    pac_setting_set("temp_crit", NULL);
    temp_init();
    assert(cpu_warn_c == 85 && cpu_hot_c == 95);
}

static void test_real_sensors(void) {
    hwmon_root = "/sys/class/hwmon";
    Reading r[CLASS_COUNT];
    memset(levels, 0, sizeof levels);
    read_all(r);
    if (!r[CLASS_CPU].present) {
        puts("temp: (real-sensor check skipped: no coretemp/k10temp here)");
        return;
    }
    assert(r[CLASS_CPU].hottest > 10 && r[CLASS_CPU].hottest < 115);       /* a plausible CPU temperature */
    assert(r[CLASS_CPU].margin < 1000);
}

int main(void) {
    test_limits();
    test_check();
    test_init();
    test_real_sensors();
    puts("temp: ok");
    return 0;
}
