#ifndef CLOCK_H
#define CLOCK_H

#include <time.h>

/* The time and the date in the middle of the screen, for a moment (`pacreact time`, and the chime). */
void clock_show(void);
/* Shows it every `clock_every` minutes (default 30, so at :00 and :30; 1 to 60; 0 turns it off), counted
 * from the hour. Not while do-not-disturb is on. */
void clock_init(void);
/* The first moment after `after` that is a whole multiple of `every` minutes past the hour. */
time_t clock_next(time_t after, int every);

#endif
