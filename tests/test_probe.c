/* Network slowdown decision logic: alerts need consecutive bad probes, recovery needs
 * consecutive good ones, and neither repeats. */
#include "../src/hud.h"
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

void probe_result(bool ok, int rtt_ms);

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
int loop_add(int fd, unsigned ev, void *fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
bool net_online(void) { return true; }

int main(void) {
    probe_result(true, 20);   assert(shown == 0);
    probe_result(true, 900);  assert(shown == 0);                 /* one slow probe is noise */
    probe_result(true, 30);   assert(shown == 0);                 /* and it resets the streak */
    probe_result(true, 900);
    probe_result(true, 800);  assert(shown == 1 && last_level == LEVEL_WARN);
    assert(strstr(last, "slow") && strstr(last, "800"));
    probe_result(true, 700);  assert(shown == 1);                 /* no repeats while degraded */
    probe_result(false, 0);   assert(shown == 1);
    probe_result(true, 20);   assert(shown == 1);                 /* one good probe is not recovery */
    probe_result(true, 25);   assert(shown == 2 && !strcmp(last, "Network back to normal"));
    probe_result(true, 25);   assert(shown == 2);

    probe_result(false, 0);
    probe_result(false, 0);   assert(shown == 3 && strstr(last, "no reply"));

    puts("probe: ok");
    return 0;
}
