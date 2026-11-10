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
} SoundKind;

void sound_init(void);
void sound_play(SoundKind kind);
/* A sound requested by a notification (its sound-file or sound-name hint): an absolute file
 * path or a name from the freedesktop sound theme, such as "message". */
void sound_play_spec(const char *spec);

/* Exposed for testing: resolves a file path or theme name to an existing absolute path. */
bool sound_resolve(const char *spec, char *out, size_t n);

#endif
