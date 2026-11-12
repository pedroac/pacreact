#include "alerts.h"
#include "display.h"
#include "hud.h"
#include "i18n.h"
#include "dnd.h"
#include "history.h"
#include "ipc.h"
#include "loop.h"
#include "sound.h"
#include "text.h"
#ifndef NO_NOTIFY
#include "notify.h"
#endif
#include "sources.h"
#include "timer.h"
#include "version.h"
#include <stdbool.h>
#include <sys/epoll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <unistd.h>

#define DEFAULT_TIMEOUT_MS 3000

static const char usage_text[] =
"usage: pachud                      run the daemon\n"
    "       pachud toast [-l info|warn|crit] [-t SECONDS] TEXT...\n"
    "       pachud slider PERCENT LABEL...\n"
    "       pachud timer DURATION [LABEL...]   e.g. 90s, 5m, 1h30m\n"
    "       pachud alarm [-r daily|weekdays|weekends|mon,wed,...] [YYYY-MM-DD] HH:MM [LABEL...]\n"
    "       pachud snooze [DURATION]           re-arm the last timer/alarm that went off\n"
    "       pachud pause ID | resume ID | extend ID DURATION   (timers)\n"
    "       pachud list | cancel ID|all\n"
    "       pachud dismiss                     close the HUD on screen\n"
    "       pachud dnd [on [DURATION]|off|toggle|status]\n"
    "       pachud action [N]                  invoke action N of the notification on screen\n"
    "       pachud history [N|clear]           the last notifications (kept in memory only)\n"
    "       pachud count                       notifications on screen or waiting (for a status bar)\n";

static int usage(void) {
    fputs(usage_text, stderr);
    return 2;
}

static int client_toast(int argc, char **argv) {
    const char *level = "info";
    double secs = DEFAULT_TIMEOUT_MS / 1000.0;
    int i = 2;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i += 2) {
        if (i + 1 >= argc) return usage();
        if (!strcmp(argv[i], "-l")) level = argv[i + 1];
        else if (!strcmp(argv[i], "-t")) secs = atof(argv[i + 1]);
        else return usage();
    }
    if (i >= argc || secs <= 0) return usage();

    char msg[1024];
    int n = snprintf(msg, sizeof msg, "toast\t%s\t%d\t", level, (int)(secs * 1000));
    for (; i < argc && n < (int)sizeof msg - 1; i++)
        n += snprintf(msg + n, sizeof msg - n, "%s%s", argv[i], i + 1 < argc ? " " : "");

    int r = ipc_send(msg);
    if (r < 0) fputs(_("pachud: daemon not running\n"), stderr);
    return r != 0;
}

/* Commands shaped "cmd ARG [REST...]": the daemon gets them tab-separated. */
static int client_generic(int argc, char **argv, int min_args) {
    if (argc < 2 + min_args) return usage();
    char msg[1024];
    int n = snprintf(msg, sizeof msg, "%s", argv[1]);
    if (argc > 2) n += snprintf(msg + n, sizeof msg - n, "\t%s", argv[2]);
    if (argc > 3) n += snprintf(msg + n, sizeof msg - n, "\t");
    for (int i = 3; i < argc && n < (int)sizeof msg - 1; i++)
        n += snprintf(msg + n, sizeof msg - n, "%s%s", argv[i], i + 1 < argc ? " " : "");
    int r = ipc_send(msg);
    if (r < 0) fputs(_("pachud: daemon not running\n"), stderr);
    return r != 0;
}

static bool looks_like_date(const char *s) {  /* YYYY-MM-DD */
    return strlen(s) == 10 && s[4] == '-' && s[7] == '-';
}

/* pachud alarm [-r RECUR] [YYYY-MM-DD] HH:MM [LABEL...]. The daemon takes one field: "HH:MM",
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

    char *nargv[argc + 1];
    int n = 0;
    nargv[n++] = argv[0];
    nargv[n++] = argv[1];
    nargv[n++] = spec;
    for (; i < argc; i++) nargv[n++] = argv[i];
    return client_generic(n, nargv, 1);
}

static void handle(char *msg, char *reply, size_t n) {
    char *save, *cmd = strtok_r(msg, "\t", &save);
    if (cmd && !strcmp(cmd, "toast")) {
        char *lvl = strtok_r(NULL, "\t", &save), *ms = strtok_r(NULL, "\t", &save);
        char *text = save;
        Level level = LEVEL_INFO;
        if (!lvl || !ms || !*text) { snprintf(reply, n, "%s", _("bad toast message")); return; }
        if (!strcmp(lvl, "warn")) level = LEVEL_WARN;
        else if (!strcmp(lvl, "crit")) level = LEVEL_CRIT;
        else if (strcmp(lvl, "info")) { snprintf(reply, n, "%s", _("level must be info, warn or crit")); return; }
        hud_toast(text, level, atoi(ms));
        snprintf(reply, n, "ok");
        return;
    }
    if (cmd && !strcmp(cmd, "slider")) {
        char *pct = strtok_r(NULL, "\t", &save), *label = save;
        if (!pct || !*label) { snprintf(reply, n, "%s", _("bad slider message")); return; }
        hud_slider(label, atoi(pct), false);
        snprintf(reply, n, "ok");
        return;
    }
    if (cmd && !strcmp(cmd, "count")) {
        snprintf(reply, n, "ok %d", hud_card_count());
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
    handle(buf, reply, sizeof reply);
    ipc_reply(cfd, reply);
}

static void on_signal(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    quit = true;
}

static int run_daemon(void) {
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
    if (lfd == -2) { fputs(_("pachud: already running\n"), stderr); return 1; }
    if (lfd < 0) { perror("pachud: socket"); return 1; }

    text_init();
    sound_init();
    alerts_init();
    Display *dpy = display_open();
    if (!dpy) { fputs(_("pachud: cannot connect to a Wayland compositor with layer-shell\n"), stderr); return 1; }
    hud_init(dpy);

    if (loop_init() < 0) { perror("pachud: epoll"); return 1; }
    loop_add(display_fd(dpy), EPOLLIN, on_wayland, NULL);
    loop_add(lfd, EPOLLIN, on_ipc, &lfd);
    loop_add(sfd, EPOLLIN, on_signal, NULL);
    power_init();
    mem_init();
    net_init();
    probe_init();
    disk_init();
    temp_init();
    jack_init();
    locks_init();
#ifndef NO_NOTIFY
    bt_init();
#endif
    backlight_init();
    audio_init();
#ifndef NO_NOTIFY
    notify_init();
#endif
    timer_init();
    dnd_init();

    while (!quit) {
        if (display_before_poll(dpy) < 0) break;
        int r = loop_wait(hud_timeout_ms());
        if (display_after_poll(dpy, wl_readable) < 0 || r < 0) break;
        wl_readable = false;
        hud_tick();
    }

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
        puts("pachud " PACHUD_VERSION);
        return 0;
    }
    if (!strcmp(argv[1], "toast")) return client_toast(argc, argv);
    if (!strcmp(argv[1], "slider")) return client_generic(argc, argv, 2);
    if (!strcmp(argv[1], "alarm")) return client_alarm(argc, argv);
    if (!strcmp(argv[1], "timer") || !strcmp(argv[1], "cancel") || !strcmp(argv[1], "pause") ||
        !strcmp(argv[1], "resume"))
        return client_generic(argc, argv, 1);
    if (!strcmp(argv[1], "extend")) return client_generic(argc, argv, 2);
    if (!strcmp(argv[1], "list") || !strcmp(argv[1], "dismiss") || !strcmp(argv[1], "dnd") ||
        !strcmp(argv[1], "snooze") || !strcmp(argv[1], "action") || !strcmp(argv[1], "history") ||
        !strcmp(argv[1], "count"))
        return client_generic(argc, argv, 0);
    return usage();
}
