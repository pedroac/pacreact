#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stddef.h>

/* Path of a file in pachud's state directory ($XDG_STATE_HOME/pachud, default
 * ~/.local/state/pachud). make_dir creates the directory first (for writing). */
void state_file(const char *name, char *out, size_t n, bool make_dir);

#endif
