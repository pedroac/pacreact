#ifndef ACTIVITY_H
#define ACTIVITY_H

#include "display.h"
#include <stdbool.h>

/* Whether the user is at the computer, from the compositor's idle notifications (ext-idle-notify-v1,
 * which Hyprland, sway and others have). The user is idle after peek_idle minutes (default 5, 1
 * to 120) without touching the keyboard or the pointer, and has had a break once that has lasted
 * peek_break minutes (default 15, 1 to 720). Without the protocol nothing is ever reported: the
 * user is taken to be at the computer all the time. */

/* Called when the user goes idle (active false, `secs` is how long they had already been idle) and
 * when they are back (active true; `restart` is whether the time away was a whole break). */
void activity_on_change(void (*fn)(bool active, long secs, bool restart));
void activity_init(Display *dpy);
long activity_break_secs(void);

#endif
