#ifndef UTIL_H
#define UTIL_H

#include "length.h"
#include <stdbool.h>
#include <stddef.h>
#include <time.h>

/* Small helpers that more than one part of the daemon needs. */

/* The monotonic clock in milliseconds. Inline, and calling clock_gettime directly, so that a test
 * that defines that name as a fake before including a source file gets its clock. */
static inline long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

/* The full path of an executable: `name` itself if it has a slash in it, else the first match
 * along $PATH. */
bool find_in_path(const char *name, char *out, size_t n);

/* The directories to look for shared data in, as a ':'-separated list: $XDG_DATA_HOME (or
 * ~/.local/share) first, then $XDG_DATA_DIRS (or /usr/local/share:/usr/share). The first entry is
 * empty when neither is known; callers skip empty entries. */
void data_dirs(char *out, size_t n);

/* A whole number from a setting, strictly: all of VALUE must be digits (with a sign, and leading
 * space forgiven), or it is not a number. Unset or empty gives `def`, quietly. Anything else wrong is
 * said on stderr, by NAME, and then: not a number gives `def`; a number outside lo..hi is brought to
 * the nearest end of it. `unit` ("pixels", "seconds") is for the message. */
long int_value(const char *name, const char *value, long def, long lo, long hi, const char *unit);

/* int_value() for the setting NAME (a key of the settings file) */
long setting_int(const char *name, long def, long lo, long hi, const char *unit);

/* A size setting: a whole number of pixels as before, or a number with a unit that means the same
 * on every screen (`3mm`, `0.2in`, `10pt`) or the screen's own (`5%`, of its shorter side). Read once with
 * setting_size(), which says what is wrong with it, then turned into pixels with size_px() for the
 * screen it is shown on (as often as that changes). */
typedef struct {
    bool set;          /* a usable value was given */
    bool unit;         /* it has a unit: `len` is the value; else `px` is */
    long px;
    PacLength len;
} Size;

/* The setting NAME as a Size. Unset or empty is not set, quietly; a value that is neither a whole
 * number nor a length is said on stderr and is not set. A plain number outside lo..hi is brought
 * to the nearest end of it, as setting_int() does. */
Size setting_size(const char *name, long lo, long hi);

/* The size in logical pixels, or `def` if it is not set. `px_per_mm` and `whole` are as for
 * pac_length_px(). A length is brought into lo..hi without a word, since the screen it lands on
 * may differ from the one it was written for. */
long size_px(const Size *size, long def, long lo, long hi, double px_per_mm, double whole);

#endif
