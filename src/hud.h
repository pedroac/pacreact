#ifndef HUD_H
#define HUD_H

#include "display.h"
#include "image.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum { LEVEL_INFO, LEVEL_WARN, LEVEL_CRIT } Level;

void hud_init(Display *dpy);
/* Shows a one-line toast for timeout_ms, stacked with the others. The same text again refreshes
 * the one already shown. */
void hud_toast(const char *text, Level level, int timeout_ms);
/* Shows (or refreshes) a labelled level bar in the stack. Values above 100 are shown but the bar stays
 * full; dim greys it out (muted). */
void hud_slider(const char *label, int percent, bool dim);

/* OSD (on-screen display) versions: centered on screen, single instance, no queue. */
void hud_osd_slider(const char *label, int percent, bool dim, OsdAnchor anchor);
void hud_osd_hide(void);
#define HUD_MAX_ACTIONS 3

/* A notification card: summary, wrapped body, an optional icon (raw pixels, else a PNG path)
 * and optional actions. A card with the id of one already on screen or queued replaces it in
 * place. Cards stack, newest on top; when the stack is full they wait for room. Every string
 * is copied. */
typedef struct {
    uint32_t id;
    const char *summary, *body;
    const char *icon_path;        /* absolute path to a PNG, or "" */
    const uint8_t *image;         /* IMAGE_BYTES of premultiplied ARGB32, or NULL; wins over icon_path */
    Level level;
    int timeout_ms;
    const char *const *actions;   /* key, label, key, label, ... */
    int n_actions;                /* the number of strings in `actions`: two per action */
} HudCard;

void hud_card(const HudCard *card);

/* The key of the n-th (1-based) action of the newest card on screen, or NULL if it has no such
 * action. Also gives the card's notification id. The key stays valid until the HUD changes.
 * The "default" action is only offered when it is the card's only one. */
const char *hud_action_key(int n, uint32_t *id);

/* Reasons passed to the closed callback, matching the notification spec. */
enum { HUD_CLOSED_EXPIRED = 1, HUD_CLOSED_DISMISSED = 2, HUD_CLOSED_OTHER = 4 };
typedef void (*HudClosedFn)(uint32_t id, int reason);
/* Called when a card leaves the screen or the queue for any reason but being replaced or
 * removed through hud_remove(). */
void hud_on_closed(HudClosedFn fn);
/* Silently removes a card, shown or queued. Returns whether it existed. */
bool hud_remove(uint32_t id);
/* Closes the newest alert on screen (or the newest item when there is none). */
void hud_dismiss(void);
/* Closes the item with this id as dismissed by the user. Returns whether it was on screen. */
bool hud_dismiss_id(uint32_t id);

/* Milliseconds until the visible HUD expires, or -1 when nothing is shown. */
int hud_timeout_ms(void);
/* Advances the HUD if the visible item has expired. */
void hud_tick(void);

#endif
