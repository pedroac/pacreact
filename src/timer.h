#ifndef TIMER_H
#define TIMER_H

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Loads persisted entries and arms the timerfd. */
void timer_init(void);
/* Handles the IPC commands timer, alarm, list and cancel. Returns false if cmd is not one of
 * them. args is the rest of the message after the command (tab-separated). */
bool timer_command(const char *cmd, char *args, char *reply, size_t n);

/* "1h30m", "90s", "5m": every part needs a unit. Seconds, or -1 if invalid. */
long timer_parse_duration(const char *s);
/* "HH:MM": the next such wall-clock time after now. Returns -1 if invalid. */
time_t timer_parse_alarm(const char *s, time_t now);
/* "daily", "weekdays", "weekends", or a list such as "mon,wed,fri". Returns a bitmask of
 * days (bit 0 = Sunday, like tm_wday), or -1 if invalid. */
int timer_parse_recur(const char *s);
/* The first HH:MM strictly after `after` that falls on one of `days` (0 = any day), or -1. */
time_t timer_next_alarm(int hour, int min, unsigned days, time_t after);

#endif
