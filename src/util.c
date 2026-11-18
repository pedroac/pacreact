#include "util.h"
#include "i18n.h"
#include "setting.h"
#include <errno.h>
#include <math.h>
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
        fprintf(stderr, _("pacreact: %s must be a whole number of %s, not \"%s\"; ignored\n"), name, unit, value);
        return def;
    }
    if (n < lo || n > hi) {
        long to = n < lo ? lo : hi;
        fprintf(stderr, _("pacreact: %s=%ld is outside %ld to %ld; using %ld\n"), name, n, lo, hi, to);
        return to;
    }
    return n;
}

long setting_int(const char *name, long def, long lo, long hi, const char *unit) {
    return int_value(name, pac_setting(name), def, lo, hi, unit);
}

Size setting_size(const char *name, long lo, long hi) {
    Size size = { 0 };
    const char *value = pac_setting(name);
    if (!value || !*value) return size;
    char *end;
    errno = 0;
    strtol(value, &end, 10);
    if (end != value && !*end && errno != ERANGE) {
        size.px = int_value(name, value, 0, lo, hi, "pixels");
        size.set = true;
    } else if (pac_length_parse(value, &size.len)) {
        size.set = size.unit = true;
    } else {
        fprintf(stderr, _("pacreact: %s must be a whole number of pixels or a length such as 4mm, 0.2in, 10pt or 5%%, not \"%s\"; ignored\n"), name, value);
    }
    return size;
}

long size_px(const Size *size, long def, long lo, long hi, double px_per_mm, double whole) {
    if (!size->set) return def;
    long px = size->unit ? lround(pac_length_px(size->len, px_per_mm, whole)) : size->px;
    return px < lo ? lo : px > hi ? hi : px;
}
