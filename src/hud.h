#ifndef HUD_H
#define HUD_H

#include "display.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { LEVEL_INFO, LEVEL_WARN, LEVEL_CRIT } Level;

void hud_init(Display *dpy);
/* Shows (or replaces) a one-line toast for timeout_ms. */
void hud_toast(const char *text, Level level, int timeout_ms);
/* Shows (or replaces) a labelled level bar. Values above 100 are shown but the bar stays
 * full; dim greys it out (muted). */
void hud_slider(const char *label, int percent, bool dim);
/* A notification card: summary, wrapped body, optional PNG icon (absolute path). A card with
 * the id of one already on screen or queued replaces it in place. Cards queue rather than
 * replace each other. */
void hud_card(uint32_t id, const char *summary, const char *body, const char *icon_path,
              Level level, int timeout_ms);

/* Reasons passed to the closed callback, matching the notification spec. */
enum { HUD_CLOSED_EXPIRED = 1, HUD_CLOSED_DISMISSED = 2, HUD_CLOSED_OTHER = 4 };
typedef void (*HudClosedFn)(uint32_t id, int reason);
/* Called when a card leaves the screen or the queue for any reason but being replaced or
 * removed through hud_remove(). */
void hud_on_closed(HudClosedFn fn);
/* Silently removes a card, shown or queued. Returns whether it existed. */
bool hud_remove(uint32_t id);
/* Closes what is on screen and moves to the next queued item. */
void hud_dismiss(void);

/* Milliseconds until the visible HUD expires, or -1 when nothing is shown. */
int hud_timeout_ms(void);
/* Advances the HUD if the visible item has expired. */
void hud_tick(void);

#endif
