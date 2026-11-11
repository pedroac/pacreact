#include "state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void state_file(const char *name, char *out, size_t n, bool make_dir) {
    const char *base = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[400];
    if (base && *base) snprintf(dir, sizeof dir, "%s", base);
    else snprintf(dir, sizeof dir, "%s/.local/state", home ? home : "/tmp");
    if (make_dir) mkdir(dir, 0700);
    strncat(dir, "/pachud", sizeof dir - strlen(dir) - 1);
    if (make_dir) mkdir(dir, 0700);
    snprintf(out, n, "%s/%s", dir, name);
}
