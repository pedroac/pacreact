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
       CMD_SUBSCRIBE_EVENT = 66 };
enum { MASK_SINK = 0x1, MASK_SOURCE = 0x2, MASK_SERVER = 0x80 };
enum { FACILITY_MASK = 0xF, FACILITY_SINK = 0, FACILITY_SOURCE = 1, FACILITY_SERVER = 7 };

typedef enum { S_DOWN, S_AUTH, S_NAME, S_SUBSCRIBE, S_READY } State;

static int sock = -1, debounce_fd = -1, reconnect_fd = -1;
static State state = S_DOWN;
static uint32_t next_tag;
static int reconnect_s = RECONNECT_MIN_S;
/* What is watched: the default output and the default input, each with the same volume and
 * mute fields in the server's reply. One request is outstanding at a time. */
typedef enum { T_SINK, T_SOURCE, T_COUNT } TrackId;

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
};
static int pending = -1;   /* the track the outstanding request is for, or -1 */
static bool announce;      /* whether the outstanding answer may show a HUD */

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
        put_u32(&o, INVALID_INDEX);
        put_str(&o, k->object);
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

/* A sink and a source reply start the same way: index, name, description, sample spec,
 * channel map, owner module, then the volume and the mute flag, which is all that is read. */
static void handle_info(TrackId t, In *in) {
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

    /* A different default output (a headset connected over USB or Bluetooth, a monitor's speakers):
     * say so, instead of a volume bar that would just be about the new device. */
    if (t == T_SINK && name[0] && strcmp(name, k->name) != 0) {
        bool known = k->name[0] != '\0';   /* the first reading only learns the name */
        snprintf(k->name, sizeof k->name, "%s", name);
        if (known) {
            if (announce) {
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
    if (announce) {
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
        put_u32(&o, MASK_SINK | MASK_SOURCE | MASK_SERVER);
        if (!send_packet(&o)) disconnect();
        break;
    case S_SUBSCRIBE:
        state = S_READY;
        reconnect_s = RECONNECT_MIN_S;
        want_info(T_SINK, false);  /* learn the current state; later changes compare against it */
        want_info(T_SOURCE, false);
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
        if (tracks[T_SINK].dirty || tracks[T_SOURCE].dirty) arm(debounce_fd, DEBOUNCE_MS);
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
