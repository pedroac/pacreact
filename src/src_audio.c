#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Volume lives in the sound server (PulseAudio, or PipeWire's pulse layer), not the kernel.
 * This speaks just enough of the PulseAudio native protocol over its unix socket: subscribe to
 * sink and server events, and read the default sink's volume and mute state when they change.
 * The socket is one more fd in the loop, so there is nothing to wake while the level is
 * stable, and no helper process. */

#define PROTOCOL_VERSION 35
#define INVALID_INDEX 0xFFFFFFFFu
#define VOLUME_NORM 0x10000u
#define COOKIE_LEN 256
#define MAX_FRAME 60000
#define DESCRIPTOR_LEN 20

#define DEBOUNCE_MS 40     /* coalesces a volume drag into few redraws */
#define RECONNECT_MIN_S 5
#define RECONNECT_MAX_S 300  /* the server may be absent; don't hammer */

enum { CMD_ERROR = 0, CMD_REPLY = 2, CMD_AUTH = 8, CMD_SET_CLIENT_NAME = 9,
       CMD_GET_SINK_INFO = 21, CMD_GET_SOURCE_INFO = 23, CMD_SUBSCRIBE = 35,
       CMD_SUBSCRIBE_EVENT = 66, CMD_GET_CARD_INFO_LIST = 89 };
enum { MASK_SINK = 0x1, MASK_SOURCE = 0x2, MASK_SERVER = 0x80, MASK_CARD = 0x200 };
enum { FACILITY_MASK = 0xF, FACILITY_SINK = 0, FACILITY_SOURCE = 1, FACILITY_SERVER = 7,
       FACILITY_CARD = 9 };
/* A card port: whether something is plugged into it, its direction and what kind it is */
enum { AVAILABLE_NO = 1 };
enum { DIRECTION_OUTPUT = 1, DIRECTION_INPUT = 2 };
enum { PORT_LINE = 4, PORT_MIC = 5, PORT_HEADPHONES = 3, PORT_HEADSET = 6 };

typedef enum { S_DOWN, S_AUTH, S_NAME, S_SUBSCRIBE, S_READY } State;

static int sock = -1, debounce_fd = -1, reconnect_fd = -1;
static State state = S_DOWN;
static uint32_t next_tag;
static int reconnect_s = RECONNECT_MIN_S;
/* What is watched: the default output and the default input, each with the same volume and
 * mute fields in the server's reply. One request is outstanding at a time. */
typedef enum { T_SINK, T_SOURCE, T_CARDS, T_COUNT } TrackId;

typedef struct {
    const char *object;    /* the alias the server resolves to the current default device */
    uint32_t command;
    int last_pct, last_muted;
    char name[128];        /* the output's name, to notice that the default changed (outputs only) */
    bool dirty;            /* an event arrived: ask once the burst settles */
    bool want, want_announce;  /* a request is queued, and whether its answer may show a HUD */
} Track;

static Track tracks[T_COUNT] = {
    [T_SINK] = { "@DEFAULT_SINK@", CMD_GET_SINK_INFO, -1, -1, "", false, false, false },
    [T_SOURCE] = { "@DEFAULT_SOURCE@", CMD_GET_SOURCE_INFO, -1, -1, "", false, false, false },
    [T_CARDS] = { NULL, CMD_GET_CARD_INFO_LIST, -1, -1, "", false, false, false },
};
static int pending = -1;   /* the track the outstanding request is for, or -1 */
static bool announce;      /* whether the outstanding answer may show a HUD */
#define PLUG_QUIET_MS 1500  /* after a jack plug: its port's remembered volume is not the user's doing */
static long long quiet_until_ms;

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

void audio_jack_plugged(void) { quiet_until_ms = now_ms() + PLUG_QUIET_MS; }

/* The status icon for a level: the speaker with more or fewer waves, struck through when muted */
static const char *icon_for(TrackId t, int pct, bool muted) {
    if (t == T_SOURCE)
        return muted ? "microphone-sensitivity-muted" : pct < 34 ? "microphone-sensitivity-low"
             : pct < 67 ? "microphone-sensitivity-medium" : "microphone-sensitivity-high";
    if (muted || pct <= 0) return "audio-volume-muted";
    if (pct > 100) return "audio-volume-overamplified,audio-volume-high";
    return pct < 34 ? "audio-volume-low" : pct < 67 ? "audio-volume-medium" : "audio-volume-high";
}

static const char *label_for(TrackId t, bool muted) {
    if (t == T_SOURCE) return muted ? _("Microphone — muted") : _("Microphone");
    return muted ? _("Volume — muted") : _("Volume");
}

static uint8_t inbuf[DESCRIPTOR_LEN + MAX_FRAME];
static size_t inlen;

/* ---- tagstruct writing ---- */

typedef struct { uint8_t b[512]; size_t n; } Out;

static void put_u32(Out *o, uint32_t v) {
    o->b[o->n++] = 'L';
    v = htonl(v);
    memcpy(o->b + o->n, &v, 4);
    o->n += 4;
}

static void put_str(Out *o, const char *s) {
    size_t l = strlen(s) + 1;
    o->b[o->n++] = 't';
    memcpy(o->b + o->n, s, l);
    o->n += l;
}

static void put_arbitrary(Out *o, const void *d, uint32_t l) {
    uint32_t be = htonl(l);
    o->b[o->n++] = 'x';
    memcpy(o->b + o->n, &be, 4);
    memcpy(o->b + o->n + 4, d, l);
    o->n += 4 + l;
}

static bool send_packet(const Out *o) {
    uint8_t hdr[DESCRIPTOR_LEN] = {0};
    uint32_t len = htonl(o->n), channel = htonl(0xFFFFFFFFu);  /* control channel */
    memcpy(hdr, &len, 4);
    memcpy(hdr + 4, &channel, 4);
    struct iovec iov[2] = { { hdr, sizeof hdr }, { (void *)o->b, o->n } };
    return writev(sock, iov, 2) == (ssize_t)(sizeof hdr + o->n);
}

static void command(Out *o, uint32_t cmd) {
    o->n = 0;
    put_u32(o, cmd);
    put_u32(o, ++next_tag);
}

/* ---- tagstruct reading ---- */

typedef struct { const uint8_t *p, *end; bool bad; } In;

static bool need(In *in, size_t n) {
    if (in->bad || (size_t)(in->end - in->p) < n) { in->bad = true; return false; }
    return true;
}

static uint32_t get_u32(In *in) {
    if (!need(in, 5) || in->p[0] != 'L') { in->bad = true; return 0; }
    uint32_t v;
    memcpy(&v, in->p + 1, 4);
    in->p += 5;
    return ntohl(v);
}

/* A string into out ("" for a null string). */
static void get_string(In *in, char *out, size_t n) {
    out[0] = '\0';
    if (!need(in, 1)) return;
    if (*in->p == 'N') { in->p++; return; }
    if (*in->p != 't') { in->bad = true; return; }
    const uint8_t *nul = memchr(in->p + 1, 0, in->end - in->p - 1);
    if (!nul) { in->bad = true; return; }
    snprintf(out, n, "%s", (const char *)in->p + 1);
    in->p = nul + 1;
}

static void skip_sample_spec(In *in) {  /* 'a' format(1) channels(1) rate(4) */
    if (!need(in, 7) || in->p[0] != 'a') { in->bad = true; return; }
    in->p += 7;
}

static void skip_channel_map(In *in) {  /* 'm' channels(1) positions(channels) */
    if (!need(in, 2) || in->p[0] != 'm') { in->bad = true; return; }
    size_t n = in->p[1];
    if (!need(in, 2 + n)) return;
    in->p += 2 + n;
}

/* 'v' channels(1) volume(4 each); returns the loudest channel. */
static uint32_t get_cvolume(In *in) {
    if (!need(in, 2) || in->p[0] != 'v') { in->bad = true; return 0; }
    size_t n = in->p[1];
    if (!need(in, 2 + 4 * n)) return 0;
    uint32_t max = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t v;
        memcpy(&v, in->p + 2 + 4 * i, 4);
        v = ntohl(v);
        if (v > max) max = v;
    }
    in->p += 2 + 4 * n;
    return max;
}

static bool get_bool(In *in) {
    if (!need(in, 1) || (*in->p != '1' && *in->p != '0')) { in->bad = true; return false; }
    return *in->p++ == '1';
}

/* ---- connection ---- */

static void arm(int fd, long ms) {
    struct itimerspec its = { .it_value = { .tv_sec = ms / 1000, .tv_nsec = ms % 1000 * 1000000L } };
    timerfd_settime(fd, 0, &its, NULL);
}

static void report_ports(const PortChanges *c);   /* below, with the card readings */

static void disconnect(void) {
    if (sock >= 0) {
        loop_remove(sock);
        close(sock);
        sock = -1;
    }
    state = S_DOWN;
    inlen = 0;
    pending = -1;
    for (int t = 0; t < T_COUNT; t++) tracks[t].want = tracks[t].dirty = false;
    report_ports(NULL);
    arm(reconnect_fd, reconnect_s * 1000L);
    reconnect_s = reconnect_s * 2 > RECONNECT_MAX_S ? RECONNECT_MAX_S : reconnect_s * 2;
}

static bool server_path(struct sockaddr_un *a) {
    const char *server = getenv("PULSE_SERVER"), *dir = getenv("XDG_RUNTIME_DIR");
    a->sun_family = AF_UNIX;
    if (server && *server) {
        if (!strncmp(server, "unix:", 5)) server += 5;
        if (*server != '/') return false;  /* TCP and friends: not supported */
        return snprintf(a->sun_path, sizeof a->sun_path, "%s", server) < (int)sizeof a->sun_path;
    }
    if (!dir || !*dir) return false;
    return snprintf(a->sun_path, sizeof a->sun_path, "%s/pulse/native", dir) < (int)sizeof a->sun_path;
}

/* Sends the next queued request, if none is outstanding. */
static void request_next(void) {
    if (pending >= 0 || state != S_READY) return;
    for (int t = 0; t < T_COUNT; t++) {
        Track *k = &tracks[t];
        if (!k->want) continue;
        k->want = false;
        Out o;
        command(&o, k->command);
        if (k->object) {
            put_u32(&o, INVALID_INDEX);
            put_str(&o, k->object);
        }
        if (!send_packet(&o)) { disconnect(); return; }
        pending = t;
        announce = k->want_announce;
        return;
    }
}

static void want_info(TrackId t, bool show) {
    Track *k = &tracks[t];
    k->want_announce = k->want ? (k->want_announce || show) : show;
    k->want = true;
    request_next();
}

static uint8_t get_u8(In *in) {
    if (!need(in, 2) || in->p[0] != 'B') { in->bad = true; return 0; }
    uint8_t v = in->p[1];
    in->p += 2;
    return v;
}

static void skip_s64(In *in) {  /* 'r' and eight bytes */
    if (!need(in, 9) || in->p[0] != 'r') { in->bad = true; return; }
    in->p += 9;
}

/* 'P', then key (string), length (u32) and value (arbitrary) until a null string */
static void skip_proplist(In *in) {
    if (!need(in, 1) || in->p[0] != 'P') { in->bad = true; return; }
    in->p++;
    while (!in->bad && need(in, 1) && *in->p != 'N') {
        char key[128];
        get_string(in, key, sizeof key);
        get_u32(in);
        if (!need(in, 5) || in->p[0] != 'x') { in->bad = true; return; }
        uint32_t n;
        memcpy(&n, in->p + 1, 4);
        n = ntohl(n);
        if (!need(in, 5 + (size_t)n)) return;
        in->p += 5 + n;
    }
    if (!in->bad) in->p++;   /* the null string that ends it */
}

/* ---- card ports: what a jack plug did, as the server sees it ---- */

#define MAX_PORTS 64

typedef struct {
    char key[200];     /* card and port name */
    bool present;      /* something is plugged in, or the hardware cannot tell */
} PortState;

static PortState port_state[MAX_PORTS];
static int n_port_state;
static bool ports_known;            /* a first reading was taken: later ones can be compared with it */
static PortChanges changes;         /* what flipped since the last answer */
static void (*port_cb)(const PortChanges *);

static void report_ports(const PortChanges *c) {
    void (*cb)(const PortChanges *) = port_cb;
    if (!cb) return;      /* nobody is waiting: what flipped is kept for whoever asks next */
    port_cb = NULL;
    cb(c);
    if (c) memset(&changes, 0, sizeof changes);
}

/* One reading of every card: remember the ports that matter to a jack, and note which of them went
 * from absent to present or back. A port that is "unknown" counts as present: a codec that cannot
 * detect its headphones reports that, and moving from "no" to it still means something happened. */
static void handle_cards(In *in) {
    PortState now[MAX_PORTS];
    PortChanges found = {0};
    int n_now = 0;

    while (!in->bad && in->p < in->end) {
        get_u32(in);                       /* index */
        char card[96], text[128];
        get_string(in, card, sizeof card);
        get_u32(in);                       /* owner module */
        get_string(in, text, sizeof text); /* driver */
        uint32_t profiles = get_u32(in);
        for (uint32_t i = 0; i < profiles && !in->bad; i++) {
            get_string(in, text, sizeof text);   /* name */
            get_string(in, text, sizeof text);   /* description */
            get_u32(in); get_u32(in); get_u32(in); get_u32(in);   /* sinks, sources, priority, available */
        }
        get_string(in, text, sizeof text); /* active profile */
        skip_proplist(in);
        uint32_t ports = get_u32(in);
        for (uint32_t i = 0; i < ports && !in->bad; i++) {
            char port[96];
            get_string(in, port, sizeof port);
            get_string(in, text, sizeof text);   /* description */
            get_u32(in);                         /* priority */
            uint32_t available = get_u32(in);
            uint8_t direction = get_u8(in);
            skip_proplist(in);
            uint32_t n = get_u32(in);            /* the profiles it is in */
            for (uint32_t j = 0; j < n && !in->bad; j++) get_string(in, text, sizeof text);
            skip_s64(in);                        /* latency offset */
            get_string(in, text, sizeof text);   /* availability group */
            uint32_t type = get_u32(in);
            if (in->bad) break;

            int *kind = type == PORT_HEADPHONES ? &found.headphones : type == PORT_HEADSET ? &found.headset
                      : type == PORT_MIC ? &found.mic
                      : type == PORT_LINE && direction == DIRECTION_OUTPUT ? &found.lineout : NULL;
            if (!kind || n_now >= MAX_PORTS) continue;       /* HDMI, speakers, line in ...: not a jack */
            PortState *st = &now[n_now++];
            snprintf(st->key, sizeof st->key, "%s/%s", card, port);
            st->present = available != AVAILABLE_NO;
            for (int k = 0; k < n_port_state; k++) {
                if (strcmp(port_state[k].key, st->key) || port_state[k].present == st->present) continue;
                *kind = st->present ? 1 : -1;
                if (getenv("PACHUD_JACKS_DEBUG")) {
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    fprintf(stderr, "jacks %ld.%03ld port %s: %s (available=%u)\n", (long)ts.tv_sec % 1000,
                            ts.tv_nsec / 1000000, st->key, st->present ? "present" : "absent", available);
                }
                break;
            }
        }
    }
    if (in->bad) return;   /* keep the earlier reading rather than compare with half of this one */

    memcpy(port_state, now, sizeof(PortState) * n_now);
    n_port_state = n_now;
    ports_known = true;
    if (found.headphones) changes.headphones = found.headphones;
    if (found.headset) changes.headset = found.headset;
    if (found.mic) changes.mic = found.mic;
    if (found.lineout) changes.lineout = found.lineout;
}

void audio_port_changes(void (*cb)(const PortChanges *)) {
    if (state != S_READY || !ports_known) { cb(NULL); return; }
    port_cb = cb;
    want_info(T_CARDS, false);   /* answered once the reading after this call has been compared */
}

/* A sink and a source reply start the same way: index, name, description, sample spec,
 * channel map, owner module, then the volume and the mute flag, which is all that is read. */
static void handle_info(TrackId t, In *in) {
    if (t == T_CARDS) {
        handle_cards(in);
        if (!tracks[T_CARDS].want) report_ports(in->bad ? NULL : &changes);
        return;
    }
    get_u32(in);      /* index */
    char name[128], description[128];
    get_string(in, name, sizeof name);
    get_string(in, description, sizeof description);
    skip_sample_spec(in);
    skip_channel_map(in);
    get_u32(in);      /* owner module */
    uint32_t volume = get_cvolume(in);
    bool muted = get_bool(in);
    if (in->bad) return;

    Track *k = &tracks[t];
    int pct = (int)(((uint64_t)volume * 100 + VOLUME_NORM / 2) / VOLUME_NORM);

    if (getenv("PACHUD_JACKS_DEBUG")) {
        long long left = quiet_until_ms - now_ms();
        fprintf(stderr, "jacks %lld.%03lld volume %s \"%s\" %d%%%s (was %d%% on \"%s\") announce=%d quiet_left=%lld ms\n",
                now_ms() / 1000 % 1000, now_ms() % 1000, t == T_SINK ? "output" : "input", name, pct, muted ? " muted" : "",
                k->last_pct, k->name, announce, left > 0 ? left : 0);
    }

    /* A different default output (a headset connected over USB or Bluetooth, a monitor's speakers):
     * say so, instead of a volume bar that would just be about the new device. A different default
     * input (plugging a headset moves it to the headset's microphone) is only learned: the jack or
     * device toast already said what happened, and its volume is not something the user changed. */
    if (name[0] && strcmp(name, k->name) != 0) {
        bool known = k->name[0] != '\0';   /* the first reading only learns the name */
        snprintf(k->name, sizeof k->name, "%s", name);
        if (known) {
            if (announce && t == T_SINK && now_ms() >= quiet_until_ms) {
                char msg[192];
                snprintf(msg, sizeof msg, _("Audio output: %s"), description[0] ? description : name);
                hud_osd_slider(msg, pct, muted, OSD_CENTER, "audio-speakers,audio-card");  /* show device change with current volume */
            }
            k->last_pct = pct;
            k->last_muted = muted;
            return;
        }
    }
    if (pct == k->last_pct && muted == k->last_muted) return;  /* e.g. a device state change */
    k->last_pct = pct;
    k->last_muted = muted;
    if (announce && now_ms() >= quiet_until_ms) {
        hud_osd_slider(label_for(t, muted), pct, muted, OSD_CENTER, icon_for(t, pct, muted));
        if (t == T_SINK) sound_play(SOUND_VOLUME);   /* the output, not the microphone */
    }
}

static void on_reply(In *in) {
    Out o;
    switch (state) {
    case S_AUTH:
        state = S_NAME;
        command(&o, CMD_SET_CLIENT_NAME);
        o.b[o.n++] = 'P';  /* property list: application.name = "pachud" */
        put_str(&o, "application.name");
        put_u32(&o, 7);
        put_arbitrary(&o, "pachud", 7);
        o.b[o.n++] = 'N';
        if (!send_packet(&o)) disconnect();
        break;
    case S_NAME:
        state = S_SUBSCRIBE;
        command(&o, CMD_SUBSCRIBE);
        put_u32(&o, MASK_SINK | MASK_SOURCE | MASK_SERVER | MASK_CARD);
        if (!send_packet(&o)) disconnect();
        break;
    case S_SUBSCRIBE:
        state = S_READY;
        reconnect_s = RECONNECT_MIN_S;
        want_info(T_SINK, false);  /* learn the current state; later changes compare against it */
        want_info(T_SOURCE, false);
        want_info(T_CARDS, false);  /* the ports as they are, before any plug */
        break;
    case S_READY:
        if (pending < 0) break;
        int t = pending;
        pending = -1;
        handle_info(t, in);
        request_next();
        break;
    case S_DOWN:
        break;
    }
}

static void handle_frame(const uint8_t *payload, size_t len) {
    In in = { payload, payload + len, false };
    uint32_t cmd = get_u32(&in);
    get_u32(&in);  /* tag: requests are strictly sequential, so the state says what this answers */
    if (in.bad) return;

    if (cmd == CMD_REPLY) {
        on_reply(&in);
    } else if (cmd == CMD_ERROR) {
        if (state == S_READY) {  /* e.g. no default source yet: not fatal */
            if (pending == T_CARDS) report_ports(NULL);
            pending = -1;
            request_next();
        } else {
            disconnect();
        }
    } else if (cmd == CMD_SUBSCRIBE_EVENT) {
        uint32_t event = get_u32(&in);
        get_u32(&in);  /* index */
        uint32_t facility = event & FACILITY_MASK;
        if (in.bad) return;
        if (facility == FACILITY_SINK || facility == FACILITY_SERVER) tracks[T_SINK].dirty = true;
        if (facility == FACILITY_SOURCE || facility == FACILITY_SERVER) tracks[T_SOURCE].dirty = true;
        if (facility == FACILITY_CARD) tracks[T_CARDS].dirty = true;
        if (tracks[T_SINK].dirty || tracks[T_SOURCE].dirty || tracks[T_CARDS].dirty) arm(debounce_fd, DEBOUNCE_MS);
    }
}

static void on_data(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    ssize_t n = recv(sock, inbuf + inlen, sizeof inbuf - inlen, MSG_DONTWAIT);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { disconnect(); return; }
    if (n < 0) return;
    inlen += n;

    while (inlen >= DESCRIPTOR_LEN) {
        uint32_t len, channel;
        memcpy(&len, inbuf, 4);
        memcpy(&channel, inbuf + 4, 4);
        len = ntohl(len);
        channel = ntohl(channel);
        if (len > MAX_FRAME) { disconnect(); return; }
        if (inlen < DESCRIPTOR_LEN + len) break;

        if (channel == 0xFFFFFFFFu) handle_frame(inbuf + DESCRIPTOR_LEN, len);
        if (sock < 0) return;  /* the handler gave up on the connection */

        inlen -= DESCRIPTOR_LEN + len;
        memmove(inbuf, inbuf + DESCRIPTOR_LEN + len, inlen);
    }
}

static void connect_server(void) {
    struct sockaddr_un a;
    if (!server_path(&a)) { arm(reconnect_fd, RECONNECT_MAX_S * 1000L); return; }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { arm(reconnect_fd, reconnect_s * 1000L); return; }
    /* Local connect finishes (or fails) at once, so do it blocking, then go non-blocking. */
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        arm(reconnect_fd, reconnect_s * 1000L);
        reconnect_s = reconnect_s * 2 > RECONNECT_MAX_S ? RECONNECT_MAX_S : reconnect_s * 2;
        return;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    sock = fd;
    inlen = 0;
    loop_add(sock, EPOLLIN, on_data, NULL);

    /* Over a local socket the server authenticates us by our uid; the cookie can be empty. */
    static const uint8_t cookie[COOKIE_LEN];
    Out o;
    state = S_AUTH;
    command(&o, CMD_AUTH);
    put_u32(&o, PROTOCOL_VERSION);
    put_arbitrary(&o, cookie, COOKIE_LEN);
    if (!send_packet(&o)) disconnect();
}

static void on_debounce(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(debounce_fd, &n, sizeof n) <= 0 || state != S_READY) return;
    for (int t = 0; t < T_COUNT; t++) {
        if (!tracks[t].dirty) continue;
        tracks[t].dirty = false;
        want_info(t, true);
    }
}

static void on_reconnect(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(reconnect_fd, &n, sizeof n) > 0 && state == S_DOWN) connect_server();
}

void audio_init(void) {
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    reconnect_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (debounce_fd < 0 || reconnect_fd < 0) return;
    loop_add(debounce_fd, EPOLLIN, on_debounce, NULL);
    loop_add(reconnect_fd, EPOLLIN, on_reconnect, NULL);
    connect_server();
}
