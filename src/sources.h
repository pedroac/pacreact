#ifndef SOURCES_H
#define SOURCES_H

#include <stdbool.h>
#include <stddef.h>

/* pacreact watches nothing itself any more: pacreactd does (the battery, the sound, the devices, the network, the screen,
 * printing, disk space, ...) and tells it, and pacreact decides what is worth showing. */
void daemon_init(void);     /* connects to pacreactd, and shows what it says: alerts, bars and messages */
/* Tells pacreactd (and so pacbar and the others) a key, in a namespace pacreact owns while it owns the source: "notifications.count",
 * "audio.volume". NULL says it is gone. Said again by itself if the daemon restarts. See pacreactd's docs/protocol.md. */
void daemon_publish(const char *key, const char *value);
void jack_describe(char *out, size_t n);   /* what is plugged in, as words each led by a space: " headset", " none"... (from pacreactd; src_daemon.c) */
#endif
