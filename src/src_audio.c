#include "hud.h"
#include "i18n.h"
#include "loop.h"
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
       CMD_GET_SINK_INFO = 21, CMD_SUBSCRIBE = 35, CMD_SUBSCRIBE_EVENT = 66 };
enum { MASK_SINK = 0x1, MASK_SERVER = 0x80 };
enum { FACILITY_MASK = 0xF, FACILITY_SINK = 0, FACILITY_SERVER = 7 };

typedef enum { S_DOWN, S_AUTH, S_NAME, S_SUBSCRIBE, S_READY } State;

static int sock = -1, debounce_fd = -1, reconnect_fd = -1;
static State state = S_DOWN;
static uint32_t next_tag;
static int reconnect_s = RECONNECT_MIN_S;
static bool info_pending, info_again, announce;
static int last_pct = -1, last_muted = -1;

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

static void skip_string(In *in) {
    if (!need(in, 1)) return;
    if (*in->p == 'N') { in->p++; return; }
    if (*in->p != 't') { in->bad = true; return; }
    const uint8_t *nul = memchr(in->p + 1, 0, in->end - in->p - 1);
    if (!nul) { in->bad = true; return; }
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
    info_pending = info_again = false;
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

static void request_sink_info(bool show) {
    if (info_pending) { info_again = info_again || show; return; }
    Out o;
    command(&o, CMD_GET_SINK_INFO);
    put_u32(&o, INVALID_INDEX);
    put_str(&o, "@DEFAULT_SINK@");
    if (!send_packet(&o)) { disconnect(); return; }
    info_pending = true;
    announce = show;
}

static void handle_sink_info(In *in) {
    get_u32(in);      /* index */
    skip_string(in);  /* name, description */
    skip_string(in);
    skip_sample_spec(in);
    skip_channel_map(in);
    get_u32(in);      /* owner module */
    uint32_t volume = get_cvolume(in);
    bool muted = get_bool(in);
    if (in->bad) return;

    int pct = (int)(((uint64_t)volume * 100 + VOLUME_NORM / 2) / VOLUME_NORM);
    if (pct == last_pct && muted == last_muted) return;  /* e.g. a sink state change */
    last_pct = pct;
    last_muted = muted;
    if (announce) hud_slider(muted ? _("Volume — muted") : _("Volume"), pct, muted);
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
        put_u32(&o, MASK_SINK | MASK_SERVER);
        if (!send_packet(&o)) disconnect();
        break;
    case S_SUBSCRIBE:
        state = S_READY;
        reconnect_s = RECONNECT_MIN_S;
        request_sink_info(false);  /* learn the current state; later changes compare against it */
        break;
    case S_READY:
        if (!info_pending) break;
        info_pending = false;
        handle_sink_info(in);
        if (info_again) { info_again = false; request_sink_info(true); }
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
        if (state == S_READY) {  /* e.g. no default sink yet: not fatal */
            info_pending = false;
        } else {
            disconnect();
        }
    } else if (cmd == CMD_SUBSCRIBE_EVENT) {
        uint32_t event = get_u32(&in);
        get_u32(&in);  /* index */
        uint32_t facility = event & FACILITY_MASK;
        if (!in.bad && (facility == FACILITY_SINK || facility == FACILITY_SERVER))
            arm(debounce_fd, DEBOUNCE_MS);
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
    if (read(debounce_fd, &n, sizeof n) > 0 && state == S_READY) request_sink_info(true);
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
