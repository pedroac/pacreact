/* USB devices: what a device is (from a sysfs tree the test builds), what it is called, and what is said
 * about it and when. src_usb.c is included for its statics; the HUD, the loop and the clock are stubbed. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static long long fake_ms = 1000000;
static int fake_clock_gettime(clockid_t c, struct timespec *ts) {
    (void)c;
    ts->tv_sec = fake_ms / 1000;
    ts->tv_nsec = (fake_ms % 1000) * 1000000L;
    return 0;
}
#define clock_gettime fake_clock_gettime

static char toasts[16][160], icons[16][160];
static int ntoasts;
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    (void)l; (void)ms;
    assert(ntoasts < 16);
    snprintf(toasts[ntoasts], sizeof toasts[0], "%s", t);
    snprintf(icons[ntoasts], sizeof icons[0], "%s", icon ? icon : "");
    ntoasts++;
}
void hud_toast(const char *t, Level l, int ms) { hud_toast_icon(t, l, ms, NULL); }
static SoundKind last_sound = SOUND_KINDS;
static int nsounds;
void sound_play(SoundKind k) { last_sound = k; nsounds++; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; return 0; }

#include "../src/src_usb.c"

/* ---- a sysfs of the test's own ---- */

static char root[200];

static void put(const char *dir, const char *attr, const char *value) {
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/%s", dir, attr);
    FILE *f = fopen(path, "w");
    assert(f);
    fprintf(f, "%s\n", value);
    fclose(f);
}

static void mk(const char *path) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "mkdir -p %s", path);
    assert(system(cmd) == 0);
}

typedef struct { const char *cls, *proto, *pen_key; } Ifc;

/* A device "1-N" under root/devices/pci/usb1, with interfaces; returns its DEVPATH */
static const char *make_device(const char *port, const char *removable, const char *devclass, const char *vid,
                               const char *pid, const char *vendor, const char *product, const Ifc *ifcs, int n) {
    static char devpath[192];
    snprintf(devpath, sizeof devpath, "/devices/pci/usb1/%s", port);
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s%s", root, devpath);
    mk(dir);
    put(dir, "removable", removable);
    put(dir, "bDeviceClass", devclass);
    put(dir, "idVendor", vid);
    put(dir, "idProduct", pid);
    if (vendor) put(dir, "manufacturer", vendor);
    if (product) put(dir, "product", product);
    for (int i = 0; i < n; i++) {
        char idir[PATH_MAX];
        assert(snprintf(idir, sizeof idir, "%s/%s:1.%d", dir, port, i) < (int)sizeof idir);
        mk(idir);
        put(idir, "bInterfaceClass", ifcs[i].cls);
        put(idir, "bInterfaceProtocol", ifcs[i].proto);
        if (ifcs[i].pen_key) {      /* the HID node and its input device, as the kernel lays them out */
            char cdir[PATH_MAX];
            assert(snprintf(cdir, sizeof cdir, "%s/0003:1234:5678.0001/input/input7/capabilities", idir) < (int)sizeof cdir);
            mk(cdir);
            put(cdir, "key", ifcs[i].pen_key);
        }
    }
    char link[PATH_MAX], target[PATH_MAX];
    snprintf(link, sizeof link, "%s/bus/usb/devices", root);
    mk(link);
    snprintf(link, sizeof link, "%s/bus/usb/devices/%s", root, port);
    snprintf(target, sizeof target, "../../../devices/pci/usb1/%s", port);
    unlink(link);
    assert(symlink(target, link) == 0);
    return devpath;
}

/* A kernel uevent for a USB device, built the way the kernel sends it */
static size_t uevent(char *buf, const char *action, const char *devpath, const char *product, const char *devtype,
                     const char *subsystem) {
    size_t n = 0;
    n += sprintf(buf + n, "%s@%s", action, devpath) + 1;
    n += sprintf(buf + n, "ACTION=%s", action) + 1;
    n += sprintf(buf + n, "DEVPATH=%s", devpath) + 1;
    n += sprintf(buf + n, "SUBSYSTEM=%s", subsystem) + 1;
    n += sprintf(buf + n, "DEVTYPE=%s", devtype) + 1;
    n += sprintf(buf + n, "PRODUCT=%s", product) + 1;
    return n;
}

static void event(const char *action, const char *devpath, const char *product) {
    char buf[1024];
    size_t n = uevent(buf, action, devpath, product, "usb_device", "usb");
    usb_uevent(buf, n);
}

static void forget(void) {
    memset(devs, 0, sizeof devs);
    ntoasts = nsounds = 0;
    enabled = (1u << U_KINDS) - 1;
    off = false;
}

/* ---- pure parts ---- */

static void test_bitmap(void) {
    /* BTN_TOOL_PEN is bit 320: the sixth word from the right. A Wacom pen, a mouse, a pad, a keyboard */
    assert(bitmap_has("1e01 0 0 0 0 0", BTN_TOOL_PEN_CODE));
    assert(!bitmap_has("1f0000 0 0 0 0", BTN_TOOL_PEN_CODE));
    assert(!bitmap_has("800 f 0 0 0 0", BTN_TOOL_PEN_CODE));
    assert(!bitmap_has("1000000000007 ff9f207ac14057ff febeffdfffefffff fffffffffffffffe", BTN_TOOL_PEN_CODE));
    assert(bitmap_has("ff 0 0 0 0 0", 0x140 + 7) && !bitmap_has("ff 0 0 0 0 0", 0x140 + 8));
    assert(!bitmap_has("", BTN_TOOL_PEN_CODE) && !bitmap_has("0", BTN_TOOL_PEN_CODE));
}

static void test_names(void) {
    char out[96];
    make_name("Wacom Co.,Ltd.", "Intuos BT M", out, sizeof out);
    assert(!strcmp(out, "Wacom Intuos BT M"));                /* the company without its legal form */
    make_name("HP", "ENVY 4520 series", out, sizeof out);
    assert(!strcmp(out, "HP ENVY 4520 series"));
    make_name("Logitech", "Logitech C920", out, sizeof out);
    assert(!strcmp(out, "Logitech C920"));                    /* not said twice */
    make_name("", "USB Keyboard", out, sizeof out);
    assert(!strcmp(out, "USB Keyboard"));
    make_name("SanDisk Corp.", "", out, sizeof out);
    assert(!strcmp(out, "SanDisk"));
    make_name("", "", out, sizeof out);
    assert(!strcmp(out, "USB device"));
    make_name("Evil", "name\x1b[31m\nred", out, sizeof out);   /* a device chooses its own strings */
    assert(!strchr(out, '\n') && !strchr(out, 0x1b));

    assert(same_product("56a/378/100", "056a/0378"));
    assert(!same_product("56a/378/100", "056a/0379"));
    assert(!same_product("", "056a/0378"));
}

static void test_kinds_setting(void) {
    bool none;
    unsigned all = (1u << U_KINDS) - 1;
    assert(parse_kinds(NULL, &none) == all && !none);
    assert(parse_kinds("", &none) == all && parse_kinds("all", &none) == all);
    assert(parse_kinds("0", &none) == 0 && none);
    assert(parse_kinds("camera,printer", &none) == ((1u << U_CAMERA) | (1u << U_PRINTER)) && !none);
    assert(parse_kinds("Tablet, storage", &none) == (1u << U_TABLET));              /* a name with a space in front is not a kind: said, and left out */
    assert(parse_kinds("camera,bogus", &none) == (1u << U_CAMERA));                 /* a bad one is said and left out */
    assert(parse_kinds("bogus", &none) == all);                                     /* nothing usable: everything */
}

/* ---- what a device is ---- */

static Found what(const char *devpath, bool *announced) {
    char full[PATH_MAX];
    snprintf(full, sizeof full, "%s%s", root, devpath);
    Found f = {0};
    *announced = classify(full, &f);
    return f;
}

static void test_classify(void) {
    bool yes;
    Found f;

    /* a webcam has a microphone too: still a webcam */
    const Ifc cam[] = { { "0e", "00", NULL }, { "0e", "00", NULL }, { "01", "00", NULL }, { "01", "00", NULL } };
    f = what(make_device("1-1", "removable", "ef", "046d", "082d", "Logitech", "HD Pro Webcam C920", cam, 4), &yes);
    assert(yes && f.kind == U_CAMERA && !strcmp(f.name, "Logitech HD Pro Webcam C920") && strstr(f.icon, "camera"));

    const Ifc printer[] = { { "07", "04", NULL }, { "ff", "01", NULL }, { "07", "04", NULL } };
    f = what(make_device("1-2", "removable", "00", "03f0", "d711", "HP", "ENVY 4520 series", printer, 3), &yes);
    assert(yes && f.kind == U_PRINTER && !strcmp(f.name, "HP ENVY 4520 series") && !strcmp(f.icon, "printer"));

    const Ifc drive[] = { { "08", "50", NULL } };
    f = what(make_device("1-3", "removable", "00", "0781", "5581", "SanDisk", "Ultra", drive, 1), &yes);
    assert(yes && f.kind == U_STORAGE && strstr(f.icon, "drive-removable"));

    const Ifc headset[] = { { "01", "00", NULL }, { "01", "00", NULL }, { "03", "00", NULL } };
    f = what(make_device("1-4", "removable", "00", "1b3f", "0001", NULL, "USB Headset", headset, 3), &yes);
    assert(yes && f.kind == U_AUDIO);

    /* a drawing tablet is a HID device like a mouse: told apart by its pen, whoever makes it */
    const Ifc tablet[] = { { "03", "00", "1e01 0 0 0 0 0" } };
    f = what(make_device("1-5", "removable", "00", "1234", "5678", "Acme", "Sketch Pad", tablet, 1), &yes);
    assert(yes && f.kind == U_TABLET && !strcmp(f.icon, "input-tablet,input-mouse"));
    /* ... or by its maker, when the input device has not appeared yet */
    const Ifc bare[] = { { "03", "00", NULL } };
    f = what(make_device("1-6", "removable", "00", "056a", "0378", "Wacom Co.,Ltd.", "Intuos BT M", bare, 1), &yes);
    assert(yes && f.kind == U_TABLET && !strcmp(f.name, "Wacom Intuos BT M"));

    const Ifc kbd[] = { { "03", "01", NULL }, { "03", "00", NULL } };
    f = what(make_device("1-7", "removable", "00", "413c", "2113", NULL, "USB Keyboard", kbd, 2), &yes);
    assert(yes && f.kind == U_INPUT && !strcmp(f.icon, "input-keyboard"));
    const Ifc mouse[] = { { "03", "02", NULL } };
    f = what(make_device("1-8", "removable", "00", "30fa", "0400", NULL, "USB Optical Mouse", mouse, 1), &yes);
    assert(yes && f.kind == U_INPUT && !strcmp(f.icon, "input-mouse"));

    const Ifc phone[] = { { "ff", "42", NULL } };
    f = what(make_device("1-9", "unknown", "00", "18d1", "4ee1", "Google", "Pixel", phone, 1), &yes);
    assert(yes && f.kind == U_OTHER);                         /* "unknown" whether removable: announced */

    /* what is not announced: hubs, Bluetooth adapters, devices built in */
    const Ifc hub[] = { { "09", "00", NULL } };
    what(make_device("1-10", "removable", "09", "05e3", "0610", NULL, "USB2.0 Hub", hub, 1), &yes);
    assert(!yes);
    const Ifc bt[] = { { "e0", "01", NULL }, { "e0", "01", NULL } };
    what(make_device("1-11", "removable", "e0", "8087", "0026", NULL, NULL, bt, 2), &yes);
    assert(!yes);
    what(make_device("1-12", "fixed", "00", "04f2", "b6d0", "Chicony", "Integrated Camera", cam, 4), &yes);
    assert(!yes);                                             /* a laptop's own webcam */
    what("/devices/pci/usb1/gone", &yes);
    assert(!yes);                                             /* nothing there */
}

/* ---- what is said, and when ---- */

static void test_announcements(void) {
    forget();
    const Ifc printer[] = { { "07", "04", NULL } };
    const char *path = make_device("2-1", "removable", "00", "03f0", "d711", "HP", "ENVY 4520 series", printer, 1);

    event("add", path, "3f0/d711/100");
    assert(ntoasts == 0);                                     /* its names are not there yet */
    fake_ms += SRC_USB_SETTLE_MS - 1;
    settle();
    assert(ntoasts == 0);
    fake_ms += 1;
    settle();
    assert(ntoasts == 1 && !strcmp(toasts[0], "HP ENVY 4520 series connected") && !strcmp(icons[0], "printer"));
    assert(nsounds == 1 && last_sound == SOUND_DEVICE_ADDED);

    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 2 && !strcmp(toasts[1], "HP ENVY 4520 series disconnected") && last_sound == SOUND_DEVICE_REMOVED);
    assert(!find(path));                                      /* the slot is free again */

    /* the same event twice is one device */
    forget();
    event("add", path, "3f0/d711/100");
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1);

    /* out and straight back (a resume, a loose cable): nothing to say */
    forget();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    ntoasts = 0;
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS / 2;
    settle();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && find(path)->state == D_SHOWN);
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1 && strstr(toasts[0], "disconnected"));  /* and it can still be taken out */

    /* a different device in the same port at once: the first is gone for good, then the new one appears */
    forget();
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    ntoasts = 0;
    event("remove", path, "3f0/d711/100");
    const Ifc drive[] = { { "08", "50", NULL } };
    make_device("2-1", "removable", "00", "0781", "5581", "SanDisk", "Ultra", drive, 1);
    event("add", path, "781/5581/100");
    assert(ntoasts == 1 && !strcmp(toasts[0], "HP ENVY 4520 series disconnected"));
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 2 && !strcmp(toasts[1], "SanDisk Ultra connected") && !strcmp(icons[1], "drive-removable-media-usb,drive-removable-media"));

    /* out again before it was read: it was never announced, so there is nothing to take back */
    forget();
    event("add", path, "3f0/d711/100");
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(path));

    /* hubs and the like are known, never announced, and leave quietly */
    forget();
    const Ifc hub[] = { { "09", "00", NULL } };
    const char *hpath = make_device("2-2", "removable", "09", "05e3", "0610", NULL, "USB2.0 Hub", hub, 1);
    event("add", hpath, "5e3/610/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 0 && find(hpath)->state == D_QUIET);
    event("remove", hpath, "5e3/610/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(hpath));

    /* only the kinds asked for */
    forget();
    enabled = (1u << U_CAMERA) | (1u << U_TABLET);
    event("add", path, "3f0/d711/100");                       /* a printer: not asked for */
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 0);
    event("remove", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0);

    /* other events are not ours: interfaces, other subsystems, other actions */
    forget();
    char buf[1024];
    size_t n = uevent(buf, "add", path, "3f0/d711/100", "usb_interface", "usb");
    usb_uevent(buf, n);
    n = uevent(buf, "add", path, "3f0/d711/100", "usb_device", "block");
    usb_uevent(buf, n);
    n = uevent(buf, "change", path, "3f0/d711/100", "usb_device", "usb");
    usb_uevent(buf, n);
    n = uevent(buf, "bind", path, "3f0/d711/100", "usb_device", "usb");
    usb_uevent(buf, n);
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(path));

    /* turned off */
    forget();
    off = true;
    event("add", path, "3f0/d711/100");
    fake_ms += SRC_USB_SETTLE_MS * 2;
    settle();
    assert(ntoasts == 0 && !find(path));
    off = false;

    /* a full table drops the newcomer instead of crashing */
    forget();
    for (int i = 0; i < MAX_DEVICES; i++) {
        char p[64];
        snprintf(p, sizeof p, "/devices/pci/usb1/fill-%d", i);
        event("add", p, "1/2/3");
    }
    event("add", path, "3f0/d711/100");
    assert(!find(path));
    forget();
}

/* ---- start-up: what is already there is known, and not announced ---- */

static void test_start_up(void) {
    forget();
    const Ifc mouse[] = { { "03", "02", NULL } };
    const char *mpath = make_device("3-1", "removable", "00", "30fa", "0400", NULL, "USB Optical Mouse", mouse, 1);
    scan();
    assert(ntoasts == 0 && find(mpath) && find(mpath)->state == D_SHOWN);
    assert(!strcmp(find(mpath)->found.name, "USB Optical Mouse"));

    /* so taking it out is announced, though it was never announced coming in */
    event("remove", mpath, "30fa/400/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 1 && !strcmp(toasts[0], "USB Optical Mouse disconnected") && !strcmp(icons[0], "input-mouse-off,input-mouse"));
    forget();

    /* and the same device is recognised coming back, whichever way the number is written */
    scan();
    assert(find(mpath));
    event("remove", mpath, "30fa/400/100");
    fake_ms += SRC_USB_SETTLE_MS / 2;
    event("add", mpath, "30fa/400/100");
    fake_ms += SRC_USB_SETTLE_MS;
    settle();
    assert(ntoasts == 0 && find(mpath)->state == D_SHOWN);
}

int main(void) {
    snprintf(root, sizeof root, "/tmp/pacreact-usb-XXXXXX");
    assert(mkdtemp(root));
    char *real = realpath(root, NULL);                       /* the tree is found through its real path */
    assert(real && strlen(real) < sizeof root);
    snprintf(root, sizeof root, "%s", real);
    free(real);
    sysroot = root;

    test_bitmap();
    test_names();
    test_kinds_setting();
    test_classify();
    test_announcements();
    test_start_up();

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    assert(system(cmd) == 0);
    puts("usb: ok");
    return 0;
}
