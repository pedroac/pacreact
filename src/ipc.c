#include "ipc.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void sock_path(struct sockaddr_un *a) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    a->sun_family = AF_UNIX;
    if (dir && *dir) snprintf(a->sun_path, sizeof a->sun_path, "%s/pachud.sock", dir);
    else snprintf(a->sun_path, sizeof a->sun_path, "/tmp/pachud-%u.sock", (unsigned)getuid());
}

static int connect_daemon(void) {
    struct sockaddr_un a = {0};
    sock_path(&a);
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

int ipc_listen(void) {
    struct sockaddr_un a = {0};
    sock_path(&a);

    int probe = connect_daemon();
    if (probe >= 0) { close(probe); return -2; }
    unlink(a.sun_path);  /* stale socket from a dead daemon */

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ipc_accept(int lfd, char *buf, size_t n) {
    int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) return -1;

    /* The client sends right after connecting; don't let a silent one stall the daemon. */
    struct pollfd p = { .fd = cfd, .events = POLLIN };
    ssize_t len = -1;
    if (poll(&p, 1, 200) > 0) len = recv(cfd, buf, n - 1, 0);
    if (len <= 0) { close(cfd); return -1; }
    buf[len] = '\0';
    return cfd;
}

void ipc_reply(int cfd, const char *msg) {
    (void)!send(cfd, msg, strlen(msg), MSG_NOSIGNAL);
    close(cfd);
}

int ipc_send(const char *msg) {
    int fd = connect_daemon();
    if (fd < 0) return -1;
    if (send(fd, msg, strlen(msg), MSG_NOSIGNAL) < 0) { close(fd); return -1; }

    char reply[4096];
    ssize_t len = recv(fd, reply, sizeof reply - 1, 0);
    close(fd);
    if (len < 0) return -1;
    reply[len] = '\0';
    if (!strncmp(reply, "ok", 2) && (reply[2] == '\0' || reply[2] == ' ')) {
        if (reply[2]) puts(reply + 3);
        return 0;
    }
    fprintf(stderr, "pachud: %s\n", reply);
    return 1;
}
