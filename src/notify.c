#include "notify.h"
#include "hud.h"
#include "loop.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>

#define PATH "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"

#define MIN_MS 1500
#define MAX_MS 30000  /* nothing can click a card away, so "never expire" is capped */
#define LOW_MS 3000
#define NORMAL_MS 6000
#define CRITICAL_MS 15000

enum { REASON_CLOSED_BY_CALL = 3 };

static sd_bus *bus;
static uint32_t next_id = 1;

static int method_caps(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    /* No actions and no markup rendering: only plain summary and body. */
    return sd_bus_reply_method_return(m, "as", 1, "body");
}

static int method_info(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    return sd_bus_reply_method_return(m, "ssss", "pachud", "pachud", "0.1", "1.2");
}

static void emit_closed(uint32_t id, int reason) {
    if (bus) sd_bus_emit_signal(bus, PATH, IFACE, "NotificationClosed", "uu", id, (uint32_t)reason);
}

int notify_timeout_ms(int urgency, int32_t expire) {
    int ms = urgency >= 2 ? CRITICAL_MS : urgency == 0 ? LOW_MS : NORMAL_MS;
    if (expire > 0) ms = expire < MIN_MS ? MIN_MS : expire;
    else if (expire == 0 && urgency < 2) ms = MAX_MS;  /* "never": as long as we dare */
    return ms > MAX_MS ? MAX_MS : ms;
}

static int method_notify(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    const char *app, *icon, *summary, *body;
    uint32_t replaces;
    int32_t expire;
    int urgency = 1;
    const char *image_path = NULL;

    int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
    if (r < 0) return r;
    r = sd_bus_message_skip(m, "as");  /* actions: not supported */
    if (r < 0) return r;

    r = sd_bus_message_enter_container(m, 'a', "{sv}");
    if (r < 0) return r;
    while ((r = sd_bus_message_enter_container(m, 'e', "sv")) > 0) {
        const char *key, *contents;
        char type;
        r = sd_bus_message_read(m, "s", &key);
        if (r < 0) return r;
        r = sd_bus_message_peek_type(m, &type, &contents);
        if (r < 0) return r;

        if (!strcmp(key, "urgency") && contents && !strcmp(contents, "y")) {
            uint8_t u;
            r = sd_bus_message_read(m, "v", "y", &u);
            urgency = u;
        } else if (!strcmp(key, "image-path") && contents && !strcmp(contents, "s")) {
            r = sd_bus_message_read(m, "v", "s", &image_path);
        } else {
            r = sd_bus_message_skip(m, "v");
        }
        if (r < 0) return r;
        r = sd_bus_message_exit_container(m);
        if (r < 0) return r;
    }
    if (r < 0) return r;
    r = sd_bus_message_exit_container(m);
    if (r < 0) return r;
    r = sd_bus_message_read(m, "i", &expire);
    if (r < 0) return r;

    int ms = notify_timeout_ms(urgency, expire);

    /* A replacement keeps its id; an id we no longer hold simply becomes a new card. */
    uint32_t id = replaces ? replaces : next_id++;
    if (next_id == 0) next_id = 1;

    hud_card(id, summary, body, image_path && *image_path ? image_path : icon,
             urgency >= 2 ? LEVEL_CRIT : LEVEL_INFO, ms);
    (void)app;
    return sd_bus_reply_method_return(m, "u", id);
}

static int method_close(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    uint32_t id;
    int r = sd_bus_message_read(m, "u", &id);
    if (r < 0) return r;
    if (hud_remove(id)) emit_closed(id, REASON_CLOSED_BY_CALL);
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("GetCapabilities", "", "as", method_caps, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetServerInformation", "", "ssss", method_info, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_SIGNAL("ActionInvoked", "us", 0),
    SD_BUS_VTABLE_END
};

static void on_bus(void *ctx, uint32_t ev) {
    (void)ctx; (void)ev;
    int r;
    while ((r = sd_bus_process(bus, NULL)) > 0) {}
    if (r < 0) {  /* the bus went away */
        loop_remove(sd_bus_get_fd(bus));
        sd_bus_flush_close_unref(bus);
        bus = NULL;
    }
}

void notify_init(void) {
    if (sd_bus_open_user(&bus) < 0) { bus = NULL; return; }

    int r = sd_bus_add_object_vtable(bus, NULL, PATH, IFACE, vtable, NULL);
    if (r >= 0) r = sd_bus_request_name(bus, IFACE, 0);  /* 0: fail rather than queue */
    if (r < 0) {
        if (r == -EEXIST)
            fputs("pachud: another notification daemon owns org.freedesktop.Notifications "
                  "(stop mako/dunst/...); notifications disabled\n", stderr);
        else
            fprintf(stderr, "pachud: notifications disabled: %s\n", strerror(-r));
        sd_bus_flush_close_unref(bus);
        bus = NULL;
        return;
    }

    hud_on_closed(emit_closed);
    loop_add(sd_bus_get_fd(bus), EPOLLIN, on_bus, NULL);
    while (sd_bus_process(bus, NULL) > 0) {}  /* anything already queued */
}
