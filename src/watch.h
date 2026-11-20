#ifndef WATCH_H
#define WATCH_H

/* What `pacreact watch` tells a status bar. Each source keeps one value under a short name ("jacks",
 * "camera", "printer"); a change goes to everyone watching as one line, "name value". */

/* Called when a value changes (before the watchers hear of it): for the one that tells pacreactd. */
void watch_on_change(void (*fn)(const char *name, const char *value));
/* Calls fn for each value there is now: for one that registered after some were set. */
void watch_each(void (*fn)(const char *name, const char *value));
/* Sets a value. Nothing happens, and nobody is told, when it is what it already was. */
void watch_set(const char *name, const char *value);
/* A client that asked to watch: kept, and sent every current value at once. Returns 0, or -1 if there is no room. */
int watch_add(int cfd);

#endif
