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
void usb_init(void);        /* USB devices connecting: webcams, printers, tablets, drives (kernel uevents and sysfs) */
void usb_uevent(const char *msg, size_t len);   /* one kernel uevent, from the shared socket in src_power.c */
void print_init(void);       /* print jobs and printer problems, from CUPS (needs sd-bus; not in NOTIFY=0 builds) */
void print_shutdown(void);   /* gives CUPS its subscription back */
/* Handles "print": the jobs that have not finished. Returns false if cmd is not "print". */
bool print_command(const char *cmd, char *args, char *reply, size_t n);
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
