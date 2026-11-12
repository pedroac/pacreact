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

/* ---- status icons ---- */

static const char *const status_contexts[] = { "status", "devices", "legacy", "actions", "notifications", "apps" };
static const int status_sizes[] = { 24, 22, 32, 16, 48 };

/* The variant's file in one theme, in the layouts themes use: symbolic/CONTEXT and scalable/CONTEXT
 * (Adwaita), CONTEXT/SIZE (Breeze), SIZExSIZE/CONTEXT (the legacy raster themes). */
static bool status_in_theme(const char *dir, const char *variant, bool svg, char *out, size_t n) {
    char cand[1024];
    for (size_t c = 0; c < sizeof status_contexts / sizeof *status_contexts; c++) {
        const char *ctx = status_contexts[c];
        if (svg) {
            static const char *const vector[] = { "symbolic", "scalable" };
            for (size_t v = 0; v < 2; v++) {
                int r = snprintf(cand, sizeof cand, "%s/%s/%s/%s.svg", dir, vector[v], ctx, variant);
                if (r > 0 && (size_t)r < sizeof cand && readable(cand) && put(out, n, cand)) return true;
            }
            for (size_t s = 0; s < sizeof status_sizes / sizeof *status_sizes; s++) {
                int r = snprintf(cand, sizeof cand, "%s/%s/%d/%s.svg", dir, ctx, status_sizes[s], variant);
                if (r > 0 && (size_t)r < sizeof cand && readable(cand) && put(out, n, cand)) return true;
            }
        }
        for (size_t s = 0; s < sizeof status_sizes / sizeof *status_sizes; s++) {
            int r = snprintf(cand, sizeof cand, "%s/%dx%d/%s/%s.png", dir, status_sizes[s], status_sizes[s], ctx, variant);
            if (r > 0 && (size_t)r < sizeof cand && readable(cand) && put(out, n, cand)) return true;
            r = snprintf(cand, sizeof cand, "%s/%s/%d/%s.png", dir, ctx, status_sizes[s], variant);
            if (r > 0 && (size_t)r < sizeof cand && readable(cand) && put(out, n, cand)) return true;
        }
    }
    return false;
}

/* One name, through the themes and the places icons are kept in. */
static bool status_one(const char *name, bool svg, char *out, size_t n, bool *symbolic) {
    char theme[128], bases[2048], dir[1024], variant[160];
    configured_theme(theme, sizeof theme);
    const char *themes[5] = { theme, "hicolor", "Adwaita", "AdwaitaLegacy", NULL };

    const char *home = getenv("HOME"), *xdg_home = getenv("XDG_DATA_HOME"), *xdg_dirs = getenv("XDG_DATA_DIRS");
    char user_data[512] = "";
    if (xdg_home && *xdg_home) snprintf(user_data, sizeof user_data, "%s", xdg_home);
    else if (home) snprintf(user_data, sizeof user_data, "%s/.local/share", home);
    snprintf(bases, sizeof bases, "%s:%s", user_data, xdg_dirs && *xdg_dirs ? xdg_dirs : "/usr/local/share:/usr/share");

    for (int t = 0; t < 4; t++) {
        if (!*themes[t]) continue;
        char copy[2048];
        snprintf(copy, sizeof copy, "%s", bases);
        char *save, *base = strtok_r(copy, ":", &save);
        for (; base; base = strtok_r(NULL, ":", &save)) {
            if (!*base) continue;
            int r = snprintf(dir, sizeof dir, "%s/icons/%s", base, themes[t]);
            if (r <= 0 || (size_t)r >= sizeof dir) continue;
            for (int sym = 1; sym >= 0; sym--) {   /* symbolic first */
                snprintf(variant, sizeof variant, sym ? "%s-symbolic" : "%s", name);
                if (status_in_theme(dir, variant, svg, out, n)) {
                    *symbolic = sym;
                    return true;
                }
            }
        }
    }
    return false;
}

bool icon_find_status(const char *names, bool svg, char *out, size_t n, bool *symbolic) {
    if (!names || !*names) return false;
    char list[256];
    snprintf(list, sizeof list, "%s", names);
    char *save, *name = strtok_r(list, ",", &save);
    for (; name; name = strtok_r(NULL, ",", &save)) {
        /* A theme name never contains a path separator: refuse anything that could wander off */
        if (!*name || strchr(name, '/') || name[0] == '.' || strlen(name) > 100) continue;
        if (status_one(name, svg, out, n, symbolic)) return true;
    }
    return false;
}
