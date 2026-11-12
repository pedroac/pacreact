#include "notify.h"
#include "dnd.h"
#include "history.h"
#include "hud.h"
#include "i18n.h"
#include "icon.h"
#include "image.h"
#include "loop.h"
#include "sound.h"
#include "version.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <systemd/sd-bus.h>

#define PATH "/org/freedesktop/Notifications"
#define IFACE "org.freedesktop.Notifications"

#define MIN_MS 1500
#define LOW_MS 3000
#define NORMAL_MS 6000

enum { REASON_CLOSED_BY_CALL = 3, REASON_UNDEFINED = 4 };

/* One "default" action plus the ones the card can show, as key/label pairs */
#define MAX_ACTION_STRINGS ((HUD_MAX_ACTIONS + 1) * 2)

static sd_bus *bus;
static uint32_t next_id = 1;

static int method_caps(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    /* No markup rendering: plain summary and body. Actions are offered as numbered hints and
     * invoked with `pachud action N`, since the HUD cannot be clicked. */
    return sd_bus_reply_method_return(m, "as", 4, "actions", "body", "icon-static", "sound");
}

static int method_info(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    return sd_bus_reply_method_return(m, "ssss", "pachud", "pachud", PACHUD_VERSION, "1.2");
}

static void emit_closed(uint32_t id, int reason) {
    history_close(id, reason);
    if (bus) sd_bus_emit_signal(bus, PATH, IFACE, "NotificationClosed", "uu", id, (uint32_t)reason);
}

/* A click on a card is the spec's "default" action, for apps that offered one. */
static void on_card_click(uint32_t id, const char *token) {
    if (!bus) return;
    /* The token first, so the app can use it to take focus as it handles the action */
    if (token && *token) sd_bus_emit_signal(bus, PATH, IFACE, "ActivationToken", "us", id, token);
    sd_bus_emit_signal(bus, PATH, IFACE, "ActionInvoked", "us", id, "default");
}

/* The spec: -1 leaves it to the server (low and normal get a sane timeout; critical ones "should
 * not automatically expire"), 0 never expires, anything else is a number of milliseconds. */
int notify_timeout_ms(int urgency, int32_t expire) {
    if (expire > 0) return expire < MIN_MS ? MIN_MS : expire;
    if (expire == 0 || urgency >= 2) return HUD_NEVER;
    return urgency == 0 ? LOW_MS : NORMAL_MS;
}

/* An image-data hint: (width, height, rowstride, has_alpha, bits, channels, pixels). Returns a
 * negative errno for a malformed message; *ok says whether the pixels were usable. */
static int read_image(sd_bus_message *m, uint8_t *out, bool *ok) {
    int32_t w, h, stride, bits, channels;
    int alpha;
    const void *data;
    size_t len;

    int r = sd_bus_message_enter_container(m, 'v', "(iiibiiay)");
    if (r < 0) return r;
    r = sd_bus_message_enter_container(m, 'r', "iiibiiay");
    if (r < 0) return r;
    r = sd_bus_message_read(m, "iiibii", &w, &h, &stride, &alpha, &bits, &channels);
    if (r < 0) return r;
    r = sd_bus_message_read_array(m, 'y', &data, &len);
    if (r < 0) return r;
    r = sd_bus_message_exit_container(m);
    if (r < 0) return r;
    r = sd_bus_message_exit_container(m);
    if (r < 0) return r;

    *ok = image_from_hint(w, h, stride, alpha, bits, channels, data, len, out);
    return 0;
}

static int method_notify(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud; (void)err;
    const char *app, *icon, *summary, *body;
    uint32_t replaces;
    int32_t expire;
    int urgency = 1;
    const char *image_path = NULL, *sound_file = NULL, *sound_name = NULL;
    int suppress_sound = 0, resident = 0, transient = 0;
    static uint8_t image[IMAGE_BYTES];
    bool has_image = false;

    int r = sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body);
    if (r < 0) return r;

    /* Actions: key, label, key, label... Extra ones beyond what a card can show are dropped. */
    const char *actions[MAX_ACTION_STRINGS];
    int n_actions = 0;
    r = sd_bus_message_enter_container(m, 'a', "s");
    if (r < 0) return r;
    const char *str;
    while ((r = sd_bus_message_read(m, "s", &str)) > 0)
        if (n_actions < MAX_ACTION_STRINGS) actions[n_actions++] = str;
    if (r < 0) return r;
    r = sd_bus_message_exit_container(m);
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
        } else if ((!strcmp(key, "image-data") || !strcmp(key, "image_data") || !strcmp(key, "icon_data")) &&
                   contents && !strcmp(contents, "(iiibiiay)")) {
            r = read_image(m, image, &has_image);  /* the 1.2, 1.1 and 1.0 spellings */
        } else if (!strcmp(key, "sound-file") && contents && !strcmp(contents, "s")) {
            r = sd_bus_message_read(m, "v", "s", &sound_file);
        } else if (!strcmp(key, "sound-name") && contents && !strcmp(contents, "s")) {
            r = sd_bus_message_read(m, "v", "s", &sound_name);
        } else if (!strcmp(key, "transient") && contents && !strcmp(contents, "b")) {
            r = sd_bus_message_read(m, "v", "b", &transient);
        } else if (!strcmp(key, "resident") && contents && !strcmp(contents, "b")) {
            r = sd_bus_message_read(m, "v", "b", &resident);
        } else if (!strcmp(key, "suppress-sound") && contents && !strcmp(contents, "b")) {
            r = sd_bus_message_read(m, "v", "b", &suppress_sound);
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

    /* Do-not-disturb: the app still gets an id, but nothing is shown or played. A card it
     * replaces is withdrawn, since what it showed is out of date. */
    bool shown = !dnd_suppress(urgency);
    if (!shown && replaces) hud_remove(id);
    /* The history keeps what apps sent, except what they marked transient. */
    if (!transient) history_record(id, app, summary, body, urgency, shown ? HIST_OPEN : HIST_SKIPPED);
    if (shown) {
        /* Icon: raw pixels first, then the image-path hint, then app_icon (a path or a theme name) */
        char icon_file[512] = "";
        if (!has_image && !(image_path && icon_find(image_path, icon_file, sizeof icon_file)))
            icon_find(icon, icon_file, sizeof icon_file);

        HudCard card = {
            .id = id, .summary = summary, .body = body, .icon_path = icon_file,
            .image = has_image ? image : NULL,
            .level = urgency >= 2 ? LEVEL_CRIT : LEVEL_INFO, .timeout_ms = ms,
            .actions = actions, .n_actions = n_actions, .resident = resident,
        };
        hud_card(&card);
    }
    if (shown && !suppress_sound) {  /* what the app asked for, else the configured default (if any) */
        if (sound_file && *sound_file) sound_play_spec(sound_file);
        else if (sound_name && *sound_name) sound_play_spec(sound_name);
        else sound_play(SOUND_NOTIFY);
    }
    r = sd_bus_reply_method_return(m, "u", id);
    /* After the reply, so the app has the id the signal is about: nothing will ever be shown
     * for it, so tell it the notification is closed rather than leave it waiting. */
    if (!shown) emit_closed(id, REASON_UNDEFINED);
    return r;
}

bool notify_invoke_action(int n, char *reply, size_t reply_n) {
    uint32_t id;
    const char *key = hud_action_key(n, &id);
    if (!key) { snprintf(reply, reply_n, "%s", _("no such action on screen")); return false; }
    if (!bus) { snprintf(reply, reply_n, "%s", _("notifications unavailable")); return false; }
    /* Tell the app first (the key lives in the card), then close it, unless the card is resident:
     * closing sends NotificationClosed with the "dismissed by the user" reason. */
    sd_bus_emit_signal(bus, PATH, IFACE, "ActionInvoked", "us", id, key);
    hud_action_invoked(id);
    snprintf(reply, reply_n, "ok");
    return true;
}

static int method_close(sd_bus_message *m, void *ud, sd_bus_error *err) {
    (void)ud;
    uint32_t id;
    int r = sd_bus_message_read(m, "u", &id);
    if (r < 0) return r;
    /* The spec: the method emits NotificationClosed, and a notification that no longer exists
     * gets an error back. */
    if (!hud_remove(id))
        return sd_bus_error_setf(err, SD_BUS_ERROR_INVALID_ARGS, "no notification with id %u", id);
    emit_closed(id, REASON_CLOSED_BY_CALL);
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
    SD_BUS_SIGNAL("ActivationToken", "us", 0),
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
    hud_on_click(on_card_click);
    loop_add(sd_bus_get_fd(bus), EPOLLIN, on_bus, NULL);
    while (sd_bus_process(bus, NULL) > 0) {}  /* anything already queued */
}
