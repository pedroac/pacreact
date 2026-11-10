#ifndef SOURCES_H
#define SOURCES_H

#include <stdbool.h>

/* Each source registers its fds with the loop and reports through hud_*(). A source whose
 * kernel interface is missing (no battery, no backlight, ...) quietly does nothing. */
void power_init(void);      /* battery level and AC, from kernel uevents */
void mem_init(void);        /* memory pressure, from PSI triggers */
void net_init(void);        /* online/offline, from rtnetlink */
bool net_online(void);      /* whether the network source currently sees a default route */
void probe_init(void);      /* opt-in latency probe (PACHUD_PROBE_HOST) */
void backlight_init(void);  /* screen brightness, from sysfs notifications */
void audio_init(void);      /* output volume and mute, from `pactl subscribe` */

#endif
