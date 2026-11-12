/* Exercises the battery/charger decision logic and uevent parsing without a display. */
#include "../src/hud.h"
#include "../src/sound.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void battery_update(int capacity, bool discharging);
void charger_update(bool online);
void power_uevent(const char *msg, size_t len);

static char last[128];
static int shown;
static Level last_level;

static char toast_icon[96];   /* the status icon of the last toast, for the tests that care */
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    extern void hud_toast(const char *, Level, int);
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    hud_toast(t, l, ms);
}
void hud_toast(const char *text, Level level, int ms) {
    (void)ms;
    snprintf(last, sizeof last, "%s", text);
    last_level = level;
    shown++;
}
static SoundKind last_sound = SOUND_KINDS;
static int nsounds;
void sound_play(SoundKind k) { last_sound = k; nsounds++; }
static int pokes;
void temp_poke(void) { pokes++; }
void usb_uevent(const char *msg, size_t len) { (void)msg; (void)len; }
static bool alerts_open = true;
bool alert_allowed(const char *source, int level) { (void)source; (void)level; return alerts_open; }
int loop_add(int fd, unsigned ev, void *fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

static size_t uevent(char *out, const char *props[]) {
    size_t n = 0;
    for (; *props; props++) n += sprintf(out + n, "%s", *props) + 1;
    return n;
}

static void test_charger_sounds(void) {
    charger_update(true);                                   /* whatever it was, it is plugged now */
    int before = nsounds;
    charger_update(true);                                   /* no change: nothing */
    assert(nsounds == before);
    charger_update(false);
    assert(nsounds == before + 1 && last_sound == SOUND_POWER_UNPLUGGED);
    assert(!strncmp(toast_icon, "battery", 7));
    charger_update(true);
    assert(nsounds == before + 2 && last_sound == SOUND_POWER_PLUGGED);
    assert(!strncmp(toast_icon, "ac-adapter", 10));
}

int main(void) {
    battery_update(50, true);   assert(shown == 0);
    battery_update(15, true);   assert(shown == 1 && last_level == LEVEL_WARN);
    assert(!strncmp(toast_icon, "battery-low", 11));
    battery_update(14, true);   assert(shown == 1);                /* once per crossing */
    battery_update(5, true);    assert(shown == 2 && last_level == LEVEL_CRIT);
    assert(!strncmp(toast_icon, "battery-caution", 15));         /* the emptier icon, and a fallback to the other */
    battery_update(4, true);    assert(shown == 2);
    battery_update(10, false);  assert(shown == 2);                /* charging re-arms */
    battery_update(15, true);   assert(shown == 3 && last_level == LEVEL_WARN);
    battery_update(40, true);                                      /* recovered above re-arm */
    battery_update(15, true);   assert(shown == 4);

    int snd = nsounds;
    charger_update(true);       assert(shown == 4 && nsounds == snd);   /* first state is silent, sound included */
    charger_update(false);      assert(shown == 5 && !strcmp(last, "Charger disconnected"));
    assert(nsounds == snd + 1 && last_sound == SOUND_POWER_UNPLUGGED);

    char buf[512];
    const char *bat[] = { "change@/devices/BAT0", "SUBSYSTEM=power_supply", "POWER_SUPPLY_TYPE=Battery",
                          "POWER_SUPPLY_CAPACITY=3", "POWER_SUPPLY_STATUS=Discharging", NULL };
    battery_update(60, true);
    size_t n = uevent(buf, bat);
    power_uevent(buf, n);       assert(shown == 6 && !strcmp(last, "Battery critical — 3%"));

    const char *other[] = { "change@/devices/x", "SUBSYSTEM=block", "POWER_SUPPLY_TYPE=Battery", NULL };
    n = uevent(buf, other);
    power_uevent(buf, n);       assert(shown == 6);

    /* An alert left out as "the same as last time" is not shown, but it still counts as given:
     * the same condition must not come back later as a fresh alert */
    battery_update(60, true);                                  /* re-arm everything */
    alerts_open = false;
    int before = shown;
    battery_update(10, true);
    assert(shown == before);                                   /* warning held back */
    alerts_open = true;
    battery_update(9, true);
    assert(shown == before);                                   /* and not repeated afterwards */
    battery_update(4, true);
    assert(shown == before + 1 && last_level == LEVEL_CRIT);   /* a worse level is a new alert */
    alerts_open = false;
    battery_update(60, true);
    battery_update(3, true);
    assert(shown == before + 1);                               /* critical held back too */
    alerts_open = true;
    battery_update(2, true);
    assert(shown == before + 1);

    test_charger_sounds();
    puts("power: ok");
    return 0;
}
