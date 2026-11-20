#include "activity.h"
#include "alerts.h"
#include "clock.h"
#include "settings_file.h"
#include "display.h"
#include "hud.h"
#include "i18n.h"
#include "dnd.h"
#include "history.h"
#include "ipc.h"
#include "loop.h"
#include "paint.h"
#include "setting.h"
#include "speech.h"
#include "sound.h"
#include "text.h"
#ifndef NO_NOTIFY
#include "notify.h"
#endif
#include "sources.h"
#include "timer.h"
#include "version.h"
#include "watch.h"
#include <stdbool.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <sys/signalfd.h>
#include <unistd.h>
#include "tunables.h"


static const char usage_text[] =
"usage: pacreact                      run the daemon\n"
    "       pacreact toast [-l info|warn|crit] [-t SECONDS] [-i ICON] TEXT...\n"
    "       pacreact slider [-i ICON] PERCENT LABEL...\n"
    "       pacreact timer DURATION [LABEL...]   e.g. 90s, 5m, 1h30m\n"
    "       pacreact alarm [-r daily|weekdays|weekends|mon,wed,...] [YYYY-MM-DD] HH:MM [LABEL...]\n"
    "       pacreact peek [-r daily|weekdays|weekends|mon,wed,...] HH:MM [LABEL...]   show the time, quietly\n"
    "       pacreact peek every DURATION [LABEL...]   show the time after that much use, e.g. 30m, 1h\n"
    "       pacreact time                        show the time in the middle of the screen\n"
    "       pacreact snooze [DURATION]           re-arm the last timer/alarm that went off\n"
    "       pacreact pause ID | resume ID | extend ID DURATION   (timers)\n"
    "       pacreact list | cancel ID|all\n"
    "       pacreact dismiss                     close the HUD on screen\n"
    "       pacreact dnd [on [DURATION]|off|toggle|status]\n"
    "       pacreact action [N]                  invoke action N of the notification on screen\n"
    "       pacreact history [N|clear]           the last notifications (kept in memory only)\n"
    "       pacreact print                       the print jobs that have not finished (runs pacreactd print)\n"
    "       pacreact count                       notifications on screen or waiting (for a status bar)\n"
    "       pacreact watch                       print each change of the monitors, for a status bar (see pacreactd watch)\n"
    "       pacreact jacks                       what is plugged into the audio jacks (for a status bar)\n"
    "       pacreact expand                      show more of the newest notification, or less again\n";

static int usage(void) {
    fputs(usage_text, stderr);
    return 2;
}

/* pacreact slider [-i ICON] PERCENT LABEL...: the daemon takes percent, icon ("-" for none), label */
static int client_slider(int argc, char **argv) {
    const char *icon = "-";
    int i = 2;
    if (i + 1 < argc && !strcmp(argv[i], "-i")) { icon = argv[i + 1]; i += 2; }
    if (i + 1 >= argc) return usage();

    char msg[1024];
    int n = snprintf(msg, sizeof msg, "slider\t%s\t%s\t", argv[i], icon);
    for (i++; i < argc && n < (int)sizeof msg - 1; i++)
        n += snprintf(msg + n, sizeof msg - n, "%s%s", argv[i], i + 1 < argc ? " " : "");
    int r = ipc_send(msg);
    if (r < 0) fputs(_("pacreact: daemon not running\n"), stderr);
    return r != 0;
}

static int client_toast(int argc, char **argv) {
    const char *level = "info", *icon = "-";   /* "-": no icon, since an empty field would not survive */
    double secs = DEFAULT_TIMEOUT_MS / 1000.0;
    int i = 2;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i += 2) {
        if (i + 1 >= argc) return usage();
        if (!strcmp(argv[i], "-l")) level = argv[i + 1];
        else if (!strcmp(argv[i], "-t")) secs = atof(argv[i + 1]);
        else if (!strcmp(argv[i], "-i")) icon = argv[i + 1];
        else return usage();
    }
    if (i >= argc || secs <= 0) return usage();

    char msg[1024];
    int n = snprintf(msg, sizeof msg, "toast\t%s\t%d\t%s\t", level, (int)(secs * 1000), icon);
    for (; i < argc && n < (int)sizeof msg - 1; i++)
        n += snprintf(msg + n, sizeof msg - n, "%s%s", argv[i], i + 1 < argc ? " " : "");

    int r = ipc_send(msg);
    if (r < 0) fputs(_("pacreact: daemon not running\n"), stderr);
    return r != 0;
}

/* Commands shaped "cmd ARG [REST...]": the daemon gets them tab-separated. */

/* snprintf() returns the length the text would have had; the length of what was written is never
 * past the end of the buffer, so that the next write cannot start there. */
static int clamp(int written, size_t size) {
    return written < 0 ? 0 : written >= (int)size ? (int)size - 1 : written;
}

static int client_generic(int argc, char **argv, int min_args) {
    if (argc < 2 + min_args) return usage();
    char msg[1024];
    int n = clamp(snprintf(msg, sizeof msg, "%s", argv[1]), sizeof msg);
    if (argc > 2) n = clamp(n + snprintf(msg + n, sizeof msg - n, "\t%s", argv[2]), sizeof msg);
    if (argc > 3) n = clamp(n + snprintf(msg + n, sizeof msg - n, "\t"), sizeof msg);
    for (int i = 3; i < argc && n < (int)sizeof msg - 1; i++)
        n = clamp(n + snprintf(msg + n, sizeof msg - n, "%s%s", argv[i], i + 1 < argc ? " " : ""), sizeof msg);
    int r = ipc_send(msg);
    if (r < 0) fputs(_("pacreact: daemon not running\n"), stderr);
    return r != 0;
}

static bool looks_like_date(const char *s) {  /* YYYY-MM-DD */
    return strlen(s) == 10 && s[4] == '-' && s[7] == '-';
}

/* pacreact alarm [-r RECUR] [YYYY-MM-DD] HH:MM [LABEL...]. The daemon takes one field: "HH:MM",
 * "HH:MM@RECUR" or "YYYY-MM-DDTHH:MM". */
static int client_alarm(int argc, char **argv) {
    int i = 2;
    const char *recur = NULL;
    if (i + 1 < argc && !strcmp(argv[i], "-r")) { recur = argv[i + 1]; i += 2; }
    if (i >= argc) return usage();

    char spec[64];
    if (looks_like_date(argv[i]) && i + 1 < argc) {
        snprintf(spec, sizeof spec, "%sT%s", argv[i], argv[i + 1]);
        i += 2;
    } else {
        snprintf(spec, sizeof spec, "%s", argv[i]);
        i++;
    }
    if (recur) {
        size_t len = strlen(spec);
        snprintf(spec + len, sizeof spec - len, "@%s", recur);
    }

    if (argc >= CLIENT_ARGV_MAX) return usage();
    char *nargv[CLIENT_ARGV_MAX];
    int n = 0;
    nargv[n++] = argv[0];
    nargv[n++] = argv[1];
    nargv[n++] = spec;
    for (; i < argc; i++) nargv[n++] = argv[i];
    return client_generic(n, nargv, 1);
}

/* pacreact peek [-r RECUR] HH:MM [LABEL...] and pacreact peek every DURATION [LABEL...]: the daemon takes
 * "HH:MM", "HH:MM@RECUR" or "every@DURATION" as its one field, like an alarm. */
static int client_peek(int argc, char **argv) {
    if (argc > 3 && !strcmp(argv[2], "every")) {
        char spec[64];
        snprintf(spec, sizeof spec, "every@%s", argv[3]);
        if (argc >= CLIENT_ARGV_MAX) return usage();
        char *nargv[CLIENT_ARGV_MAX];
        int n = 0;
        nargv[n++] = argv[0];
        nargv[n++] = argv[1];
        nargv[n++] = spec;
        for (int i = 4; i < argc; i++) nargv[n++] = argv[i];
        return client_generic(n, nargv, 1);
    }
    return client_alarm(argc, argv);
}

static void handle(char *msg, char *reply, size_t n) {
    char *save, *cmd = strtok_r(msg, "\t", &save);
    if (cmd && !strcmp(cmd, "toast")) {
        char *lvl = strtok_r(NULL, "\t", &save), *ms = strtok_r(NULL, "\t", &save);
        char *icon = strtok_r(NULL, "\t", &save);
        char *text = save;
        Level level = LEVEL_INFO;
        if (!lvl || !ms || !icon || !*text) { snprintf(reply, n, "%s", _("bad toast message")); return; }
        if (!strcmp(lvl, "warn")) level = LEVEL_WARN;
        else if (!strcmp(lvl, "crit")) level = LEVEL_CRIT;
        else if (strcmp(lvl, "info")) { snprintf(reply, n, "%s", _("level must be info, warn or crit")); return; }
        hud_toast_icon(text, level, atoi(ms), strcmp(icon, "-") ? icon : NULL);
        snprintf(reply, n, "ok");
        return;
    }
    if (cmd && !strcmp(cmd, "slider")) {
        char *pct = strtok_r(NULL, "\t", &save), *icon = strtok_r(NULL, "\t", &save), *label = save;
        if (!pct || !icon || !*label) { snprintf(reply, n, "%s", _("bad slider message")); return; }
        hud_slider(label, atoi(pct), false, strcmp(icon, "-") ? icon : NULL);
        snprintf(reply, n, "ok");
        return;
    }
    if (cmd && !strcmp(cmd, "time")) {
        clock_show();
        snprintf(reply, n, "ok");
        return;
    }
    if (cmd && !strcmp(cmd, "expand")) {
        int open = hud_toggle_expand();
        if (open < 0) snprintf(reply, n, "%s", _("no notification on screen"));
        else snprintf(reply, n, "ok %s", open ? _("expanded") : _("collapsed"));
        return;
    }
    if (cmd && !strcmp(cmd, "count")) {
        snprintf(reply, n, "ok %d", hud_card_count());
        return;
    }
    if (cmd && !strcmp(cmd, "jacks")) {
        char plugged[48];
        jack_describe(plugged, sizeof plugged);
        snprintf(reply, n, "ok%s", plugged);
        return;
    }
    if (cmd && !strcmp(cmd, "action")) {
#ifndef NO_NOTIFY
        char *which = strtok_r(NULL, "\t", &save);
        int number = which ? atoi(which) : 1;
        if (notify_invoke_action(number < 1 ? 1 : number, reply, n)) return;
#else
        snprintf(reply, n, "%s", _("this build has no notification support"));
#endif
        return;
    }
    if (cmd && !strcmp(cmd, "dismiss")) {
        hud_dismiss();
        snprintf(reply, n, "ok");
        return;
    }
    if (cmd && dnd_command(cmd, save ? save : (char *)"", reply, n)) return;
#ifndef NO_NOTIFY
    if (cmd && history_command(cmd, save ? save : (char *)"", reply, n)) return;
#else
    if (cmd && !strcmp(cmd, "history")) {
        snprintf(reply, n, "%s", _("this build has no notification support"));
        return;
    }
#endif
    if (cmd && timer_command(cmd, save ? save : (char *)"", reply, n)) return;
    snprintf(reply, n, "%s", _("unknown command"));
}

static bool quit, wl_readable;

static void on_wayland(void *ctx, uint32_t ev) {
    (void)ctx;
    wl_readable = ev & EPOLLIN;
    if (ev & (EPOLLERR | EPOLLHUP)) quit = true;
}

static void on_ipc(void *ctx, uint32_t ev) {
    (void)ev;
    char buf[1100], reply[4096];
    int cfd = ipc_accept(*(int *)ctx, buf, sizeof buf);
    if (cfd < 0) return;
    if (!strcmp(buf, "watch")) {   /* kept open: the answer is every change from now on */
        if (watch_add(cfd) < 0) { ipc_reply(cfd, _("too many programs are watching")); }
        return;
    }
    handle(buf, reply, sizeof reply);
    ipc_reply(cfd, reply);
}

static void on_signal(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    quit = true;
}

static int run_daemon(void) {
    settings_load();
    /* The notification history lives in this process: keep it out of core dumps and out of reach of
     * other processes that ptrace it or read /proc/PID/mem. */
    prctl(PR_SET_DUMPABLE, 0);
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    sigprocmask(SIG_BLOCK, &mask, NULL);
    int sfd = signalfd(-1, &mask, SFD_CLOEXEC);

    static int lfd;
    lfd = ipc_listen();
    if (lfd == -2) { fputs(_("pacreact: already running\n"), stderr); return 1; }
    if (lfd < 0) { perror("pacreact: socket"); return 1; }

    text_init();
    sound_init();
    alerts_init();
    Display *dpy = display_open();
    if (!dpy) { fputs(_("pacreact: cannot connect to a Wayland compositor with layer-shell\n"), stderr); return 1; }
    /* Speech, unless speech = no: opened here, where a server that is not running is said once and the HUD goes on. */
    const char *speech = pac_setting("speech");
    bool speaking = (!speech || strcmp(speech, "no")) && speech_open("pacreact", pac_setting("language"));
    paint_init();   /* font_size and width, before anything is measured */
    paint_set_screen(display_px_per_mm(dpy), display_output_short_side(dpy), display_output_width(dpy));   /* sizes in mm are in pixels of this screen */
    hud_init(dpy);
    hud_set_speech(speaking);

    if (loop_init() < 0) { perror("pacreact: epoll"); return 1; }
    loop_add(display_fd(dpy), EPOLLIN, on_wayland, NULL);
    loop_add(lfd, EPOLLIN, on_ipc, &lfd);
    loop_add(sfd, EPOLLIN, on_signal, NULL);
    daemon_init();
#ifndef NO_NOTIFY
    notify_init();
#endif
    timer_init();
    clock_init();
    activity_init(dpy);
    activity_on_change(timer_activity);
    timer_usage_config(activity_break_secs());
    dnd_init();

    while (!quit) {
        if (display_before_poll(dpy) < 0) break;
        int r = loop_wait(hud_timeout_ms());
        char count[16];   /* what the bell of a status bar shows: told when it changes, which is only after something woke us */
        snprintf(count, sizeof count, "%d", hud_card_count());
        daemon_publish("notifications.count", count);
        if (display_after_poll(dpy, wl_readable) < 0 || r < 0) break;
        wl_readable = false;
        hud_tick();
    }

    speech_close();
    display_close(dpy);
    text_finish();
    return 0;
}

int main(int argc, char **argv) {
    if (argc == 1) return run_daemon();
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fputs(usage_text, stdout);
        return 0;
    }
    if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
        puts("pacreact " PACREACT_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "watch")) {
        if (ipc_watch() >= 0) return 0;
        fputs(_("pacreact: daemon not running\n"), stderr);
        return 1;
    }
    if (!strcmp(argv[1], "toast")) return client_toast(argc, argv);
    if (!strcmp(argv[1], "slider")) return client_slider(argc, argv);
    if (!strcmp(argv[1], "alarm")) return client_alarm(argc, argv);
    if (!strcmp(argv[1], "peek")) return client_peek(argc, argv);
    if (!strcmp(argv[1], "timer") || !strcmp(argv[1], "cancel") || !strcmp(argv[1], "pause") ||
        !strcmp(argv[1], "resume"))
        return client_generic(argc, argv, 1);
    if (!strcmp(argv[1], "extend")) return client_generic(argc, argv, 2);
    if (!strcmp(argv[1], "list") || !strcmp(argv[1], "dismiss") || !strcmp(argv[1], "dnd") ||
        !strcmp(argv[1], "snooze") || !strcmp(argv[1], "action") || !strcmp(argv[1], "history") ||
        !strcmp(argv[1], "count") || !strcmp(argv[1], "jacks") || !strcmp(argv[1], "expand") || !strcmp(argv[1], "time"))
        return client_generic(argc, argv, 0);
    if (!strcmp(argv[1], "print")) {   /* the queue is asked of CUPS by pacreactd, which has the code for it */
        execlp("pacreactd", "pacreactd", "print", (char *)NULL);
        fputs(_("pacreact: print needs pacreactd, which was not found\n"), stderr);
        return 1;
    }
    return usage();
}
