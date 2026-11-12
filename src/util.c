#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static bool executable_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

bool find_in_path(const char *name, char *out, size_t n) {
    if (strchr(name, '/')) {
        if (!executable_file(name) || strlen(name) >= n) return false;
        snprintf(out, n, "%s", name);
        return true;
    }
    const char *path = getenv("PATH");
    if (!path) return false;
    char copy[2048], full[1024];
    snprintf(copy, sizeof copy, "%s", path);
    char *save, *dir = strtok_r(copy, ":", &save);
    for (; dir; dir = strtok_r(NULL, ":", &save)) {
        int r = snprintf(full, sizeof full, "%s/%s", *dir ? dir : ".", name);
        if (r > 0 && (size_t)r < sizeof full && executable_file(full) && (size_t)r < n) {
            snprintf(out, n, "%s", full);
            return true;
        }
    }
    return false;
}

void data_dirs(char *out, size_t n) {
    const char *home = getenv("HOME"), *xdg_home = getenv("XDG_DATA_HOME"), *xdg_dirs = getenv("XDG_DATA_DIRS");
    char user[512] = "";
    if (xdg_home && *xdg_home) snprintf(user, sizeof user, "%s", xdg_home);
    else if (home) snprintf(user, sizeof user, "%s/.local/share", home);
    snprintf(out, n, "%s:%s", user, xdg_dirs && *xdg_dirs ? xdg_dirs : "/usr/local/share:/usr/share");
}

long int_value(const char *name, const char *value, long def, long lo, long hi, const char *unit) {
    if (!value || !*value) return def;
    char *end;
    errno = 0;
    long n = strtol(value, &end, 10);
    if (end == value || *end || errno == ERANGE) {
        fprintf(stderr, "pachud: %s must be a whole number of %s, not \"%s\"; ignored\n", name, unit, value);
        return def;
    }
    if (n < lo || n > hi) {
        long to = n < lo ? lo : hi;
        fprintf(stderr, "pachud: %s=%ld is outside %ld to %ld; using %ld\n", name, n, lo, hi, to);
        return to;
    }
    return n;
}

long env_int(const char *name, long def, long lo, long hi, const char *unit) {
    return int_value(name, getenv(name), def, lo, hi, unit);
}
