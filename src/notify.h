#ifndef NOTIFY_H
#define NOTIFY_H

#include <stdint.h>

/* Becomes the session's org.freedesktop.Notifications server. If another daemon owns the
 * name (mako, dunst, ...) it says so and notifications stay disabled. */
void notify_init(void);

/* How long a card stays up, from its urgency hint (0 low, 1 normal, 2 critical) and the
 * requested expire_timeout in ms (-1 server default, 0 never). Exposed for testing. */
int notify_timeout_ms(int urgency, int32_t expire);

#endif
