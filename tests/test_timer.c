/* Duration and alarm-time parsing. */
#include "../src/timer.h"
#include "../src/hud.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

void hud_toast(const char *t, Level l, int ms) { (void)t; (void)l; (void)ms; }
int loop_add(int fd, unsigned ev, void *fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

int main(void) {
    assert(timer_parse_duration("90s") == 90);
    assert(timer_parse_duration("5m") == 300);
    assert(timer_parse_duration("1h30m") == 5400);
    assert(timer_parse_duration("2d") == 172800);
    assert(timer_parse_duration("5") == -1);      /* unit required */
    assert(timer_parse_duration("") == -1);
    assert(timer_parse_duration("m5") == -1);
    assert(timer_parse_duration("0s") == -1);
    assert(timer_parse_duration("31d") == -1);
    assert(timer_parse_duration("99999999999s") == -1);

    setenv("TZ", "UTC", 1);
    tzset();
    time_t noon = 1700049600;  /* 2023-11-15 12:00:00 UTC */
    assert(timer_parse_alarm("13:30", noon) == noon + 5400);
    assert(timer_parse_alarm("12:00", noon) == noon + 86400);   /* not in the future: tomorrow */
    assert(timer_parse_alarm("07:30", noon) == noon + 19 * 3600 + 30 * 60);
    assert(timer_parse_alarm("24:00", noon) == -1);
    assert(timer_parse_alarm("7", noon) == -1);
    assert(timer_parse_alarm("07:30pm", noon) == -1);

    /* Recurrence: noon on Wednesday 2023-11-15 (tm_wday 3). */
    assert(timer_parse_recur("daily") == 0x7F);
    assert(timer_parse_recur("weekdays") == 0x3E);
    assert(timer_parse_recur("weekends") == 0x41);
    assert(timer_parse_recur("mon,wed,fri") == ((1 << 1) | (1 << 3) | (1 << 5)));
    assert(timer_parse_recur("Sat,SUNDAY") == 0x41);
    assert(timer_parse_recur("") == -1);
    assert(timer_parse_recur("funday") == -1);
    assert(timer_parse_recur("mon,") == 0x02);          /* trailing comma is harmless */
    assert(timer_parse_recur("mon wed") == -1);

    const unsigned weekdays = 0x3E, weekends = 0x41;
    assert(timer_next_alarm(13, 0, weekdays, noon) == noon + 3600);              /* later today */
    assert(timer_next_alarm(7, 30, weekdays, noon) == noon + 19 * 3600 + 1800);  /* Thu */
    assert(timer_next_alarm(13, 0, weekends, noon) == noon + 3 * 86400 + 3600);  /* Sat */
    assert(timer_next_alarm(12, 0, 1u << 3, noon) == noon + 7 * 86400);          /* Wed only: next week */
    assert(timer_next_alarm(9, 0, 0, noon) == noon + 21 * 3600);                 /* any day: tomorrow */
    /* Friday evening to Monday morning skips the weekend. */
    time_t fri = noon + 2 * 86400 + 8 * 3600;  /* Fri 20:00 */
    assert(timer_next_alarm(7, 0, weekdays, fri) == fri + 59 * 3600);            /* Mon 07:00 */

    puts("timer: ok");
    return 0;
}
