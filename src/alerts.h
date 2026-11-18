#ifndef ALERTS_H
#define ALERTS_H

#include <stdbool.h>

/* Some alerts describe a condition that is still true when the daemon starts: memory pressure
 * that was already there, a battery that was already low. Showing them again after every
 * restart is noise. So the last alert shown for each source is remembered (in the state
 * directory), and during a short grace period after start-up an alert that is not worse than
 * one shown in the last 30 minutes is not shown again: the same alert, or a milder one that
 * the earlier one already covered. A worse one always comes through. After the grace period
 * everything behaves as usual. startup_grace sets the period in seconds (default 60; 0 turns it off). */

void alerts_init(void);

/* Ask before showing an alert that can fire at start-up. `source` names where it comes from
 * ("memory", "battery"); `level` is how severe it is (a Level). Returns false if it should be
 * left out; otherwise it is remembered as shown and true is returned. */
bool alert_allowed(const char *source, int level);

#endif
