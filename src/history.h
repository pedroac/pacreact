#ifndef HISTORY_H
#define HISTORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The last notifications, in RAM only: a fixed ring that overwrites its oldest entry, so memory
 * never grows and nothing is written to disk. Entries hold text only (no icon pixels). `pachud
 * history` lists them; `pachud history clear` wipes the ring. */
#define HISTORY_MAX 64

/* EXPIRED, DISMISSED and CLOSED have the NotificationClosed reason codes of the spec as values. */
typedef enum {
    HIST_OPEN,       /* on screen, or waiting for room to be */
    HIST_EXPIRED = 1,
    HIST_DISMISSED,  /* by the user */
    HIST_CLOSED,     /* by the app, with CloseNotification */
    HIST_SKIPPED,    /* by do-not-disturb: never shown */
    HIST_DROPPED,    /* anything else: for example it did not fit and was never shown */
} HistReason;

/* A notification arrived. One that replaces an entry still open updates it in place; anything
 * else is a new entry, pushing the oldest out when the ring is full. */
void history_record(uint32_t id, const char *app, const char *summary, const char *body,
                    int urgency, HistReason state);
/* A notification left the screen, with the NotificationClosed reason code of the spec
 * (1 expired, 2 dismissed, 3 closed by call, anything else dropped). Entries that are already
 * closed, or unknown, are left alone. */
void history_close(uint32_t id, int reason);
void history_clear(void);
/* Handles "history" with args "", "N" (how many newest ones to list) or "clear". Returns false
 * if cmd is not "history". */
bool history_command(const char *cmd, char *args, char *reply, size_t n);

#endif
