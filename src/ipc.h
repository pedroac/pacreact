#ifndef IPC_H
#define IPC_H

#include <stddef.h>

/* Daemon side. Returns the listening fd, -1 on error, -2 if another daemon already runs. */
int ipc_listen(void);
/* Accepts one client and reads its message (NUL-terminated, at most n-1 bytes) into buf.
 * Returns the client fd to reply on (close it with ipc_reply) or -1. */
int ipc_accept(int lfd, char *buf, size_t n);
void ipc_reply(int cfd, const char *msg);

/* Client side: sends msg and waits for the reply. Returns 0 on "ok", 1 on an error reply
 * (printed to stderr), -1 if no daemon is reachable. */
int ipc_send(const char *msg);

#endif
