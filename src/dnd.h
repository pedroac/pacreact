#ifndef DND_H
#define DND_H

#include <stdbool.h>
#include <stddef.h>

/* Do-not-disturb: while on, notification cards below critical urgency are not shown (and
 * make no sound), and are counted so turning it off can say how many were missed. Timers,
 * alarms, battery and memory alerts and sliders are not affected, apart from the opt-in sounds
 * for power, devices and network, which are held back (sound_set_quiet). It lives in memory only. */

void dnd_init(void);
bool dnd_active(void);
/* Called for each incoming notification: true if it must be dropped (and counts it). */
bool dnd_suppress(int urgency);
/* Handles "dnd" with args "", "status", "on [DURATION]", "off" or "toggle". Returns false if
 * cmd is not "dnd". */
bool dnd_command(const char *cmd, char *args, char *reply, size_t n);

#endif
