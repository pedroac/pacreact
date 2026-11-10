#ifndef NOTIFY_H
#define NOTIFY_H

/* Becomes the session's org.freedesktop.Notifications server. If another daemon owns the
 * name (mako, dunst, ...) it says so and notifications stay disabled. */
void notify_init(void);

#endif
