#ifndef PACREACT_TUNABLES_H
#define PACREACT_TUNABLES_H
/* Everything in pacreact that a person may want to change: sizes, limits, timings and defaults. Each can
 * be set when building, without editing a file:
 *
 *     make CFLAGS="-O2 -DNAME=value"
 *
 * Build every file with the same values. A value that would make the code unsafe or useless is
 * refused by the checks at the end of this file, at build time. */

/* ---- activity.c ---- */
/* Idle default min (minutes). The reason for this default is not written down yet. */
#ifndef IDLE_DEFAULT_MIN
#define IDLE_DEFAULT_MIN 5
#endif
/* Break default min (minutes). The reason for this default is not written down yet. */
#ifndef BREAK_DEFAULT_MIN
#define BREAK_DEFAULT_MIN 15
#endif

/* ---- clock.c ---- */
/* Minutes between the times the clock shows itself, counted from the hour: 30 is at :00 and :30. */
#ifndef CLOCK_EVERY_DEFAULT_MIN
#define CLOCK_EVERY_DEFAULT_MIN 30
#endif

/* ---- alerts.c ---- */
/* Max sources. The reason for this default is not written down yet. */
#ifndef MAX_SOURCES
#define MAX_SOURCES 8
#endif
/* Default grace s (seconds). The reason for this default is not written down yet. */
#ifndef DEFAULT_GRACE_S
#define DEFAULT_GRACE_S 60
#endif
/* Max grace s (seconds). The reason for this default is not written down yet. */
#ifndef MAX_GRACE_S
#define MAX_GRACE_S 3600
#endif
/* Repeat window s (seconds). The reason for this default is not written down yet. */
#ifndef REPEAT_WINDOW_S
#define REPEAT_WINDOW_S (30 * 60)
#endif

/* ---- display.c ---- */
/* From the screen's edge, in pixels; margin */
#ifndef DEFAULT_MARGIN
#define DEFAULT_MARGIN 16
#endif
/* Margin max. The reason for this default is not written down yet. */
#ifndef MARGIN_MAX
#define MARGIN_MAX 200
#endif
/* Default fade ms (milliseconds). The reason for this default is not written down yet. */
#ifndef DEFAULT_FADE_MS
#define DEFAULT_FADE_MS 120
#endif
/* Max outputs. The reason for this default is not written down yet. */
#ifndef MAX_OUTPUTS
#define MAX_OUTPUTS 8
#endif
/* The least space between the deck and the stack for them to count as apart */
#ifndef DECK_GAP
#define DECK_GAP 16
#endif

/* ---- display.h ---- */
/* Click rects max. The reason for this default is not written down yet. */
#ifndef CLICK_RECTS_MAX
#define CLICK_RECTS_MAX 8
#endif

/* ---- dnd.c ---- */
/* Note ms (milliseconds). The reason for this default is not written down yet. */
#ifndef NOTE_MS
#define NOTE_MS 2500
#endif

/* ---- history.c ---- */
/* App max. The reason for this default is not written down yet. */
#ifndef APP_MAX
#define APP_MAX 64
#endif
/* Summary max. The reason for this default is not written down yet. */
#ifndef SUMMARY_MAX
#define SUMMARY_MAX 256
#endif
/* Body max. The reason for this default is not written down yet. */
#ifndef BODY_MAX
#define BODY_MAX 512
#endif
/* Shown default. The reason for this default is not written down yet. */
#ifndef SHOWN_DEFAULT
#define SHOWN_DEFAULT 10
#endif
/* Bytes of each listed in a line */
#ifndef SUMMARY_SHOWN
#define SUMMARY_SHOWN 80
#endif
/* Body shown. The reason for this default is not written down yet. */
#ifndef BODY_SHOWN
#define BODY_SHOWN 100
#endif

/* ---- history.h ---- */
/* The last notifications, in RAM only: a fixed ring that overwrites its oldest entry, so memory never grows and nothing is written to disk. Entries hold text only (no icon pixels). `pacreact history` lists them; `pacreact history clear` wipes the ring. */
#ifndef HISTORY_MAX
#define HISTORY_MAX 64
#endif

/* ---- hud.c ---- */
/* Osd timeout ms (milliseconds). The reason for this default is not written down yet. */
#ifndef OSD_TIMEOUT_MS
#define OSD_TIMEOUT_MS 1000
#endif
/* Clock osd ms (milliseconds). The reason for this default is not written down yet. */
#ifndef CLOCK_OSD_MS
#define CLOCK_OSD_MS 2500
#endif
/* Stack max. The reason for this default is not written down yet. */
#ifndef STACK_MAX
#define STACK_MAX 4
#endif
/* Deck max. The reason for this default is not written down yet. */
#ifndef DECK_MAX
#define DECK_MAX 3
#endif
/* Queue max. The reason for this default is not written down yet. */
#ifndef QUEUE_MAX
#define QUEUE_MAX 8
#endif
/* Slider ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SLIDER_MS
#define SLIDER_MS 1500
#endif
/* Reading a long notification. The pointer over a card holds it (its timeout waits) and, after a moment, opens it to show more of its body; `pacreact expand` does the same from the keyboard, for as long as it is not toggled again. When a card stops being held it stays at least this long. */
#ifndef HOVER_DELAY_MS
#define HOVER_DELAY_MS 250
#endif
/* Hold grace ms (milliseconds). The reason for this default is not written down yet. */
#ifndef HOLD_GRACE_MS
#define HOLD_GRACE_MS 2000
#endif
/* The compositor always answers; one that does not must not block clicks */
#ifndef TOKEN_WAIT_MS
#define TOKEN_WAIT_MS 2000
#endif

/* ---- hud.h ---- */
/* Hud max actions. The reason for this default is not written down yet. */
#ifndef HUD_MAX_ACTIONS
#define HUD_MAX_ACTIONS 3
#endif

/* ---- icon.c ---- */
/* Cache. The reason for this default is not written down yet. */
#ifndef ICON_CACHE
#define ICON_CACHE 16
#endif

/* ---- image.c ---- */
/* Beyond this it is not an icon */
#ifndef MAX_DIM
#define MAX_DIM 4096
#endif

/* ---- image.h ---- */
/* Notification icons sent as raw pixels (the image-data hint) are scaled down once, when they arrive, to a fixed square ARGB buffer, so a card never holds more than this. */
#ifndef IMAGE_PX
#define IMAGE_PX 64
#endif
/* Image bytes. The reason for this default is not written down yet. */
#ifndef IMAGE_BYTES
#define IMAGE_BYTES (IMAGE_PX * IMAGE_PX * 4)
#endif

/* ---- main.c ---- */
/* Default timeout ms (milliseconds). The reason for this default is not written down yet. */
#ifndef DEFAULT_TIMEOUT_MS
#define DEFAULT_TIMEOUT_MS 3000
#endif
/* The most words a client command line is rebuilt from: a fixed array, not one sized by argc. */
#ifndef CLIENT_ARGV_MAX
#define CLIENT_ARGV_MAX 64
#endif

/* ---- notify.c ---- */
/* Min ms (milliseconds). The reason for this default is not written down yet. */
#ifndef MIN_MS
#define MIN_MS 1500
#endif
/* Low ms (milliseconds). The reason for this default is not written down yet. */
#ifndef LOW_MS
#define LOW_MS 3000
#endif
/* Normal ms (milliseconds). The reason for this default is not written down yet. */
#ifndef NORMAL_MS
#define NORMAL_MS 6000
#endif
/* One "default" action plus the ones the card can show, as key/label pairs */
#ifndef MAX_ACTION_STRINGS
#define MAX_ACTION_STRINGS ((HUD_MAX_ACTIONS + 1) * 2)
#endif

/* ---- paint.c ---- */
/* Toast lines. The reason for this default is not written down yet. */
#ifndef TOAST_LINES
#define TOAST_LINES 3
#endif
/* Card body lines. The reason for this default is not written down yet. */
#ifndef CARD_BODY_LINES
#define CARD_BODY_LINES 4
#endif
/* Card body lines expanded. The reason for this default is not written down yet. */
#ifndef CARD_BODY_LINES_EXPANDED
#define CARD_BODY_LINES_EXPANDED 20
#endif

/* ---- paint.h ---- */
/* Paint font min (minutes). The reason for this default is not written down yet. */
#ifndef PAINT_FONT_MIN
#define PAINT_FONT_MIN 10
#endif
/* Paint font max. The reason for this default is not written down yet. */
#ifndef PAINT_FONT_MAX
#define PAINT_FONT_MAX 48
#endif
/* Paint width min (minutes). The reason for this default is not written down yet. */
#ifndef PAINT_WIDTH_MIN
#define PAINT_WIDTH_MIN 200
#endif
/* Paint width max. The reason for this default is not written down yet. */
#ifndef PAINT_WIDTH_MAX
#define PAINT_WIDTH_MAX 1200
#endif
/* The size of the smallest text on the HUD (the actions on a card), in pixels at the default 16 px
 * font: what text_size_min and text_size_max hold between their limits. */
#ifndef PAINT_SMALLEST_TEXT
#define PAINT_SMALLEST_TEXT 12
#endif
/* The width of the items in the stack at the default font, in pixels: three fit across 1204. */
#ifndef PAINT_ITEM_DESIGN_W
#define PAINT_ITEM_DESIGN_W 380
#endif
/* The status icons (see sicon.h): on unless icons = 0, at icon_size pixels or, by default, 24 at the default font size and larger or smaller with it. */
#ifndef PAINT_ICON_MIN
#define PAINT_ICON_MIN 12
#endif
/* Paint icon max. The reason for this default is not written down yet. */
#ifndef PAINT_ICON_MAX
#define PAINT_ICON_MAX 128
#endif
/* The most of a notification body that is kept (bytes); a longer one is cut. */
#ifndef PAINT_BODY_MAX
#define PAINT_BODY_MAX 2048
#endif

/* ---- pngload.c ---- */
/* File max. The reason for this default is not written down yet. */
#ifndef FILE_MAX
#define FILE_MAX (4 << 20)
#endif
/* Dim max. The reason for this default is not written down yet. */
#ifndef DIM_MAX
#define DIM_MAX 2048
#endif

/* ---- scale.h ---- */
/* Scale min (minutes). The reason for this default is not written down yet. */
#ifndef SCALE_MIN
#define SCALE_MIN 120
#endif
/* Scale max. The reason for this default is not written down yet. */
#ifndef SCALE_MAX
#define SCALE_MAX 480
#endif

/* ---- shape.c ---- */
/* Max chars. The reason for this default is not written down yet. */
#ifndef MAX_CHARS
#define MAX_CHARS 160
#endif

/* ---- sicon.c ---- */
/* Cache. The reason for this default is not written down yet. */
#ifndef SICON_CACHE
#define SICON_CACHE 32
#endif
/* The longest a conversion may take */
#ifndef SVG_WAIT_MS
#define SVG_WAIT_MS 1500
#endif
/* The most of PNG taken from it: an icon is a few hundred bytes */
#ifndef SVG_OUT_MAX
#define SVG_OUT_MAX (256 * 1024)
#endif

/* ---- sound.c ---- */
/* A burst of events plays once, not as a stack of overlapping sounds */
#ifndef MIN_GAP_MS
#define MIN_GAP_MS 400
#endif

/* How long a volume or brightness bar must stay unchanged before its value is spoken: a key held
 * down moves it many times a second, and only where it stops is worth hearing */
#ifndef SPEECH_SLIDER_SETTLE_MS
#define SPEECH_SLIDER_SETTLE_MS 500
#endif

/* ---- src_audio.c ---- */
/* Max frame. The reason for this default is not written down yet. */
#ifndef MAX_FRAME
#define MAX_FRAME 60000
#endif
/* Coalesces a volume drag into few redraws */
#ifndef SRC_AUDIO_DEBOUNCE_MS
#define SRC_AUDIO_DEBOUNCE_MS 40
#endif
/* Reconnect min s (seconds). The reason for this default is not written down yet. */
#ifndef RECONNECT_MIN_S
#define RECONNECT_MIN_S 5
#endif
/* The server may be absent; don't hammer */
#ifndef RECONNECT_MAX_S
#define RECONNECT_MAX_S 300
#endif
/* A jack event and the server's answer to it are this close */
#ifndef JACK_MS
#define JACK_MS 1000
#endif
/* The server moves the port first and applies that port's volume a moment later */
#ifndef SRC_AUDIO_SETTLE_MS
#define SRC_AUDIO_SETTLE_MS 500
#endif
/* Max ports. The reason for this default is not written down yet. */
#ifndef MAX_PORTS
#define MAX_PORTS 64
#endif

/* ---- src_bt.c ---- */
/* Toast ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_BT_TOAST_MS
#define SRC_BT_TOAST_MS 3000
#endif

/* ---- src_disk.c ---- */
/* Default warn pct (percent). The reason for this default is not written down yet. */
#ifndef DEFAULT_WARN_PCT
#define DEFAULT_WARN_PCT 10
#endif
/* Default crit pct (percent). The reason for this default is not written down yet. */
#ifndef DEFAULT_CRIT_PCT
#define DEFAULT_CRIT_PCT 5
#endif
/* With little room left in percent, under 1 GiB is critical */
#ifndef CRIT_BYTES
#define CRIT_BYTES (1ULL << 30)
#endif
/* It must recover this far before it can alert again */
#ifndef REARM_MARGIN_PCT
#define REARM_MARGIN_PCT 3
#endif
/* Default interval s (seconds). The reason for this default is not written down yet. */
#ifndef SRC_DISK_DEFAULT_INTERVAL_S
#define SRC_DISK_DEFAULT_INTERVAL_S 300
#endif
/* Max fs. The reason for this default is not written down yet. */
#ifndef MAX_FS
#define MAX_FS 16
#endif
/* Alert ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_DISK_ALERT_MS
#define SRC_DISK_ALERT_MS 8000
#endif

/* ---- src_jack.c ---- */
/* Max jacks. The reason for this default is not written down yet. */
#ifndef MAX_JACKS
#define MAX_JACKS 16
#endif
/* Toast ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_JACK_TOAST_MS
#define SRC_JACK_TOAST_MS 2500
#endif
/* Max pins. The reason for this default is not written down yet. */
#ifndef MAX_PINS
#define MAX_PINS 32
#endif

/* ---- src_locks.c ---- */
/* Max keyboards. The reason for this default is not written down yet. */
#ifndef MAX_KEYBOARDS
#define MAX_KEYBOARDS 8
#endif
/* Toast ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_LOCKS_TOAST_MS
#define SRC_LOCKS_TOAST_MS 1500
#endif

/* ---- src_mem.c ---- */
/* Cooldown secs (seconds). The reason for this default is not written down yet. */
#ifndef COOLDOWN_SECS
#define COOLDOWN_SECS 30
#endif
/* Fallback poll: alert below this share of MemAvailable... */
#ifndef LOW_PCT
#define LOW_PCT 10
#endif
/* ...and re-arm above this one */
#ifndef SRC_MEM_REARM_PCT
#define SRC_MEM_REARM_PCT 15
#endif
/* Poll secs (seconds). The reason for this default is not written down yet. */
#ifndef SRC_MEM_POLL_SECS
#define SRC_MEM_POLL_SECS 10
#endif

/* ---- src_net.c ---- */
/* Link flaps and resume bursts settle before we judge */
#ifndef SRC_NET_DEBOUNCE_MS
#define SRC_NET_DEBOUNCE_MS 1000
#endif

/* ---- src_power.c ---- */
/* Warn pct (percent). The reason for this default is not written down yet. */
#ifndef WARN_PCT
#define WARN_PCT 15
#endif
/* Crit pct (percent). The reason for this default is not written down yet. */
#ifndef CRIT_PCT
#define CRIT_PCT 5
#endif
/* Rearm pct (percent). The reason for this default is not written down yet. */
#ifndef SRC_POWER_REARM_PCT
#define SRC_POWER_REARM_PCT 20
#endif
/* Fallback for drivers that don't emit uevents on capacity changes */
#ifndef SRC_POWER_POLL_SECS
#define SRC_POWER_POLL_SECS 60
#endif
/* Alert ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_POWER_ALERT_MS
#define SRC_POWER_ALERT_MS 8000
#endif

/* ---- src_print.c ---- */
/* Toast ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_PRINT_TOAST_MS
#define SRC_PRINT_TOAST_MS 4000
#endif
/* Lease s (seconds). The reason for this default is not written down yet. */
#ifndef LEASE_S
#define LEASE_S 86400
#endif
/* Retry min s (seconds). The reason for this default is not written down yet. */
#ifndef RETRY_MIN_S
#define RETRY_MIN_S 60
#endif
/* Retry max s (seconds). The reason for this default is not written down yet. */
#ifndef RETRY_MAX_S
#define RETRY_MAX_S 3600
#endif
/* Http timeout s (seconds). The reason for this default is not written down yet. */
#ifndef HTTP_TIMEOUT_S
#define HTTP_TIMEOUT_S 2
#endif
/* Max printers. The reason for this default is not written down yet. */
#ifndef MAX_PRINTERS
#define MAX_PRINTERS 8
#endif
/* Name max shown. The reason for this default is not written down yet. */
#ifndef NAME_MAX_SHOWN
#define NAME_MAX_SHOWN 80
#endif

/* ---- src_probe.c ---- */
/* Default interval s (seconds). The reason for this default is not written down yet. */
#ifndef SRC_PROBE_DEFAULT_INTERVAL_S
#define SRC_PROBE_DEFAULT_INTERVAL_S 30
#endif
/* Timeout ms (milliseconds). The reason for this default is not written down yet. */
#ifndef TIMEOUT_MS
#define TIMEOUT_MS 2000
#endif
/* Slow ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SLOW_MS
#define SLOW_MS 400
#endif
/* Consecutive bad probes before alerting */
#ifndef BAD_TO_ALERT
#define BAD_TO_ALERT 2
#endif
/* Consecutive good probes before saying it recovered */
#ifndef GOOD_TO_CLEAR
#define GOOD_TO_CLEAR 2
#endif

/* ---- src_temp.c ---- */
/* Default interval s (seconds). The reason for this default is not written down yet. */
#ifndef SRC_TEMP_DEFAULT_INTERVAL_S
#define SRC_TEMP_DEFAULT_INTERVAL_S 120
#endif
/* Min interval s (seconds). The reason for this default is not written down yet. */
#ifndef MIN_INTERVAL_S
#define MIN_INTERVAL_S 10
#endif
/* Cpu default limit. The reason for this default is not written down yet. */
#ifndef CPU_DEFAULT_LIMIT
#define CPU_DEFAULT_LIMIT 100
#endif
/* Cpu default warn. The reason for this default is not written down yet. */
#ifndef CPU_DEFAULT_WARN
#define CPU_DEFAULT_WARN 85
#endif
/* Cpu default hot. The reason for this default is not written down yet. */
#ifndef CPU_DEFAULT_HOT
#define CPU_DEFAULT_HOT 95
#endif
/* Ssd default limit. The reason for this default is not written down yet. */
#ifndef SSD_DEFAULT_LIMIT
#define SSD_DEFAULT_LIMIT 80
#endif
/* Hysteresis c (degrees Celsius). The reason for this default is not written down yet. */
#ifndef HYSTERESIS_C
#define HYSTERESIS_C 5
#endif
/* Alert ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_TEMP_ALERT_MS
#define SRC_TEMP_ALERT_MS 8000
#endif

/* ---- sound.c ---- */
/* Seconds a sound player may run before it is killed. A normal one finishes in well under a
 * second; a hung one must not stay behind. */
#ifndef SOUND_PLAYER_DEADLINE_S
#define SOUND_PLAYER_DEADLINE_S 5
#endif

/* ---- src_usb.c ---- */
/* Max devices. The reason for this default is not written down yet. */
#ifndef MAX_DEVICES
#define MAX_DEVICES 64
#endif
/* Names and interfaces appear after the device does; a re-enumeration is a remove and an add */
#ifndef SRC_USB_SETTLE_MS
#define SRC_USB_SETTLE_MS 800
#endif
/* Toast ms (milliseconds). The reason for this default is not written down yet. */
#ifndef SRC_USB_TOAST_MS
#define SRC_USB_TOAST_MS 3000
#endif

/* ---- text.c ---- */
/* Max text. The reason for this default is not written down yet. */
#ifndef MAX_TEXT
#define MAX_TEXT 1024
#endif

/* ---- timer.c ---- */
/* Max entries. The reason for this default is not written down yet. */
#ifndef MAX_ENTRIES
#define MAX_ENTRIES 32
#endif
/* Max secs (seconds). The reason for this default is not written down yet. */
#ifndef MAX_SECS
#define MAX_SECS (30L * 86400)
#endif
/* Label max. The reason for this default is not written down yet. */
#ifndef LABEL_MAX
#define LABEL_MAX 128
#endif
/* Fire ms (milliseconds). The reason for this default is not written down yet. */
#ifndef FIRE_MS
#define FIRE_MS 15000
#endif
/* How long a peek at the time stays */
#ifndef PEEK_MS
#define PEEK_MS 4000
#endif
/* A peek this late is not shown: the time it was for has gone */
#ifndef PEEK_STALE_S
#define PEEK_STALE_S 120
#endif
/* Min every s (seconds). The reason for this default is not written down yet. */
#ifndef MIN_EVERY_S
#define MIN_EVERY_S 60
#endif

/* A zero or negative limit would make the code unsafe or useless. */
_Static_assert(IDLE_DEFAULT_MIN > 0, "IDLE_DEFAULT_MIN must be positive");
_Static_assert(BREAK_DEFAULT_MIN > 0, "BREAK_DEFAULT_MIN must be positive");
_Static_assert(MAX_SOURCES > 0, "MAX_SOURCES must be positive");
_Static_assert(DEFAULT_GRACE_S > 0, "DEFAULT_GRACE_S must be positive");
_Static_assert(MAX_GRACE_S > 0, "MAX_GRACE_S must be positive");
_Static_assert(REPEAT_WINDOW_S > 0, "REPEAT_WINDOW_S must be positive");
_Static_assert(DEFAULT_MARGIN > 0, "DEFAULT_MARGIN must be positive");
_Static_assert(MARGIN_MAX > 0, "MARGIN_MAX must be positive");
_Static_assert(DEFAULT_FADE_MS > 0, "DEFAULT_FADE_MS must be positive");
_Static_assert(MAX_OUTPUTS > 0, "MAX_OUTPUTS must be positive");
_Static_assert(DECK_GAP > 0, "DECK_GAP must be positive");
_Static_assert(CLICK_RECTS_MAX > 0, "CLICK_RECTS_MAX must be positive");
_Static_assert(NOTE_MS > 0, "NOTE_MS must be positive");
_Static_assert(APP_MAX > 0, "APP_MAX must be positive");
_Static_assert(SUMMARY_MAX > 0, "SUMMARY_MAX must be positive");
_Static_assert(BODY_MAX > 0, "BODY_MAX must be positive");
_Static_assert(SHOWN_DEFAULT > 0, "SHOWN_DEFAULT must be positive");
_Static_assert(SUMMARY_SHOWN > 0, "SUMMARY_SHOWN must be positive");
_Static_assert(BODY_SHOWN > 0, "BODY_SHOWN must be positive");
_Static_assert(HISTORY_MAX > 0, "HISTORY_MAX must be positive");
_Static_assert(OSD_TIMEOUT_MS > 0, "OSD_TIMEOUT_MS must be positive");
_Static_assert(CLOCK_OSD_MS > 0, "CLOCK_OSD_MS must be positive");
_Static_assert(STACK_MAX > 0, "STACK_MAX must be positive");
_Static_assert(DECK_MAX > 0, "DECK_MAX must be positive");
_Static_assert(QUEUE_MAX > 0, "QUEUE_MAX must be positive");
_Static_assert(SLIDER_MS > 0, "SLIDER_MS must be positive");
_Static_assert(HOVER_DELAY_MS > 0, "HOVER_DELAY_MS must be positive");
_Static_assert(HOLD_GRACE_MS > 0, "HOLD_GRACE_MS must be positive");
_Static_assert(TOKEN_WAIT_MS > 0, "TOKEN_WAIT_MS must be positive");
_Static_assert(HUD_MAX_ACTIONS > 0, "HUD_MAX_ACTIONS must be positive");
_Static_assert(ICON_CACHE > 0, "ICON_CACHE must be positive");
_Static_assert(MAX_DIM > 0, "MAX_DIM must be positive");
_Static_assert(IMAGE_PX > 0, "IMAGE_PX must be positive");
_Static_assert(IMAGE_BYTES > 0, "IMAGE_BYTES must be positive");
_Static_assert(DEFAULT_TIMEOUT_MS > 0, "DEFAULT_TIMEOUT_MS must be positive");
_Static_assert(CLIENT_ARGV_MAX > 0, "CLIENT_ARGV_MAX must be positive");
_Static_assert(MIN_MS > 0, "MIN_MS must be positive");
_Static_assert(LOW_MS > 0, "LOW_MS must be positive");
_Static_assert(NORMAL_MS > 0, "NORMAL_MS must be positive");
_Static_assert(MAX_ACTION_STRINGS > 0, "MAX_ACTION_STRINGS must be positive");
_Static_assert(TOAST_LINES > 0, "TOAST_LINES must be positive");
_Static_assert(CARD_BODY_LINES > 0, "CARD_BODY_LINES must be positive");
_Static_assert(CARD_BODY_LINES_EXPANDED > 0, "CARD_BODY_LINES_EXPANDED must be positive");
_Static_assert(PAINT_FONT_MIN > 0, "PAINT_FONT_MIN must be positive");
_Static_assert(PAINT_FONT_MAX > 0, "PAINT_FONT_MAX must be positive");
_Static_assert(PAINT_WIDTH_MIN > 0, "PAINT_WIDTH_MIN must be positive");
_Static_assert(PAINT_WIDTH_MAX > 0, "PAINT_WIDTH_MAX must be positive");
_Static_assert(PAINT_SMALLEST_TEXT > 0 && PAINT_ITEM_DESIGN_W >= PAINT_WIDTH_MIN, "the HUD design sizes are wrong");
_Static_assert(PAINT_ICON_MIN > 0, "PAINT_ICON_MIN must be positive");
_Static_assert(PAINT_ICON_MAX > 0, "PAINT_ICON_MAX must be positive");
_Static_assert(PAINT_BODY_MAX > 0, "PAINT_BODY_MAX must be positive");
_Static_assert(FILE_MAX > 0, "FILE_MAX must be positive");
_Static_assert(DIM_MAX > 0, "DIM_MAX must be positive");
_Static_assert(SCALE_MIN > 0, "SCALE_MIN must be positive");
_Static_assert(SCALE_MAX > 0, "SCALE_MAX must be positive");
_Static_assert(MAX_CHARS > 0, "MAX_CHARS must be positive");
_Static_assert(SICON_CACHE > 0, "SICON_CACHE must be positive");
_Static_assert(SVG_WAIT_MS > 0, "SVG_WAIT_MS must be positive");
_Static_assert(SVG_OUT_MAX > 0, "SVG_OUT_MAX must be positive");
_Static_assert(MIN_GAP_MS > 0, "MIN_GAP_MS must be positive");
_Static_assert(SPEECH_SLIDER_SETTLE_MS > 0, "SPEECH_SLIDER_SETTLE_MS must be positive");
_Static_assert(MAX_FRAME > 0, "MAX_FRAME must be positive");
_Static_assert(SRC_AUDIO_DEBOUNCE_MS > 0, "SRC_AUDIO_DEBOUNCE_MS must be positive");
_Static_assert(RECONNECT_MIN_S > 0, "RECONNECT_MIN_S must be positive");
_Static_assert(RECONNECT_MAX_S > 0, "RECONNECT_MAX_S must be positive");
_Static_assert(JACK_MS > 0, "JACK_MS must be positive");
_Static_assert(SRC_AUDIO_SETTLE_MS > 0, "SRC_AUDIO_SETTLE_MS must be positive");
_Static_assert(MAX_PORTS > 0, "MAX_PORTS must be positive");
_Static_assert(SRC_BT_TOAST_MS > 0, "SRC_BT_TOAST_MS must be positive");
_Static_assert(DEFAULT_WARN_PCT > 0, "DEFAULT_WARN_PCT must be positive");
_Static_assert(DEFAULT_CRIT_PCT > 0, "DEFAULT_CRIT_PCT must be positive");
_Static_assert(CRIT_BYTES > 0, "CRIT_BYTES must be positive");
_Static_assert(REARM_MARGIN_PCT > 0, "REARM_MARGIN_PCT must be positive");
_Static_assert(SRC_DISK_DEFAULT_INTERVAL_S > 0, "SRC_DISK_DEFAULT_INTERVAL_S must be positive");
_Static_assert(MAX_FS > 0, "MAX_FS must be positive");
_Static_assert(SRC_DISK_ALERT_MS > 0, "SRC_DISK_ALERT_MS must be positive");
_Static_assert(MAX_JACKS > 0, "MAX_JACKS must be positive");
_Static_assert(SRC_JACK_TOAST_MS > 0, "SRC_JACK_TOAST_MS must be positive");
_Static_assert(MAX_PINS > 0, "MAX_PINS must be positive");
_Static_assert(MAX_KEYBOARDS > 0, "MAX_KEYBOARDS must be positive");
_Static_assert(SRC_LOCKS_TOAST_MS > 0, "SRC_LOCKS_TOAST_MS must be positive");
_Static_assert(COOLDOWN_SECS > 0, "COOLDOWN_SECS must be positive");
_Static_assert(LOW_PCT > 0, "LOW_PCT must be positive");
_Static_assert(SRC_MEM_REARM_PCT > 0, "SRC_MEM_REARM_PCT must be positive");
_Static_assert(SRC_MEM_POLL_SECS > 0, "SRC_MEM_POLL_SECS must be positive");
_Static_assert(SRC_NET_DEBOUNCE_MS > 0, "SRC_NET_DEBOUNCE_MS must be positive");
_Static_assert(WARN_PCT > 0, "WARN_PCT must be positive");
_Static_assert(CRIT_PCT > 0, "CRIT_PCT must be positive");
_Static_assert(SRC_POWER_REARM_PCT > 0, "SRC_POWER_REARM_PCT must be positive");
_Static_assert(SRC_POWER_POLL_SECS > 0, "SRC_POWER_POLL_SECS must be positive");
_Static_assert(SRC_POWER_ALERT_MS > 0, "SRC_POWER_ALERT_MS must be positive");
_Static_assert(SRC_PRINT_TOAST_MS > 0, "SRC_PRINT_TOAST_MS must be positive");
_Static_assert(LEASE_S > 0, "LEASE_S must be positive");
_Static_assert(RETRY_MIN_S > 0, "RETRY_MIN_S must be positive");
_Static_assert(RETRY_MAX_S > 0, "RETRY_MAX_S must be positive");
_Static_assert(HTTP_TIMEOUT_S > 0, "HTTP_TIMEOUT_S must be positive");
_Static_assert(MAX_PRINTERS > 0, "MAX_PRINTERS must be positive");
_Static_assert(NAME_MAX_SHOWN > 0, "NAME_MAX_SHOWN must be positive");
_Static_assert(SRC_PROBE_DEFAULT_INTERVAL_S > 0, "SRC_PROBE_DEFAULT_INTERVAL_S must be positive");
_Static_assert(TIMEOUT_MS > 0, "TIMEOUT_MS must be positive");
_Static_assert(SLOW_MS > 0, "SLOW_MS must be positive");
_Static_assert(BAD_TO_ALERT > 0, "BAD_TO_ALERT must be positive");
_Static_assert(GOOD_TO_CLEAR > 0, "GOOD_TO_CLEAR must be positive");
_Static_assert(SRC_TEMP_DEFAULT_INTERVAL_S > 0, "SRC_TEMP_DEFAULT_INTERVAL_S must be positive");
_Static_assert(MIN_INTERVAL_S > 0, "MIN_INTERVAL_S must be positive");
_Static_assert(CPU_DEFAULT_LIMIT > 0, "CPU_DEFAULT_LIMIT must be positive");
_Static_assert(CPU_DEFAULT_WARN > 0, "CPU_DEFAULT_WARN must be positive");
_Static_assert(CPU_DEFAULT_HOT > 0, "CPU_DEFAULT_HOT must be positive");
_Static_assert(SSD_DEFAULT_LIMIT > 0, "SSD_DEFAULT_LIMIT must be positive");
_Static_assert(HYSTERESIS_C > 0, "HYSTERESIS_C must be positive");
_Static_assert(SRC_TEMP_ALERT_MS > 0, "SRC_TEMP_ALERT_MS must be positive");
_Static_assert(MAX_DEVICES > 0, "MAX_DEVICES must be positive");
_Static_assert(SRC_USB_SETTLE_MS > 0, "SRC_USB_SETTLE_MS must be positive");
_Static_assert(SRC_USB_TOAST_MS > 0, "SRC_USB_TOAST_MS must be positive");
_Static_assert(SOUND_PLAYER_DEADLINE_S > 0, "SOUND_PLAYER_DEADLINE_S must be positive");
_Static_assert(MAX_TEXT > 0, "MAX_TEXT must be positive");
_Static_assert(MAX_ENTRIES > 0, "MAX_ENTRIES must be positive");
_Static_assert(MAX_SECS > 0, "MAX_SECS must be positive");
_Static_assert(LABEL_MAX > 0, "LABEL_MAX must be positive");
_Static_assert(FIRE_MS > 0, "FIRE_MS must be positive");
_Static_assert(PEEK_MS > 0, "PEEK_MS must be positive");
_Static_assert(PEEK_STALE_S > 0, "PEEK_STALE_S must be positive");
_Static_assert(MIN_EVERY_S > 0, "MIN_EVERY_S must be positive");

#endif
