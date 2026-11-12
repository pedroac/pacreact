#ifndef UTIL_H
#define UTIL_H

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

/* int_value() for the environment variable NAME */
long env_int(const char *name, long def, long lo, long hi, const char *unit);

#endif
