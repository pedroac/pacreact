#ifndef CONFIG_FILE_H
#define CONFIG_FILE_H

/* The optional settings file, ~/.local/pac/pachud.conf (shared with the other pac* tools): "key =
 * value" lines, '#' starts a comment. Each key is an environment variable without the PACHUD_ prefix,
 * in lower case: `font_size = 22` is PACHUD_FONT_SIZE=22. The environment wins over the file, so a
 * unit file or a shell can still override one setting. A bad line or an unknown key is said on stderr
 * and skipped. */

/* Reads the file at `path`; returns how many lines were skipped, or -1 if it cannot be opened. */
int config_load_file(const char *path);

/* Reads the default file, if there is one. Call it before anything reads its settings. */
void config_load(void);

#endif
