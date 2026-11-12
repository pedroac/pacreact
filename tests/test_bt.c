/* Bluetooth connect/disconnect: against a private dbus-daemon, with a forked fake BlueZ that owns
 * org.bluez, answers Alias reads and emits real PropertiesChanged signals on command. Skipped when
 * dbus-daemon is not installed. src_bt.c is included for its statics. */
#include "../src/hud.h"
#include "../src/loop.h"
#include "../src/sound.h"
#include <assert.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char toasts[16][160];
static int ntoasts, removed, registered;
static Level last_level;

static SoundKind last_sound = SOUND_KINDS;
void sound_play(SoundKind k) { last_sound = k; }
static char toast_icon[96];   /* the status icon of the last toast, for the tests that care */
void hud_toast_icon(const char *t, Level l, int ms, const char *icon) {
    extern void hud_toast(const char *, Level, int);
    snprintf(toast_icon, sizeof toast_icon, "%s", icon ? icon : "");
    hud_toast(t, l, ms);
}
void hud_toast(const char *t, Level l, int ms) { (void)ms; snprintf(toasts[ntoasts++ % 16], sizeof toasts[0], "%s", t); last_level = l; }
int loop_add(int fd, uint32_t ev, LoopFn fn, void *ctx) { (void)fd; (void)ev; (void)fn; (void)ctx; registered++; return 0; }
int loop_remove(int fd) { (void)fd; removed++; return 0; }

#include "../src/src_bt.c"

#define DEVICE "/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF"

/* ---- the fake BlueZ (a child process) ---- */

static char fake_alias[256] = "WH-1000XM4";

static int get_alias(sd_bus *b, const char *path, const char *iface, const char *prop,
                     sd_bus_message *reply, void *ud, sd_bus_error *err) {
    (void)b; (void)path; (void)iface; (void)prop; (void)ud; (void)err;
    return sd_bus_message_append(reply, "s", fake_alias);
}

static const sd_bus_vtable device_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_PROPERTY("Alias", "s", get_alias, 0, SD_BUS_VTABLE_PROPERTY_CONST),
    SD_BUS_VTABLE_END
};

/* PropertiesChanged on `path` for `iface`, with Connected (if >= 0) and an unrelated property. */
static void emit(sd_bus *b, const char *path, const char *iface, int connected, bool with_other) {
    sd_bus_message *m = NULL;
    assert(sd_bus_message_new_signal(b, &m, path, "org.freedesktop.DBus.Properties", "PropertiesChanged") >= 0);
    assert(sd_bus_message_append(m, "s", iface) >= 0);
    assert(sd_bus_message_open_container(m, 'a', "{sv}") >= 0);
    if (with_other) assert(sd_bus_message_append(m, "{sv}", "RSSI", "n", (int16_t)-60) >= 0);
    if (connected >= 0) assert(sd_bus_message_append(m, "{sv}", "Connected", "b", connected) >= 0);
    assert(sd_bus_message_close_container(m) >= 0);
    assert(sd_bus_message_append(m, "as", 0) >= 0);
    assert(sd_bus_send(b, m, NULL) >= 0);
    sd_bus_message_unref(m);
    sd_bus_flush(b);
}

static void fake_bluez(int cmd_fd, int ready_fd) {
    sd_bus *b = NULL, *impostor = NULL;
    if (sd_bus_open_system(&b) < 0 || sd_bus_request_name(b, "org.bluez", 0) < 0) _exit(1);
    if (sd_bus_add_object_vtable(b, NULL, DEVICE, "org.bluez.Device1", device_vtable, NULL) < 0) _exit(1);
    if (sd_bus_open_system(&impostor) < 0) _exit(1);   /* a connection that does not own org.bluez */
    (void)!write(ready_fd, "ready\n", 6);

    char buf[256];
    size_t have = 0;
    for (;;) {
        struct pollfd p[2] = { { .fd = sd_bus_get_fd(b), .events = POLLIN }, { .fd = cmd_fd, .events = POLLIN } };
        sd_bus_process(b, NULL);
        if (poll(p, 2, 100) <= 0) continue;
        if (p[1].revents & POLLIN) {
            ssize_t n = read(cmd_fd, buf + have, sizeof buf - 1 - have);
            if (n <= 0) _exit(0);
            have += n;
            buf[have] = '\0';
            char *nl;
            while ((nl = strchr(buf, '\n'))) {
                *nl = '\0';
                if (!strcmp(buf, "connect")) emit(b, DEVICE, "org.bluez.Device1", 1, false);
                else if (!strcmp(buf, "disconnect")) emit(b, DEVICE, "org.bluez.Device1", 0, false);
                else if (!strcmp(buf, "both")) emit(b, DEVICE, "org.bluez.Device1", 1, true);
                else if (!strcmp(buf, "rssi")) emit(b, DEVICE, "org.bluez.Device1", -1, true);
                else if (!strcmp(buf, "adapter")) emit(b, "/org/bluez/hci0", "org.bluez.Adapter1", 1, false);
                else if (!strcmp(buf, "unknown")) emit(b, "/org/bluez/hci0/dev_NO_SUCH", "org.bluez.Device1", 1, false);
                else if (!strcmp(buf, "spoof")) emit(impostor, DEVICE, "org.bluez.Device1", 1, false);
                else if (!strncmp(buf, "alias ", 6)) snprintf(fake_alias, sizeof fake_alias, "%s", buf + 6);
                memmove(buf, nl + 1, have - (size_t)(nl + 1 - buf) + 1);
                have -= (size_t)(nl + 1 - buf);
            }
        }
        sd_bus_process(b, NULL);
    }
}

/* ---- driving it ---- */

static int to_fake = -1;

static void command(const char *cmd) {
    char line[64];
    snprintf(line, sizeof line, "%s\n", cmd);
    assert(write(to_fake, line, strlen(line)) == (ssize_t)strlen(line));
}

/* Processes our bus for up to `ms`; returns whether a toast arrived. */
static bool wait_for_toast(int before, int ms) {
    for (int waited = 0; waited < ms; waited += 50) {
        while (sd_bus_process(bus, NULL) > 0) {}
        if (ntoasts > before) return true;
        sd_bus_wait(bus, 50 * 1000);
    }
    return false;
}

int main(void) {
    if (system("command -v dbus-daemon >/dev/null 2>&1") != 0) {
        puts("bt: (skipped: dbus-daemon is not installed)");
        return 0;
    }
    FILE *p = popen("dbus-daemon --session --fork --print-address --print-pid", "r");
    assert(p);
    char address[512] = "", pidline[32] = "";
    assert(fgets(address, sizeof address, p) && fgets(pidline, sizeof pidline, p));
    pclose(p);
    address[strcspn(address, "\n")] = '\0';
    pid_t daemon_pid = (pid_t)atoi(pidline);
    assert(daemon_pid > 1 && address[0]);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);     /* the "system" bus is the private one */

    /* a system bus that is not there: quietly nothing */
    setenv("DBUS_SYSTEM_BUS_ADDRESS", "unix:path=/nonexistent/bus", 1);
    registered = 0;
    bt_init();
    assert(bus == NULL && registered == 0);

    setenv("PACHUD_BLUETOOTH", "0", 1);
    setenv("DBUS_SYSTEM_BUS_ADDRESS", address, 1);
    bt_init();
    assert(bus == NULL && registered == 0);
    unsetenv("PACHUD_BLUETOOTH");

    int cmd[2], ready[2];
    assert(pipe(cmd) == 0 && pipe(ready) == 0);
    pid_t fake = fork();
    assert(fake >= 0);
    if (fake == 0) {
        close(cmd[1]);
        close(ready[0]);
        fake_bluez(cmd[0], ready[1]);
    }
    close(cmd[0]);
    close(ready[1]);
    to_fake = cmd[1];
    char r[8];
    assert(read(ready[0], r, 6) == 6);                  /* the fake owns org.bluez now */

    bt_init();
    assert(bus != NULL && registered == 1);

    /* connecting and disconnecting, with the name BlueZ gives the device */
    int before = ntoasts;
    command("connect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 connected") && last_level == LEVEL_INFO);
    assert(last_sound == SOUND_DEVICE_ADDED);
    assert(!strncmp(toast_icon, "bluetooth-active", 16));
    before = ntoasts;
    command("disconnect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 disconnected"));
    assert(last_sound == SOUND_DEVICE_REMOVED);
    assert(!strncmp(toast_icon, "bluetooth-disconnected", 22));

    /* Connected together with other changed properties still counts */
    before = ntoasts;
    command("both");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: WH-1000XM4 connected"));

    /* the name is read each time */
    command("alias Kitchen speaker");
    before = ntoasts;
    command("disconnect");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: Kitchen speaker disconnected"));

    /* a device BlueZ cannot name still gets a message */
    before = ntoasts;
    command("unknown");
    assert(wait_for_toast(before, 3000));
    assert(!strcmp(toasts[(ntoasts - 1) % 16], "Bluetooth: ? connected"));

    /* things that are not a connection: other properties, other interfaces, other senders */
    before = ntoasts;
    command("rssi");
    command("adapter");
    command("spoof");
    assert(!wait_for_toast(before, 800));
    assert(ntoasts == before);

    /* and it is still listening afterwards */
    command("connect");
    assert(wait_for_toast(before, 3000));

    /* the system bus going away: the connection is released */
    kill(fake, SIGKILL);
    waitpid(fake, NULL, 0);
    kill(daemon_pid, SIGTERM);
    for (int i = 0; i < 40 && bus; i++) {
        removed = 0;
        on_bus(NULL, 0);
        if (bus) usleep(50000);
    }
    assert(bus == NULL && removed == 1);

    close(cmd[1]);
    close(ready[0]);
    puts("bt: ok");
    return 0;
}
