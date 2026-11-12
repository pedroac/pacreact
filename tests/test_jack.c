/* Audio jacks: what a device name means, how plug events become one message, the event
 * handling (fed through a pipe, with other events mixed in) and a read-only probe of the real
 * devices. src_jack.c is included for its statics. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

static char toasts[8][96];
static int ntoasts, registered, removed;
static Level last_level;

static SoundKind last_sound = SOUND_KINDS;
static int nsounds;
void sound_play(SoundKind k) { last_sound = k; nsounds++; }
void hud_toast(const char *t, Level l, int ms) { (void)ms; snprintf(toasts[ntoasts++ % 8], sizeof toasts[0], "%s", t); last_level = l; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
int loop_remove(int fd) { (void)fd; removed++; return 0; }

#include "../src/src_jack.c"

static void test_kinds(void) {
    /* the real device names of an Intel HDA machine */
    assert(jack_kind("HDA Intel PCH Front Headphone") == K_HEADPHONES);
    assert(jack_kind("HDA Intel PCH Headset Mic") == K_MIC);
    assert(jack_kind("HDA Intel PCH Front Mic") == K_MIC);
    assert(jack_kind("HDA Intel PCH Line Out") == K_LINEOUT);
    assert(jack_kind("HDA Intel PCH Line") == K_NONE);                 /* line in */
    assert(jack_kind("HDA NVidia HDMI/DP,pcm=3") == K_NONE);           /* a monitor waking up is not a jack */
    assert(jack_kind("HDA ATI HDMI HDMI/DP,pcm=3") == K_NONE);
    assert(jack_kind("DisplayPort Audio") == K_NONE);
    assert(jack_kind("Logitech USB Headset") == K_HEADPHONES);
    assert(jack_kind("Sleep Button") == K_NONE && jack_kind("Power Button") == K_NONE);
    assert(jack_kind("AT Translated Set 2 keyboard") == K_NONE);
    assert(jack_kind("") == K_NONE && jack_kind(NULL) == K_NONE);
    assert(jack_kind("front headphone") == K_HEADPHONES);              /* case does not matter */
}

static void test_flush(void) {
    ntoasts = 0;
    memset(pending, 0, sizeof pending);
    jack_flush();
    assert(ntoasts == 0 && nsounds == 0);                              /* nothing happened: no sound either */

    pending[K_HEADPHONES] = 1;
    jack_flush();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Headphones connected") && last_level == LEVEL_INFO);
    assert(nsounds == 1 && last_sound == SOUND_DEVICE_ADDED);
    assert(pending[K_HEADPHONES] == 0);                                /* consumed */
    pending[K_HEADPHONES] = -1;
    jack_flush();
    assert(!strcmp(toasts[1], "Headphones disconnected") && last_sound == SOUND_DEVICE_REMOVED);

    /* headphone and microphone contacts together are a headset */
    pending[K_HEADPHONES] = 1; pending[K_MIC] = 1;
    jack_flush();
    assert(ntoasts == 3 && !strcmp(toasts[2], "Headset connected") && last_sound == SOUND_DEVICE_ADDED);
    pending[K_HEADPHONES] = -1; pending[K_MIC] = -1;
    jack_flush();
    assert(!strcmp(toasts[3], "Headset disconnected") && last_sound == SOUND_DEVICE_REMOVED);
    /* ...but not when they disagree */
    pending[K_HEADPHONES] = 1; pending[K_MIC] = -1;
    jack_flush();
    assert(!strcmp(toasts[4], "Headphones connected"));

    pending[K_MIC] = 1;
    jack_flush();
    assert(!strcmp(toasts[5], "Microphone connected"));
    pending[K_MIC] = -1;
    jack_flush();
    assert(!strcmp(toasts[6], "Microphone disconnected"));
    pending[K_LINEOUT] = 1;
    jack_flush();
    assert(!strcmp(toasts[7 % 8], "Line out connected"));
    pending[K_LINEOUT] = -1;
    jack_flush();
    assert(!strcmp(toasts[(ntoasts - 1) % 8], "Line out disconnected") && ntoasts == 9);
}

static void send(int fd, int type, int code, int value) {
    struct input_event ev = { .type = type, .code = code, .value = value };
    assert(write(fd, &ev, sizeof ev) == (ssize_t)sizeof ev);
}

static Jack make_jack(JackKind kind, bool plugged, int *write_end) {
    int p[2];
    assert(pipe2(p, O_NONBLOCK) == 0);                     /* like a real evdev descriptor */
    *write_end = p[1];
    return (Jack){ .fd = p[0], .kind = kind, .plugged = plugged };
}

static void test_events(void) {
    ntoasts = 0;
    memset(pending, 0, sizeof pending);
    int w;
    Jack j = make_jack(K_HEADPHONES, false, &w);

    /* other devices' events, other switches and key presses are all ignored */
    send(w, EV_KEY, KEY_A, 1);
    send(w, EV_SW, SW_LID, 1);
    send(w, EV_SW, SW_MICROPHONE_INSERT, 1);                  /* not this jack's switch */
    send(w, EV_SYN, SYN_REPORT, 0);
    on_events(&j, EPOLLIN);
    assert(!j.plugged && pending[K_HEADPHONES] == 0 && pending[K_MIC] == 0);

    /* plugging in */
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    send(w, EV_SYN, SYN_REPORT, 0);
    on_events(&j, EPOLLIN);
    assert(j.plugged && pending[K_HEADPHONES] == 1);
    jack_flush();
    assert(ntoasts == 1 && !strcmp(toasts[0], "Headphones connected"));

    /* the same state again is not a change */
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    on_events(&j, EPOLLIN);
    assert(pending[K_HEADPHONES] == 0);

    /* plugging out, and in and out again quickly within one debounce window: the last state wins */
    send(w, EV_SW, SW_HEADPHONE_INSERT, 0);
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    send(w, EV_SW, SW_HEADPHONE_INSERT, 0);
    on_events(&j, EPOLLIN);
    assert(!j.plugged && pending[K_HEADPHONES] == -1);
    jack_flush();
    assert(!strcmp(toasts[1], "Headphones disconnected"));

    /* a headset: two devices, one message */
    int w2;
    Jack mic = make_jack(K_MIC, false, &w2);
    send(w, EV_SW, SW_HEADPHONE_INSERT, 1);
    send(w2, EV_SW, SW_MICROPHONE_INSERT, 1);
    on_events(&j, EPOLLIN);
    on_events(&mic, EPOLLIN);
    jack_flush();
    assert(ntoasts == 3 && !strcmp(toasts[2], "Headset connected"));

    /* an empty read changes nothing */
    on_events(&j, EPOLLIN);
    assert(ntoasts == 3);

    /* the device disappearing: the slot is released */
    Jack gone = make_jack(K_HEADPHONES, true, &w);
    close(gone.fd);                                           /* reading a closed fd fails like a vanished device */
    jacks[0] = gone;
    removed = 0;
    on_events(&jacks[0], EPOLLIN);
    assert(jacks[0].fd == -1 && removed == 1);
    close(w); close(w2); close(j.fd); close(mic.fd);
}

static void test_real_devices(void) {
    for (int i = 0; i < MAX_JACKS; i++) jacks[i].fd = -1;
    started = true;
    ntoasts = registered = 0;
    memset(pending, 0, sizeof pending);
    jack_init();

    int open_jacks = 0;
    for (int i = 0; i < MAX_JACKS; i++) {
        if (jacks[i].fd < 0) continue;
        open_jacks++;
        assert(jacks[i].kind != K_NONE && strncmp(jacks[i].path, "/dev/input/event", 16) == 0);
    }
    assert(ntoasts == 0);                                     /* what is plugged in now is not news */
    printf("jack: %d real jack device(s) found and opened\n", open_jacks);

    /* looking again does not open them twice */
    int before = registered;
    rescan();
    assert(registered == before);

    setenv("PACHUD_JACKS", "0", 1);
    registered = 0;
    jack_init();
    assert(registered == 0);
    unsetenv("PACHUD_JACKS");
}

int main(void) {
    test_kinds();
    test_flush();
    test_events();
    test_real_devices();
    puts("jack: ok");
    return 0;
}
