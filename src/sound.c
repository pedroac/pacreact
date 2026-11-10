#include "sound.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_ALARM "alarm-clock-elapsed"
#define MIN_GAP_MS 400  /* a burst of events plays once, not as a stack of overlapping sounds */

static bool enabled;
static char player[256];
static char files[3][512];  /* indexed by SoundKind; empty = silent */
static long long last_ms;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static bool is_file(const char *path) {
    return access(path, R_OK) == 0;
}

/* Looks for NAME.oga/.ogg/.wav in the freedesktop sound theme, in the user's and the system's
 * data directories. */
static bool theme_lookup(const char *name, char *out, size_t n) {
    static const char *const exts[] = { ".oga", ".ogg", ".wav" };
    const char *home = getenv("HOME"), *xdg_home = getenv("XDG_DATA_HOME");
    const char *dirs = getenv("XDG_DATA_DIRS");
    char list[2048];
    snprintf(list, sizeof list, "%s:%s",
             xdg_home && *xdg_home ? xdg_home : "", dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
    char fallback[512];
    if (!(xdg_home && *xdg_home) && home) {
        snprintf(fallback, sizeof fallback, "%s/.local/share", home);
        snprintf(list, sizeof list, "%s:%s", fallback, dirs && *dirs ? dirs : "/usr/local/share:/usr/share");
    }

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
    if (strchr(name, '/')) return is_file(name) && access(name, X_OK) == 0;
    const char *path = getenv("PATH");
    if (!path) return false;
    char copy[2048], full[512];
    snprintf(copy, sizeof copy, "%s", path);
    char *save, *dir = strtok_r(copy, ":", &save);
    for (; dir; dir = strtok_r(NULL, ":", &save)) {
        snprintf(full, sizeof full, "%s/%s", dir, name);
        if (access(full, X_OK) == 0) return true;
    }
    return false;
}

static void configure(SoundKind kind, const char *env, const char *fallback) {
    const char *v = getenv(env);
    const char *spec = v ? v : fallback;  /* an empty variable switches that sound off */
    files[kind][0] = '\0';
    if (spec && *spec && strcmp(spec, "0") != 0) sound_resolve(spec, files[kind], sizeof files[kind]);
}

void sound_init(void) {
    const char *off = getenv("PACHUD_SOUND");
    enabled = !(off && !strcmp(off, "0"));
    player[0] = '\0';

    const char *chosen = getenv("PACHUD_PLAYER");
    if (chosen && *chosen) {
        if (find_player(chosen)) snprintf(player, sizeof player, "%s", chosen);
    } else {
        static const char *const candidates[] = { "pw-play", "paplay" };
        for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
            if (find_player(candidates[i])) { snprintf(player, sizeof player, "%s", candidates[i]); break; }
        }
    }

    configure(SOUND_ALARM, "PACHUD_ALARM_SOUND", DEFAULT_ALARM);
    configure(SOUND_NOTIFY, "PACHUD_NOTIFY_SOUND", NULL);
    configure(SOUND_ALERT, "PACHUD_ALERT_SOUND", NULL);

    signal(SIGCHLD, SIG_IGN);  /* players are fire-and-forget: let the kernel reap them */
}

static void play_file(const char *file) {
    if (!enabled || !*player || !*file) return;
    long long now = now_ms();
    if (last_ms && now - last_ms < MIN_GAP_MS) return;
    last_ms = now;

    pid_t pid = fork();
    if (pid != 0) return;  /* parent (or fork failure: no sound is not worth failing over) */

    sigset_t none;
    sigemptyset(&none);
    sigprocmask(SIG_SETMASK, &none, NULL);  /* the daemon blocks SIGINT/SIGTERM; a player must not */
    signal(SIGCHLD, SIG_DFL);
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    if (!freopen("/dev/null", "r", stdin) || !freopen("/dev/null", "w", stdout) ||
        !freopen("/dev/null", "w", stderr))
        _exit(127);
    execlp(player, player, file, (char *)NULL);
    _exit(127);
}

void sound_play(SoundKind kind) {
    play_file(files[kind]);
}

void sound_play_spec(const char *spec) {
    char path[512];
    if (sound_resolve(spec, path, sizeof path)) play_file(path);
}
