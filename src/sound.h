#ifndef SOUND_H
#define SOUND_H

#include <stdbool.h>
#include <stddef.h>

/* Sound is played by an external player (pw-play or paplay), so nothing audio-related is
 * linked into the daemon. Alarms and timers play a sound by default; everything else is
 * silent unless asked for, in the environment or, for notifications, by the sending app. */

typedef enum {
    SOUND_ALARM,   /* a timer or alarm went off */
    SOUND_NOTIFY,  /* a notification card, when PACHUD_NOTIFY_SOUND is set */
    SOUND_ALERT,   /* battery or memory alerts, when PACHUD_ALERT_SOUND is set */
    SOUND_VOLUME,  /* the output volume or mute changed, when PACHUD_SOUND_VOLUME is set; it answers
                    * something the user just did, so do-not-disturb does not hold it back */
    /* Things that merely happened: opt-in, one variable each for the pair (PACHUD_SOUND_POWER,
     * PACHUD_SOUND_DEVICES, PACHUD_SOUND_NETWORK), and held back while do-not-disturb is on. */
    SOUND_POWER_PLUGGED, SOUND_POWER_UNPLUGGED,    /* the charger */
    SOUND_DEVICE_ADDED, SOUND_DEVICE_REMOVED,      /* a headset, headphones, a Bluetooth device */
    SOUND_NETWORK_UP, SOUND_NETWORK_DOWN,          /* the default route appeared or went */
    SOUND_KINDS
} SoundKind;

void sound_init(void);
void sound_play(SoundKind kind);
/* While quiet, the opt-in event sounds above are not played (do-not-disturb); alarms, notification
 * sounds that are not up to do-not-disturb, and alerts are unaffected. */
void sound_set_quiet(bool quiet);
/* A sound requested by a notification (its sound-file or sound-name hint): an absolute file
 * path or a name from the freedesktop sound theme, such as "message". */
void sound_play_spec(const char *spec);

/* Exposed for testing: resolves a file path or theme name to an existing absolute path. */
bool sound_resolve(const char *spec, char *out, size_t n);

#endif
