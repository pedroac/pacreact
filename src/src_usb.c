#include "hud.h"
#include "setting.h"
#include "i18n.h"
#include "loop.h"
#include "sound.h"
#include "sources.h"
#include "util.h"
#include <dirent.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include "tunables.h"

/* USB devices the user plugs in: a webcam, a printer, a drawing tablet, a drive, a headset, a keyboard.
 * The kernel announces each one as a uevent (the same socket as the charger's), but with nothing a person
 * would call a name: that is in sysfs, which is read a moment later, once the device's interfaces are
 * there. Nothing is announced for what is found at start-up, nor for hubs, Bluetooth adapters and devices
 * the kernel says are built in (a laptop's own webcam), which are not things the user connects.
 * usb = 0 turns this off; a comma separated list (camera,printer,tablet,storage,audio,input,other)
 * says which kinds to announce, and unset means all. */


typedef enum { U_CAMERA, U_PRINTER, U_STORAGE, U_AUDIO, U_TABLET, U_INPUT, U_OTHER, U_KINDS } UsbKind;

static const char *const kind_names[U_KINDS] = { "camera", "printer", "storage", "audio", "tablet", "input", "other" };

static const char *sysroot = "/sys";   /* a test points it at a tree of its own */
static unsigned enabled = (1u << U_KINDS) - 1;
static bool off;
static int tfd = -1;

/* ---- what a device is, from sysfs ---- */

typedef struct {
    char name[96];
    UsbKind kind;
    const char *icon;
} Found;

/* An attribute's text, without the newline and the padding some devices put after it. */
static bool read_text(const char *dir, const char *attr, char *out, size_t n) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    bool ok = fgets(out, (int)n, f) != NULL;
    fclose(f);
    if (!ok) return false;
    size_t len = strcspn(out, "\n");
    while (len && (out[len - 1] == ' ' || out[len - 1] == '\t' || out[len - 1] == '\r')) len--;
    out[len] = '\0';
    return true;
}

/* A device describes itself, so its strings are not to be trusted: no control characters in what is shown. */
static void clean(char *s) {
    for (; *s; s++)
        if ((unsigned char)*s < 0x20 || *s == 0x7f) *s = ' ';
}

/* "Wacom Co.,Ltd." is "Wacom": the company's name without its legal form. */
static void short_vendor(char *v) {
    char *cut = strchr(v, ',');
    if (cut) *cut = '\0';
    static const char *const forms[] = { " Co.", " Co ", " Inc", " Corp", " Ltd", " GmbH", " LLC", " Limited", " Technology" };
    for (size_t i = 0; i < sizeof forms / sizeof *forms; i++) {
        char *p = strcasestr(v, forms[i]);
        if (p && p != v) *p = '\0';
    }
    size_t len = strlen(v);
    while (len && v[len - 1] == ' ') v[--len] = '\0';
}

static void make_name(const char *vendor_in, const char *product, char *out, size_t n) {
    char vendor[64];
    snprintf(vendor, sizeof vendor, "%s", vendor_in);
    clean(vendor);
    short_vendor(vendor);
    char first[32];
    snprintf(first, sizeof first, "%.*s", (int)strcspn(vendor, " "), vendor);
    /* the brand in front, unless the product already says it */
    if (*product && *vendor && !(*first && strcasestr(product, first))) {
        if (snprintf(out, n, "%s %s", vendor, product) < 0) out[0] = '\0';   /* cut to fit, as intended */
    }
    else if (*product) snprintf(out, n, "%s", product);
    else if (*vendor) snprintf(out, n, "%s", vendor);
    else snprintf(out, n, "%s", _("USB device"));
    clean(out);
}

/* Bit `bit` of a capabilities bitmap, as sysfs writes it: hexadecimal words of a long each, the highest
 * first, separated by spaces. */
static bool bitmap_has(const char *text, int bit) {
    const int width = (int)(sizeof(unsigned long) * 8);
    int words = 0;
    for (const char *p = text; *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        words++;
        while (*p && *p != ' ') p++;
    }
    int want = words - 1 - bit / width;     /* the index of that word counted from the left */
    if (want < 0 || want >= words) return false;
    const char *p = text;
    for (int i = 0; i <= want; i++) {
        while (*p == ' ') p++;
        if (i == want) return (strtoul(p, NULL, 16) >> (bit % width)) & 1;
        while (*p && *p != ' ') p++;
    }
    return false;
}

#define BTN_TOOL_PEN_CODE 0x140

/* Whether the input devices in `input_dir` (".../input", holding input0, input1...) include a pen. */
static bool pen_in(const char *input_dir) {
    DIR *in = opendir(input_dir);
    if (!in) return false;
    struct dirent *i;
    bool pen = false;
    while (!pen && (i = readdir(in))) {
        if (strncmp(i->d_name, "input", 5)) continue;
        char caps[PATH_MAX], text[512];
        snprintf(caps, sizeof caps, "%s/%s/capabilities", input_dir, i->d_name);
        if (read_text(caps, "key", text, sizeof text) && bitmap_has(text, BTN_TOOL_PEN_CODE)) pen = true;
    }
    closedir(in);
    return pen;
}

/* Whether an interface has an input device with a pen on it: that is what a drawing tablet is, whoever made
 * it. The input devices hang under the interface's HID node, or sometimes straight under the interface. */
static bool has_pen(const char *iface_dir) {
    char input[PATH_MAX];
    if (snprintf(input, sizeof input, "%s/input", iface_dir) < (int)sizeof input && pen_in(input)) return true;
    DIR *d = opendir(iface_dir);
    if (!d) return false;
    struct dirent *e;
    bool pen = false;
    while (!pen && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "input")) continue;
        if (snprintf(input, sizeof input, "%s/%s/input", iface_dir, e->d_name) < (int)sizeof input) pen = pen_in(input);
    }
    closedir(d);
    return pen;
}

/* Drawing tablet makers, for when the input node is not there yet to ask. */
static bool tablet_vendor(const char *vid) {
    static const char *const ids[] = { "056a", "256c", "28bd", "5543", "172f" };   /* Wacom, Huion, XP-Pen, UC-Logic, Waltop */
    for (size_t i = 0; i < sizeof ids / sizeof *ids; i++)
        if (!strcasecmp(vid, ids[i])) return true;
    return false;
}

enum { C_AUDIO = 1 << 0, C_HID = 1 << 1, C_PRINTER = 1 << 2, C_STORAGE = 1 << 3, C_HUB = 1 << 4, C_VIDEO = 1 << 5,
       C_WIRELESS = 1 << 6, C_OTHER = 1 << 7 };

/* What the device at syspath is, or false for what is not announced: hubs, Bluetooth adapters, and
 * devices that are built in. */
static bool classify(const char *syspath, Found *out) {
    char text[128], removable[16] = "", vid[16] = "", vendor[64] = "", product[64] = "";
    read_text(syspath, "removable", removable, sizeof removable);
    if (!strcmp(removable, "fixed")) return false;

    unsigned classes = 0;
    bool keyboard = false, mouse = false, pen = false;
    if (read_text(syspath, "bDeviceClass", text, sizeof text) && strtol(text, NULL, 16) == 0x09) return false;

    const char *base = strrchr(syspath, '/');
    base = base ? base + 1 : syspath;
    size_t blen = strlen(base);
    DIR *d = opendir(syspath);
    if (!d) return false;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, base, blen) || e->d_name[blen] != ':') continue;   /* an interface: "1-2:1.0" */
        char idir[PATH_MAX];
        if (snprintf(idir, sizeof idir, "%s/%s", syspath, e->d_name) >= (int)sizeof idir) continue;
        if (!read_text(idir, "bInterfaceClass", text, sizeof text)) continue;
        long cls = strtol(text, NULL, 16);
        switch (cls) {
        case 0x01: classes |= C_AUDIO; break;
        case 0x03:
            classes |= C_HID;
            if (read_text(idir, "bInterfaceProtocol", text, sizeof text)) {   /* boot protocols: 1 keyboard, 2 mouse */
                long proto = strtol(text, NULL, 16);
                keyboard |= proto == 1;
                mouse |= proto == 2;
            }
            if (!pen && has_pen(idir)) pen = true;
            break;
        case 0x07: classes |= C_PRINTER; break;
        case 0x08: classes |= C_STORAGE; break;
        case 0x09: classes |= C_HUB; break;
        case 0x0e: classes |= C_VIDEO; break;
        case 0xe0: classes |= C_WIRELESS; break;
        default: classes |= C_OTHER; break;
        }
    }
    closedir(d);
    if (classes & C_HUB && !(classes & ~C_HUB)) return false;
    if (classes & C_WIRELESS && !(classes & ~C_WIRELESS)) return false;    /* a Bluetooth adapter: its devices are reported by bluez */

    read_text(syspath, "idVendor", vid, sizeof vid);
    read_text(syspath, "manufacturer", vendor, sizeof vendor);
    read_text(syspath, "product", product, sizeof product);
    clean(product);
    make_name(vendor, product, out->name, sizeof out->name);

    if (classes & C_VIDEO) { out->kind = U_CAMERA; out->icon = "camera-web,camera-video,camera-photo"; }
    else if (classes & C_PRINTER) { out->kind = U_PRINTER; out->icon = "printer"; }
    else if (classes & C_STORAGE) { out->kind = U_STORAGE; out->icon = "drive-removable-media-usb,drive-removable-media"; }
    else if (classes & C_AUDIO) { out->kind = U_AUDIO; out->icon = "audio-headset,audio-card"; }
    else if (classes & C_HID && (pen || tablet_vendor(vid))) { out->kind = U_TABLET; out->icon = "input-tablet,input-mouse"; }
    else if (classes & C_HID) {
        out->kind = U_INPUT;
        out->icon = keyboard ? "input-keyboard" : mouse ? "input-mouse" : "input-gaming,input-mouse";
    } else { out->kind = U_OTHER; out->icon = "preferences-desktop-peripherals,drive-removable-media-usb"; }
    return true;
}

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
            if (!strcasecmp(tok, kind_names[k])) break;
        if (k < U_KINDS) mask |= 1u << k;
        else fprintf(stderr, _("pacreact: usb: \"%s\" is not one of camera, printer, tablet, storage, audio, input, other\n"), tok);
    }
    if (!mask) { fprintf(stderr, _("pacreact: usb names no kind; announcing all\n")); return (1u << U_KINDS) - 1; }
    return mask;
}

/* ---- the devices, and what is waiting to be said about them ---- */

typedef enum {
    D_FREE,
    D_ADDING,     /* appeared; its names are read once things have settled */
    D_SHOWN,      /* here, and announced */
    D_QUIET,      /* here, and not announced: a hub, a built-in device, a kind that is turned off */
    D_REMOVING,   /* gone; the announcement waits to see whether it comes straight back */
} DevState;

typedef struct {
    DevState state;
    char path[192];       /* DEVPATH: "/devices/pci0000:00/.../usb1/1-11" */
    char product[48];     /* PRODUCT= of the uevent: vendor, product and revision, to tell a return from a swap */
    long long due;        /* when the wait is over, for ADDING and REMOVING */
    Found found;
} Dev;

static Dev devs[MAX_DEVICES];

/* "56a/378/100" (a uevent: hex without leading zeros, then the revision) and "056a/0378" (read from sysfs) are
 * the same device: compare the vendor and product numbers. */
static bool same_product(const char *a, const char *b) {
    char *ea, *eb;
    unsigned long va = strtoul(a, &ea, 16), vb = strtoul(b, &eb, 16);
    if (*ea != '/' || *eb != '/') return false;
    return va == vb && strtoul(ea + 1, NULL, 16) == strtoul(eb + 1, NULL, 16);
}

static Dev *find(const char *path) {
    for (int i = 0; i < MAX_DEVICES; i++)
        if (devs[i].state != D_FREE && !strcmp(devs[i].path, path)) return &devs[i];
    return NULL;
}

static Dev *take(const char *path, const char *product) {
    for (int i = 0; i < MAX_DEVICES; i++) {
        if (devs[i].state != D_FREE) continue;
        memset(&devs[i], 0, sizeof devs[i]);
        snprintf(devs[i].path, sizeof devs[i].path, "%s", path);
        snprintf(devs[i].product, sizeof devs[i].product, "%s", product ? product : "");
        return &devs[i];
    }
    return NULL;
}

static void rearm(void) {
    if (tfd < 0) return;
    long long first = 0;
    for (int i = 0; i < MAX_DEVICES; i++)
        if ((devs[i].state == D_ADDING || devs[i].state == D_REMOVING) && (!first || devs[i].due < first)) first = devs[i].due;
    struct itimerspec its = {0};
    if (first) {
        long long wait = first - now_ms();
        if (wait < 1) wait = 1;
        its.it_value.tv_sec = wait / 1000;
        its.it_value.tv_nsec = (wait % 1000) * 1000000L;
    }
    timerfd_settime(tfd, 0, &its, NULL);
}

static void say(const Dev *d, bool connected) {
    char msg[160];
    snprintf(msg, sizeof msg, connected ? _("%s connected") : _("%s disconnected"), d->found.name);
    /* Gone: the first name's "-off" glyph, when the Nerd Font has one (a theme passes over it) */
    char icon[160];
    if (connected) {
        snprintf(icon, sizeof icon, "%s", d->found.icon);
    } else {
        int first = (int)strcspn(d->found.icon, ",");
        snprintf(icon, sizeof icon, "%.*s-off,%s", first, d->found.icon, d->found.icon);
    }
    hud_toast_icon(msg, LEVEL_INFO, SRC_USB_TOAST_MS, icon);
    sound_play(connected ? SOUND_DEVICE_ADDED : SOUND_DEVICE_REMOVED);
}

/* A device has been here long enough to be read: what is it, and is it for announcing? */
static void settle_adding(Dev *d, bool announce) {
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s%s", sysroot, d->path);
    if (!classify(full, &d->found) || !(enabled >> d->found.kind & 1)) {
        d->state = D_QUIET;
        return;
    }
    d->state = D_SHOWN;
    if (announce) say(d, true);
}

/* Acts on whatever has waited long enough. */
static void settle(void) {
    long long now = now_ms();
    for (int i = 0; i < MAX_DEVICES; i++) {
        Dev *d = &devs[i];
        if (d->due > now) continue;
        if (d->state == D_ADDING) {
            settle_adding(d, true);
        } else if (d->state == D_REMOVING) {
            say(d, false);
            d->state = D_FREE;
        }
    }
    rearm();
}

static void on_timer(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    uint64_t n;
    if (read(tfd, &n, sizeof n) > 0) settle();
}

static void device_added(const char *path, const char *product) {
    Dev *d = find(path);
    if (d && d->state == D_REMOVING) {
        if (same_product(d->product, product)) {   /* the same device straight back (a resume, a loose cable): nothing happened */
            d->state = D_SHOWN;
            rearm();
            return;
        }
        say(d, false);                        /* a different one in the same place: the first is gone for good */
        d->state = D_FREE;
        d = NULL;
    }
    if (d) return;                            /* already known: an event twice */
    d = take(path, product);
    if (!d) return;
    d->state = D_ADDING;
    d->due = now_ms() + SRC_USB_SETTLE_MS;
    rearm();
}

static void device_removed(const char *path) {
    Dev *d = find(path);
    if (!d) return;
    if (d->state == D_SHOWN) {
        d->state = D_REMOVING;
        d->due = now_ms() + SRC_USB_SETTLE_MS;
    } else {
        d->state = D_FREE;                    /* never announced: nothing to take back */
    }
    rearm();
}

/* A kernel uevent is "action@path\0KEY=VALUE\0KEY=VALUE\0...". */
void usb_uevent(const char *msg, size_t len) {
    if (off) return;
    const char *end = msg + len;
    const char *action = NULL, *subsystem = NULL, *type = NULL, *path = NULL, *product = "";
    for (const char *p = msg + strlen(msg) + 1; p < end; p += strlen(p) + 1) {
        if (!strncmp(p, "ACTION=", 7)) action = p + 7;
        else if (!strncmp(p, "SUBSYSTEM=", 10)) subsystem = p + 10;
        else if (!strncmp(p, "DEVTYPE=", 8)) type = p + 8;
        else if (!strncmp(p, "DEVPATH=", 8)) path = p + 8;
        else if (!strncmp(p, "PRODUCT=", 8)) product = p + 8;
    }
    if (!action || !subsystem || strcmp(subsystem, "usb") || !type || strcmp(type, "usb_device") || !path) return;
    if (strlen(path) >= sizeof devs[0].path) return;
    if (!strcmp(action, "add")) device_added(path, product);
    else if (!strcmp(action, "remove")) device_removed(path);
}

/* The devices that are here at start-up are known, so that taking one out is announced, but not announced themselves. */
static void scan(void) {
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/bus/usb/devices", sysroot);
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    size_t root_len = strlen(sysroot);
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;   /* interfaces are looked at with their device */
        char link[PATH_MAX], real[PATH_MAX];
        if (snprintf(link, sizeof link, "%s/%s", dir, e->d_name) >= (int)sizeof link) continue;
        if (!realpath(link, real) || strncmp(real, sysroot, root_len) || strlen(real + root_len) >= sizeof devs[0].path) continue;
        char product[48] = "", vid[16] = "", pid[16] = "";
        read_text(real, "idVendor", vid, sizeof vid);
        read_text(real, "idProduct", pid, sizeof pid);
        snprintf(product, sizeof product, "%s/%s", vid, pid);
        Dev *dev = take(real + root_len, product);
        if (!dev) break;
        settle_adding(dev, false);
    }
    closedir(d);
}

void usb_init(void) {
    const char *value = pac_setting("usb");
    enabled = parse_kinds(value, &off);
    if (off) return;
    tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (tfd >= 0) loop_add(tfd, EPOLLIN, on_timer, NULL);
    scan();
}
