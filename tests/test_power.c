/* Exercises the battery/charger decision logic and uevent parsing without a display. */
#include "../src/hud.h"
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

void hud_toast(const char *text, Level level, int ms) {
    (void)ms;
    snprintf(last, sizeof last, "%s", text);
    last_level = level;
    shown++;
}
int loop_add(int fd, unsigned ev, void *fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

static size_t uevent(char *out, const char *props[]) {
    size_t n = 0;
    for (; *props; props++) n += sprintf(out + n, "%s", *props) + 1;
    return n;
}

int main(void) {
    battery_update(50, true);   assert(shown == 0);
    battery_update(15, true);   assert(shown == 1 && last_level == LEVEL_WARN);
    battery_update(14, true);   assert(shown == 1);                /* once per crossing */
    battery_update(5, true);    assert(shown == 2 && last_level == LEVEL_CRIT);
    battery_update(4, true);    assert(shown == 2);
    battery_update(10, false);  assert(shown == 2);                /* charging re-arms */
    battery_update(15, true);   assert(shown == 3 && last_level == LEVEL_WARN);
    battery_update(40, true);                                      /* recovered above re-arm */
    battery_update(15, true);   assert(shown == 4);

    charger_update(true);       assert(shown == 4);                /* first state is silent */
    charger_update(false);      assert(shown == 5 && !strcmp(last, "Charger disconnected"));

    char buf[512];
    const char *bat[] = { "change@/devices/BAT0", "SUBSYSTEM=power_supply", "POWER_SUPPLY_TYPE=Battery",
                          "POWER_SUPPLY_CAPACITY=3", "POWER_SUPPLY_STATUS=Discharging", NULL };
    battery_update(60, true);
    size_t n = uevent(buf, bat);
    power_uevent(buf, n);       assert(shown == 6 && !strcmp(last, "Battery critical — 3%"));

    const char *other[] = { "change@/devices/x", "SUBSYSTEM=block", "POWER_SUPPLY_TYPE=Battery", NULL };
    n = uevent(buf, other);
    power_uevent(buf, n);       assert(shown == 6);

    puts("power: ok");
    return 0;
}
