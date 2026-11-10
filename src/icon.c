#include "icon.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CACHE 16

static const int sizes[] = { 48, 64, 32, 96, 128, 256, 24, 22, 16 };
static const char *const contexts[] = { "apps", "status", "devices", "actions", "places",
                                        "mimetypes", "categories", "emblems", "notifications" };

static bool readable(const char *path) { return access(path, R_OK) == 0; }

/* Copies src into out only if it fits whole: a cut-off path would just be a wrong path. */
static bool put(char *out, size_t n, const char *src) {
    size_t len = strlen(src);
    if (len >= n) return false;
    memcpy(out, src, len + 1);
    return true;
}

static bool candidate(char *cand, size_t n, const char *a, const char *b, int size,
                      const char *name, bool sxs) {
    int r = sxs ? snprintf(cand, n, "%s/%dx%d/%s/%s.png", a, size, size, b, name)
                : snprintf(cand, n, "%s/%s/%d/%s.png", a, b, size, name);
    return r > 0 && (size_t)r < n;
}

/* GTK's configured icon theme, if there is one. */
static void configured_theme(char *out, size_t n) {
    out[0] = '\0';
    const char *home = getenv("HOME"), *cfg = getenv("XDG_CONFIG_HOME");
    char path[512];
    if (cfg && *cfg) snprintf(path, sizeof path, "%s/gtk-3.0/settings.ini", cfg);
    else if (home) snprintf(path, sizeof path, "%s/.config/gtk-3.0/settings.ini", home);
    else return;

    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        if (strncmp(line, "gtk-icon-theme-name", 19)) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        eq++;
        while (*eq == ' ') eq++;
        eq[strcspn(eq, "\r\n")] = '\0';
        if (*eq && !strchr(eq, '/') && *eq != '.') snprintf(out, n, "%s", eq);
        break;
    }
    fclose(f);
}

/* Tries the two layouts themes use: SIZExSIZE/CONTEXT/ and CONTEXT/SIZE/. */
static bool in_theme_dir(const char *theme_dir, const char *name, char *out, size_t n) {
    char cand[1024];
    for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++) {
        for (size_t c = 0; c < sizeof contexts / sizeof *contexts; c++) {
            if (candidate(cand, sizeof cand, theme_dir, contexts[c], sizes[s], name, true) &&
                readable(cand) && put(out, n, cand))
                return true;
            if (candidate(cand, sizeof cand, theme_dir, contexts[c], sizes[s], name, false) &&
                readable(cand) && put(out, n, cand))
                return true;
        }
    }
    return false;
}

static bool lookup(const char *name, char *out, size_t n) {
    char theme[128], bases[2048], dir[1024];
    configured_theme(theme, sizeof theme);
    const char *themes[4] = { theme, "hicolor", "Adwaita", NULL };

    const char *home = getenv("HOME"), *xdg_home = getenv("XDG_DATA_HOME"), *xdg_dirs = getenv("XDG_DATA_DIRS");
    char user_data[512] = "";
    if (xdg_home && *xdg_home) snprintf(user_data, sizeof user_data, "%s", xdg_home);
    else if (home) snprintf(user_data, sizeof user_data, "%s/.local/share", home);
    snprintf(bases, sizeof bases, "%s:%s", user_data,
             xdg_dirs && *xdg_dirs ? xdg_dirs : "/usr/local/share:/usr/share");

    for (int t = 0; t < 3; t++) {
        if (!*themes[t]) continue;
        char copy[2048];
        snprintf(copy, sizeof copy, "%s", bases);
        char *save, *base = strtok_r(copy, ":", &save);
        for (; base; base = strtok_r(NULL, ":", &save)) {
            if (!*base) continue;
            int r = snprintf(dir, sizeof dir, "%s/icons/%s", base, themes[t]);
            if (r > 0 && (size_t)r < sizeof dir && in_theme_dir(dir, name, out, n)) return true;
        }
    }

    /* Plain pixmaps, and icons dropped straight into an icons directory */
    char copy[4096], cand[1024];
    snprintf(copy, sizeof copy, "%s:/usr/share", bases);
    char *save, *base = strtok_r(copy, ":", &save);
    for (; base; base = strtok_r(NULL, ":", &save)) {
        if (!*base) continue;
        int r = snprintf(cand, sizeof cand, "%s/pixmaps/%s.png", base, name);
        if (r > 0 && (size_t)r < sizeof cand && readable(cand) && put(out, n, cand)) return true;
        r = snprintf(cand, sizeof cand, "%s/icons/%s.png", base, name);
        if (r > 0 && (size_t)r < sizeof cand && readable(cand) && put(out, n, cand)) return true;
    }
    return false;
}

bool icon_find(const char *spec, char *out, size_t n) {
    if (!spec || !*spec) return false;
    if (!strncmp(spec, "file://", 7)) spec += 7;
    if (spec[0] == '/') return readable(spec) && put(out, n, spec);
    /* A theme name never contains a path separator: refuse anything that could wander off. */
    if (strchr(spec, '/') || spec[0] == '.' || strlen(spec) > 120) return false;

    /* The same few names come back over and over (every notification from an app). Only hits
     * are remembered, so an icon installed later is found without a restart. */
    static struct { char name[128]; char path[512]; } cache[CACHE];
    static int next;
    for (int i = 0; i < CACHE; i++) {
        if (cache[i].name[0] && !strcmp(cache[i].name, spec) && readable(cache[i].path)) {
            return put(out, n, cache[i].path);
        }
    }

    char path[512];
    bool found = lookup(spec, path, sizeof path);
    if (found) {
        int slot = next++ % CACHE;
        snprintf(cache[slot].name, sizeof cache[slot].name, "%s", spec);
        snprintf(cache[slot].path, sizeof cache[slot].path, "%s", path);
        return put(out, n, path);
    }
    return found;
}
