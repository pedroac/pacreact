#include "alerts.h"
#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include "daemon_value.h"
#include "watch.h"
#include "tunables.h"
#include "setting.h"
#include <errno.h>
#include <strings.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <net/if.h>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* What pacreactd says about the battery and the charger, memory, the temperature and the network, shown as
 * toasts, and what pacreact knows that the others may want (the notifications waiting, and the sources that
 * have not moved into pacreactd yet), told to it. pacreactd (docs/protocol.md) watches and keeps state; this decides what is worth a toast.
 * A key's level rising (battery low, memory short, a sensor hot) is an alert. What is in the
 * snapshot at connect time still goes through alert_allowed(), so a condition that was there before
 * a restart is not shown again; the charger is only announced when it changes after the snapshot. */

#define DAEMON_SUBSCRIPTION "subscribe power mem temp net audio usb printer backlight lock disk bt input jack camera"
#define SOCKET_NAME "pacreactd.sock"
#define MAX_PUBLISHED 24
#define MAX_OWNED 8

static int sock = -1, watch_fd = -1;    /* the daemon, and inotify on the directory its socket appears in */
static bool synced;                     /* the snapshot is over: what comes now is news */
static int ac = -1;                     /* the charger: on 1, off 0, unknown -1 */
static Level battery_seen;              /* the level the battery key had, to see it rise */
static Level temp_seen[2];              /* cpu, ssd */
static int online = -1;                 /* net.online: on 1, off 0, unknown -1 */
static char device[IF_NAMESIZE];        /* net.device, for the message */
static char output[64];                 /* audio.output: the default output's description, for the message */
/* The devices that are connected, as pacreactd keeps them (usb.ID, bt.ID, jack.ID, input.ID): what each was called and what it was, so that its going can be said
 * as what it was. A device is there from its key's appearing until its going; its ID is not given again. */
static struct { char key[24], kind[16], bus[12], name[64]; } devices_there[MAX_DEVICES_SEEN];
static long available_mb = -1;          /* mem.available, for the memory alert */
static time_t last_mem_alert;           /* when the last memory alert was shown, and how bad it was */
static Level last_mem_level;

static Level attrs_level(const char *attrs) {
    return daemon_has_attr(attrs, "level", "critical") ? LEVEL_CRIT : daemon_has_attr(attrs, "level", "warning") ? LEVEL_WARN : LEVEL_INFO;
}

static void lost(void);

static void battery(int percent, Level level) {
    if (level <= battery_seen) { battery_seen = level; return; }
    battery_seen = level;
    if (!alert_allowed("battery", level)) return;
    char msg[128];
    snprintf(msg, sizeof msg, level == LEVEL_CRIT ? _("Battery critical — %d%%") : _("Battery low — %d%%"), percent);
    hud_toast_icon(msg, level, SRC_POWER_ALERT_MS,
                   level == LEVEL_CRIT ? "battery-caution,battery-low" : "battery-low,battery-caution");
    sound_play(SOUND_ALERT);
}

static void charger(bool on) {
    int prev = ac;
    ac = on;
    if (!synced || prev < 0 || prev == ac) return;
    hud_toast_icon(on ? _("Charger connected") : _("Charger disconnected"), LEVEL_INFO, 2500,
                   on ? "ac-adapter,battery-full-charging" : "battery,battery-full");
    sound_play(on ? SOUND_POWER_PLUGGED : SOUND_POWER_UNPLUGGED);
}

static void memory(Level level) {
    if (level == LEVEL_INFO) return;
    time_t now = time(NULL);
    /* The cooldown holds back repeats, not escalations: a critical alert right after a warning
     * must still come through. */
    if (last_mem_alert && now - last_mem_alert < COOLDOWN_SECS && level <= last_mem_level) return;
    /* Left out as "nothing new since before a restart": that must not start the cooldown, or a
     * worse alert arriving a moment later would be held back by it. */
    if (!alert_allowed("memory", level)) return;
    last_mem_alert = now;
    last_mem_level = level;
    char msg[128];
    snprintf(msg, sizeof msg, _("Memory pressure — %ld MB available"), available_mb);
    hud_toast_icon(msg, level, 6000, level == LEVEL_CRIT ? "dialog-error,dialog-warning" : "dialog-warning");
    sound_play(SOUND_ALERT);
}

static void temperature(int which, int celsius, Level level) {
    if (level <= temp_seen[which]) { temp_seen[which] = level; return; }
    temp_seen[which] = level;
    if (!alert_allowed(which ? "temp-ssd" : "temp-cpu", level)) return;
    char msg[96];
    if (which == 0)
        snprintf(msg, sizeof msg, level == LEVEL_CRIT ? _("CPU temperature critical — %d °C") : _("CPU temperature high — %d °C"), celsius);
    else
        snprintf(msg, sizeof msg, level == LEVEL_CRIT ? _("SSD temperature critical — %d °C") : _("SSD temperature high — %d °C"), celsius);
    hud_toast_icon(msg, level, SRC_TEMP_ALERT_MS,
                   level == LEVEL_CRIT ? "dialog-error,dialog-warning" : "dialog-warning");
    sound_play(SOUND_ALERT);
}

static void network(bool is_online) {
    int prev = online;
    online = is_online;
    if (!synced || prev < 0 || prev == online) return;
    if (!is_online) {
        hud_toast_icon(_("Offline"), LEVEL_WARN, 4000, "network-offline,network-error");
        sound_play(SOUND_NETWORK_DOWN);
        return;
    }
    char msg[128];
    snprintf(msg, sizeof msg, _("Online — %s"), device[0] ? device : _("network"));
    /* wlan0, wlp3s0: a wireless interface by its name; anything else is taken to be a cable */
    hud_toast_icon(msg, LEVEL_INFO, 3000, device[0] == 'w' && device[1] == 'l' ? "network-wireless,network-wired" : "network-wired,network-idle");
    sound_play(SOUND_NETWORK_UP);
}

/* ---- sound ---- */

/* The default output (0) and input (1): the level and mute as last said, and as last shown or learned (`base`), so
 * that what comes in one burst of lines is one bar. */
static struct { int pct, muted, base_pct, base_muted; bool dirty, device; } track[2] = { { -1, -1, -1, -1, false, false }, { -1, -1, -1, -1, false, false } };

/* The status icon for a level: the speaker with more or fewer waves, struck through when muted */
static const char *icon_for(int t, int pct, bool muted) {
    if (t == 1)
        return muted ? "microphone-sensitivity-muted" : pct < 34 ? "microphone-sensitivity-low"
             : pct < 67 ? "microphone-sensitivity-medium" : "microphone-sensitivity-high";
    if (muted || pct <= 0) return "audio-volume-muted";
    if (pct > 100) return "audio-volume-overamplified,audio-volume-high";
    return pct < 34 ? "audio-volume-low" : pct < 67 ? "audio-volume-medium" : "audio-volume-high";
}

static const char *label_for(int t, bool muted) {
    if (t == 1) return muted ? _("Microphone — muted") : _("Microphone");
    return muted ? _("Volume — muted") : _("Volume");
}

/* After a burst of lines: a level or mute that moved from one we knew, and not because the device did, is the person's: a bar. */
static void flush_audio(void) {
    for (int t = 0; t < 2; t++) {
        if (!track[t].dirty) continue;
        track[t].dirty = false;
        /* Each counts as changed only from a value it had: a mute that first appears after the level is learned, not shown. */
        bool changed = (track[t].base_pct >= 0 && track[t].pct != track[t].base_pct) ||
                       (track[t].base_muted >= 0 && track[t].muted != track[t].base_muted);
        bool show = changed && track[t].pct >= 0 && !track[t].device;
        track[t].base_pct = track[t].pct;
        track[t].base_muted = track[t].muted;
        track[t].device = false;
        if (!show) continue;
        hud_osd_slider(label_for(t, track[t].muted > 0), track[t].pct, track[t].muted > 0, OSD_CENTER, icon_for(t, track[t].pct, track[t].muted > 0));
        if (t == 0) sound_play(SOUND_VOLUME);   /* the output, not the microphone */
    }
}

static void forget_audio(int t) {
    track[t].pct = track[t].muted = track[t].base_pct = track[t].base_muted = -1;
    track[t].dirty = track[t].device = false;
}

/* A different default output is said once, as a message, unless a 3.5 mm jack moved it: the jack's message already told of the headset. */
static void audio_output(const char *description, bool jack) {
    bool known = output[0] && strcmp(output, description) != 0;
    snprintf(output, sizeof output, "%s", description);
    if (!known || jack || !synced) return;
    char msg[192];
    snprintf(msg, sizeof msg, _("Audio output: %s"), description);
    hud_toast(msg, LEVEL_INFO, 2500);
}

/* What a plug was, as the sound server and the jack's contacts settled it (pacreactd): "headset plugged", "device unplugged". */
static void jack_event(const char *event) {
    static const struct { const char *kind, *on, *off, *on_icon, *off_icon; } kinds[] = {
        { "device", N_("Audio device connected"), N_("Audio device disconnected"), "audio-card,audio-headphones", "audio-card,audio-headphones" },
        { "headset", N_("Headset connected"), N_("Headset disconnected"), "audio-headset,audio-headphones", "audio-headset-off,audio-headset,audio-headphones" },
        { "headphones", N_("Headphones connected"), N_("Headphones disconnected"), "audio-headphones,audio-headset", "audio-headphones-off,audio-headphones,audio-headset" },
        { "lineout", N_("Line out connected"), N_("Line out disconnected"), "audio-speakers", "audio-speakers" },
        { "mic", N_("Microphone connected"), N_("Microphone disconnected"), "audio-input-microphone", "audio-input-microphone-off,audio-input-microphone" },
    };
    char kind[16], direction[16];
    if (sscanf(event, "%15s %15s", kind, direction) != 2) return;
    bool plugged = !strcmp(direction, "plugged");
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(kind, kinds[i].kind)) continue;
        hud_toast_icon(plugged ? _(kinds[i].on) : _(kinds[i].off), LEVEL_INFO, SRC_JACK_TOAST_MS, plugged ? kinds[i].on_icon : kinds[i].off_icon);
        sound_play(plugged ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED);
        return;
    }
}

static bool jack_of_kind(const char *kind) {
    for (int i = 0; i < MAX_DEVICES_SEEN; i++)
        if (!strncmp(devices_there[i].key, "jack.", 5) && !strcmp(devices_there[i].kind, kind)) return true;
    return false;
}

/* What is plugged in now, as words for `pacreact jacks`, each led by a space: " headset", " headphones", " microphone", " lineout", " none". */
void jack_describe(char *out, size_t n) {
    bool hs = jack_of_kind("headset") || jack_of_kind("device"), hp = jack_of_kind("headphones"), mic = jack_of_kind("mic"), line = jack_of_kind("lineout");
    snprintf(out, n, "%s%s%s%s%s", hs ? " headset" : "", hp ? " headphones" : "", mic ? " microphone" : "", line ? " lineout" : "",
             hs || hp || mic || line ? "" : " none");
}

/* ---- the screen, the lock keys, Bluetooth ---- */

static int backlight = -1;              /* backlight.level as last said */
static int locks_known[2] = { -1, -1 };  /* lock.caps, lock.num */

/* What is said of the screen's brightness: a change from a level we knew is the person's, and a bar. */
static void brightness(int percent) {
    int prev = backlight;
    backlight = percent;
    if (prev < 0 || prev == percent) return;
    hud_osd_slider(_("Brightness"), percent, false, OSD_CENTER, "display-brightness");
}

/* A lock key changed from a state we knew: said. The first reading is the starting point, not news. */
static void lock_key(int which, bool on) {
    int prev = locks_known[which];
    locks_known[which] = on;
    if (prev < 0 || prev == on) return;
    const char *msg = which == 0 ? (on ? _("Caps Lock on") : _("Caps Lock off")) : (on ? _("Num Lock on") : _("Num Lock off"));
    hud_toast(msg, LEVEL_INFO, SRC_LOCKS_TOAST_MS);
}

/* What BlueZ said (pacreactd): "5 connected WH-1000XM4", "6 disconnected". */
static void bluetooth_event(const char *event) {
    unsigned number;
    char direction[16];
    int used = 0;
    if (sscanf(event, "%u %15s %n", &number, direction, &used) < 2) return;
    bool connected = !strcmp(direction, "connected");
    const char *name = event + used;
    char msg[160];
    snprintf(msg, sizeof msg, connected ? _("Bluetooth: %s connected") : _("Bluetooth: %s disconnected"), *name ? name : "?");
    hud_toast_icon(msg, LEVEL_INFO, SRC_BT_TOAST_MS,
                   connected ? "bluetooth-active,bluetooth" : "bluetooth-disconnected,bluetooth-disabled,bluetooth");
    sound_play(connected ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED);
}

/* ---- disk space ---- */

/* The filesystems as pacreactd names them (disk.NAME), with the mount point and the bytes left that it says beside the percentage,
 * and the level last seen, to see it rise. */
static struct { char name[24], mount[64]; unsigned long long avail; Level seen; } disks[MAX_DISKS];

void disk_format_size(unsigned long long bytes, char *out, size_t n) {
    if (bytes >= (1ULL << 30)) snprintf(out, n, "%.1f GB", (double)bytes / (double)(1ULL << 30));
    else if (bytes >= (1ULL << 20)) snprintf(out, n, "%llu MB", bytes >> 20);
    else snprintf(out, n, "%llu KB", bytes >> 10);
}

static int disk_slot(const char *name, bool make) {
    int free_slot = -1;
    for (int i = 0; i < MAX_DISKS; i++) {
        if (!strcmp(disks[i].name, name)) return i;
        if (!disks[i].name[0] && free_slot < 0) free_slot = i;
    }
    if (!make || free_slot < 0) return -1;
    snprintf(disks[free_slot].name, sizeof disks[0].name, "%s", name);
    disks[free_slot].seen = LEVEL_INFO;
    return free_slot;
}

/* disk.NAME with a level that rose: low, or almost full. What the daemon said before it (the mount and the bytes) is what is said. */
static void disk_level(int slot, int percent, Level level) {
    if (level <= disks[slot].seen) { disks[slot].seen = level; return; }
    disks[slot].seen = level;
    const char *mount = disks[slot].mount[0] ? disks[slot].mount : disks[slot].name;
    char key[96], size[32], msg[512];
    snprintf(key, sizeof key, "disk:%s", mount);
    if (!alert_allowed(key, level)) return;
    disk_format_size(disks[slot].avail, size, sizeof size);
    if (level == LEVEL_CRIT) snprintf(msg, sizeof msg, _("Disk almost full — %s: %d%% free (%s)"), mount, percent, size);
    else snprintf(msg, sizeof msg, _("Disk space low — %s: %d%% free (%s)"), mount, percent, size);
    hud_toast_icon(msg, level, SRC_DISK_ALERT_MS, level == LEVEL_CRIT ? "drive-harddisk,dialog-error" : "drive-harddisk,dialog-warning");
    sound_play(SOUND_ALERT);
}

/* ---- the network probe ---- */

static char probe_state[8], probe_host[64];   /* net.probe as last said ("" unknown), and the address it probes */
static long latency_ms;

/* ok to slow or down, or back: said. What first appears is learned. */
static void probe(const char *state) {
    bool known = probe_state[0] != '\0', changed = strcmp(probe_state, state) != 0;
    snprintf(probe_state, sizeof probe_state, "%s", state);
    if (!known || !changed || !synced) return;
    char msg[128];
    if (!strcmp(state, "slow")) {
        snprintf(msg, sizeof msg, _("Network slow — %ld ms"), latency_ms);
        hud_toast_icon(msg, LEVEL_WARN, 6000, "network-error,dialog-warning");
    } else if (!strcmp(state, "down")) {
        snprintf(msg, sizeof msg, _("Network not answering — no reply from %s"), probe_host[0] ? probe_host : _("probe host"));
        hud_toast_icon(msg, LEVEL_WARN, 6000, "network-error,dialog-warning");
    } else if (!strcmp(state, "ok")) {
        hud_toast_icon(_("Network back to normal"), LEVEL_INFO, 3000, "network-idle,network-wired");
    }
}

/* ---- printing ---- */

static bool print_off, print_start = true;   /* print = 0 says nothing, print = done leaves the "Printing" message out */

/* What CUPS said (pacreactd): "5 job-done Quarterly report.pdf", "6 paper HP ENVY 4520 series". */
static void printer_event(const char *event) {
    static const struct { const char *kind, *format, *icon; Level level; bool alert, job; } kinds[] = {
        { "job-started", N_("Printing — %s"), "printer", LEVEL_INFO, false, true },
        { "job-done", N_("Printed — %s"), "printer", LEVEL_INFO, false, true },
        { "job-done-errors", N_("Printed with errors — %s"), "printer-warning,printer-error,printer", LEVEL_WARN, false, true },
        { "job-failed", N_("Print failed — %s"), "printer-error,printer-warning,printer", LEVEL_WARN, true, true },
        { "paper", N_("%s: out of paper"), "printer-error,printer-warning,printer", LEVEL_WARN, true, false },
        { "jam", N_("%s: paper jam"), "printer-error,printer-warning,printer", LEVEL_WARN, true, false },
        { "ink-empty", N_("%s: out of ink or toner"), "printer-error,printer-warning,printer", LEVEL_WARN, true, false },
        { "ink-low", N_("%s: ink or toner low"), "printer-warning,printer", LEVEL_INFO, false, false },
        { "cover", N_("%s: cover open"), "printer-error,printer-warning,printer", LEVEL_WARN, true, false },
        { "offline", N_("%s: offline"), "printer-off,printer-error,printer-warning,printer", LEVEL_WARN, true, false },
        { "stopped", N_("%s: stopped"), "printer-error,printer-warning,printer", LEVEL_WARN, true, false },
        { "ready", N_("%s: ready"), "printer", LEVEL_INFO, false, false },
    };
    unsigned number;
    char kind[24];
    int used = 0;
    if (sscanf(event, "%u %23s %n", &number, kind, &used) < 2) return;
    const char *arg = event + used;
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(kind, kinds[i].kind)) continue;
        if (print_off || (!strcmp(kind, "job-started") && !print_start)) return;
        char msg[256];
        snprintf(msg, sizeof msg, _(kinds[i].format), kinds[i].job && !*arg ? _("Document") : arg);
        hud_toast_icon(msg, kinds[i].level, SRC_PRINT_TOAST_MS, kinds[i].icon);
        if (kinds[i].alert) sound_play(SOUND_ALERT);
        return;
    }
}

/* ---- USB devices ---- */

typedef enum { U_CAMERA, U_PRINTER, U_STORAGE, U_AUDIO, U_TABLET, U_INPUT, U_OTHER, U_KINDS } UsbKind;
static const char *const usb_kind_names[U_KINDS] = { "camera", "printer", "storage", "audio", "tablet", "input", "other" };
static unsigned usb_enabled = (1u << U_KINDS) - 1;   /* the kinds that are announced */
static bool usb_off;

/* usb: unset or "all" is everything, "0" nothing, else the kinds named. A name that is not a kind is said. */
static unsigned parse_kinds(const char *value, bool *none) {
    *none = false;
    if (!value || !*value || !strcmp(value, "all")) return (1u << U_KINDS) - 1;
    if (!strcmp(value, "0")) { *none = true; return 0; }
    unsigned mask = 0;
    char copy[128];
    snprintf(copy, sizeof copy, "%s", value);
    for (char *save, *tok = strtok_r(copy, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        int k;
        for (k = 0; k < U_KINDS; k++)
            if (!strcasecmp(tok, usb_kind_names[k])) break;
        if (k < U_KINDS) mask |= 1u << k;
        else fprintf(stderr, _("pacreact: usb: \"%s\" is not one of camera, printer, tablet, storage, audio, input, other\n"), tok);
    }
    if (!mask) { fprintf(stderr, _("pacreact: usb names no kind; announcing all\n")); return (1u << U_KINDS) - 1; }
    return mask;
}

/* A device that settled (pacreactd): "7 plugged storage SanDisk Ultra", "8 unplugged mouse". Said if its kind is asked for. */
static void usb_event(const char *event) {
    static const struct { const char *kind, *icon; UsbKind group; } kinds[] = {
        { "camera", "camera-web,camera-video,camera-photo", U_CAMERA }, { "printer", "printer", U_PRINTER },
        { "scanner", "scanner,document-scan,printer", U_PRINTER },
        { "storage", "drive-removable-media-usb,drive-removable-media", U_STORAGE }, { "audio", "audio-headset,audio-card", U_AUDIO },
        { "tablet", "input-tablet,input-mouse", U_TABLET }, { "keyboard", "input-keyboard", U_INPUT },
        { "mouse", "input-mouse", U_INPUT }, { "gamepad", "input-gaming,input-mouse", U_INPUT },
        { "other", "preferences-desktop-peripherals,drive-removable-media-usb", U_OTHER },
    };
    unsigned number;
    char direction[16], kind[16];
    int used = 0;
    if (sscanf(event, "%u %15s %15s %n", &number, direction, kind, &used) < 3) return;
    const char *name = event + used;
    bool plugged = !strcmp(direction, "plugged");
    for (size_t i = 0; i < sizeof kinds / sizeof *kinds; i++) {
        if (strcmp(kind, kinds[i].kind) || !(usb_enabled >> kinds[i].group & 1)) continue;
        char msg[160], icon[160];
        snprintf(msg, sizeof msg, plugged ? _("%s connected") : _("%s disconnected"), *name ? name : _("USB device"));
        if (plugged) {
            snprintf(icon, sizeof icon, "%s", kinds[i].icon);
        } else {   /* gone: the first name's "-off" glyph, when the Nerd Font has one (a theme passes over it) */
            int first = (int)strcspn(kinds[i].icon, ",");
            snprintf(icon, sizeof icon, "%.*s-off,%s", first, kinds[i].icon, kinds[i].icon);
        }
        hud_toast_icon(msg, LEVEL_INFO, SRC_USB_TOAST_MS, icon);
        sound_play(plugged ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED);
        return;
    }
}

/* usb.ID, bt.ID, jack.ID, input.ID: a device, and no key under it. */
static bool device_key(const char *key) {
    static const char *const spaces[] = { "usb", "bt", "jack", "input" };
    return daemon_device_key(key, spaces, sizeof spaces / sizeof *spaces);
}

/* What a device's connecting, or its going, is said as: the messages of each kind, fed what pacreactd says of it. */
static void announce_device(const char *key, bool plugged, const char *kind, const char *bus, const char *name) {
    char event[160];
    if (!strncmp(key, "input.", 6)) {   /* what is plugged into USB is said; a Bluetooth one is said by bt.ID, and the machine's own is no event */
        if (usb_off || strcmp(bus, "usb")) return;
        snprintf(event, sizeof event, "0 %s %s%s%s", plugged ? "plugged" : "unplugged", strcmp(kind, "touchpad") ? kind : "mouse", *name ? " " : "", name);
        usb_event(event);
    } else if (!strncmp(key, "usb.", 4)) {
        if (usb_off) return;
        snprintf(event, sizeof event, "0 %s %s%s%s", plugged ? "plugged" : "unplugged", kind, *name ? " " : "", name);
        usb_event(event);
    } else if (!strncmp(key, "bt.", 3)) {
        snprintf(event, sizeof event, "0 %s%s%s", plugged ? "connected" : "disconnected", *name ? " " : "", name);
        bluetooth_event(event);
    } else {
        snprintf(event, sizeof event, "%s %s", kind, plugged ? "plugged" : "unplugged");
        jack_event(event);
    }
}

/* A device key arrived (value non-NULL) or went (NULL). A new one is a connection, said after the snapshot; one that goes is said as it was. */
static void device_line(const char *key, char *value) {
    int slot = -1, free_slot = -1;
    for (int i = 0; i < MAX_DEVICES_SEEN; i++) {
        if (!strcmp(devices_there[i].key, key)) slot = i;
        if (!devices_there[i].key[0] && free_slot < 0) free_slot = i;
    }
    if (!value) {
        if (slot < 0) return;
        char kind[16], bus[12], name[64];
        snprintf(bus, sizeof bus, "%s", devices_there[slot].bus);
        snprintf(kind, sizeof kind, "%s", devices_there[slot].kind);
        snprintf(name, sizeof name, "%s", devices_there[slot].name);
        memset(&devices_there[slot], 0, sizeof devices_there[slot]);
        if (synced) announce_device(key, false, kind, bus, name);
        return;
    }
    const char *attrs = daemon_split_attrs(value);
    char kind[16], bus[12];
    daemon_attr(attrs, "kind", kind, sizeof kind);
    daemon_attr(attrs, "bus", bus, sizeof bus);
    if (slot >= 0) { snprintf(devices_there[slot].kind, sizeof devices_there[slot].kind, "%s", kind); return; }   /* a change of what it says, not a connection */
    if (free_slot < 0) return;
    snprintf(devices_there[free_slot].key, sizeof devices_there[free_slot].key, "%s", key);
    snprintf(devices_there[free_slot].kind, sizeof devices_there[free_slot].kind, "%s", kind);
    snprintf(devices_there[free_slot].bus, sizeof devices_there[free_slot].bus, "%s", bus);
    snprintf(devices_there[free_slot].name, sizeof devices_there[free_slot].name, "%s", value);
    if (synced) announce_device(key, true, kind, bus, value);
}

/* "event KEY KIND [ARG]": something that happened once, told to those who were listening. */
static void event_line(char *rest) {
    char *kind = strchr(rest, ' ');
    if (!kind) return;
    *kind++ = '\0';
    char *arg = strchr(kind, ' ');
    if (arg) *arg++ = '\0';
    if (!synced || strncmp(rest, "printer.", 8)) return;
    char text[160];
    snprintf(text, sizeof text, "0 %s%s%s", kind, arg ? " " : "", arg ? arg : "");
    printer_event(text);
}

/* One datagram of pacreactd: "key value", "key" (gone), "sync", "event KEY KIND [ARG]", or a reply (ok, error ...). */
static void line(char *text) {
    if (!strcmp(text, "sync")) { synced = true; return; }
    if (!strncmp(text, "event ", 6)) { event_line(text + 6); return; }
    char *value = strchr(text, ' ');
    if (!value) {   /* a key that is gone */
        if (device_key(text)) { device_line(text, NULL); return; }
        if (!strcmp(text, "net.device")) device[0] = '\0';
        else if (!strcmp(text, "audio.output")) output[0] = '\0';
        else if (!strcmp(text, "backlight.level")) backlight = -1;
        else if (!strcmp(text, "lock.caps")) locks_known[0] = -1;
        else if (!strcmp(text, "lock.num")) locks_known[1] = -1;
        else if (!strcmp(text, "net.probe")) probe_state[0] = '\0';
        else if (!strncmp(text, "disk.", 5)) {   /* a filesystem that was unmounted: it can be told anew */
            char name[24];
            snprintf(name, sizeof name, "%.23s", text + 5);
            char *dot = strchr(name, '.');
            if (dot) *dot = '\0';
            int slot = disk_slot(name, false);
            if (slot >= 0) memset(&disks[slot], 0, sizeof disks[slot]);
        }
        else if (!strncmp(text, "audio.mic.", 10)) forget_audio(1);
        else if (!strcmp(text, "audio.volume") || !strcmp(text, "audio.mute")) forget_audio(0);
        return;
    }
    if (!strchr(text, '.') || strchr(text, '.') > value) {   /* a reply, not a key: "error ..." is worth knowing */
        if (!strncmp(text, "error", 5)) fprintf(stderr, "pacreact: pacreactd: %s\n", text);
        return;
    }
    *value++ = '\0';
    if (device_key(text)) { device_line(text, value); return; }
    const char *attrs = daemon_split_attrs(value);
    Level level = attrs_level(attrs);
    bool by_device = daemon_has_attr(attrs, "cause", "device"), by_jack = daemon_has_attr(attrs, "cause", "jack");
    if (!strcmp(text, "power.battery")) battery(atoi(value), level);
    else if (!strcmp(text, "power.ac")) charger(!strcmp(value, "on"));
    else if (!strcmp(text, "mem.available")) available_mb = atol(value);
    else if (!strcmp(text, "mem.pressure")) memory(level);
    else if (!strcmp(text, "temp.cpu")) temperature(0, atoi(value), level);
    else if (!strcmp(text, "temp.ssd")) temperature(1, atoi(value), level);
    else if (!strcmp(text, "net.device")) snprintf(device, sizeof device, "%s", value);
    else if (!strcmp(text, "net.online")) network(!strcmp(value, "on"));
    else if (!strcmp(text, "audio.volume") || !strcmp(text, "audio.mic.volume") || !strcmp(text, "audio.mute") || !strcmp(text, "audio.mic.mute")) {
        int t = !strncmp(text, "audio.mic.", 10);
        if (strstr(text, "volume")) track[t].pct = atoi(value);
        else track[t].muted = !strncmp(value, "on", 2);
        track[t].dirty = true;
        track[t].device |= by_device;
    }
    else if (!strcmp(text, "backlight.level")) brightness(atoi(value));
    else if (!strcmp(text, "lock.caps") || !strcmp(text, "lock.num")) lock_key(text[5] == 'n', !strncmp(value, "on", 2));
    else if (!strcmp(text, "net.latency")) latency_ms = atol(value);
    else if (!strcmp(text, "net.probe.host")) snprintf(probe_host, sizeof probe_host, "%s", value);
    else if (!strcmp(text, "net.probe")) probe(value);
    else if (!strncmp(text, "disk.", 5)) {
        char name[24];
        snprintf(name, sizeof name, "%.23s", text + 5);
        char *dot = strchr(name, '.');
        const char *field = dot ? dot + 1 : NULL;
        if (dot) *dot = '\0';
        int slot = disk_slot(name, true);
        if (slot < 0) return;
        if (field && !strcmp(field, "mount")) snprintf(disks[slot].mount, sizeof disks[slot].mount, "%s", value);
        else if (field && !strcmp(field, "free")) disks[slot].avail = strtoull(value, NULL, 10);
        else if (!field) disk_level(slot, atoi(value), level);
    }
    else if (!strcmp(text, "audio.output")) audio_output(value, by_jack);
}

/* ---- what we tell it ---- */

/* Everything told, to say again to a daemon that has just started: it keeps what a program says only while
 * that program is connected. */
static struct { char key[32], value[64]; } published[MAX_PUBLISHED];
static int n_published;
static char owned[MAX_OWNED][16];       /* the namespaces claimed on this connection */
static int n_owned;

static bool send_text(const char *text) {
    if (sock < 0) return false;
    if (send(sock, text, strlen(text), MSG_NOSIGNAL | MSG_DONTWAIT) >= 0) return true;
    lost();   /* it cannot take it: start again, and say everything again when it is back */
    return false;
}

/* Claims the namespace of key unless it is claimed. */
static bool own(const char *key) {
    const char *dot = strchr(key, '.');
    size_t n = dot ? (size_t)(dot - key) : 0;
    if (!n || n >= sizeof owned[0]) return false;
    for (int i = 0; i < n_owned; i++)
        if (!strncmp(owned[i], key, n) && !owned[i][n]) return true;
    if (n_owned == MAX_OWNED) return false;
    char msg[40];
    snprintf(msg, sizeof msg, "own %.*s", (int)n, key);
    if (!send_text(msg)) return false;
    snprintf(owned[n_owned++], sizeof owned[0], "%.*s", (int)n, key);
    return true;
}

static void send_published(int i) {
    char msg[110];
    snprintf(msg, sizeof msg, "publish %s %s", published[i].key, published[i].value);
    if (own(published[i].key)) send_text(msg);
}

void daemon_publish(const char *key, const char *value) {
    int i = 0;
    while (i < n_published && strcmp(published[i].key, key)) i++;
    if (!value) {   /* gone */
        if (i == n_published) return;
        published[i] = published[--n_published];
        char msg[40];
        snprintf(msg, sizeof msg, "retract %s", key);
        send_text(msg);
        return;
    }
    if (i == n_published) {
        if (n_published == MAX_PUBLISHED || strlen(key) >= sizeof published[0].key || strlen(value) >= sizeof published[0].value) return;
        snprintf(published[i].key, sizeof published[i].key, "%s", key);
        n_published++;
    } else if (!strcmp(published[i].value, value)) {
        return;
    }
    snprintf(published[i].value, sizeof published[i].value, "%s", value);
    send_published(i);
}

/* ---- the connection ---- */

/* $XDG_RUNTIME_DIR is private to the user by definition. Without it the socket is in a private
 * directory of its own that pacreactd made (one that is not ours and private is refused). */
static bool socket_dir(char *dir, size_t n) {
    const char *run = getenv("XDG_RUNTIME_DIR");
    if (run && *run) return snprintf(dir, n, "%s", run) < (int)n;
    snprintf(dir, n, "/tmp/pacreactd-%u", (unsigned)geteuid());
    struct stat st;
    return lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == geteuid() && !(st.st_mode & 077);
}

static bool socket_path(struct sockaddr_un *a) {
    char dir[sizeof a->sun_path - 16];
    a->sun_family = AF_UNIX;
    return socket_dir(dir, sizeof dir) && snprintf(a->sun_path, sizeof a->sun_path, "%s/" SOCKET_NAME, dir) < (int)sizeof a->sun_path;
}

/* The daemon went, or was not there: what it said is forgotten, and it is waited for. Nothing runs on a timer:
 * the socket is renamed into place when the daemon listens, and inotify says so. */
static void lost(void) {
    if (sock >= 0) { loop_remove(sock); close(sock); sock = -1; }
    synced = false;   /* what it says when it is back is a snapshot again */
    n_owned = 0;
    forget_audio(0);
    forget_audio(1);
    output[0] = '\0';
    backlight = -1;
    locks_known[0] = locks_known[1] = -1;
    probe_state[0] = '\0';
    memset(disks, 0, sizeof disks);
    memset(devices_there, 0, sizeof devices_there);
}

static void on_readable(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[256];
    while (sock >= 0) {
        ssize_t n = recv(sock, buf, sizeof buf - 1, MSG_DONTWAIT);
        if (n < 0 && (errno == EAGAIN || errno == EINTR)) break;
        if (n <= 0) { lost(); return; }
        buf[n] = '\0';
        line(buf);
    }
    flush_audio();   /* what came in this burst, as one bar */
}

static void connect_daemon(void) {
    struct sockaddr_un a = {0};
    if (sock >= 0 || !socket_path(&a)) return;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return; }
    sock = fd;
    loop_add(sock, EPOLLIN, on_readable, NULL);
    if (!send_text(DAEMON_SUBSCRIPTION)) return;
    for (int i = 0; i < n_published && sock >= 0; i++) send_published(i);
}

/* The socket appeared in its directory: the daemon is listening. */
static void on_socket_dir(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t n = read(watch_fd, buf, sizeof buf);
    for (char *p = buf; n > 0 && p < buf + n;) {
        const struct inotify_event *e = (const struct inotify_event *)p;
        if (e->len && !strcmp(e->name, SOCKET_NAME)) connect_daemon();
        p += sizeof *e + e->len;
    }
}

void daemon_init(void) {
    char dir[256];
    usb_enabled = parse_kinds(pac_setting("usb"), &usb_off);
    const char *print = pac_setting("print");   /* unset or 1: all of it; 0: nothing; done: not the start of a job */
    print_off = print && !strcmp(print, "0");
    print_start = !(print && !strcmp(print, "done"));
    /* Watch first, then connect: a daemon that starts in between is then not missed. */
    watch_fd = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
    if (watch_fd >= 0 && socket_dir(dir, sizeof dir) && inotify_add_watch(watch_fd, dir, IN_MOVED_TO | IN_CREATE) >= 0)
        loop_add(watch_fd, EPOLLIN, on_socket_dir, NULL);
    connect_daemon();
}
