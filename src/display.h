#ifndef DISPLAY_H
#define DISPLAY_H

#include <cairo/cairo.h>
#include <stdbool.h>

/* The Wayland connection stays open, but the layer surface and its buffers exist only while
 * a HUD is on screen. */
typedef struct Display Display;
typedef struct Buf Buf;

Display *display_open(void);
void display_close(Display *dpy);
int display_fd(Display *dpy);
/* Call before poll() on display_fd(), then display_after_poll() with whether it was readable.
 * Both return -1 when the connection is gone. */
int display_before_poll(Display *dpy);
int display_after_poll(Display *dpy, bool readable);

/* Allocates a buffer of w x h logical pixels and returns a cairo context drawing into it
 * (already scaled for HiDPI). Hand both to display_present(). */
Buf *display_buf(Display *dpy, int w, int h, cairo_t **cr);
/* Takes ownership of buf and cr; shows the surface, or resizes it, as needed. */
void display_present(Display *dpy, Buf *buf, cairo_t *cr);
void display_hide(Display *dpy);

/* OSD (on-screen display) surface for user-triggered feedback: volume, device changes.
 * Separate from notifications; positioned at screen center. Updates in place (no queueing). */
/* Where the OSD sits: the compositor centres it on OSD_CENTER (no anchor), else it hugs that edge. */
typedef enum { OSD_CENTER, OSD_TOP, OSD_BOTTOM } OsdAnchor;
void display_osd_present(Display *dpy, Buf *buf, cairo_t *cr, OsdAnchor anchor);
void display_osd_hide(Display *dpy);
/* Clicks (PACHUD_CLICK=0 turns them off): the areas of the HUD surface that take clicks, in
 * logical pixels from its top-left corner; everywhere else clicks pass through to the windows
 * below. fn gets the position of each left or right click inside one of them; secondary is
 * true for the right button. */
typedef struct { int x, y, w, h; } ClickRect;
#define CLICK_RECTS_MAX 8
/* The HUD has two areas, each its own surface: the notification stack, and the attention deck at
 * the top centre. Clicks and rectangles are per area. */
#define DISPLAY_AREA_STACK 0
#define DISPLAY_AREA_DECK 1
void display_set_click_rects(Display *dpy, int area, const ClickRect *rects, int n);
void display_on_click(Display *dpy, void (*fn)(int area, int x, int y, bool secondary));
/* The pointer over the HUD: fn gets its position (logical pixels from the HUD's top-left corner)
 * as it enters and moves, and over = false when it leaves. Only where clicks are taken, so only
 * over notification cards (and not at all with PACHUD_CLICK=0). */
void display_on_hover(Display *dpy, void (*fn)(int area, int x, int y, bool over));
/* Asks the compositor for an activation token (xdg-activation-v1) for the click being handled, so
 * the app whose notification was clicked is allowed to take focus. fn runs with the token once
 * the compositor has answered. Returns false, with fn never called, when this is not possible
 * (no such protocol, or no click to base it on): the caller then goes on without a token. Only
 * valid inside a click callback, since it is tied to that click's input serial. */
bool display_request_token(Display *dpy, void (*fn)(const char *token, void *ctx), void *ctx);
/* The attention deck: its own surface at the top centre, for what must not be ignored. Shown and
 * hidden like the stack (buffers from display_buf), but with no fade, and it takes clicks over the
 * rectangles given for DISPLAY_AREA_DECK. */
void display_deck_present(Display *dpy, Buf *buf, cairo_t *cr);
void display_deck_hide(Display *dpy);
/* Whether the deck can sit apart from the stack, with items item_width wide. It cannot when the
 * stack itself is at the top centre (PACHUD_ANCHOR=top), and it cannot when the screen is too
 * narrow for the stack in its top corner and the deck in the middle to clear each other; the
 * attention items then join the stack. The screen is the output named by PACHUD_OUTPUT, else the
 * one the HUD last appeared on, else the narrowest; its logical width comes from xdg-output and
 * follows it as it changes. When the width is not known, it is taken to be enough. */
bool display_deck_apart(Display *dpy, int item_width);
/* The logical width of that output in pixels, or 0 if it is not known. */
int display_output_width(Display *dpy);
/* Where the stack sits horizontally within its surface: 0 left, 1 centre, 2 right (PACHUD_ANCHOR). */
int display_align(Display *dpy);
/* The scale buffers are rendered at, in 120ths (see scale.h). */
int display_scale(Display *dpy);
/* Called when the compositor reports a different preferred scale while a HUD is up: the owner
 * should draw it again so it is sharp on the monitor it is on. */
void display_on_rescale(Display *dpy, void (*fn)(void));

#endif
