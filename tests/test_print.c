/* Printing: the IPP requests and answers (the answers below are real ones from a CUPS 2.4), the HTTP exchange
 * over a socket the test serves, what is said about jobs and printers, and the queue listing.
 * src_print.c is included for its statics; the HUD and the loop are stubbed. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static char toasts[32][256], icons[32][128];
static Level levels[32];
static int ntoasts, alerts;
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    (void)ms;
    assert(ntoasts < 32);
    snprintf(toasts[ntoasts], sizeof toasts[0], "%s", t);
    snprintf(icons[ntoasts], sizeof icons[0], "%s", icon ? icon : "");
    levels[ntoasts++] = l;
}
void hud_toast(const char *t, Level l, int ms) { hud_toast_icon(t, l, ms, NULL); }
void sound_play(SoundKind k) { if (k == SOUND_ALERT) alerts++; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }

#include "../src/src_print.c"

/* ---- answers as CUPS sent them ---- */

static const char SUBSCRIBED[] =   /* Create-Printer-Subscriptions: id 42, lease 86400 */
    "010100000000000101470012617474726962757465732d6368617273657400057574662d3848001b617474726962757465732d6e6174"
    "75726"
    "16c2d6c616e67756167650002656e0644000d6e6f746966792d6576656e747300157072696e7465722d73746174652d6368616e6765"
    "6444000000117072696e7465722d72657374617274656444000000107072696e7465722d73687574646f776e440000000f7072696e74"
    "65722d73746f70706564440000000d6a6f622d636f6d706c657465642100156e6f746966792d6c656173652d6475726174696f6e0004"
    "000151804500146e6f746966792d726563697069656e742d7572690007646275733a2f2f42001b6e6f746966792d737562736372696265"
    "722d757365722d6e616d650007706564726f61632100146e6f746966792d74696d652d696e74657276616c00040000000021001"
    "66e6f746966792d737562736372697074696f6e2d696400040000002a03";

static const char JOBS_ONE[] =     /* Get-Jobs: job 98, held, "pacreact test job 2" */
    "010100000000000101470012617474726962757465732d6368617273657400057574662d3848001b617474726962757465732d6e61"
    "747572616c2d6c616e67756167650002656e0245000f6a6f622d7072696e7465722d75726900306970703a2f2f6c6f63616c686f73"
    "743a3633312f7072696e746572732f48505f454e56595f343532305f7365726965734200196a6f622d6f726967696e6174696e672d"
    "757365722d6e616d650007706564726f61634200086a6f622d6e616d65001370616372656163742074657374206a6f6220322100066a6f"
    "622d69640004000000622300096a6f622d737461746500040000000403";

static const char JOBS_NONE[] =    /* Get-Jobs with nothing waiting: just the operation attributes */
    "010100000000000101470012617474726962757465732d6368617273657400057574662d3848001b617474726962757465732d6e61"
    "747572616c2d6c616e67756167650002656e03";

static size_t unhex(const char *hex, uint8_t *out) {
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(hex + 2 * i, "%2x", &v);
        out[i] = (uint8_t)v;
    }
    return n;
}

/* ---- the pieces ---- */

static void test_requests(void) {
    Ipp o;
    ipp_start(&o, OP_RENEW_SUB);
    ipp_int(&o, "notify-subscription-id", 42);
    put8(&o, TAG_SUBSCRIPTION);
    ipp_int(&o, "notify-lease-duration", 86400);
    put8(&o, TAG_END);
    assert(!o.bad);
    assert(o.b[0] == 1 && o.b[1] == 1);                              /* IPP 1.1 */
    assert(o.b[2] == 0x00 && o.b[3] == 0x1A);                        /* Renew-Subscription */
    assert(o.b[4] == 0 && o.b[5] == 0 && o.b[6] == 0 && o.b[7] == 1);
    assert(o.b[o.n - 1] == TAG_END);

    /* what a server would read back */
    IppIter it;
    IppAttr a;
    iter_start(&it, o.b, o.n);
    assert(iter_next(&it, &a) && !strcmp(a.name, "attributes-charset") && a.tag == T_CHARSET && a.group == TAG_OPERATION);
    assert(iter_next(&it, &a) && !strcmp(a.name, "attributes-natural-language"));
    assert(iter_next(&it, &a) && !strcmp(a.name, "printer-uri") && a.tag == T_URI && a.len == strlen("ipp://localhost/"));
    assert(iter_next(&it, &a) && !strcmp(a.name, "requesting-user-name") && a.len == strlen(user_name()));
    assert(iter_next(&it, &a) && !strcmp(a.name, "notify-subscription-id") && a.tag == T_INTEGER);
    int v;
    assert(attr_int(&a, &v) && v == 42);
    assert(iter_next(&it, &a) && !strcmp(a.name, "notify-lease-duration") && a.group == TAG_SUBSCRIPTION && attr_int(&a, &v) && v == 86400);
    assert(!iter_next(&it, &a));

    /* the subscription: three events under one name */
    ipp_start(&o, OP_CREATE_SUB);
    subscription_attrs(&o);
    put8(&o, TAG_END);
    iter_start(&it, o.b, o.n);
    int events = 0, recipient = 0;
    while (iter_next(&it, &a)) {
        if (!strcmp(a.name, "notify-events")) events++;
        if (!strcmp(a.name, "notify-recipient-uri") && a.len == 7 && !memcmp(a.value, "dbus://", 7)) recipient++;
    }
    assert(events == 4 && recipient == 1);
    iter_start(&it, o.b, o.n);
    bool marked = false;
    while (iter_next(&it, &a))
        if (!strcmp(a.name, "notify-user-data") && a.tag == T_OCTETS) {
            char want[32];
            snprintf(want, sizeof want, "pacreact:%d", (int)getpid());
            marked = a.len == strlen(want) && !memcmp(a.value, want, a.len);
        }
    assert(marked);                                                  /* whose it is: this process's */

    /* too much to say: it is refused, not overrun */
    ipp_start(&o, OP_GET_JOBS);
    char big[600];
    memset(big, 'x', sizeof big - 1);
    big[sizeof big - 1] = '\0';
    for (int i = 0; i < 5; i++) ipp_str(&o, T_NAME, "x", big);
    assert(o.bad && o.n <= sizeof o.b);
}

static void test_answers(void) {
    uint8_t b[1024];
    size_t n = unhex(SUBSCRIBED, b);
    assert(ipp_status(b, n) == 0);
    int id = -1, lease = 0;
    read_subscription(b, n, &id, &lease);
    assert(id == 42 && lease == 86400);

    /* a short or cut answer is never read past its end */
    assert(ipp_status(b, 4) == -1);
    for (size_t cut = 0; cut < n; cut += 7) {
        IppIter it;
        IppAttr a;
        iter_start(&it, b, cut);
        while (iter_next(&it, &a)) {}
    }
    int id2 = -1, lease2 = 0;
    read_subscription(b, 30, &id2, &lease2);
    assert(id2 == -1);

    /* the queue: one held job, with its attributes in the order CUPS sent them (the id is not first) */
    char reply[1024];
    n = unhex(JOBS_ONE, b);
    format_jobs(b, n, reply, sizeof reply);
    assert(!strcmp(reply, "ok 98 held HP_ENVY_4520_series pacreact test job 2"));
    n = unhex(JOBS_NONE, b);
    format_jobs(b, n, reply, sizeof reply);
    assert(!strcmp(reply, "ok no print jobs"));

    /* several jobs, in their states, and a name that is not to be trusted */
    Ipp o;
    memset(&o, 0, sizeof o);
    put16(&o, 0x0101); put16(&o, 0); put32(&o, 1);
    put8(&o, TAG_OPERATION);
    ipp_str(&o, T_CHARSET, "attributes-charset", "utf-8");
    static const struct { int id, state; const char *name; } jobs[] = {
        { 7, 5, "Quarterly report.pdf" }, { 8, 3, "evil\x1b[31m\nname" }, { 9, 6, "x" },
    };
    for (size_t i = 0; i < 3; i++) {
        put8(&o, TAG_JOB);
        ipp_str(&o, T_NAME, "job-name", jobs[i].name);
        ipp_str(&o, T_URI, "job-printer-uri", "ipp://localhost:631/printers/Office_Laser");
        ipp_int(&o, "job-id", jobs[i].id);
        uint8_t st[4] = { 0, 0, 0, (uint8_t)jobs[i].state };
        ipp_attr(&o, T_ENUM, "job-state", st, 4);
    }
    put8(&o, TAG_END);
    format_jobs(o.b, o.n, reply, sizeof reply);
    assert(strstr(reply, "7 printing Office_Laser Quarterly report.pdf\n8 waiting Office_Laser evil"));
    assert(strstr(reply, "\n9 stopped Office_Laser x") && !strchr(strstr(reply, "evil"), 0x1b));
    int lines = 0;
    for (const char *p = reply; *p; p++) lines += *p == '\n';
    assert(lines == 2);                                       /* a name cannot start a new line */

    /* http: only a 200 has a body that is ours */
    const uint8_t *body;
    size_t bn;
    const char ok[] = "HTTP/1.0 200 OK\r\nContent-Length: 3\r\n\r\nabc";
    assert(http_body((const uint8_t *)ok, sizeof ok - 1, &body, &bn) && bn == 3 && !memcmp(body, "abc", 3));
    const char bad[] = "HTTP/1.1 500 Server Error\r\n\r\n";
    assert(!http_body((const uint8_t *)bad, sizeof bad - 1, &body, &bn));
    assert(!http_body((const uint8_t *)"HTTP/1.0 200 OK\r\nno end", 23, &body, &bn));
    assert(!http_body((const uint8_t *)"", 0, &body, &bn));
}

/* ---- the socket: a server of the test's own, answering one connection after another ---- */

static char sock_path[100];

static pid_t serve(const char *const *answers_hex, int n) {
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s", sock_path);
    unlink(sock_path);
    assert(bind(lfd, (struct sockaddr *)&a, sizeof a) == 0 && listen(lfd, 4) == 0);
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid) { close(lfd); return pid; }
    for (int i = 0; i < n; i++) {
        int c = accept(lfd, NULL, NULL);
        char req[4096];
        ssize_t got = recv(c, req, sizeof req, 0);
        if (answers_hex[i] == NULL) { close(c); continue; }                  /* hangs up */
        /* the request is a POST of IPP with its length */
        if (got < 20 || strncmp(req, "POST / HTTP/1.0\r\n", 17) || !strstr(req, "Content-Type: application/ipp")) _exit(3);
        uint8_t body[2048];
        size_t bn = unhex(answers_hex[i], body);
        char head[128];
        int hl = snprintf(head, sizeof head, "HTTP/1.0 200 OK\r\nContent-Type: application/ipp\r\nContent-Length: %zu\r\n\r\n", bn);
        send(c, head, (size_t)hl, 0);
        send(c, body, bn, 0);
        close(c);
    }
    _exit(0);
}

static void test_cups_socket(void) {
    snprintf(sock_path, sizeof sock_path, "/tmp/pacreact-print-%d.sock", (int)getpid());
    setenv("CUPS_SERVER", sock_path, 1);
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(tfd >= 0);

    /* subscribing */
    const char *answers[] = { SUBSCRIBED };
    pid_t pid = serve(answers, 1);
    sub_id = -1;
    assert(subscribe() && sub_id == 42);
    int st;
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);                    /* the server saw a proper request */

    /* the queue */
    const char *jobs[] = { JOBS_ONE, JOBS_NONE };
    pid = serve(jobs, 2);
    char reply[1024];
    assert(print_command("print", "", reply, sizeof reply) && !strcmp(reply, "ok 98 held HP_ENVY_4520_series pacreact test job 2"));
    assert(print_command("print", "", reply, sizeof reply) && !strcmp(reply, "ok no print jobs"));
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(!print_command("timer", "", reply, sizeof reply));      /* not ours */

    /* CUPS hangs up, or is not there at all */
    const char *hang[] = { NULL };
    pid = serve(hang, 1);
    assert(print_command("print", "", reply, sizeof reply) && !strncmp(reply, "cannot reach", 12));
    waitpid(pid, &st, 0);
    unlink(sock_path);
    assert(print_command("print", "", reply, sizeof reply) && !strncmp(reply, "cannot reach", 12));

    /* a failed renewal means starting over: subscribing again */
    sub_id = 42;
    const char *again[] = { NULL, SUBSCRIBED };
    pid = serve(again, 2);
    renew();
    assert(sub_id == -1);
    assert(subscribe() && sub_id == 42);
    waitpid(pid, &st, 0);

    unlink(sock_path);
    unsetenv("CUPS_SERVER");
}

/* ---- leaving no subscription behind ---- */

static const char *hex_of(const Ipp *o) {
    static char h[4096];
    for (size_t i = 0; i < o->n; i++) sprintf(h + 2 * i, "%02x", o->b[i]);
    return h;
}

/* A Get-Subscriptions answer: each subscription a group of its own, with an id and perhaps user data */
static const char *subscriptions_answer(int dead_pid) {
    static Ipp o;
    memset(&o, 0, sizeof o);
    put16(&o, 0x0101); put16(&o, 0); put32(&o, 1);
    put8(&o, TAG_OPERATION);
    ipp_str(&o, T_CHARSET, "attributes-charset", "utf-8");
    char dead[32], mine[32];
    snprintf(dead, sizeof dead, "pacreact:%d", dead_pid);
    snprintf(mine, sizeof mine, "pacreact:%d", (int)getpid());
    const struct { int id; const char *data; } subs[] = {
        { 10, dead },          /* a pacreact that is gone: cancelled */
        { 11, "other" },       /* someone else's */
        { 12, NULL },          /* no user data */
        { 13, "pacreact" },      /* an earlier version's, which cannot be told apart: cancelled */
        { 14, "pacreact-not" },  /* not ours */
        { 15, mine },          /* this very process */
        { 16, "pacreact:1" },    /* a process that runs (init, belonging to another user): left alone */
        { 17, "pacreact:abc" },  /* not a pid */
    };
    for (size_t i = 0; i < sizeof subs / sizeof *subs; i++) {
        put8(&o, TAG_SUBSCRIPTION);
        ipp_str(&o, T_URI, "notify-recipient-uri", "dbus://");
        if (subs[i].data) ipp_attr(&o, T_OCTETS, "notify-user-data", subs[i].data, strlen(subs[i].data));
        ipp_int(&o, "notify-subscription-id", subs[i].id);
    }
    put8(&o, TAG_END);
    return hex_of(&o);
}

static void test_no_leftovers(void) {
    snprintf(sock_path, sizeof sock_path, "/tmp/pacreact-print-%d.sock", (int)getpid());
    setenv("CUPS_SERVER", sock_path, 1);
    int st;

    /* at start: ours from a run that is over are cancelled, not other programs', and not one that still runs */
    pid_t gone = fork();
    if (gone == 0) _exit(0);
    waitpid(gone, NULL, 0);                                           /* a pid that is certainly not running now */
    n_cancelled = 0;
    const char *answers[] = { subscriptions_answer((int)gone), JOBS_NONE, JOBS_NONE };
    pid_t pid = serve(answers, 3);
    cancel_old();
    waitpid(pid, &st, 0);
    assert(WIFEXITED(st) && WEXITSTATUS(st) == 0);
    assert(n_cancelled == 2 && cancelled[0] == 10 && cancelled[1] == 13);

    /* nothing to cancel when CUPS says nothing, or is not there */
    n_cancelled = 0;
    const char *none[] = { JOBS_NONE };
    pid = serve(none, 1);
    cancel_old();
    waitpid(pid, &st, 0);
    unlink(sock_path);
    cancel_old();
    assert(n_cancelled == 0);

    /* on the way out, ours is given back */
    sub_id = 42;
    const char *bye[] = { JOBS_NONE };
    pid = serve(bye, 1);
    print_shutdown();
    waitpid(pid, &st, 0);
    assert(n_cancelled == 1 && cancelled[0] == 42 && sub_id == -1);
    print_shutdown();                                                 /* and only once */
    assert(n_cancelled == 1);

    unlink(sock_path);
    unsetenv("CUPS_SERVER");
}

static void test_duplicates(void) {
    memset(seen, 0, sizeof seen);
    seen_next = 0;
    ntoasts = 0;
    job_completed("p", 5, JOB_COMPLETED, "none", "Report");
    job_completed("p", 5, JOB_COMPLETED, "none", "Report");           /* a second subscription's copy */
    job_completed("p", 5, JOB_COMPLETED, "none", "Report");
    assert(ntoasts == 1);
    job_completed("p", 6, JOB_COMPLETED, "none", "Other");            /* another job */
    job_completed("q", 5, JOB_COMPLETED, "none", "Report");           /* the same number on another printer */
    assert(ntoasts == 3);
    for (uint32_t id = 100; id < 120; id++) job_completed("p", id, JOB_COMPLETED, "none", "x");
    assert(ntoasts == 23);                                            /* the memory is short, and does not fill up */
    job_completed("p", 5, JOB_COMPLETED, "none", "Report");           /* long forgotten: said again */
    assert(ntoasts == 24);
}

/* ---- what is said ---- */

static void test_jobs(void) {
    ntoasts = alerts = 0;
    job_event("HP_ENVY_4520_series", JOB_COMPLETED, "job-completed-successfully", "Quarterly report.pdf");
    assert(ntoasts == 1 && !strcmp(toasts[0], "Printed — Quarterly report.pdf") && levels[0] == LEVEL_INFO);
    assert(!strcmp(icons[0], "printer") && alerts == 0);

    job_event("p", JOB_COMPLETED, "job-completed-with-errors", "Draft");
    assert(ntoasts == 2 && !strcmp(toasts[1], "Printed with errors — Draft") && levels[1] == LEVEL_WARN);

    job_event("p", JOB_ABORTED, "aborted-by-system", "Big file");
    assert(ntoasts == 3 && !strcmp(toasts[2], "Print failed — Big file") && levels[2] == LEVEL_WARN && alerts == 1);

    job_event("p", JOB_CANCELED, "job-canceled-by-user", "Mistake");     /* you cancelled it: you know */
    assert(ntoasts == 3);

    job_event("p", JOB_COMPLETED, "none", "");
    assert(!strcmp(toasts[3], "Printed — Document"));

    job_event("p", JOB_COMPLETED, "none", "evil\x1b[31m\nname");
    assert(!strchr(toasts[4], '\n') && !strchr(toasts[4], 0x1b));
    char longname[400];
    memset(longname, 'a', sizeof longname - 1);
    longname[sizeof longname - 1] = '\0';
    job_event("p", JOB_COMPLETED, "none", longname);
    assert(strlen(toasts[5]) < 120 && strstr(toasts[5], "…"));
    job_event("p", 6, "none", "x");                                       /* other states: nothing */
    assert(ntoasts == 6);
}

static void test_problems(void) {
    assert(problems("none") == 0 && problems("") == 0 && problems(NULL) == 0);
    assert(problems("media-empty-error") == P_PAPER);
    assert(problems("media-needed-report") == P_PAPER);
    assert(problems("toner-low-warning,media-jam-report") == (P_INK_LOW | P_JAM));
    assert(problems("marker-supply-empty-error") == P_INK_EMPTY && problems("marker-supply-low-report") == P_INK_LOW);
    assert(problems("cover-open-error") == P_COVER && problems("door-open-report") == P_COVER);
    assert(problems("offline-report") == P_OFFLINE);
    assert(problems("paused,moving-to-paused,connecting-to-device") == 0);   /* not problems */
    assert(problems("media-empty") == P_PAPER);                              /* no suffix is fine */
    assert(problems("media-empty-extra") == 0);                              /* a different reason */
}

static void test_job_start(void) {
    memset(seen, 0, sizeof seen);
    seen_next = 0;
    ntoasts = 0;
    announce_start = true;
    job_state("HP_ENVY_4520_series", 7, JOB_PROCESSING, "Quarterly report.pdf");
    assert(ntoasts == 1 && !strcmp(toasts[0], "Printing — Quarterly report.pdf") && levels[0] == LEVEL_INFO && !strcmp(icons[0], "printer"));
    job_state("HP_ENVY_4520_series", 7, JOB_PROCESSING, "Quarterly report.pdf");   /* again (a retry, a second subscription) */
    assert(ntoasts == 1);
    job_state("HP_ENVY_4520_series", 7, 3, "Quarterly report.pdf");                /* only going to the printer is news */
    job_state("HP_ENVY_4520_series", 7, 4, "Quarterly report.pdf");
    job_state("HP_ENVY_4520_series", 7, 6, "Quarterly report.pdf");
    assert(ntoasts == 1);

    job_state("p", 8, JOB_PROCESSING, "");
    assert(ntoasts == 2 && !strcmp(toasts[1], "Printing — Document"));
    job_state("p", 9, JOB_PROCESSING, "evil\x1b[31m\nname");
    assert(!strchr(toasts[2], '\n') && !strchr(toasts[2], 0x1b));

    /* its end is still said, though its start was: they are different events of one job */
    job_completed("HP_ENVY_4520_series", 7, JOB_COMPLETED, "none", "Quarterly report.pdf");
    assert(ntoasts == 4 && !strcmp(toasts[3], "Printed — Quarterly report.pdf"));

    /* print=done: no toast for the start, the rest as before */
    announce_start = false;
    job_state("p", 20, JOB_PROCESSING, "Quiet");
    assert(ntoasts == 4);
    job_completed("p", 20, JOB_COMPLETED, "none", "Quiet");
    assert(ntoasts == 5);
    announce_start = true;
}

static void test_printer_state(void) {
    memset(printers, 0, sizeof printers);
    ntoasts = alerts = 0;
    const char *p = "HP_ENVY_4520_series";

    printer_event(p, 3, "none");                                      /* all well, and nothing said */
    assert(ntoasts == 0);
    printer_event(p, 5, "media-empty-error");
    assert(ntoasts == 1 && !strcmp(toasts[0], "HP ENVY 4520 series: out of paper") && levels[0] == LEVEL_WARN && alerts == 1);
    printer_event(p, 5, "media-empty-error");                         /* the same again: not said again */
    assert(ntoasts == 1);
    printer_event(p, 5, "media-empty-error,toner-low-warning");       /* a new one is */
    assert(ntoasts == 2 && !strcmp(toasts[1], "HP ENVY 4520 series: ink or toner low") && levels[1] == LEVEL_INFO);
    printer_event(p, 3, "toner-low-warning");                         /* paper is back, ink is still low: not "ready" yet */
    assert(ntoasts == 2);
    printer_event(p, 3, "none");                                      /* everything cleared */
    assert(ntoasts == 3 && !strcmp(toasts[2], "HP ENVY 4520 series: ready") && levels[2] == LEVEL_INFO);
    printer_event(p, 3, "none");
    assert(ntoasts == 3);

    /* each problem has its words */
    printer_event(p, 5, "media-jam-error");
    assert(!strcmp(toasts[3], "HP ENVY 4520 series: paper jam"));
    printer_event(p, 5, "marker-supply-empty-error");
    assert(!strcmp(toasts[4], "HP ENVY 4520 series: out of ink or toner"));
    printer_event(p, 5, "cover-open-error");
    assert(!strcmp(toasts[5], "HP ENVY 4520 series: cover open"));
    printer_event(p, 5, "offline-report");
    assert(!strcmp(toasts[6], "HP ENVY 4520 series: offline"));

    /* stopped with nothing to say why, but not when it was paused on purpose */
    printer_event("Office_Laser", 5, "none");
    assert(ntoasts == 8 && !strcmp(toasts[7], "Office Laser: stopped"));
    printer_event("Paused_One", 5, "paused");
    assert(ntoasts == 8);

    /* printers are told apart; the table is not overrun */
    memset(printers, 0, sizeof printers);
    ntoasts = 0;
    for (int i = 0; i < MAX_PRINTERS + 3; i++) {
        char name[32];
        snprintf(name, sizeof name, "Printer_%d", i);
        printer_event(name, 5, "media-empty-error");
    }
    assert(ntoasts == MAX_PRINTERS);                                  /* the ones that did not fit are not tracked */
    memset(printers, 0, sizeof printers);
}

static void test_names(void) {
    char out[64];
    printer_label("HP_ENVY_4520_series", out, sizeof out);
    assert(!strcmp(out, "HP ENVY 4520 series"));
    clean_name("a\tb\nc", out, sizeof out);
    assert(!strcmp(out, "a b c"));
    assert(user_name() && *user_name());
    assert(!strcmp(job_state_word(3), "waiting") && !strcmp(job_state_word(5), "printing") && !strcmp(job_state_word(99), "?"));
}

int main(void) {
    test_requests();
    test_answers();
    test_cups_socket();
    test_no_leftovers();
    test_duplicates();
    test_jobs();
    test_job_start();
    test_problems();
    test_printer_state();
    test_names();
    puts("print: ok");
    return 0;
}
