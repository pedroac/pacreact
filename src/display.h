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
/* The scale buffers are rendered at, in 120ths (see scale.h). */
int display_scale(Display *dpy);
/* Called when the compositor reports a different preferred scale while a HUD is up: the owner
 * should draw it again so it is sharp on the monitor it is on. */
void display_on_rescale(Display *dpy, void (*fn)(void));

#endif
