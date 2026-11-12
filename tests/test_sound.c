/* Sound: how names and paths are resolved, which sounds are on by default, and that a player
 * is actually started with the right file. The "player" is a shell script that records its
 * arguments. */
#include "../src/sound.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char root[64], theme[128], user[128], out[160], player_path[160];

static void touch(const char *path) {
    int fd = open(path, O_CREAT | O_WRONLY, 0644);
    assert(fd >= 0);
    close(fd);
}

/* What the fake player was last asked to play, waiting briefly for it to run. */
static bool played(char *file, size_t n) {
    for (int i = 0; i < 100; i++) {
        FILE *f = fopen(out, "r");
        if (f) {
            char line[256] = "";
            bool got = fgets(line, sizeof line, f) != NULL;
            fclose(f);
            if (got) {
                line[strcspn(line, "\n")] = '\0';
                snprintf(file, n, "%s", line);
                return true;
            }
        }
        usleep(20000);
    }
    return false;
}

static void reset_out(void) {
    unlink(out);
    usleep(450000);  /* past the minimum gap between two sounds */
}

static void setup(void) {
    snprintf(root, sizeof root, "/tmp/pachud-sound-XXXXXX");
    assert(mkdtemp(root));
    snprintf(theme, sizeof theme, "%s/data/sounds/freedesktop/stereo", root);
    snprintf(user, sizeof user, "%s/home/sounds/freedesktop/stereo", root);
    char cmd[512];
    snprintf(cmd, sizeof cmd, "mkdir -p %s %s", theme, user);
    assert(system(cmd) == 0);

    char p[256];
    snprintf(p, sizeof p, "%s/alarm-clock-elapsed.oga", theme); touch(p);
    snprintf(p, sizeof p, "%s/message.ogg", theme);             touch(p);
    snprintf(p, sizeof p, "%s/bell.wav", theme);                touch(p);
    snprintf(p, sizeof p, "%s/message.oga", user);              touch(p);   /* the user's overrides */
    /* Files that only a path trick could reach: one above the theme directory, and names that
     * would look like options or hidden files */
    snprintf(p, sizeof p, "%s/../secret.oga", theme);           touch(p);
    snprintf(p, sizeof p, "%s/-evil.oga", theme);               touch(p);
    snprintf(p, sizeof p, "%s/.hidden.oga", theme);             touch(p);

    snprintf(out, sizeof out, "%s/played", root);
    snprintf(player_path, sizeof player_path, "%s/player.sh", root);
    FILE *f = fopen(player_path, "w");
    assert(f);
    fprintf(f, "#!/bin/sh\necho \"$1\" > %s\n", out);
    fclose(f);
    chmod(player_path, 0755);

    setenv("XDG_DATA_DIRS", strcat(strcpy(p, root), "/data"), 1);
    snprintf(p, sizeof p, "%s/home", root);
    setenv("XDG_DATA_HOME", p, 1);
}

static void test_resolve(void) {
    char path[512], expect[512];

    /* Theme names: user directory first, then system; any of .oga/.ogg/.wav */
    assert(sound_resolve("message", path, sizeof path));
    snprintf(expect, sizeof expect, "%s/message.oga", user);
    assert(!strcmp(path, expect));
    assert(sound_resolve("bell", path, sizeof path) && strstr(path, "/bell.wav"));
    assert(sound_resolve("alarm-clock-elapsed", path, sizeof path));
    assert(!sound_resolve("no-such-sound", path, sizeof path));

    /* Absolute paths must exist; nothing else with a slash is accepted */
    snprintf(expect, sizeof expect, "%s/bell.wav", theme);
    assert(sound_resolve(expect, path, sizeof path) && !strcmp(path, expect));
    assert(!sound_resolve("/no/such/file.oga", path, sizeof path));
    assert(!sound_resolve("../../etc/passwd", path, sizeof path));
    assert(!sound_resolve("../secret", path, sizeof path));          /* exists, but outside the theme */
    assert(!sound_resolve("sub/dir", path, sizeof path));
    assert(!sound_resolve("-evil", path, sizeof path));      /* would look like a player option */
    assert(!sound_resolve(".hidden", path, sizeof path));
    assert(!sound_resolve("", path, sizeof path) && !sound_resolve(NULL, path, sizeof path));

    /* Only regular files: a pipe, a directory or a device would hang or confuse the player */
    char odd[512];
    snprintf(odd, sizeof odd, "%s/pipe.oga", theme);
    assert(mkfifo(odd, 0600) == 0);
    assert(!sound_resolve(odd, path, sizeof path));
    snprintf(odd, sizeof odd, "%s/dir.oga", theme);
    assert(mkdir(odd, 0700) == 0 && !sound_resolve(odd, path, sizeof path));
    assert(!sound_resolve("/dev/zero", path, sizeof path) && !sound_resolve("/dev/null", path, sizeof path));
    assert(!sound_resolve("/tmp", path, sizeof path));
}

static void test_defaults_and_playing(void) {
    char got[512], expect[512];
    unsetenv("PACHUD_SOUND");
    unsetenv("PACHUD_ALARM_SOUND");
    unsetenv("PACHUD_NOTIFY_SOUND");
    unsetenv("PACHUD_ALERT_SOUND");
    setenv("PACHUD_PLAYER", player_path, 1);
    sound_init();

    /* Alarms sound by default... */
    reset_out();
    sound_play(SOUND_ALARM);
    assert(played(got, sizeof got));
    snprintf(expect, sizeof expect, "%s/alarm-clock-elapsed.oga", theme);
    assert(!strcmp(got, expect));

    /* ...two in quick succession play once */
    unlink(out);
    sound_play(SOUND_ALARM);
    usleep(300000);
    assert(access(out, F_OK) != 0);

    /* ...notifications and alerts are silent unless asked for */
    reset_out();
    sound_play(SOUND_NOTIFY);
    sound_play(SOUND_ALERT);
    usleep(300000);
    assert(access(out, F_OK) != 0);

    /* an app's own request (name or path) is honoured */
    sound_play_spec("bell");
    assert(played(got, sizeof got) && strstr(got, "/bell.wav"));
    reset_out();
    sound_play_spec("no-such-sound");
    usleep(300000);
    assert(access(out, F_OK) != 0);
}

static void test_configuration(void) {
    char got[512];

    /* Opting in to the others, and out of the alarm sound */
    setenv("PACHUD_NOTIFY_SOUND", "message", 1);
    setenv("PACHUD_ALERT_SOUND", "bell", 1);
    setenv("PACHUD_ALARM_SOUND", "", 1);
    sound_init();
    reset_out();
    sound_play(SOUND_ALARM);
    usleep(300000);
    assert(access(out, F_OK) != 0);               /* an empty value means silent */

    sound_play(SOUND_NOTIFY);
    assert(played(got, sizeof got) && strstr(got, "/message.oga"));
    reset_out();
    sound_play(SOUND_ALERT);
    assert(played(got, sizeof got) && strstr(got, "/bell.wav"));

    /* "0" turns one sound off too; an unknown name leaves it silent rather than failing */
    setenv("PACHUD_ALERT_SOUND", "0", 1);
    setenv("PACHUD_NOTIFY_SOUND", "nonexistent", 1);
    sound_init();
    reset_out();
    sound_play(SOUND_ALERT);
    sound_play(SOUND_NOTIFY);
    usleep(300000);
    assert(access(out, F_OK) != 0);

    /* Everything off */
    setenv("PACHUD_SOUND", "0", 1);
    unsetenv("PACHUD_ALARM_SOUND");
    sound_init();
    reset_out();
    sound_play(SOUND_ALARM);
    sound_play_spec("bell");
    usleep(300000);
    assert(access(out, F_OK) != 0);

    /* No usable player: silently no sound */
    unsetenv("PACHUD_SOUND");
    setenv("PACHUD_PLAYER", "/no/such/player", 1);
    sound_init();
    reset_out();
    sound_play(SOUND_ALARM);
    usleep(300000);
    assert(access(out, F_OK) != 0);
}

int main(void) {
    setup();
    test_resolve();
    test_defaults_and_playing();
    test_configuration();
    signal(SIGCHLD, SIG_DFL);  /* sound_init() ignores it, which would make system() fail */
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    puts("sound: ok");
    return 0;
}
