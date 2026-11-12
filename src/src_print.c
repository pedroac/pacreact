#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include <pwd.h>
#include <signal.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <systemd/sd-bus.h>
#include <unistd.h>

/* Printing: a job that starts, finishes or fails, and a printer that needs attention (out of paper, a jam, ink or
 * toner, a cover, offline). CUPS announces these on the system bus, but only to whoever has asked: the
 * subscription is made over IPP on CUPS's socket, as GNOME's print notifications do, and renewed before
 * its lease runs out. The signals arrive on the bus connection, one more fd in the loop, so nothing runs
 * while nothing prints. `pachud print` lists the queue by asking CUPS when it is run.
 * PACHUD_PRINT=0 turns the toasts off, PACHUD_PRINT=done only the one for a job starting;
 * PACHUD_PRINT_DEBUG=1 says what CUPS and the bus did. */

#define TOAST_MS 4000
#define LEASE_S 86400
#define RETRY_MIN_S 60
#define RETRY_MAX_S 3600
#define HTTP_TIMEOUT_S 2
#define MAX_PRINTERS 8
#define NAME_MAX_SHOWN 80
#define SERVER_URI "ipp://localhost/"

static sd_bus *bus;
static int tfd = -1;
static int sub_id = -1;
static int retry_s = RETRY_MIN_S;
static bool announce_start = true;   /* PACHUD_PRINT=done leaves the "Printing" toast out */

static bool debug_on(void) { return getenv("PACHUD_PRINT_DEBUG") != NULL; }
#define DEBUG(...) do { if (debug_on()) { fprintf(stderr, "print: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

/* ---- IPP: requests are built, answers read, by hand ---- */

enum { OP_GET_JOBS = 0x000A, OP_CREATE_SUB = 0x0016, OP_GET_SUBS = 0x0019, OP_RENEW_SUB = 0x001A, OP_CANCEL_SUB = 0x001B };
#define MARKER "pachud"    /* notify-user-data of our subscriptions is "pachud:PID": whose they are, and whether that one still runs */
enum { TAG_OPERATION = 0x01, TAG_JOB = 0x02, TAG_END = 0x03, TAG_SUBSCRIPTION = 0x06,
       T_INTEGER = 0x21, T_ENUM = 0x23, T_KEYWORD = 0x44, T_URI = 0x45, T_CHARSET = 0x47, T_LANGUAGE = 0x48,
       T_NAME = 0x42, T_BOOLEAN = 0x22, T_OCTETS = 0x30 };

typedef struct { uint8_t b[2048]; size_t n; bool bad; } Ipp;

static void put(Ipp *o, const void *d, size_t n) {
    if (o->n + n > sizeof o->b) { o->bad = true; return; }
    memcpy(o->b + o->n, d, n);
    o->n += n;
}
static void put8(Ipp *o, unsigned v) { uint8_t c = (uint8_t)v; put(o, &c, 1); }
static void put16(Ipp *o, unsigned v) { put8(o, v >> 8); put8(o, v); }
static void put32(Ipp *o, uint32_t v) { put16(o, v >> 16); put16(o, v & 0xFFFF); }

static void ipp_attr(Ipp *o, unsigned tag, const char *name, const void *value, size_t len) {
    put8(o, tag);
    put16(o, (unsigned)strlen(name));
    put(o, name, strlen(name));
    put16(o, (unsigned)len);
    put(o, value, len);
}
static void ipp_str(Ipp *o, unsigned tag, const char *name, const char *value) { ipp_attr(o, tag, name, value, strlen(value)); }
static void ipp_int(Ipp *o, const char *name, int32_t v) {
    uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
    ipp_attr(o, T_INTEGER, name, b, 4);
}

static const char *user_name(void) {
    struct passwd *pw = getpwuid(geteuid());
    return pw && pw->pw_name ? pw->pw_name : "anonymous";
}

/* The start of a request: version 1.1, the operation, the language, and who asks and about what. */
static void ipp_start(Ipp *o, unsigned op) {
    memset(o, 0, sizeof *o);
    put16(o, 0x0101);
    put16(o, op);
    put32(o, 1);   /* request id */
    put8(o, TAG_OPERATION);
    ipp_str(o, T_CHARSET, "attributes-charset", "utf-8");
    ipp_str(o, T_LANGUAGE, "attributes-natural-language", "en");
    ipp_str(o, T_URI, "printer-uri", SERVER_URI);
    ipp_str(o, T_NAME, "requesting-user-name", user_name());
}

typedef struct { uint8_t group, tag; char name[64]; const uint8_t *value; size_t len; bool starts_group; } IppAttr;
typedef struct { const uint8_t *p, *end; uint8_t group; char last[64]; bool fresh; } IppIter;

static void iter_start(IppIter *it, const uint8_t *b, size_t n) {
    memset(it, 0, sizeof *it);
    it->p = n >= 8 ? b + 8 : b + n;   /* version, status, request id */
    it->end = b + n;
}

/* The next attribute (a further value of a multi-valued one comes with the same name), or false at the end,
 * or at anything cut short. */
static bool iter_next(IppIter *it, IppAttr *a) {
    while (it->p < it->end) {
        uint8_t tag = *it->p;
        if (tag == TAG_END) return false;
        if (tag < 0x10) { it->group = tag; it->fresh = true; it->p++; continue; }
        const uint8_t *p = it->p + 1;
        if (it->end - p < 2) return false;
        size_t nl = (size_t)(p[0] << 8 | p[1]);
        p += 2;
        if ((size_t)(it->end - p) < nl + 2) return false;
        if (nl) {
            snprintf(it->last, sizeof it->last, "%.*s", (int)(nl < sizeof it->last - 1 ? nl : sizeof it->last - 1), (const char *)p);
        }
        p += nl;
        size_t vl = (size_t)(p[0] << 8 | p[1]);
        p += 2;
        if ((size_t)(it->end - p) < vl) return false;
        a->group = it->group;
        a->starts_group = it->fresh;       /* the first attribute after a group tag: each job is a group of its own */
        it->fresh = false;
        a->tag = tag;
        snprintf(a->name, sizeof a->name, "%s", it->last);
        a->value = p;
        a->len = vl;
        it->p = p + vl;
        return true;
    }
    return false;
}

static bool attr_int(const IppAttr *a, int *v) {
    if (a->len != 4) return false;
    *v = (int)((uint32_t)a->value[0] << 24 | (uint32_t)a->value[1] << 16 | (uint32_t)a->value[2] << 8 | a->value[3]);
    return true;
}

static void attr_text(const IppAttr *a, char *out, size_t n) {
    snprintf(out, n, "%.*s", (int)(a->len < n - 1 ? a->len : n - 1), (const char *)a->value);
}

/* The status of an answer: 0 is "ok" (and the harmless "ok, but ..." ones, below 0x0100), or -1 if it is too short. */
static int ipp_status(const uint8_t *b, size_t n) {
    return n >= 8 ? b[2] << 8 | b[3] : -1;
}

/* ---- HTTP: one POST over CUPS's unix socket ---- */

static const char *server_path(void) {
    const char *s = getenv("CUPS_SERVER");
    return s && *s == '/' ? s : "/run/cups/cups.sock";
}

/* The body of an HTTP answer, if it is a 200 */
static bool http_body(const uint8_t *raw, size_t n, const uint8_t **body, size_t *bn) {
    if (n < 12 || strncmp((const char *)raw, "HTTP/1.", 7) || memcmp(raw + 9, "200", 3)) return false;
    for (size_t i = 0; i + 4 <= n; i++) {
        if (!memcmp(raw + i, "\r\n\r\n", 4)) {
            *body = raw + i + 4;
            *bn = n - i - 4;
            return true;
        }
    }
    return false;
}

/* Sends the request and returns the IPP status of the answer, with its body in resp (cap bytes) and its length
 * in *rn; -1 when CUPS could not be reached or answered something else. HTTP/1.0 keeps the answer in one piece:
 * no chunking, and the end is the end of the connection. */
static int ipp_call(const Ipp *req, uint8_t *resp, size_t cap, size_t *rn) {
    if (req->bad) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    if (snprintf(a.sun_path, sizeof a.sun_path, "%s", server_path()) >= (int)sizeof a.sun_path) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = { .tv_sec = HTTP_TIMEOUT_S };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }

    char head[256];
    int hl = snprintf(head, sizeof head, "POST / HTTP/1.0\r\nHost: localhost\r\nContent-Type: application/ipp\r\n"
                                         "Content-Length: %zu\r\n\r\n", req->n);
    bool sent = send(fd, head, (size_t)hl, MSG_NOSIGNAL) == hl && send(fd, req->b, req->n, MSG_NOSIGNAL) == (ssize_t)req->n;
    size_t got = 0;
    while (sent && got < cap) {
        ssize_t r = recv(fd, resp + got, cap - got, 0);
        if (r <= 0) break;
        got += (size_t)r;
    }
    close(fd);

    const uint8_t *body;
    size_t bn;
    if (!sent || !http_body(resp, got, &body, &bn)) return -1;
    memmove(resp, body, bn);
    *rn = bn;
    return ipp_status(resp, bn);
}

/* ---- the subscription ---- */

static void arm(long secs) {
    struct itimerspec its = { .it_value = { .tv_sec = secs } };
    if (tfd >= 0) timerfd_settime(tfd, 0, &its, NULL);
}

static void subscription_attrs(Ipp *o) {
    put8(o, TAG_SUBSCRIPTION);
    ipp_str(o, T_URI, "notify-recipient-uri", "dbus://");
    static const char *const events[] = { "job-completed", "job-state-changed", "printer-state-changed", "printer-stopped" };
    for (size_t i = 0; i < sizeof events / sizeof *events; i++) ipp_str(o, T_KEYWORD, i ? "" : "notify-events", events[i]);
    ipp_int(o, "notify-lease-duration", LEASE_S);
    char marker[32];
    snprintf(marker, sizeof marker, MARKER ":%d", (int)getpid());
    ipp_attr(o, T_OCTETS, "notify-user-data", marker, strlen(marker));
}

/* Reads the subscription's id and lease out of an answer. */
static void read_subscription(const uint8_t *resp, size_t n, int *id, int *lease) {
    IppIter it;
    IppAttr a;
    iter_start(&it, resp, n);
    while (iter_next(&it, &a)) {
        if (!strcmp(a.name, "notify-subscription-id")) attr_int(&a, id);
        else if (!strcmp(a.name, "notify-lease-duration")) attr_int(&a, lease);
    }
}

static bool subscribe(void) {
    static uint8_t resp[4096];
    Ipp o;
    ipp_start(&o, OP_CREATE_SUB);
    subscription_attrs(&o);
    put8(&o, TAG_END);
    size_t n;
    int status = ipp_call(&o, resp, sizeof resp, &n), id = -1, lease = LEASE_S;
    if (status < 0 || status >= 0x0100) { DEBUG("could not subscribe (%d)", status); return false; }
    read_subscription(resp, n, &id, &lease);
    if (id < 0) return false;
    sub_id = id;
    DEBUG("subscribed as %d, lease %d s", id, lease);
    retry_s = RETRY_MIN_S;
    arm(lease / 2 > RETRY_MIN_S ? lease / 2 : RETRY_MIN_S);   /* well before it runs out */
    return true;
}

static void renew(void) {
    static uint8_t resp[4096];
    Ipp o;
    ipp_start(&o, OP_RENEW_SUB);
    ipp_int(&o, "notify-subscription-id", sub_id);
    put8(&o, TAG_SUBSCRIPTION);
    ipp_int(&o, "notify-lease-duration", LEASE_S);
    put8(&o, TAG_END);
    size_t n;
    int status = ipp_call(&o, resp, sizeof resp, &n);
    if (status >= 0 && status < 0x0100) {
        int id = sub_id, lease = LEASE_S;
        read_subscription(resp, n, &id, &lease);
        DEBUG("renewed, lease %d s", lease);
        arm(lease / 2 > RETRY_MIN_S ? lease / 2 : RETRY_MIN_S);
        return;
    }
    DEBUG("renewal failed (%d): subscribing again", status);
    sub_id = -1;   /* CUPS forgot it (restarted, or the lease ran out): start over */
}

static int cancelled[16], n_cancelled;   /* what was cancelled, for the tests to see */

static void cancel_sub(int id) {
    static uint8_t resp[2048];
    Ipp o;
    ipp_start(&o, OP_CANCEL_SUB);
    ipp_int(&o, "notify-subscription-id", id);
    put8(&o, TAG_END);
    size_t n;
    int status = ipp_call(&o, resp, sizeof resp, &n);
    DEBUG("cancelled subscription %d (%d)", id, status);
    if (n_cancelled < 16) cancelled[n_cancelled++] = id;
}

/* Whether a subscription's user data says it was made by a pachud that is no longer running: "pachud:PID" with no such
 * process (a different one's, still running, is left alone), or the bare "pachud" of an earlier version. */
static bool left_behind(const IppAttr *a) {
    char data[32];
    attr_text(a, data, sizeof data);
    if (!strcmp(data, MARKER)) return true;
    if (strncmp(data, MARKER ":", strlen(MARKER) + 1)) return false;
    char *end;
    long pid = strtol(data + strlen(MARKER) + 1, &end, 10);
    if (*end || pid <= 0 || pid == (long)getpid()) return false;
    return kill((pid_t)pid, 0) < 0 && errno == ESRCH;
}

/* A previous run that did not get to cancel its subscription (it crashed, or was killed) leaves it behind until
 * its lease ends, and CUPS sends every signal once per subscription: those of ours whose process is gone,
 * recognised by the marker and by nothing else, are cancelled before a new one is made. */
static void cancel_old(void) {
    static uint8_t resp[8192];
    Ipp o;
    ipp_start(&o, OP_GET_SUBS);
    ipp_attr(&o, T_BOOLEAN, "my-subscriptions", "\x01", 1);
    put8(&o, TAG_END);
    size_t n;
    int status = ipp_call(&o, resp, sizeof resp, &n);
    if (status < 0 || status >= 0x0100) return;

    int ours[16], count = 0, id = -1;
    bool marked = false;
    IppIter it;
    IppAttr a;
    iter_start(&it, resp, n);
    while (iter_next(&it, &a)) {
        if (a.group != TAG_SUBSCRIPTION) continue;
        if (a.starts_group) {              /* the previous subscription is complete */
            if (marked && id >= 0 && count < 16) ours[count++] = id;
            id = -1;
            marked = false;
        }
        if (!strcmp(a.name, "notify-subscription-id")) attr_int(&a, &id);
        else if (!strcmp(a.name, "notify-user-data")) marked = left_behind(&a);
    }
    if (marked && id >= 0 && count < 16) ours[count++] = id;
    for (int i = 0; i < count; i++) cancel_sub(ours[i]);
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(tfd, &n, sizeof n) <= 0) return;
    if (sub_id >= 0) renew();
    if (sub_id < 0 && !subscribe()) {     /* CUPS is not there (yet): try again, less and less often */
        arm(retry_s);
        retry_s = retry_s * 2 > RETRY_MAX_S ? RETRY_MAX_S : retry_s * 2;
    }
}

/* ---- what is said ---- */

/* A job name is whatever the app called it (often a file name or a web page): one line, not too long. */
static void clean_name(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o < n - 1; in++) out[o++] = (unsigned char)*in < 0x20 || *in == 0x7f ? ' ' : *in;
    out[o] = '\0';
    if (o > NAME_MAX_SHOWN) {   /* cut, on a character boundary */
        size_t cut = NAME_MAX_SHOWN;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) cut--;
        snprintf(out + cut, n - cut, "…");
    }
}

/* "HP_ENVY_4520_series" is "HP ENVY 4520 series" */
static void printer_label(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s", name);
    for (char *p = out; *p; p++)
        if (*p == '_') *p = ' ';
}

enum { JOB_CANCELED = 7, JOB_ABORTED = 8, JOB_COMPLETED = 9 };

static void job_event(const char *printer, unsigned state, const char *reasons, const char *job_name) {
    (void)printer;
    char name[128 + 8], msg[256];
    clean_name(job_name, name, sizeof name);
    if (!*name) snprintf(name, sizeof name, "%s", _("Document"));
    if (state == JOB_COMPLETED) {
        bool errors = reasons && (strstr(reasons, "completed-with-errors") || strstr(reasons, "completed-with-warnings"));
        snprintf(msg, sizeof msg, errors ? _("Printed with errors — %s") : _("Printed — %s"), name);
        hud_toast_icon(msg, errors ? LEVEL_WARN : LEVEL_INFO, TOAST_MS, errors ? "printer-warning,printer-error,printer" : "printer");
    } else if (state == JOB_ABORTED) {
        snprintf(msg, sizeof msg, _("Print failed — %s"), name);
        hud_toast_icon(msg, LEVEL_WARN, TOAST_MS, "printer-error,printer-warning,printer");
        sound_play(SOUND_ALERT);
    }
    /* canceled: by you, so you know */
}

/* The same job's end can arrive more than once: CUPS sends a signal per subscription, and another program may
 * have one of its own. */
static struct { uint32_t id, state; char printer[64]; } seen[8];
static int seen_next;

static void job_completed(const char *printer, uint32_t id, uint32_t state, const char *reasons, const char *name) {
    for (size_t i = 0; i < sizeof seen / sizeof *seen; i++)
        if (seen[i].id == id && seen[i].state == state && !strcmp(seen[i].printer, printer)) return;
    seen[seen_next].id = id;
    seen[seen_next].state = state;
    snprintf(seen[seen_next].printer, sizeof seen[0].printer, "%s", printer);
    seen_next = (seen_next + 1) % (int)(sizeof seen / sizeof *seen);
    job_event(printer, state, reasons, name);
}

enum { JOB_PROCESSING = 5 };

/* A job's state changed: only its going to the printer is news (it ends, or fails, in job_completed()). */
static void job_state(const char *printer, uint32_t id, uint32_t state, const char *name) {
    if (state != JOB_PROCESSING || !announce_start) return;
    for (size_t i = 0; i < sizeof seen / sizeof *seen; i++)
        if (seen[i].id == id && seen[i].state == state && !strcmp(seen[i].printer, printer)) return;   /* once per job */
    seen[seen_next].id = id;
    seen[seen_next].state = state;
    snprintf(seen[seen_next].printer, sizeof seen[0].printer, "%s", printer);
    seen_next = (seen_next + 1) % (int)(sizeof seen / sizeof *seen);

    char shown[128 + 8], msg[256];
    clean_name(name, shown, sizeof shown);
    if (!*shown) snprintf(shown, sizeof shown, "%s", _("Document"));
    snprintf(msg, sizeof msg, _("Printing — %s"), shown);
    hud_toast_icon(msg, LEVEL_INFO, TOAST_MS, "printer");
}

enum { P_PAPER = 1 << 0, P_JAM = 1 << 1, P_INK_EMPTY = 1 << 2, P_INK_LOW = 1 << 3, P_COVER = 1 << 4, P_OFFLINE = 1 << 5,
       P_STOPPED = 1 << 6 };

/* The problems in a printer's state reasons: "media-empty-error,toner-low-warning". The suffix that says how
 * bad it is does not matter here. */
static unsigned problems(const char *reasons) {
    static const struct { const char *reason; unsigned bit; } table[] = {
        { "media-empty", P_PAPER }, { "media-needed", P_PAPER }, { "media-jam", P_JAM },
        { "toner-empty", P_INK_EMPTY }, { "marker-supply-empty", P_INK_EMPTY },
        { "toner-low", P_INK_LOW }, { "marker-supply-low", P_INK_LOW },
        { "cover-open", P_COVER }, { "door-open", P_COVER }, { "interlock-open", P_COVER },
        { "offline", P_OFFLINE },
    };
    unsigned found = 0;
    char copy[256];
    snprintf(copy, sizeof copy, "%s", reasons ? reasons : "");
    for (char *save, *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        size_t len = strlen(tok);
        static const char *const suffixes[] = { "-error", "-warning", "-report" };
        for (size_t i = 0; i < sizeof suffixes / sizeof *suffixes; i++) {
            size_t sl = strlen(suffixes[i]);
            if (len > sl && !strcmp(tok + len - sl, suffixes[i])) { tok[len - sl] = '\0'; break; }
        }
        for (size_t i = 0; i < sizeof table / sizeof *table; i++)
            if (!strcmp(tok, table[i].reason)) found |= table[i].bit;
    }
    return found;
}

typedef struct { char name[64]; unsigned problems; } Printer;
static Printer printers[MAX_PRINTERS];

static void say_problem(const char *label, unsigned bit) {
    char msg[160];
    const char *fmt = NULL, *icon = "printer-error,printer-warning,printer";
    Level level = LEVEL_WARN;
    switch (bit) {
    case P_PAPER: fmt = _("%s: out of paper"); break;
    case P_JAM: fmt = _("%s: paper jam"); break;
    case P_INK_EMPTY: fmt = _("%s: out of ink or toner"); break;
    case P_INK_LOW: fmt = _("%s: ink or toner low"); level = LEVEL_INFO; icon = "printer-warning,printer"; break;
    case P_COVER: fmt = _("%s: cover open"); break;
    case P_OFFLINE: fmt = _("%s: offline"); break;
    case P_STOPPED: fmt = _("%s: stopped"); break;
    default: return;
    }
    snprintf(msg, sizeof msg, fmt, label);
    hud_toast_icon(msg, level, TOAST_MS, icon);
    if (level == LEVEL_WARN) sound_play(SOUND_ALERT);
}

enum { PRINTER_STOPPED = 5 };

/* A printer's state changed: say what has newly gone wrong, and when it is all well again. */
static void printer_event(const char *name, unsigned state, const char *reasons) {
    Printer *p = NULL, *free_slot = NULL;
    for (int i = 0; i < MAX_PRINTERS; i++) {
        if (!strcmp(printers[i].name, name)) p = &printers[i];
        else if (!printers[i].name[0] && !free_slot) free_slot = &printers[i];
    }
    if (!p) {
        if (!free_slot) return;
        p = free_slot;
        snprintf(p->name, sizeof p->name, "%s", name);
        p->problems = 0;
    }
    unsigned now = problems(reasons);
    if (state == PRINTER_STOPPED && !now && !(reasons && strstr(reasons, "paused"))) now = P_STOPPED;   /* stopped, and nothing says why */
    unsigned appeared = now & ~p->problems;
    char label[96];
    printer_label(name, label, sizeof label);
    for (unsigned bit = 1; bit <= P_STOPPED; bit <<= 1)
        if (appeared & bit) say_problem(label, bit);
    if (p->problems && !now) {
        char msg[160];
        snprintf(msg, sizeof msg, _("%s: ready"), label);
        hud_toast_icon(msg, LEVEL_INFO, TOAST_MS, "printer");
    }
    p->problems = now;
}

/* org.cups.cupsd.Notifier signals: the printer events carry (text, printer uri, printer name, printer state,
 * state reasons, accepting jobs); the job ones go on with (job id, job state, job state reasons, job name,
 * impressions). */
static int on_signal(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    const char *member = sd_bus_message_get_member(m);
    const char *text, *uri, *name, *reasons;
    uint32_t pstate;
    int accepting;
    if (!member) return 0;
    if (sd_bus_message_read(m, "sssusb", &text, &uri, &name, &pstate, &reasons, &accepting) < 0) return 0;
    DEBUG("signal %s", member);
    if (!strcmp(member, "JobCompleted")) {
        uint32_t id, jstate, impressions;
        const char *jreasons, *jname;
        if (sd_bus_message_read(m, "uussu", &id, &jstate, &jreasons, &jname, &impressions) < 0) return 0;
        DEBUG("job %u of %s: state %u (%s), \"%s\"", id, name, jstate, jreasons, jname);
        job_completed(name, id, jstate, jreasons, jname);
    } else if (!strcmp(member, "JobStateChanged") || !strcmp(member, "JobState")) {
        uint32_t id, jstate, impressions;
        const char *jreasons, *jname;
        if (sd_bus_message_read(m, "uussu", &id, &jstate, &jreasons, &jname, &impressions) < 0) return 0;
        DEBUG("job %u of %s: now in state %u (%s), \"%s\"", id, name, jstate, jreasons, jname);
        job_state(name, id, jstate, jname);
    } else if (!strcmp(member, "PrinterStateChanged") || !strcmp(member, "PrinterStopped") ||
               !strcmp(member, "PrinterRestarted")) {
        DEBUG("printer %s: state %u, \"%s\"", name, pstate, reasons);
        printer_event(name, pstate, reasons);
    }
    return 0;
}

static void on_bus(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    while (sd_bus_process(bus, NULL) > 0) {}
}

/* ---- the queue, on request ---- */

static const char *job_state_word(int state) {
    switch (state) {
    case 3: return "waiting";
    case 4: return "held";
    case 5: return "printing";
    case 6: return "stopped";
    default: return "?";
    }
}

/* The whole reply for a Get-Jobs answer: "ok", then one line per job that has not finished, like
 * "98 held HP_ENVY_4520_series pachud test job". The attributes of a job come in no fixed order, so a job is
 * what lies between two group tags. */
static void format_jobs(const uint8_t *resp, size_t n, char *reply, size_t cap) {
    typedef struct { int id, state; char printer[128], name[128]; } Job;
    Job jobs[32];
    int count = 0;
    Job *cur = NULL;
    IppIter it;
    IppAttr a;
    iter_start(&it, resp, n);
    while (iter_next(&it, &a)) {
        if (a.group != TAG_JOB) continue;
        if (a.starts_group) {
            if (count == 32) break;
            cur = &jobs[count++];
            memset(cur, 0, sizeof *cur);
        }
        if (!cur) continue;
        if (!strcmp(a.name, "job-id")) {
            attr_int(&a, &cur->id);
        } else if (!strcmp(a.name, "job-state")) {
            attr_int(&a, &cur->state);
        } else if (!strcmp(a.name, "job-name")) {
            attr_text(&a, cur->name, sizeof cur->name);
        } else if (!strcmp(a.name, "job-printer-uri")) {
            char uri[128];
            attr_text(&a, uri, sizeof uri);
            const char *slash = strrchr(uri, '/');
            snprintf(cur->printer, sizeof cur->printer, "%s", slash ? slash + 1 : uri);
        }
    }
    if (!count) { snprintf(reply, cap, "ok %s", _("no print jobs")); return; }
    size_t len = (size_t)snprintf(reply, cap, "ok");
    for (int i = 0; i < count && len < cap - 1; i++) {
        char name[128 + 8];
        clean_name(jobs[i].name, name, sizeof name);
        len += (size_t)snprintf(reply + len, cap - len, "%s%d %s %s %s", i ? "\n" : " ", jobs[i].id,
                                job_state_word(jobs[i].state), jobs[i].printer, name);
    }
}

bool print_command(const char *cmd, char *args, char *reply, size_t n) {
    (void)args;
    if (strcmp(cmd, "print")) return false;
    static uint8_t resp[16384];
    Ipp o;
    ipp_start(&o, OP_GET_JOBS);
    ipp_str(&o, T_KEYWORD, "which-jobs", "not-completed");
    ipp_int(&o, "limit", 32);
    ipp_str(&o, T_KEYWORD, "requested-attributes", "job-id");
    static const char *const more[] = { "job-name", "job-state", "job-printer-uri" };
    for (size_t i = 0; i < sizeof more / sizeof *more; i++) ipp_str(&o, T_KEYWORD, "", more[i]);
    put8(&o, TAG_END);
    size_t rn;
    int status = ipp_call(&o, resp, sizeof resp, &rn);
    if (status < 0 || status >= 0x0100) {
        snprintf(reply, n, "%s", _("cannot reach the print system (CUPS)"));
        return true;
    }
    format_jobs(resp, rn, reply, n);
    return true;
}

void print_init(void) {
    const char *off = getenv("PACHUD_PRINT");
    if (off && !strcmp(off, "0")) return;
    announce_start = !(off && !strcmp(off, "done"));
    if (access(server_path(), F_OK) != 0) return;     /* no CUPS here: nothing to ask, and nothing to start */
    if (sd_bus_open_system(&bus) < 0) { bus = NULL; return; }
    if (sd_bus_add_match(bus, NULL, "type='signal',interface='org.cups.cupsd.Notifier'", on_signal, NULL) < 0) {
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        return;
    }
    loop_add(sd_bus_get_fd(bus), EPOLLIN, on_bus, NULL);
    while (sd_bus_process(bus, NULL) > 0) {}

    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd < 0) return;
    loop_add(tfd, EPOLLIN, on_timer, NULL);
    cancel_old();
    if (!subscribe()) { arm(retry_s); retry_s *= 2; }
}

/* On the way out: the subscription is no longer wanted, so it is not left to run out its lease. */
void print_shutdown(void) {
    if (sub_id >= 0) cancel_sub(sub_id);
    sub_id = -1;
}
