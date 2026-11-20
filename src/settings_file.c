#include "settings_file.h"
#include "config_file.h"
#include "setting.h"
#include "length.h"
#include "tunables.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* The settings the file may set: the ones the daemon reads with pac_setting(). */
static const char *const known[] = {
    "alarm_sound", "alert_sound", "anchor", "click", "clock_every", "contrast", "fade_ms", "font", "font_bold", "font_size", "icons", "icon_font",
    "icon_size", "language", "margin", "notify_sound", "osd", "output", "peek_break",
    "peek_idle", "player", "print", "reduce_motion", "scale", "sound",
    "sound_devices", "sound_network", "sound_power", "sound_volume", "speech", "startup_grace", "text_scale", "text_size_max", "text_size_min", "usb", "width",
};

/* The integer settings, with the range each is read with (see the call sites). Checking here
 * gives the file and line; int_value() then has nothing to clamp. */
static const struct { const char *key; long lo, hi; } ranges[] = {
    { "clock_every", 0, 60 },
    { "peek_idle", 1, 120 },
    { "peek_break", 1, 720 },
    { "startup_grace", 0, MAX_GRACE_S },
    { "margin", 0, MARGIN_MAX },
    { "fade_ms", 0, 2000 },
    { "icon_size", PAINT_ICON_MIN, PAINT_ICON_MAX },
    { "font_size", PAINT_FONT_MIN, PAINT_FONT_MAX },
    { "width", PAINT_WIDTH_MIN, PAINT_WIDTH_MAX },
};

/* The settings that are sizes: a number of pixels, or a length with a unit (util.h, Size). */
static bool size_key(const char *key) {
    return !strcmp(key, "margin") || !strcmp(key, "font_size") || !strcmp(key, "icon_size") || !strcmp(key, "width");
}

static bool number_ok(const char *key, const char *value) {
    for (size_t i = 0; i < sizeof ranges / sizeof *ranges; i++) {
        if (strcmp(key, ranges[i].key)) continue;
        char *end;
        errno = 0;
        long n = strtol(value, &end, 10);
        if (size_key(key)) {   /* a length with a unit is read as it is shown, on the screen it lands on */
            PacLength length;
            if (*value && end != value && !*end && !errno) return n >= ranges[i].lo && n <= ranges[i].hi;
            return pac_length_parse(value, &length);
        }
        return *value && !*end && !errno && n >= ranges[i].lo && n <= ranges[i].hi;
    }
    return true;
}

/* The USB kinds, as src_usb.c names them (kept here so a bad list has the file and line). */
static bool usb_ok(const char *value) {
    if (!*value || !strcmp(value, "all") || !strcmp(value, "0")) return true;
    static const char *const kinds[] = {
        "camera", "printer", "storage", "audio", "tablet", "input", "other",
    };
    char copy[128];
    if (strlen(value) >= sizeof copy) return false;
    snprintf(copy, sizeof copy, "%s", value);
    for (char *save, *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        bool ok = false;
        for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++)
            if (!strcasecmp(tok, kinds[i])) { ok = true; break; }
        if (!ok) return false;
    }
    return true;
}

/* Stores a setting. False for a key that is not one. */
static bool apply(const char *key, const char *value) {
    char name[64];
    size_t i;
    for (i = 0; key[i]; i++) {
        if (i + 1 >= sizeof name) return false;
        name[i] = key[i] == '-' ? '_' : (char)tolower((unsigned char)key[i]);
    }
    name[i] = '\0';
    /* text_scale is the shared name for the text size; font_size is in pixels, and 16 is 1. */
    if (strcmp(name, "text_scale") == 0) {
        double scale;
        if (!pac_text_scale_parse(value, &scale)) return false;
        char px[16];
        snprintf(px, sizeof px, "%d", (int)(scale * 16 + 0.5));
        return pac_setting_set("font_size", px);
    }
    /* The shared keys of docs/configuration.md that pacreact has: the smallest text between two
     * lengths, and speech and reduce_motion (yes or no, in any case, as every tool reads it). */
    if (strcmp(name, "text_size_min") == 0 || strcmp(name, "text_size_max") == 0) {
        PacLength length;
        return pac_length_parse(value, &length) && pac_setting_set(name, value);
    }
    if (strcmp(name, "speech") == 0 || strcmp(name, "reduce_motion") == 0) {
        bool on;
        return pac_config_bool(value, &on) && pac_setting_set(name, on ? "yes" : "no");
    }
    if (strcmp(name, "scale") == 0) {
        char *end;
        double v = strtod(value, &end);
        if (!*value || *end || !(v >= 1.0) || v > 4.0) return false;
        return pac_setting_set(name, value);
    }
    if (strcmp(name, "contrast") == 0)
        return (!*value || !strcmp(value, "normal") || !strcmp(value, "high")) &&
               pac_setting_set(name, value);
    if (strcmp(name, "icons") == 0)
        return (!*value || !strcmp(value, "auto") || !strcmp(value, "theme") || !strcmp(value, "0")) &&
               pac_setting_set(name, value);
    if (strcmp(name, "osd") == 0)
        return (!*value || !strcmp(value, "center") || !strcmp(value, "stack")) &&
               pac_setting_set(name, value);
    if (strcmp(name, "anchor") == 0)
        return (!*value || !strcmp(value, "top-right") || !strcmp(value, "top-left") ||
                !strcmp(value, "top") ||
                !strcmp(value, "bottom-right") || !strcmp(value, "bottom-left") ||
                !strcmp(value, "bottom")) && pac_setting_set(name, value);
    if (strcmp(name, "print") == 0)
        return (!*value || !strcmp(value, "1") || !strcmp(value, "done") || !strcmp(value, "0")) &&
               pac_setting_set(name, value);
    if (strcmp(name, "sound") == 0 || strcmp(name, "click") == 0)
        return (!*value || !strcmp(value, "1") || !strcmp(value, "0")) && pac_setting_set(name, value);
    if (strcmp(name, "usb") == 0) return usb_ok(value) && pac_setting_set(name, value);
    for (i = 0; i < sizeof known / sizeof *known; i++)
        if (strcmp(name, known[i]) == 0)
            return number_ok(name, value) && pac_setting_set(name, value);
    return false;
}

static bool apply_setting(const char *key, const char *value, void *data) {
    (void)data;
    return apply(key, value);
}

const char *const *settings_known(size_t *count) {
    *count = sizeof known / sizeof *known;
    return known;
}

int settings_load_file(const char *path) {
    return pac_config_load_file("pacreact", path, apply_setting, NULL);
}

void settings_load(void) {
    pac_config_load_shared("pacreact", apply_setting, NULL);   /* pac.conf first: what every tool shares */
    char path[1024];
    for (int which = 0; which < PAC_CONFIG_LOCATIONS; which++)   /* each file overrides the earlier */
        if (pac_config_path(NULL, "pacreact", which, path, sizeof path)) settings_load_file(path);
}
