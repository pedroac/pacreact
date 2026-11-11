/* The PulseAudio client: handshake packets, framing, the sink-info parser and the announce
 * logic. The module is included so its statics are reachable; the HUD and loop are stubbed.
 * The sink-info reply below is a frame captured from a real pipewire-pulse server. */
#include "../src/hud.h"
#include "../src/loop.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/timerfd.h>

static int slider_calls;
static char slider_label[128];
static int slider_pct;
static bool slider_dim;

static int toast_calls;
static char toast_text[256];
void hud_toast(const char *text, Level level, int ms) {
    (void)level; (void)ms;
    toast_calls++;
    snprintf(toast_text, sizeof toast_text, "%s", text);
}

void hud_osd_slider(const char *label, int percent, bool dim, OsdAnchor anchor) {
    (void)anchor;
    slider_calls++;
    snprintf(slider_label, sizeof slider_label, "%s", label);
    slider_pct = percent;
    slider_dim = dim;
}
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; return 0; }

#include "../src/src_audio.c"

/* ---- fixtures ---- */

typedef struct { uint8_t b[1024]; size_t n; } Buf8;

static void raw(Buf8 *o, const void *d, size_t n) { memcpy(o->b + o->n, d, n); o->n += n; }
static void be_u32(Buf8 *o, uint32_t v) { o->b[o->n++] = 'L'; v = htonl(v); raw(o, &v, 4); }

/* A GET_SINK_INFO reply payload as pipewire-pulse sent it: volume `vol` on two channels. */
static Buf8 sink_info_named(uint32_t vol, char mute, const char *name, const char *desc) {
    Buf8 o = {0};
    be_u32(&o, 2);      /* REPLY */
    be_u32(&o, 4);      /* tag */
    be_u32(&o, 0x313);  /* sink index */
    o.b[o.n++] = 't'; raw(&o, name, strlen(name) + 1);
    o.b[o.n++] = 't'; raw(&o, desc, strlen(desc) + 1);
    raw(&o, "a\x07\x02\x00\x00\xbb\x80", 7);  /* sample spec: format 7, 2 channels, 48000 Hz */
    raw(&o, "m\x02\x01\x02", 4);               /* channel map: front-left, front-right */
    be_u32(&o, 0xFFFFFFFFu);                   /* owner module */
    o.b[o.n++] = 'v'; o.b[o.n++] = 2;
    uint32_t v = htonl(vol);
    raw(&o, &v, 4);
    raw(&o, &v, 4);
    o.b[o.n++] = mute;                         /* '1' muted, '0' not */
    be_u32(&o, 0x312);                         /* monitor source: the real frame continues */
    return o;
}

static Buf8 sink_info(uint32_t vol, char mute) {
    return sink_info_named(vol, mute, "alsa_output.pci-0000_00_1f.3.analog-stereo", "Built-in Audio Analog Stereo");
}

static Buf8 framed(const Buf8 *payload) {
    Buf8 o = {0};
    uint32_t len = htonl(payload->n), ch = htonl(0xFFFFFFFFu), zero = 0;
    raw(&o, &len, 4);
    raw(&o, &ch, 4);
    raw(&o, &zero, 4);
    raw(&o, &zero, 4);
    raw(&o, &zero, 4);
    raw(&o, payload->b, payload->n);
    return o;
}

static int peer = -1;

static void connect_pair(State st) {
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    sock = sv[0];
    peer = sv[1];
    state = st;
    inlen = 0;
    pending = -1;
    for (int t = 0; t < T_COUNT; t++) {
        tracks[t].want = tracks[t].dirty = false;
        tracks[t].last_pct = tracks[t].last_muted = -1;
    }
    slider_calls = 0;
    toast_calls = 0;
    for (int t = 0; t < T_COUNT; t++) tracks[t].name[0] = '\0';
}

static void drop_pair(void) {
    if (sock >= 0) close(sock);
    sock = -1;
    if (peer >= 0) close(peer);
    peer = -1;
}

/* Reads one framed packet the client wrote, returning its payload length. */
static size_t read_packet(uint8_t *payload, size_t cap) {
    uint8_t hdr[DESCRIPTOR_LEN];
    assert(recv(peer, hdr, sizeof hdr, MSG_WAITALL | MSG_DONTWAIT) == (ssize_t)sizeof hdr);
    uint32_t len, ch;
    memcpy(&len, hdr, 4);
    memcpy(&ch, hdr + 4, 4);
    len = ntohl(len);
    assert(ntohl(ch) == 0xFFFFFFFFu);  /* control channel */
    assert(len <= cap);
    assert(recv(peer, payload, len, MSG_WAITALL | MSG_DONTWAIT) == (ssize_t)len);
    return len;
}

static uint32_t u32_at(const uint8_t *p) { uint32_t v; memcpy(&v, p + 1, 4); return ntohl(v); }

static void feed_reply(void) {  /* a bare REPLY, as the server answers handshake steps */
    uint8_t payload[10] = { 'L', 0, 0, 0, 2, 'L', 0, 0, 0, 1 };
    In in = { payload + 10, payload + 10, false };
    on_reply(&in);
}

/* ---- tests ---- */

static void test_handshake_packets(void) {
    connect_pair(S_AUTH);
    uint8_t p[512];

    feed_reply();  /* AUTH accepted -> SET_CLIENT_NAME */
    assert(state == S_NAME);
    size_t n = read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_SET_CLIENT_NAME);
    assert(p[10] == 'P');
    assert(!strcmp((char *)p + 12, "application.name"));
    assert(n == 5 + 5 + 1 + 1 + 17 + 5 + 1 + 4 + 7 + 1);  /* the proplist is exactly this long */
    assert(p[n - 1] == 'N');

    feed_reply();  /* name accepted -> SUBSCRIBE sink|server */
    assert(state == S_SUBSCRIBE);
    n = read_packet(p, sizeof p);
    assert(n == 15 && u32_at(p) == CMD_SUBSCRIBE && u32_at(p + 10) == (MASK_SINK | MASK_SOURCE | MASK_SERVER));

    feed_reply();  /* subscribed -> ready, asks for the default sink silently */
    assert(state == S_READY && pending == T_SINK && !announce);
    n = read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO && u32_at(p + 10) == INVALID_INDEX);
    assert(p[15] == 't' && !strcmp((char *)p + 16, "@DEFAULT_SINK@"));
    assert(n == 15 + 1 + 15);

    /* Once the output is known, the input is asked for (one request at a time) */
    Buf8 reply = sink_info(9172, '0');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(pending == T_SOURCE && !announce);
    n = read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SOURCE_INFO && u32_at(p + 10) == INVALID_INDEX);
    assert(p[15] == 't' && !strcmp((char *)p + 16, "@DEFAULT_SOURCE@"));
    assert(slider_calls == 0);                     /* learning the state is silent */
    drop_pair();
}

static void test_sink_info_parsing(void) {
    connect_pair(S_READY);

    /* The captured frame: 9172 on both channels, muted = 14%. Learning it is silent. */
    pending = T_SINK;
    announce = false;
    Buf8 reply = sink_info(9172, '1');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(pending == -1 && slider_calls == 0);
    assert(tracks[T_SINK].last_pct == 14 && tracks[T_SINK].last_muted == 1);

    /* The same state again changes nothing, even when announcing (a sink state change). */
    pending = T_SINK;
    announce = true;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);

    /* Unmuted at 25%. */
    reply = sink_info(0x4000, '0');
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1 && !strcmp(slider_label, "Volume") && slider_pct == 25 && !slider_dim);

    /* Muting at the same level is a change. */
    reply = sink_info(0x4000, '1');
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 2 && !strcmp(slider_label, "Volume — muted") && slider_pct == 25 && slider_dim);

    /* Rounding, and above 100%. */
    struct { uint32_t raw; int pct; } cases[] = {
        { 0, 0 }, { 0x8000, 50 }, { 0x10000, 100 }, { 0x13333, 120 }, { 0x1, 0 }, { 0x7FFF, 50 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        tracks[T_SINK].last_pct = -1;
        reply = sink_info(cases[i].raw, '0');
        pending = T_SINK;
        in = (In){ reply.b + 10, reply.b + reply.n, false };
        on_reply(&in);
        assert(slider_pct == cases[i].pct);
    }
    drop_pair();
}

static void read_reply(TrackId t, Buf8 reply, bool show) {
    pending = t;
    announce = show;
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
}

static void test_output_change(void) {
    connect_pair(S_READY);
    const char *a = "alsa_output.pci-0000_00_1f.3.analog-stereo";
    const char *b = "bluez_output.AA_BB_CC_DD_EE_FF.1";

    /* Learning the output at start-up says nothing, and remembers its name */
    read_reply(T_SINK, sink_info_named(0x4000, '0', a, "Built-in Audio"), false);
    assert(toast_calls == 0 && slider_calls == 0 && !strcmp(tracks[T_SINK].name, a));

    /* The same output again: just the volume, as before */
    read_reply(T_SINK, sink_info_named(0x8000, '0', a, "Built-in Audio"), true);
    assert(slider_calls == 1 && slider_pct == 50);

    /* A different default output: one centred bar naming it, at the new device's volume */
    read_reply(T_SINK, sink_info_named(0x6000, '0', b, "WH-1000XM4"), true);
    assert(slider_calls == 2 && !strcmp(slider_label, "Audio output: WH-1000XM4") && slider_pct == 38);
    assert(tracks[T_SINK].last_pct == 38 && !strcmp(tracks[T_SINK].name, b));
    /* ...and it is not repeated, nor does the same volume come back as a change */
    read_reply(T_SINK, sink_info_named(0x6000, '0', b, "WH-1000XM4"), true);
    assert(slider_calls == 2);
    read_reply(T_SINK, sink_info_named(0x4000, '0', b, "WH-1000XM4"), true);
    assert(slider_calls == 3 && !strcmp(slider_label, "Volume"));

    /* Back again; without a description the name is used */
    read_reply(T_SINK, sink_info_named(0x4000, '0', a, ""), true);
    assert(slider_calls == 4 && !strcmp(slider_label, "Audio output: alsa_output.pci-0000_00_1f.3.analog-stereo"));

    /* Re-learning after a reconnect (not announced) updates the name quietly */
    read_reply(T_SINK, sink_info_named(0x4000, '0', b, "WH-1000XM4"), false);
    assert(slider_calls == 4 && !strcmp(tracks[T_SINK].name, b));

    /* Only outputs: the default microphone changing is not announced here */
    read_reply(T_SOURCE, sink_info_named(0x4000, '0', "alsa_input.usb-a", "USB mic"), true);
    assert(slider_calls == 5 && tracks[T_SOURCE].name[0] == '\0');

    drop_pair();
}

static void test_microphone_track(void) {
    connect_pair(S_READY);
    announce = true;

    /* A source reply has the same layout; it is labelled as the microphone */
    pending = T_SOURCE;
    Buf8 reply = sink_info(0x8000, '0');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1 && !strcmp(slider_label, "Microphone") && slider_pct == 50 && !slider_dim);
    assert(tracks[T_SOURCE].last_pct == 50 && tracks[T_SINK].last_pct == -1);   /* kept apart */

    pending = T_SOURCE;
    reply = sink_info(0x8000, '1');
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 2 && !strcmp(slider_label, "Microphone — muted") && slider_dim);

    /* The same input state again: nothing, while the output has its own memory */
    pending = T_SOURCE;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 2);
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 3 && !strcmp(slider_label, "Volume — muted"));
    drop_pair();
}

static void test_request_queue(void) {
    connect_pair(S_READY);
    uint8_t p[256];

    /* Two wanted at once: the first goes out, the second waits for its answer */
    want_info(T_SINK, true);
    want_info(T_SOURCE, true);
    assert(pending == T_SINK && tracks[T_SOURCE].want && announce);
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO);
    assert(recv(peer, p, sizeof p, MSG_DONTWAIT) < 0);          /* nothing else was sent yet */

    Buf8 reply = sink_info(0x4000, '0');
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(pending == T_SOURCE && !tracks[T_SOURCE].want);
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SOURCE_INFO);

    /* A request for a track already waiting is not sent twice, and "announce" is not lost: a
     * silent request merged with an announcing one still announces */
    tracks[T_SINK].want = false;
    want_info(T_SINK, false);
    want_info(T_SINK, true);
    want_info(T_SINK, false);
    assert(tracks[T_SINK].want && tracks[T_SINK].want_announce);

    /* An error for the outstanding request (e.g. no default input) moves on, it is not fatal */
    uint8_t err[15] = { 'L', 0, 0, 0, 0, 'L', 0, 0, 0, 2, 'L', 0, 0, 0, 3 };
    handle_frame(err, sizeof err);
    assert(state == S_READY && sock >= 0 && pending == T_SINK);   /* the waiting sink request went out */
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO);
    drop_pair();
}

static void event_frame(uint32_t event) {
    uint8_t f[20];
    uint32_t cmd = htonl(CMD_SUBSCRIBE_EVENT), tag = htonl(0xFFFFFFFFu), ev = htonl(event), idx = htonl(7);
    size_t n = 0;
    f[n++] = 'L'; memcpy(f + n, &cmd, 4); n += 4;
    f[n++] = 'L'; memcpy(f + n, &tag, 4); n += 4;
    f[n++] = 'L'; memcpy(f + n, &ev, 4); n += 4;
    f[n++] = 'L'; memcpy(f + n, &idx, 4); n += 4;
    handle_frame(f, n);
}

static void test_events(void) {
    connect_pair(S_READY);
    uint8_t p[256];

    /* Output events dirty the output only; input events the input only */
    event_frame(0x10 | FACILITY_SINK);
    assert(tracks[T_SINK].dirty && !tracks[T_SOURCE].dirty);
    tracks[T_SINK].dirty = false;
    event_frame(0x10 | FACILITY_SOURCE);
    assert(!tracks[T_SINK].dirty && tracks[T_SOURCE].dirty);
    tracks[T_SOURCE].dirty = false;

    /* A server event (the default device changed) concerns both */
    event_frame(0x10 | FACILITY_SERVER);
    assert(tracks[T_SINK].dirty && tracks[T_SOURCE].dirty);
    tracks[T_SINK].dirty = tracks[T_SOURCE].dirty = false;

    /* Anything else (clients, cards, streams) is none of our business */
    event_frame(0x10 | 5);
    event_frame(0x00 | 2);
    assert(!tracks[T_SINK].dirty && !tracks[T_SOURCE].dirty);

    /* After the debounce, what is dirty is requested and announced */
    event_frame(0x10 | FACILITY_SERVER);
    debounce_fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    assert(debounce_fd >= 0);
    arm(debounce_fd, 1);
    struct pollfd pf = { .fd = debounce_fd, .events = POLLIN };
    assert(poll(&pf, 1, 1000) == 1);
    on_debounce(NULL, 0);
    assert(pending == T_SINK && announce && tracks[T_SOURCE].want && tracks[T_SOURCE].want_announce);
    assert(!tracks[T_SINK].dirty && !tracks[T_SOURCE].dirty);
    read_packet(p, sizeof p);
    assert(u32_at(p) == CMD_GET_SINK_INFO);
    close(debounce_fd);
    debounce_fd = -1;
    drop_pair();
}

static void test_loudest_channel(void) {
    connect_pair(S_READY);
    Buf8 reply = sink_info(0x4000, '0');
    /* Make the second channel louder: the HUD shows the loudest one. */
    uint32_t louder = htonl(0x8000);
    size_t vol2 = reply.n - 5 - 1 - 4;  /* before: mute (1) and the monitor 'L'+u32 (5) */
    memcpy(reply.b + vol2, &louder, 4);
    pending = T_SINK;
    announce = true;
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 1 && slider_pct == 50);
    drop_pair();
}

static void test_malformed_replies(void) {
    connect_pair(S_READY);
    Buf8 reply = sink_info(9172, '1');

    /* Every truncation of a valid reply must be rejected without announcing anything. */
    for (size_t cut = 10; cut < reply.n - 5; cut++) {
        pending = T_SINK;
        announce = true;
        tracks[T_SINK].last_pct = tracks[T_SINK].last_muted = -1;
        In in = { reply.b + 10, reply.b + cut, false };
        on_reply(&in);
        assert(slider_calls == 0);
    }

    /* A wrong tag where a number belongs. */
    reply = sink_info(9172, '1');
    reply.b[10] = 'x';
    pending = T_SINK;
    In in = { reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);

    /* A boolean that is neither. */
    reply = sink_info(9172, '?');
    pending = T_SINK;
    in = (In){ reply.b + 10, reply.b + reply.n, false };
    on_reply(&in);
    assert(slider_calls == 0);
    drop_pair();
}

static void test_framing(void) {
    connect_pair(S_READY);
    pending = T_SINK;
    announce = true;
    Buf8 payload = sink_info(0x8000, '0');
    Buf8 frame = framed(&payload);

    /* Delivered in pieces: nothing happens until the frame is complete. */
    assert(send(peer, frame.b, 7, 0) == 7);
    on_data(NULL, 0);
    assert(slider_calls == 0 && inlen == 7);
    assert(send(peer, frame.b + 7, 30, 0) == 30);
    on_data(NULL, 0);
    assert(slider_calls == 0);
    assert(send(peer, frame.b + 37, frame.n - 37, 0) == (ssize_t)(frame.n - 37));
    on_data(NULL, 0);
    assert(slider_calls == 1 && slider_pct == 50 && inlen == 0);

    /* Two frames in one read, and an audio-channel frame in between that must be ignored. */
    tracks[T_SINK].last_pct = -1;
    pending = T_SINK;
    Buf8 two = {0};
    raw(&two, frame.b, frame.n);
    uint32_t len = htonl(4), ch = htonl(7), zero = 0;  /* a data frame on channel 7 */
    raw(&two, &len, 4); raw(&two, &ch, 4); raw(&two, &zero, 4); raw(&two, &zero, 4); raw(&two, &zero, 4);
    raw(&two, "abcd", 4);
    Buf8 p2 = sink_info(0x4000, '0');
    Buf8 f2 = framed(&p2);
    raw(&two, f2.b, f2.n);
    assert(send(peer, two.b, two.n, 0) == (ssize_t)two.n);
    int before = slider_calls;
    on_data(NULL, 0);
    assert(slider_calls == before + 1);   /* the first reply answered the request; the second was unsolicited */
    drop_pair();
}

static void test_bad_connections(void) {
    /* A frame length beyond anything we expect: drop the connection. */
    connect_pair(S_READY);
    uint8_t hdr[DESCRIPTOR_LEN] = { 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    assert(send(peer, hdr, sizeof hdr, 0) == (ssize_t)sizeof hdr);
    on_data(NULL, 0);
    assert(sock == -1 && state == S_DOWN);
    drop_pair();

    /* The server closes: same. */
    connect_pair(S_READY);
    close(peer);
    peer = -1;
    on_data(NULL, 0);
    assert(sock == -1 && state == S_DOWN);
    drop_pair();

    /* An ERROR reply during the handshake is fatal, but not once ready. */
    connect_pair(S_NAME);
    uint8_t err[15] = { 'L', 0, 0, 0, 0, 'L', 0, 0, 0, 2, 'L', 0, 0, 0, 3 };
    handle_frame(err, sizeof err);
    assert(state == S_DOWN);
    drop_pair();

    connect_pair(S_READY);
    pending = T_SINK;
    handle_frame(err, sizeof err);
    assert(state == S_READY && pending == -1 && sock >= 0);
    drop_pair();
}

static void test_server_path(void) {
    struct sockaddr_un a;
    setenv("XDG_RUNTIME_DIR", "/run/user/1000", 1);
    unsetenv("PULSE_SERVER");
    assert(server_path(&a) && !strcmp(a.sun_path, "/run/user/1000/pulse/native"));

    setenv("PULSE_SERVER", "unix:/tmp/pa.sock", 1);
    assert(server_path(&a) && !strcmp(a.sun_path, "/tmp/pa.sock"));
    setenv("PULSE_SERVER", "/tmp/other", 1);
    assert(server_path(&a) && !strcmp(a.sun_path, "/tmp/other"));
    setenv("PULSE_SERVER", "tcp:example.org", 1);
    assert(!server_path(&a));  /* TCP is not supported */

    unsetenv("PULSE_SERVER");
    unsetenv("XDG_RUNTIME_DIR");
    assert(!server_path(&a));
}

int main(void) {
    test_handshake_packets();
    test_sink_info_parsing();
    test_output_change();
    test_microphone_track();
    test_request_queue();
    test_events();
    test_loudest_channel();
    test_malformed_replies();
    test_framing();
    test_bad_connections();
    test_server_path();
    puts("audio: ok");
    return 0;
}
