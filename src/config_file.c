#include "config_file.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The settings the file may set: the environment variables the daemon reads, without the prefix. */
static const char *const known[] = {
    "ALARM_SOUND", "ALERT_SOUND", "ANCHOR", "BLUETOOTH", "CLICK", "CONTRAST", "DISK", "DISK_CRIT", "DISK_INTERVAL",
    "DISK_WARN", "FADE_MS", "FONT", "FONT_BOLD", "FONT_SIZE", "ICONS", "ICON_SIZE", "JACKS",
    "JACKS_DEBUG", "LOCK_KEYS", "MARGIN", "NOTIFY_SOUND", "OSD", "OUTPUT", "PEEK_BREAK", "PEEK_IDLE", "PLAYER",
    "PRINT", "PRINT_DEBUG", "PROBE_HOST", "PROBE_INTERVAL", "SCALE", "SOUND", "SOUND_DEVICES",
    "SOUND_NETWORK", "SOUND_POWER", "SOUND_VOLUME", "STARTUP_GRACE", "TEMP", "TEMP_CRIT",
    "TEMP_INTERVAL", "TEMP_WARN", "USB", "WIDTH",
};

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

/* Sets PACHUD_<KEY> unless the environment already has it. False for a key that is not a setting. */
static bool apply(const char *key, const char *value) {
    char name[64] = "PACHUD_";
    size_t n = strlen(name), i;
    for (i = 0; key[i]; i++) {
        if (n + 1 >= sizeof name) return false;
        name[n++] = key[i] == '-' ? '_' : (char)toupper((unsigned char)key[i]);
    }
    name[n] = '\0';
    for (i = 0; i < sizeof known / sizeof *known; i++)
        if (strcmp(name + 7, known[i]) == 0) return setenv(name, value, 0) == 0;
    return false;
}

int config_load_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int skipped = 0;
    char line[1024];
    for (int n = 1; fgets(line, sizeof line, f); n++) {
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        char *text = trim(line);
        if (!*text) continue;
        char *eq = strchr(text, '=');
        if (!eq) {
            fprintf(stderr, "pachud: %s:%d: expected \"key = value\"\n", path, n);
            skipped++;
            continue;
        }
        *eq = '\0';
        char *key = trim(text), *value = trim(eq + 1);
        if (!apply(key, value)) {
            fprintf(stderr, "pachud: %s:%d: ignoring \"%s = %s\"\n", path, n, key, value);
            skipped++;
        }
    }
    fclose(f);
    return skipped;
}

void config_load(void) {
    const char *home = getenv("HOME");
    char path[512];
    if (!home || !*home || snprintf(path, sizeof path, "%s/.local/pac/pachud.conf", home) >= (int)sizeof path)
        return;
    config_load_file(path);
}
