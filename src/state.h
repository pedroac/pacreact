#ifndef STATE_H
#define STATE_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>

/* Path of a file in pacreact's state directory ($XDG_STATE_HOME/pacreact, default
 * ~/.local/state/pacreact). make_dir creates the directory first (for writing). */
void state_file(const char *name, char *out, size_t n, bool make_dir);

/* Opens PATH for writing as a new private file (mode 0600, not through a symbolic link, a stale
 * one removed first). NULL on failure. */
FILE *state_create(const char *path);

/* Opens PATH for reading, or NULL if it is not a regular file the person owns and nobody else
 * can write to (principle 42). */
FILE *state_open_read(const char *path);

#endif
