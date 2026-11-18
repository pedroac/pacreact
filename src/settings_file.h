#ifndef SETTINGS_FILE_H
#define SETTINGS_FILE_H

#include <stddef.h>

/* The optional settings file, pacreact.conf, read the way every pac tool's is (see
 * pacommon/config_file.h): /etc/pac/, ~/.config/pac/ and the current folder, each overriding the
 * earlier. "key = value" lines, '#' starts a comment. The keys are the ones listed in
 * pacreact.conf; the rest of the program reads them with pac_setting(). A bad line or an unknown
 * key is said on stderr and skipped. */

/* Reads the file at `path`; returns how many lines were skipped, or -1 if it cannot be opened. */
int settings_load_file(const char *path);

/* Reads those that exist. Call it before anything reads its settings. */
void settings_load(void);

/* The keys the file may set, for the test that keeps pacreact.conf complete. */
const char *const *settings_known(size_t *count);

#endif
