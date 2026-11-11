#include "hud.h"
#include "i18n.h"
#include "loop.h"
#include "sources.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>

/* Bluetooth devices connecting and disconnecting. The kernel offers nothing unprivileged for
 * this, so it listens to BlueZ's PropertiesChanged signals on the system bus (the bus
 * connection is one more fd in the loop: nothing to poll). It only works while bluetoothd runs;
 * if it starts later the signals simply begin. PACHUD_BLUETOOTH=0 turns it off. */

#define TOAST_MS 3000

static sd_bus *bus;

/* The device's name from BlueZ; the caller frees it. */
static char *device_alias(const char *path) {
    char *alias = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    if (sd_bus_get_property_string(bus, "org.bluez", path, "org.bluez.Device1", "Alias", &err, &alias) < 0) {
        sd_bus_error_free(&err);
        err = SD_BUS_ERROR_NULL;
        if (sd_bus_get_property_string(bus, "org.bluez", path, "org.bluez.Device1", "Name", &err, &alias) < 0)
            alias = NULL;
    }
    sd_bus_error_free(&err);
    return alias;
}

/* The same alias lookup, replaceable so the message can be tested without a bus. */
static char *(*alias_lookup)(const char *path) = device_alias;

static void announce(bool connected, const char *name) {
    char msg[160];
    snprintf(msg, sizeof msg, connected ? _("Bluetooth: %s connected") : _("Bluetooth: %s disconnected"),
             name && *name ? name : "?");
    hud_toast(msg, LEVEL_INFO, TOAST_MS);
}

/* org.freedesktop.DBus.Properties.PropertiesChanged(interface, changed, invalidated) of a device:
 * only the Connected property is of interest. */
static int on_changed(sd_bus_message *m, void *userdata, sd_bus_error *err) {
    (void)userdata; (void)err;
    const char *iface;
    if (sd_bus_message_read(m, "s", &iface) < 0 || strcmp(iface, "org.bluez.Device1") != 0) return 0;

    int connected = -1;
    if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0) return 0;
    int r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key, *contents;
        char type;
        if (sd_bus_message_read(m, "s", &key) < 0 || sd_bus_message_peek_type(m, &type, &contents) < 0) return 0;
        if (!strcmp(key, "Connected") && contents && !strcmp(contents, "b")) {
            int value;
            if (sd_bus_message_read(m, "v", "b", &value) < 0) return 0;
            connected = value != 0;
        } else if (sd_bus_message_skip(m, "v") < 0) {
            return 0;
        }
        if (sd_bus_message_exit_container(m) < 0) return 0;
    }
    if (connected < 0) return 0;     /* some other property of the device changed */

    const char *path = sd_bus_message_get_path(m);
    char *name = path ? alias_lookup(path) : NULL;
    announce(connected, name);
    free(name);
    return 0;
}

static void on_bus(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) {   /* the system bus went away */
        loop_remove(sd_bus_get_fd(bus));
        sd_bus_flush_close_unref(bus);
        bus = NULL;
    }
}

void bt_init(void) {
    const char *off = getenv("PACHUD_BLUETOOTH");
    if (off && !strcmp(off, "0")) return;
    if (sd_bus_open_system(&bus) < 0) { bus = NULL; return; }
    int r = sd_bus_add_match(bus, NULL,
        "type='signal',sender='org.bluez',interface='org.freedesktop.DBus.Properties',"
        "member='PropertiesChanged',arg0='org.bluez.Device1'", on_changed, NULL);
    if (r < 0) {
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        return;
    }
    loop_add(sd_bus_get_fd(bus), EPOLLIN, on_bus, NULL);
    while (sd_bus_process(bus, NULL) > 0) {}
}
