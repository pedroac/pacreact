/* The memory alert: its cooldown, and how it interacts with the start-up grace period.
 * src_mem.c is included so alert() can be called directly. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int toasts;
static Level last_level;
static bool allow = true;
static int asked;

static char toast_icon[96];   /* the status icon of the last toast, for the tests that care */
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    extern void hud_toast(const char *, Level, int);
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    hud_toast(t, l, ms);
}
void hud_toast(const char *t, Level l, int ms) { (void)t; (void)ms; toasts++; last_level = l; }
void sound_play(SoundKind k) { (void)k; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
bool alert_allowed(const char *source, int level) {
    assert(!strcmp(source, "memory") && (level == LEVEL_WARN || level == LEVEL_CRIT));
    asked++;
    return allow;
}

#include "../src/src_mem.c"

static void reset(void) { last_alert = 0; last_level = LEVEL_INFO; toasts = asked = 0; allow = true; }

int main(void) {
    /* Left out as "nothing new since the restart": nothing shown... */
    reset();
    allow = false;
    alert(LEVEL_CRIT);
    assert(toasts == 0 && asked == 1);
    /* ...and no cooldown started, so the same alert allowed a moment later is shown */
    allow = true;
    alert(LEVEL_CRIT);
    assert(toasts == 1 && last_level == LEVEL_CRIT && asked == 2);

    /* Having shown it, the cooldown holds repeats and milder ones back without even asking */
    alert(LEVEL_CRIT);
    alert(LEVEL_WARN);
    assert(toasts == 1 && asked == 2);

    /* A warning does not hold back the critical alert that follows it: escalation passes */
    reset();
    alert(LEVEL_WARN);
    assert(toasts == 1 && last_level == LEVEL_WARN);
    alert(LEVEL_WARN);
    assert(toasts == 1);                                       /* the repeat is held back */
    alert(LEVEL_CRIT);
    assert(toasts == 2 && last_level == LEVEL_CRIT);           /* the worse one is not */
    alert(LEVEL_CRIT);
    assert(toasts == 2);

    /* An escalation can itself be left out (the same critical alert as before the restart) */
    reset();
    alert(LEVEL_WARN);
    allow = false;
    alert(LEVEL_CRIT);
    assert(toasts == 1 && last_level == LEVEL_WARN);           /* nothing recorded as shown */

    puts("mem: ok");
    return 0;
}
