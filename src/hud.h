#ifndef HUD_H
#define HUD_H

#include "display.h"
#include "image.h"
#include <stdbool.h>
#include <stdint.h>
#include "tunables.h"

/* A timeout_ms that never runs out: the item stays until it is closed, removed or replaced. */
#define HUD_NEVER (-1)

typedef enum { LEVEL_INFO, LEVEL_WARN, LEVEL_CRIT } Level;


void hud_init(Display *dpy);
/* Tells the HUD whether speech is running, so a bar that moves does not wake the loop to say
 * something nobody will hear. Off until set. */
void hud_set_speech(bool on);
/* Shows a toast for timeout_ms, stacked with the others. Every item in the stack is the same width,
 * so a long text wraps onto up to three lines (ending in an ellipsis beyond that). The same text again refreshes
 * the one already shown. */
void hud_toast(const char *text, Level level, int timeout_ms);
/* The same with a status icon: the name of an icon of the theme ("battery-low"), or several, comma
 * separated, the first that exists being used. Without one that is found, it is just the text. */
void hud_toast_icon(const char *text, Level level, int timeout_ms, const char *icon);
/* Shows (or refreshes) a labelled level bar in the stack. Values above 100 are shown but the bar stays
 * full; dim greys it out (muted). */
void hud_slider(const char *label, int percent, bool dim, const char *icon);   /* icon as for toasts, or NULL */

/* OSD (on-screen display) versions: centered on screen, single instance, no queue. */
void hud_osd_slider(const char *label, int percent, bool dim, OsdAnchor anchor, const char *icon);
/* The time, big, in the middle of the screen for a moment (`pacreact time`): for a glance at the clock. */
void hud_osd_clock(const char *time_text, const char *detail);

/* The attention deck: a few things must not be ignored, so they do not go in the stack with the
 * rest but in a place of their own, at the top centre: cards that say they are `attention` (the
 * notification service sets it for critical urgency and for incoming calls) and toasts at the
 * critical level, which is what the system's own critical alerts are. At most three at a time, in
 * the order they arrived: the oldest unhandled one is in front as a full card and never changes
 * under the pointer; the others sit behind it as strips of one line. When the front one is handled
 * or leaves, the next comes forward; clicking a strip brings that one forward. The deck is the
 * stack (no separate place) when the stack itself is at the top centre, and when the screen is too
 * narrow for the two to sit apart (display_deck_apart).
 *
 * A notification card: summary, wrapped body, an optional icon (raw pixels, else a PNG path)
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
    bool resident;                /* stays after an action is invoked; see hud_action_invoked() */
    bool attention;               /* must not be ignored: shown in the attention deck, not the stack */
} HudCard;

void hud_card(const HudCard *card);

/* The key of the n-th (1-based) action of the front card of the attention deck, else of the newest
 * card in the stack, or NULL if it has no such
 * action. Also gives the card's notification id. The key stays valid until the HUD changes.
 * The "default" action is only offered when it is the card's only one. */
const char *hud_action_key(int n, uint32_t *id);

/* Called when a card with a `default` action is left-clicked: the app should hear it. token is an
 * activation token the app can use to take focus, or NULL when the compositor gave none; the
 * call comes a moment after the click, once the compositor has answered. The card is then handled
 * as by hud_action_invoked(). A left click on a card without a default action, and any right
 * click, dismiss the card without calling this. Clicks are taken only on cards, and only where
 * the display allows them (click). */
typedef void (*HudClickFn)(uint32_t id, const char *token);
void hud_on_click(HudClickFn fn);

/* Reasons passed to the closed callback, matching the notification spec. */
enum { HUD_CLOSED_EXPIRED = 1, HUD_CLOSED_DISMISSED = 2, HUD_CLOSED_OTHER = 4 };
typedef void (*HudClosedFn)(uint32_t id, int reason);
/* Called when a card leaves the screen or the queue for any reason but being replaced or
 * removed through hud_remove(). */
void hud_on_closed(HudClosedFn fn);
/* Silently removes a card, shown or queued. Returns whether it existed. */
bool hud_remove(uint32_t id);
/* How many notification cards are on screen or waiting for room (toasts and sliders are not
 * counted): what a status bar's bell wants to show. */
int hud_card_count(void);
/* Closes the front item of the attention deck, else the newest alert in the stack (or the newest
 * item when there is none). */
void hud_dismiss(void);
/* An action of the card with this id was invoked: the card closes as dismissed by the user, unless
 * it asked to stay (the `resident` hint). */
void hud_action_invoked(uint32_t id);
/* Opens the front card of the attention deck, else the newest card in the stack, to show more of its body, or closes it again: returns 1 when it
 * is now open, 0 when closed, -1 when there is no card. An open card does not expire. The pointer
 * over a card does the same for as long as it stays there (after a short delay). */
int hud_toggle_expand(void);
/* Closes the item with this id as dismissed by the user. Returns whether it was on screen. */
bool hud_dismiss_id(uint32_t id);

/* Milliseconds until the visible HUD expires, or -1 when nothing is shown. */
int hud_timeout_ms(void);
/* Advances the HUD if the visible item has expired. */
void hud_tick(void);

#endif
