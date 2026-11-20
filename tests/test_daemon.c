/* What pacreact does with what pacreactd says: which lines are an alert, the cooldown of the memory
 * alert, the charger heard only after the snapshot, the bar and the messages of the sound and the jacks, and the
 * connection (subscribing, and joining it again when the daemon comes back). src_daemon.c is included for its statics. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char last[128];
static int shown;
static Level last_level;
static char toast_icon[96];
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    (void)ms;
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    snprintf(last, sizeof last, "%s", t);
    last_level = l;
    shown++;
}
void hud_toast(const char *t, Level l, int ms) {
    (void)ms;
    toast_icon[0] = '\0';
    snprintf(last, sizeof last, "%s", t);
    last_level = l;
    shown++;
}
static int slider_calls, slider_pct;
static char slider_label[64], slider_icon[96];
static bool slider_dim;
void hud_osd_slider(const char *label, int percent, bool dim, OsdAnchor anchor, const char *icon) {
    (void)anchor;
    slider_calls++;
    snprintf(slider_label, sizeof slider_label, "%s", label);
    snprintf(slider_icon, sizeof slider_icon, "%s", icon ? icon : "");
    slider_pct = percent;
    slider_dim = dim;
}
static SoundKind last_sound = SOUND_KINDS;
static int nsounds;
static int volume_sounds;
void sound_play(SoundKind k) { last_sound = k; nsounds++; if (k == SOUND_VOLUME) volume_sounds++; }
static bool allow = true;
static char asked[24];
bool alert_allowed(const char *source, int level) { (void)level; snprintf(asked, sizeof asked, "%s", source); return allow; }
static LoopFn handler;
static int removed;
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; handler = fn; (void)ctx; return 0; }
int loop_remove(int fd) { (void)fd; removed++; return 0; }

#include "../src/src_daemon.c"

/* One line, as if it were all that came in a burst */
static void say(const char *text) {
    char copy[256];
    snprintf(copy, sizeof copy, "%s", text);
    line(copy);
    flush_audio();
}

/* Two lines that came together */
static void say2(const char *a, const char *b) {
    char copy[256];
    snprintf(copy, sizeof copy, "%s", a);
    line(copy);
    snprintf(copy, sizeof copy, "%s", b);
    line(copy);
    flush_audio();
}

static void reset(void) {
    shown = nsounds = 0; allow = true; last_mem_alert = 0; last_mem_level = LEVEL_INFO;
    battery_seen = LEVEL_INFO; temp_seen[0] = temp_seen[1] = LEVEL_INFO; ac = -1; synced = false; available_mb = -1;
    slider_calls = volume_sounds = 0; output[0] = '\0'; memset(devices_there, 0, sizeof devices_there);
    forget_audio(0); forget_audio(1);
    backlight = -1; locks_known[0] = locks_known[1] = -1; probe_state[0] = '\0'; asked[0] = '\0';
}

static void test_battery(void) {
    reset();
    say("power.battery 50"); assert(shown == 0);
    say("power.battery 15\tlevel=warning");  assert(shown == 1 && last_level == LEVEL_WARN && !strcmp(last, "Battery low — 15%"));
    assert(!strncmp(toast_icon, "battery-low", 11) && !strcmp(asked, "battery") && nsounds == 1);
    say("power.battery 14\tlevel=warning");  assert(shown == 1);               /* once per crossing */
    say("power.battery 5\tlevel=critical");  assert(shown == 2 && last_level == LEVEL_CRIT && !strcmp(last, "Battery critical — 5%"));
    assert(!strncmp(toast_icon, "battery-caution", 15));                      /* the emptier icon first */
    say("power.battery 4\tlevel=critical");  assert(shown == 2);
    say("power.battery 10");                assert(shown == 2);               /* charging: re-armed */
    say("power.battery 15\tlevel=warning");  assert(shown == 3);
    say("power.battery 40");
    say("power.battery 15\tlevel=warning");  assert(shown == 4);

    /* "The same as before the restart": not shown, and not shown again either */
    reset();
    allow = false;
    say("power.battery 10\tlevel=warning");  assert(shown == 0);
    allow = true;
    say("power.battery 9\tlevel=warning");   assert(shown == 0);
    say("power.battery 4\tlevel=critical");  assert(shown == 1 && last_level == LEVEL_CRIT);   /* a worse one is new */
}

static void test_charger(void) {
    reset();
    say("power.ac on");                      /* in the snapshot: known, not announced */
    say("sync");
    assert(shown == 0 && nsounds == 0);
    say("power.ac on");                      assert(shown == 0);
    say("power.ac off");                     assert(shown == 1 && !strcmp(last, "Charger disconnected") && last_level == LEVEL_INFO);
    assert(last_sound == SOUND_POWER_UNPLUGGED && !strncmp(toast_icon, "battery", 7));
    say("power.ac on");                      assert(shown == 2 && !strcmp(last, "Charger connected") && last_sound == SOUND_POWER_PLUGGED);
    assert(!strncmp(toast_icon, "ac-adapter", 10));
    reset();                                 /* a snapshot that changes the charger (after a reconnect) is silent too */
    say("power.ac on"); say("sync"); say("power.ac off");
    assert(shown == 1);
    synced = false;
    say("power.ac on");
    assert(shown == 1);
}

static void test_memory(void) {
    reset();
    say("mem.available 812");
    say("mem.pressure off");                 assert(shown == 0);
    say("mem.pressure on\tlevel=warning");    assert(shown == 1 && last_level == LEVEL_WARN && !strcmp(last, "Memory pressure — 812 MB available"));
    assert(!strcmp(asked, "memory"));
    say("mem.pressure on\tlevel=warning");    assert(shown == 1);                 /* the cooldown holds repeats back */
    say("mem.pressure on\tlevel=critical");   assert(shown == 2 && last_level == LEVEL_CRIT);   /* but not an escalation */
    say("mem.pressure on\tlevel=warning");    assert(shown == 2);

    /* Left out as "nothing new since the restart": no cooldown started, so the same alert allowed later is shown */
    reset();
    allow = false;
    say("mem.pressure on\tlevel=critical");   assert(shown == 0);
    allow = true;
    say("mem.pressure on\tlevel=critical");   assert(shown == 1);
    reset();
    say("mem.pressure on\tlevel=warning");
    allow = false;
    say("mem.pressure on\tlevel=critical");   assert(shown == 1 && last_level == LEVEL_WARN);
}

static void test_temperature(void) {
    reset();
    say("temp.cpu 46"); say("temp.ssd 27");  assert(shown == 0);
    say("temp.cpu 86\tlevel=warning");        assert(shown == 1 && last_level == LEVEL_WARN && !strcmp(last, "CPU temperature high — 86 °C"));
    assert(!strcmp(asked, "temp-cpu"));
    say("temp.cpu 87\tlevel=warning");        assert(shown == 1);
    say("temp.cpu 96\tlevel=critical");       assert(shown == 2 && last_level == LEVEL_CRIT && !strcmp(last, "CPU temperature critical — 96 °C"));
    say("temp.cpu 91\tlevel=critical");       assert(shown == 2);
    say("temp.cpu 70");
    say("temp.cpu 86\tlevel=warning");        assert(shown == 3);
    say("temp.ssd 74\tlevel=warning");        assert(shown == 4 && !strcmp(last, "SSD temperature high — 74 °C") && !strcmp(asked, "temp-ssd"));
    say("temp.ssd 78\tlevel=critical");       assert(shown == 5 && !strcmp(last, "SSD temperature critical — 78 °C"));
}

static void test_sound(void) {
    reset();
    /* What is in the snapshot is learned, not shown */
    say("audio.volume 25"); say("audio.mute off"); say("audio.output Speakers");
    say("audio.mic.volume 100"); say("audio.mic.mute off");
    say("sync");
    assert(slider_calls == 0 && shown == 0 && volume_sounds == 0);

    /* The person changes the volume: a bar, a tick, and the icon of the level */
    say("audio.volume 50");
    assert(slider_calls == 1 && !strcmp(slider_label, "Volume") && slider_pct == 50 && !slider_dim && volume_sounds == 1);
    assert(!strcmp(slider_icon, "audio-volume-medium"));
    say("audio.volume 50");                                   assert(slider_calls == 1);          /* no change */
    say("audio.mute on");
    assert(slider_calls == 2 && !strcmp(slider_label, "Volume — muted") && slider_dim && !strcmp(slider_icon, "audio-volume-muted"));
    say2("audio.volume 25", "audio.mute off");                /* both at once: one bar, one tick */
    assert(slider_calls == 3 && !strcmp(slider_icon, "audio-volume-low") && volume_sounds == 3);

    /* The device's own level, with the output that brought it: not the person's, and what the person does next is measured from it */
    say2("audio.volume 38\tcause=device", "audio.mute off\tcause=device");
    assert(slider_calls == 3 && volume_sounds == 3);
    say("audio.volume 40");
    assert(slider_calls == 4 && slider_pct == 40);

    /* A different output is a message (and no bar); the same one again, or the first, is not */
    say("audio.output Speakers");                             assert(shown == 0);
    say("audio.output Headset");
    assert(shown == 1 && !strcmp(last, "Audio output: Headset") && last_level == LEVEL_INFO && slider_calls == 4);
    say("audio.output Speakers\tcause=jack");                  /* a jack moved it: the jack's message said so */
    assert(shown == 1);
    say("audio.output Headset");                              assert(shown == 2);                  /* it was learned, so this is a change */

    /* The microphone has its own bar and no tick */
    say("audio.mic.volume 100");                              assert(slider_calls == 4);
    say("audio.mic.volume 20");
    assert(slider_calls == 5 && !strcmp(slider_label, "Microphone") && !strcmp(slider_icon, "microphone-sensitivity-low") && volume_sounds == 4);
    say("audio.mic.mute on");
    assert(slider_calls == 6 && !strcmp(slider_label, "Microphone — muted") && !strcmp(slider_icon, "microphone-sensitivity-muted"));

    /* Gone (no default input) and back: what comes back is new, not a change */
    say("audio.mic.volume"); say("audio.mic.mute");
    say("audio.mic.volume 90"); say("audio.mic.mute off");
    assert(slider_calls == 6);
    say("audio.mic.volume 80");
    assert(slider_calls == 7 && !strcmp(slider_icon, "microphone-sensitivity-high"));

    /* The daemon goes and comes back with a snapshot: nothing is shown for what it says, however different */
    int bars = slider_calls, toasts = shown;
    lost();
    say("audio.volume 70"); say("audio.mute off"); say("audio.output Something else");
    say("sync");
    assert(slider_calls == bars && shown == toasts);
}

static void test_icons_by_level(void) {
    assert(!strcmp(icon_for(0, 0, false), "audio-volume-muted") && !strcmp(icon_for(0, 50, true), "audio-volume-muted"));
    assert(!strcmp(icon_for(0, 1, false), "audio-volume-low") && !strcmp(icon_for(0, 33, false), "audio-volume-low"));
    assert(!strcmp(icon_for(0, 34, false), "audio-volume-medium") && !strcmp(icon_for(0, 66, false), "audio-volume-medium"));
    assert(!strcmp(icon_for(0, 67, false), "audio-volume-high") && !strcmp(icon_for(0, 100, false), "audio-volume-high"));
    assert(!strncmp(icon_for(0, 101, false), "audio-volume-overamplified", 26) && strstr(icon_for(0, 150, false), "audio-volume-high"));
    assert(!strcmp(icon_for(1, 80, true), "microphone-sensitivity-muted") && !strcmp(icon_for(1, 10, false), "microphone-sensitivity-low"));
    assert(!strcmp(icon_for(1, 50, false), "microphone-sensitivity-medium") && !strcmp(icon_for(1, 90, false), "microphone-sensitivity-high"));
}

static void test_jacks(void) {
    reset();
    say("jack.j1 \tkind=headphones");                             /* plugged in before pacreact came: learned, not said */
    assert(shown == 0);
    say("sync");

    static const struct { const char *kind, *text, *icon; } cases[] = {
        { "headset", "Headset", "audio-headset" }, { "headphones", "Headphones", "audio-headphones" },
        { "mic", "Microphone", "audio-input-microphone" }, { "lineout", "Line out", "audio-speakers" },
        { "device", "Audio device", "audio-card,audio-headphones" },
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char key[16], on[48], off[48], text[64];
        snprintf(key, sizeof key, "jack.j%zu", i + 10);
        snprintf(on, sizeof on, "%s connected", cases[i].text);
        snprintf(off, sizeof off, "%s disconnected", cases[i].text);
        snprintf(text, sizeof text, "%s \tkind=%s", key, cases[i].kind);
        int before = shown, sounds = nsounds;
        say(text);
        assert(shown == before + 1 && !strcmp(last, on) && last_level == LEVEL_INFO && !strncmp(toast_icon, cases[i].icon, strlen(cases[i].icon)));
        assert(last_sound == SOUND_DEVICE_ADDED && nsounds == sounds + 1);
        say(key);
        assert(shown == before + 2 && !strcmp(last, off) && last_sound == SOUND_DEVICE_REMOVED);
    }

    /* A kind we do not know says nothing; what is plugged in now, as the words of `pacreact jacks` */
    int before = shown;
    say("jack.j30 \tkind=frobnicator"); say("jack.j30");
    assert(shown == before);
    char words[64];
    jack_describe(words, sizeof words);
    assert(!strcmp(words, " headphones"));                         /* j1, from the snapshot */
    say("jack.j31 \tkind=headset"); say("jack.j32 \tkind=lineout");
    jack_describe(words, sizeof words);
    assert(!strcmp(words, " headset headphones lineout"));
    say("jack.j1"); say("jack.j31"); say("jack.j32");
    jack_describe(words, sizeof words);
    assert(!strcmp(words, " none"));
    jack_describe(words, 5);                                       /* too small: cut, never overrun */
    assert(strlen(words) == 4);
}
static void test_usb(void) {
    reset();
    usb_enabled = (1u << U_KINDS) - 1; usb_off = false;
    say("usb.u1 Logitech\tkind=mouse");                          /* what is there: learned, not said */
    say("usb.u1.size 3");                                          /* a property of it: nothing to say */
    say("sync");
    assert(shown == 0);

    static const struct { const char *kind, *name, *text, *icon; } cases[] = {
        { "printer", "HP ENVY 4520 series", "HP ENVY 4520 series", "printer" },
        { "scanner", "Canon CanoScan LiDE 110", "Canon CanoScan LiDE 110", "scanner,document-scan,printer" },
        { "storage", "SanDisk Ultra", "SanDisk Ultra", "drive-removable-media-usb,drive-removable-media" },
        { "mouse", "Logitech USB Optical Mouse", "Logitech USB Optical Mouse", "input-mouse" },
        { "keyboard", "USB Keyboard", "USB Keyboard", "input-keyboard" },
        { "gamepad", "Controller", "Controller", "input-gaming,input-mouse" },
        { "tablet", "Wacom Intuos BT M", "Wacom Intuos BT M", "input-tablet,input-mouse" },
        { "camera", "Logitech C920", "Logitech C920", "camera-web,camera-video,camera-photo" },
        { "audio", "USB Headset", "USB Headset", "audio-headset,audio-card" },
        { "other", "Pixel", "Pixel", "preferences-desktop-peripherals,drive-removable-media-usb" },
        { "storage", "", "USB device", "drive-removable-media-usb" },                              /* nothing to call it */
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char text[128], key[24], connected[160], disconnected[160];
        snprintf(key, sizeof key, "usb.u%zu", i + 10);
        snprintf(text, sizeof text, "%s %s\tkind=%s", key, cases[i].name, cases[i].kind);
        snprintf(connected, sizeof connected, "%s connected", cases[i].text);
        snprintf(disconnected, sizeof disconnected, "%s disconnected", cases[i].text);
        int before = shown;
        say(text);                                                 /* it appears: connected */
        assert(shown == before + 1 && !strcmp(last, connected) && last_level == LEVEL_INFO && !strncmp(toast_icon, cases[i].icon, strlen(cases[i].icon)));
        assert(last_sound == SOUND_DEVICE_ADDED);
        say(text);                                                 /* said again, with the same words: not a second connection */
        assert(shown == before + 1);
        char prop[40];
        snprintf(prop, sizeof prop, "%s.size 3", key);
        say(prop);                                                 /* what is under it is not a connection either */
        assert(shown == before + 1);
        say(key);                                                  /* it goes: disconnected, as it was called */
        assert(shown == before + 2 && !strcmp(last, disconnected) && last_sound == SOUND_DEVICE_REMOVED);
        char off_icon[160];
        int first = (int)strcspn(cases[i].icon, ",");
        snprintf(off_icon, sizeof off_icon, "%.*s-off,%s", first, cases[i].icon, cases[i].icon);
        assert(!strncmp(toast_icon, off_icon, strlen(off_icon)));
    }

    /* The mouse of the snapshot goes too: said as it was called, though it was never said coming in */
    int before = shown;
    say("usb.u1");
    assert(shown == before + 1 && !strcmp(last, "Logitech disconnected"));
    say("usb.u1");                                                 /* gone twice: nothing more */
    assert(shown == before + 1);

    /* A keyboard, a mouse, a tablet are input.ID, whatever they are plugged into: said when it is on USB (Bluetooth has bt.ID, the machine's own is no event) */
    before = shown;
    say("input.i1 Logitech K120\tkind=keyboard\tbus=usb");
    assert(shown == before + 1 && !strcmp(last, "Logitech K120 connected") && !strncmp(toast_icon, "input-keyboard", 14));
    say("input.i1");
    assert(shown == before + 2 && !strcmp(last, "Logitech K120 disconnected"));
    say("input.i2 Synaptics\tkind=touchpad\tbus=builtin");  say("input.i2");
    say("input.i3 MX Keys\tkind=keyboard\tbus=bluetooth");  say("input.i3");
    assert(shown == before + 2);
    say("input.i4 Pad\tkind=touchpad\tbus=usb");
    assert(shown == before + 3 && !strncmp(toast_icon, "input-mouse", 11));
    say("input.i4");
    before = shown;

    /* Only the kinds asked for (usb = camera,tablet), and "0" says none */
    bool none;
    assert(parse_kinds(NULL, &none) == (1u << U_KINDS) - 1 && !none);
    assert(parse_kinds("", &none) == (1u << U_KINDS) - 1 && parse_kinds("all", &none) == (1u << U_KINDS) - 1);
    assert(parse_kinds("0", &none) == 0 && none);
    usb_enabled = parse_kinds("camera,tablet", &none);
    assert(usb_enabled == ((1u << U_CAMERA) | (1u << U_TABLET)) && !none);
    before = shown;
    say("usb.u40 HP\tkind=printer");                              assert(shown == before);
    say("usb.u41 Typist\tkind=keyboard");                         assert(shown == before);   /* input: not asked for */
    say("usb.u42 Sketch\tkind=tablet");                           assert(shown == before + 1);
    say("usb.u40"); say("usb.u41");                                assert(shown == before + 1);   /* and what goes was not said coming */
    assert(parse_kinds("Tablet, storage", &none) == (1u << U_TABLET));
    assert(parse_kinds("camera,bogus", &none) == (1u << U_CAMERA));
    assert(parse_kinds("bogus", &none) == (1u << U_KINDS) - 1);
    usb_off = true;
    before = shown;
    usb_enabled = (1u << U_KINDS) - 1;
    say("usb.u50 X\tkind=mouse");                                 assert(shown == before);
    say("usb.u50");                                                assert(shown == before);
    usb_off = false;
}

static void test_printing(void) {
    reset();
    print_off = false; print_start = true;
    say("printer.state idle");
    say("event printer.p1 job-done Old report");                  /* in the snapshot: a thing of the past */
    assert(shown == 0);
    say("sync");

    static const struct { const char *event, *text, *icon; Level level; int alerts; } cases[] = {
        { "job-started Quarterly report.pdf", "Printing — Quarterly report.pdf", "printer", LEVEL_INFO, 0 },
        { "job-done Quarterly report.pdf", "Printed — Quarterly report.pdf", "printer", LEVEL_INFO, 0 },
        { "job-done-errors Draft", "Printed with errors — Draft", "printer-warning,printer-error,printer", LEVEL_WARN, 0 },
        { "job-failed Big file", "Print failed — Big file", "printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "job-done", "Printed — Document", "printer", LEVEL_INFO, 0 },              /* a job with no name */
        { "paper HP ENVY 4520 series", "HP ENVY 4520 series: out of paper", "printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "jam HP ENVY 4520 series", "HP ENVY 4520 series: paper jam", "printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "ink-empty Office Laser", "Office Laser: out of ink or toner", "printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "ink-low Office Laser", "Office Laser: ink or toner low", "printer-warning,printer", LEVEL_INFO, 0 },
        { "cover Office Laser", "Office Laser: cover open", "printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "offline Office Laser", "Office Laser: offline", "printer-off,printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "stopped Office Laser", "Office Laser: stopped", "printer-error,printer-warning,printer", LEVEL_WARN, 1 },
        { "ready Office Laser", "Office Laser: ready", "printer", LEVEL_INFO, 0 },
    };
    int alerts = 0;
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char text[128];
        snprintf(text, sizeof text, "event printer.p1 %s", cases[i].event);
        int before = shown, sounds = nsounds;
        say(text);
        assert(shown == before + 1 && !strcmp(last, cases[i].text) && last_level == cases[i].level && !strcmp(toast_icon, cases[i].icon));
        assert(nsounds == sounds + cases[i].alerts && (!cases[i].alerts || last_sound == SOUND_ALERT));
        alerts += cases[i].alerts;
    }

    /* not an event, or a kind that is not known: nothing */
    int before = shown;
    say("event printer.p1 frobnicated x"); say("event printer.p1"); say("event nonsense");
    assert(shown == before);

    /* print = done: not the start of a job, the rest as before; print = 0: nothing */
    print_start = false;
    say("event printer.p1 job-started Quiet");                    assert(shown == before);
    say("event printer.p1 job-done Quiet");                       assert(shown == before + 1);
    print_off = true;
    say("event printer.p1 job-failed Quiet");                     assert(shown == before + 1);
    print_off = false; print_start = true;
}

static void test_screen_keys_bluetooth(void) {
    reset();
    /* The brightness: what first appears is learned; a change is a bar */
    say("backlight.level 50");
    assert(slider_calls == 0);
    say("sync");
    say("backlight.level 60");
    assert(slider_calls == 1 && !strcmp(slider_label, "Brightness") && slider_pct == 60 && !slider_dim && !strcmp(slider_icon, "display-brightness"));
    say("backlight.level 60");                                assert(slider_calls == 1);          /* no change */
    say("backlight.level");                                   /* gone and back: new, not a change */
    say("backlight.level 10");                                assert(slider_calls == 1);

    /* The lock keys: the first reading is the starting point; a change is said */
    say("lock.caps off"); say("lock.num on");                 assert(shown == 0);
    say("lock.caps on");
    assert(shown == 1 && !strcmp(last, "Caps Lock on") && last_level == LEVEL_INFO);
    say("lock.caps off");                                     assert(!strcmp(last, "Caps Lock off"));
    say("lock.num off");                                      assert(!strcmp(last, "Num Lock off"));
    say("lock.num on");                                       assert(!strcmp(last, "Num Lock on") && shown == 4);
    say("lock.num on");                                       assert(shown == 4);
}

static void test_bluetooth(void) {
    reset();
    say("bt.b1 Before the snapshot ends\tkind=headset\tuid=aa-bb-cc-dd-ee-ff");   /* connected before pacreact came: learned, not said */
    assert(shown == 0);
    say("sync");
    int sounds = nsounds;
    say("bt.b2 WH-1000XM4\tkind=headset\tuid=11-22-33-44-55-66");
    assert(shown == 1 && !strcmp(last, "Bluetooth: WH-1000XM4 connected") && last_level == LEVEL_INFO);
    assert(!strncmp(toast_icon, "bluetooth-active", 16) && last_sound == SOUND_DEVICE_ADDED && nsounds == sounds + 1);
    say("bt.b2");
    assert(!strcmp(last, "Bluetooth: WH-1000XM4 disconnected") && !strncmp(toast_icon, "bluetooth-disconnected", 22) && last_sound == SOUND_DEVICE_REMOVED);
    say("bt.b3 \tkind=other");                                     /* no name */
    assert(!strcmp(last, "Bluetooth: ? connected"));
    say("bt.b1");                                                  /* the first one goes: said as it was, though it was never said coming in */
    assert(!strcmp(last, "Bluetooth: Before the snapshot ends disconnected"));
    int before = shown;
    say("bt.b9");                                                  /* a device never known: nothing */
    say("bt.b3 \tkind=other");                                    /* said twice */
    assert(shown == before);
}

static void test_disks(void) {
    char s[32];
    disk_format_size(5ULL << 30, s, sizeof s);                assert(!strcmp(s, "5.0 GB"));
    disk_format_size(1536ULL << 20, s, sizeof s);             assert(!strcmp(s, "1.5 GB"));
    disk_format_size(500ULL << 20, s, sizeof s);              assert(!strcmp(s, "500 MB"));
    disk_format_size(700 * 1024, s, sizeof s);                assert(!strcmp(s, "700 KB"));
    disk_format_size(0, s, sizeof s);                         assert(!strcmp(s, "0 KB"));

    reset();
    memset(disks, 0, sizeof disks);
    say("disk.data.mount /data"); say("disk.data.free 53687091200"); say("disk.data 50");
    say("sync");
    assert(shown == 0 && asked[0] == '\0');

    say("disk.data.mount /data"); say("disk.data.free 8589934592");
    say("disk.data 8\tlevel=warning");                          /* 8%: low */
    assert(shown == 1 && last_level == LEVEL_WARN && !strcmp(last, "Disk space low — /data: 8% free (8.0 GB)"));
    assert(!strcmp(asked, "disk:/data") && !strcmp(toast_icon, "drive-harddisk,dialog-warning"));
    say("disk.data 7\tlevel=warning");                          assert(shown == 1);                  /* no worse: nothing */
    say("disk.data.free 3221225472");
    say("disk.data 3\tlevel=critical");                         /* 3%: almost full */
    assert(shown == 2 && last_level == LEVEL_CRIT && !strcmp(last, "Disk almost full — /data: 3% free (3.0 GB)"));
    assert(!strcmp(toast_icon, "drive-harddisk,dialog-error"));
    say("disk.data 2\tlevel=critical");                         assert(shown == 2);
    say("disk.data 30");                                       /* recovered: silent */
    assert(shown == 2);
    say("disk.data 8\tlevel=warning");                          /* and it can alert again */
    assert(shown == 3);

    /* Another filesystem has its own state, and says where it is */
    say("disk.home.mount /home"); say("disk.home.free 9663676416"); say("disk.home 9\tlevel=warning");
    assert(shown == 4 && strstr(last, "/home"));

    /* "The same as before the restart" (alert_allowed says no): not shown, and not shown again either */
    say("disk.held.mount /held"); say("disk.held.free 3221225472");
    allow = false;
    say("disk.held 3\tlevel=critical");                         assert(shown == 4);
    allow = true;
    say("disk.held 3\tlevel=critical");                         assert(shown == 4);

    /* Unmounted and back: told anew */
    say("disk.held"); say("disk.held.mount"); say("disk.held.free");
    say("disk.held.mount /held"); say("disk.held.free 3221225472");
    say("disk.held 3\tlevel=critical");                         assert(shown == 5);

    /* More filesystems than slots: the extra ones are ignored, not a crash */
    for (int i = 0; i < MAX_DISKS + 4; i++) {
        char text[64];
        snprintf(text, sizeof text, "disk.fill-%d 50", i);
        say(text);
    }
}

static void test_probe(void) {
    reset();
    say("net.probe.host 1.1.1.1");
    say("net.probe ok");                                       /* in the snapshot: learned */
    say("sync");
    assert(shown == 0);
    say("net.latency 800");
    say("net.probe slow\tlevel=warning");
    assert(shown == 1 && last_level == LEVEL_WARN && !strcmp(last, "Network slow — 800 ms") && !strcmp(toast_icon, "network-error,dialog-warning"));
    say("net.probe slow\tlevel=warning");                       assert(shown == 1);                  /* no repeats */
    say("net.probe down\tlevel=warning");
    assert(shown == 2 && !strcmp(last, "Network not answering — no reply from 1.1.1.1"));
    say("net.probe ok");
    assert(shown == 3 && !strcmp(last, "Network back to normal") && last_level == LEVEL_INFO);
    say("net.probe ok");                                       assert(shown == 3);
    say("net.probe");                                          /* gone and back: learned */
    say("net.probe slow\tlevel=warning");                       assert(shown == 3);
}

/* Text a device or a document chose cannot pose as an attribute: only what follows a TAB is one */
static void test_attributes_are_not_text(void) {
    reset();
    say("audio.output Speakers"); say("sync");
    int before = shown;
    say("audio.output Headset cause=jack");                       /* a device that names itself so: still a change of output, not a jack's */
    assert(shown == before + 1 && !strcmp(last, "Audio output: Headset cause=jack"));
    say("audio.output Speakers\tcause=jack");                     /* the real thing: the jack moved it */
    assert(shown == before + 1);

    int slider = slider_calls;
    say("audio.volume 30"); say("audio.mute off");
    say("audio.volume 40 cause=device");                          /* not an attribute: a volume that is not even a number */
    assert(slider_calls >= slider);

    say("usb.u70 Fake level=critical cause=device\tkind=storage");
    assert(!strcmp(last, "Fake level=critical cause=device connected") && last_level == LEVEL_INFO);
    say("power.battery 50 level=critical");                       /* text after the number, not a level */
    battery_seen = LEVEL_INFO;
    int toasts = shown;
    say("power.battery 50 level=critical");
    assert(shown == toasts);
    say("power.battery 50\tlevel=critical");                      /* the level */
    assert(shown == toasts + 1 && last_level == LEVEL_CRIT);

    assert(daemon_has_attr("a=b\tlevel=warning\tcause=jack", "cause", "jack") && daemon_has_attr("level=warning", "level", "warning"));
    assert(!daemon_has_attr("level=warning", "level", "warn") && !daemon_has_attr("level=warning", "leve", "warning") && !daemon_has_attr(NULL, "level", "warning"));
    assert(!daemon_has_attr("xlevel=warning", "level", "warning") && !daemon_has_attr("level=warningx", "level", "warning"));
}

static void test_ignored(void) {
    reset();
    say("ok");  say("error unknown command");  say("sync");  say("power.battery");   /* a key that is gone */
    say("cpu.pressure on\tlevel=warning");  say("daemon.version 0.1.0");  say("power.battery.status discharging");
    say("temp.gpu 99\tlevel=critical");  say("power.battery x level=bogus");
    assert(shown == 0);
}

static void push(int fd, const char *text) { assert(send(fd, text, strlen(text), 0) == (ssize_t)strlen(text)); }

static void test_network(void) {
    reset();
    online = -1; device[0] = '\0';
    say("net.device wlan0"); say("net.online on");           /* in the snapshot: known, not announced */
    say("sync");
    assert(shown == 0 && online == 1);
    say("net.online on");                    assert(shown == 0);
    say("net.online off");                   assert(shown == 1 && !strcmp(last, "Offline") && last_level == LEVEL_WARN);
    assert(last_sound == SOUND_NETWORK_DOWN && online == 0);
    say("net.device");                       /* gone with the route */
    assert(!device[0]);
    say("net.device enp2s0"); say("net.online on");
    assert(shown == 2 && !strcmp(last, "Online — enp2s0") && last_level == LEVEL_INFO && last_sound == SOUND_NETWORK_UP);
    assert(!strncmp(toast_icon, "network-wired", 13));
    say("net.online off"); say("net.device wlp3s0"); say("net.online on");
    assert(shown == 4 && !strncmp(toast_icon, "network-wireless", 16));
    say("net.link wifi"); say("net.signal 60");              /* what a bar shows is not ours */
    assert(shown == 4);
}

/* A daemon of our own on a socket, made the way pacreactd makes its own: what is asked of it, what is told to it, and
 * what happens when it goes and comes back. */
static int listen_like_daemon(const char *dir) {
    struct sockaddr_un a = { .sun_family = AF_UNIX }, tmp = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s/pacreactd.sock", dir);
    snprintf(tmp.sun_path, sizeof tmp.sun_path, "%s/pacreactd.sock.new", dir);
    int lfd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0);
    assert(lfd >= 0 && bind(lfd, (struct sockaddr *)&tmp, sizeof tmp) == 0 && listen(lfd, 1) == 0);
    assert(rename(tmp.sun_path, a.sun_path) == 0);
    return lfd;
}

static const char *heard(int cfd) {
    static char buf[128];
    ssize_t n = recv(cfd, buf, sizeof buf - 1, MSG_DONTWAIT);
    buf[n < 0 ? 0 : n] = '\0';
    return buf;
}


static void test_connection(void) {
    char dir[64] = "/tmp/pacreact-daemon-XXXXXX";
    assert(mkdtemp(dir));
    setenv("XDG_RUNTIME_DIR", dir, 1);

    /* No daemon: nothing is connected, nothing is tried again on a timer, and what is told is kept */
    reset();
    daemon_init();
    assert(sock < 0 && watch_fd >= 0);
    daemon_publish("notifications.count", "2");
    daemon_publish("monitor.count", "2");

    /* It starts: the name appears, and the connection follows at once, telling what was kept */
    int lfd = listen_like_daemon(dir);
    on_socket_dir(NULL, 0);
    assert(sock >= 0 && handler == on_readable);
    int cfd = accept(lfd, NULL, NULL);
    assert(!strcmp(heard(cfd), "subscribe power mem temp net audio usb printer backlight lock disk bt input jack camera"));
    assert(!strcmp(heard(cfd), "own notifications") && !strcmp(heard(cfd), "publish notifications.count 2"));
    assert(!strcmp(heard(cfd), "own monitor") && !strcmp(heard(cfd), "publish monitor.count 2"));
    assert(!*heard(cfd));

    /* What is told after that goes out as it changes, once; a namespace is claimed once */
    daemon_publish("notifications.count", "2");
    assert(!*heard(cfd));
    daemon_publish("notifications.count", "3");
    assert(!strcmp(heard(cfd), "publish notifications.count 3") && !*heard(cfd));
    daemon_publish("monitor.count", NULL);
    assert(!strcmp(heard(cfd), "retract monitor.count"));
    daemon_publish("monitor.count", NULL);
    assert(!*heard(cfd));
    daemon_publish("usb.mouse", "on");
    assert(!strcmp(heard(cfd), "own usb") && !strcmp(heard(cfd), "publish usb.mouse on"));

    push(cfd, "ok");
    push(cfd, "power.battery 12\tlevel=warning");
    push(cfd, "sync");
    on_readable(NULL, 0);
    assert(shown == 1 && synced);

    /* It goes: what it said is forgotten */
    close(cfd);
    on_readable(NULL, 0);
    assert(sock < 0 && !synced && removed == 1 && n_owned == 0);
    close(lfd);
    char path[96];
    snprintf(path, sizeof path, "%s/pacreactd.sock", dir);
    unlink(path);

    /* It is back: everything told is told again, and its snapshot is not news */
    lfd = listen_like_daemon(dir);
    on_socket_dir(NULL, 0);
    assert(sock >= 0);
    cfd = accept(lfd, NULL, NULL);
    assert(!strcmp(heard(cfd), "subscribe power mem temp net audio usb printer backlight lock disk bt input jack camera"));
    int told_again = 0;
    for (const char *m; *(m = heard(cfd));) told_again += !strncmp(m, "publish ", 8);
    assert(told_again == 2);                 /* the count and the mouse; the monitors were retracted */
    push(cfd, "power.battery 12\tlevel=warning");
    on_readable(NULL, 0);
    assert(shown == 1);                      /* the same level as before: no second toast */

    /* Something else appearing in the directory is not the daemon */
    close(cfd);
    on_readable(NULL, 0);
    close(lfd);
    unlink(path);
    char other[96];
    snprintf(other, sizeof other, "%s/other", dir);
    FILE *f = fopen(other, "w");
    assert(f);
    fclose(f);
    on_socket_dir(NULL, 0);
    assert(sock < 0);

    char cmd[100];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
}

int main(void) {
    test_battery();
    test_charger();
    test_memory();
    test_temperature();
    test_network();
    test_sound();
    test_icons_by_level();
    test_jacks();
    test_usb();
    test_printing();
    test_screen_keys_bluetooth();
    test_bluetooth();
    test_disks();
    test_probe();
    test_attributes_are_not_text();
    test_ignored();
    test_connection();
    puts("daemon: ok");
    return 0;
}
