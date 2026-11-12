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
void bt_init(void);         /* Bluetooth devices connecting (needs sd-bus; not in NOTIFY=0 builds) */
void locks_init(void);     /* Caps Lock / Num Lock (opt-in: PACHUD_LOCK_KEYS=1) */
void jack_init(void);       /* audio jacks: headphones, headset, microphone, line out */
void temp_init(void);       /* CPU and SSD temperature: hwmon sensors, thermal events, adaptive timer */
void temp_poke(void);       /* a kernel thermal event happened: look now */
void disk_init(void);       /* low disk space: timer plus mount-table changes */
/* Which jack-type card ports (headphones, headset or plain microphone, line out) went from absent to
 * present (+1) or back (-1) since the last answer; 0 for no change. */
typedef struct { int headphones, headset, mic, lineout; } PortChanges;
void audio_port_changes(void (*cb)(const PortChanges *));   /* NULL when the server cannot say */
void audio_init(void);      /* output volume and mute, from `pactl subscribe` */

#endif
