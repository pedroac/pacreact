#include "sound.h"
#include "setting.h"
#include "i18n.h"
#include "util.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "tunables.h"

#define DEFAULT_ALARM "alarm-clock-elapsed"

static bool enabled;
static char player[256];
static bool quiet;
static char files[SOUND_KINDS][512];  /* indexed by SoundKind; empty = silent */
static long long last_ms;

/* A regular file we can read. Not a pipe or a device: a notification names the path, and a player
 * waiting forever on a pipe would be left behind every time. */
static bool is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, R_OK) == 0;
}

/* Looks for NAME.oga/.ogg/.wav in the freedesktop sound theme, in the user's and the system's
 * data directories. */
static bool theme_lookup(const char *name, char *out, size_t n) {
    static const char *const exts[] = { ".oga", ".ogg", ".wav" };
    char list[2048];
    data_dirs(list, sizeof list);
    char *save, *dir = strtok_r(list, ":", &save);
    for (; dir; dir = strtok_r(NULL, ":", &save)) {
        if (!*dir) continue;
        for (size_t e = 0; e < sizeof exts / sizeof *exts; e++) {
            snprintf(out, n, "%s/sounds/freedesktop/stereo/%s%s", dir, name, exts[e]);
            if (is_file(out)) return true;
        }
    }
    return false;
}

bool sound_resolve(const char *spec, char *out, size_t n) {
    if (!spec || !*spec) return false;
    if (spec[0] == '/') {
        if (!is_file(spec)) return false;
        snprintf(out, n, "%s", spec);
        return true;
    }
    /* A theme name: no path separators, so it can never point outside the theme directories
     * or look like an option to the player. */
    if (strchr(spec, '/') || spec[0] == '-' || spec[0] == '.') return false;
    char found[512];  /* the lookup tries candidates in its buffer: only keep a hit */
    if (!theme_lookup(spec, found, sizeof found)) return false;
    snprintf(out, n, "%s", found);
    return true;
}

static bool find_player(const char *name) {
    char full[512];
    return find_in_path(name, full, sizeof full);
}

static void configure(SoundKind kind, const char *env, const char *fallback) {
    const char *v = pac_setting(env);
    const char *spec = v ? v : fallback;  /* an empty variable switches that sound off */
    files[kind][0] = '\0';
    if (spec && *spec && strcmp(spec, "0") != 0) sound_resolve(spec, files[kind], sizeof files[kind]);
}

/* The value of an opt-in variable, or NULL when it is off: unset, empty or "0". */
static const char *opt_in(const char *env) {
    const char *v = pac_setting(env);
    return v && *v && strcmp(v, "0") != 0 ? v : NULL;
}

static bool is_on(const char *v) { return !strcmp(v, "1") || !strcmp(v, "on"); }

/* sound_volume: off unless set. "1" (or "on") plays the sound theme's own sound; anything
 * else is a name or path. */
static void configure_toggle(SoundKind kind, const char *env, const char *theme_name) {
    files[kind][0] = '\0';
    const char *v = opt_in(env);
    if (v) sound_resolve(is_on(v) ? theme_name : v, files[kind], sizeof files[kind]);
}

/* sound_power and its siblings: off unless set. "1" (or "on") plays the sound theme's own
 * pair for the event; a single name or path plays for both directions; "UP,DOWN" gives each its own,
 * and an empty half is silent. Names and paths are as for the other sounds. */
static void configure_pair(SoundKind up, SoundKind down, const char *env, const char *theme_up,
                           const char *theme_down) {
    files[up][0] = files[down][0] = '\0';
    const char *v = opt_in(env);
    if (!v) return;

    char spec[1024];
    snprintf(spec, sizeof spec, "%s", v);
    const char *first = spec, *second = spec;
    char *comma = strchr(spec, ',');
    if (is_on(spec)) {
        first = theme_up;
        second = theme_down;
    } else if (comma) {
        *comma = '\0';
        second = comma + 1;
    }
    if (*first) sound_resolve(first, files[up], sizeof files[up]);
    if (*second) sound_resolve(second, files[down], sizeof files[down]);
}

void sound_set_quiet(bool q) { quiet = q; }

void sound_init(void) {
    const char *off = pac_setting("sound");
    enabled = !(off && !strcmp(off, "0"));
    player[0] = '\0';

    const char *chosen = pac_setting("player");
    if (chosen && *chosen) {
        if (find_player(chosen)) snprintf(player, sizeof player, "%s", chosen);
    } else {
        static const char *const candidates[] = { "pw-play", "paplay" };
        for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
            if (find_player(candidates[i])) { snprintf(player, sizeof player, "%s", candidates[i]); break; }
        }
    }

    configure(SOUND_ALARM, "alarm_sound", DEFAULT_ALARM);
    configure(SOUND_NOTIFY, "notify_sound", NULL);
    configure(SOUND_ALERT, "alert_sound", NULL);
    configure_toggle(SOUND_VOLUME, "sound_volume", "audio-volume-change");
    /* Whoever hears the volume said needs to hear each key press too, so it ticks unless told not to. */
    const char *speech = pac_setting("speech");
    if (!pac_setting("sound_volume") && !(speech && !strcmp(speech, "no")))
        sound_resolve("audio-volume-change", files[SOUND_VOLUME], sizeof files[SOUND_VOLUME]);
    configure_pair(SOUND_POWER_PLUGGED, SOUND_POWER_UNPLUGGED, "sound_power", "power-plug", "power-unplug");
    configure_pair(SOUND_DEVICE_ADDED, SOUND_DEVICE_REMOVED, "sound_devices", "device-added", "device-removed");
    configure_pair(SOUND_NETWORK_UP, SOUND_NETWORK_DOWN, "sound_network",
                   "network-connectivity-established", "network-connectivity-lost");

    if (!player[0])
        fputs(_("pacreact: no sound player found (install pw-play or paplay, or set player)\n"), stderr);

    signal(SIGCHLD, SIG_IGN);  /* players are fire-and-forget: let the kernel reap them */
}

/* every: play even within MIN_GAP_MS of the last sound (a tick for each key press) */
static void play_file(const char *file, bool every) {
    if (!enabled || !*player || !*file) return;
    long long now = now_ms();
    if (!every && last_ms && now - last_ms < MIN_GAP_MS) return;
    last_ms = now;

    pid_t pid = fork();
    if (pid != 0) return;  /* parent (or fork failure: no sound is not worth failing over) */

    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);  /* the daemon blocks SIGINT/SIGTERM; a player must not */
    signal(SIGCHLD, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    alarm(SOUND_PLAYER_DEADLINE_S);   /* a player that hangs is killed, not left behind */
    if (!freopen("/dev/null", "r", stdin) || !freopen("/dev/null", "w", stdout) ||
        !freopen("/dev/null", "w", stderr))
        _exit(127);
    execlp(player, player, file, (char *)NULL);
    _exit(127);
}

void sound_play(SoundKind kind) {
    if (quiet && kind >= SOUND_POWER_PLUGGED) return;
    play_file(files[kind], kind == SOUND_VOLUME);
}

void sound_play_spec(const char *spec) {
    char path[512];
    if (sound_resolve(spec, path, sizeof path)) play_file(path, false);
}
