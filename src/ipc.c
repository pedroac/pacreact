#include "ipc.h"
#include "i18n.h"
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

/* A directory only this user can enter: made if wanted, and refused when it exists but is not ours
 * and private (so nobody else can have planted or can reach what is in it). */
static int private_dir(const char *path, bool create) {
    if (create && mkdir(path, 0700) < 0 && errno != EEXIST) return -1;
    struct stat st;
    if (lstat(path, &st) < 0 || !S_ISDIR(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 077)) return -1;
    return 0;
}

/* $XDG_RUNTIME_DIR is private to the user by definition. Without it the socket goes in a private
 * directory of its own: a socket straight in the shared /tmp could be squatted by another user. */
static int sock_path(struct sockaddr_un *a, bool create) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    a->sun_family = AF_UNIX;
    if (dir && *dir) {
        snprintf(a->sun_path, sizeof a->sun_path, "%s/pacreact.sock", dir);
        return 0;
    }
    char priv[64];
    snprintf(priv, sizeof priv, "/tmp/pacreact-%u", (unsigned)geteuid());
    if (private_dir(priv, create) < 0) return -1;
    snprintf(a->sun_path, sizeof a->sun_path, "%s/pacreact.sock", priv);
    return 0;
}

static int connect_daemon(void) {
    struct sockaddr_un a = {0};
    if (sock_path(&a, false) < 0) return -1;
    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { close(fd); return -1; }
    return fd;
}

int ipc_listen(void) {
    struct sockaddr_un a = {0};
    if (sock_path(&a, true) < 0) return -1;

    int probe = connect_daemon();
    if (probe >= 0) { close(probe); return -2; }
    unlink(a.sun_path);  /* stale socket from a dead daemon */

    int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    /* The socket reaches the notification history: no one but this user may connect, whatever the
     * umask. */
    mode_t old = umask(0177);
    int bound = bind(fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (bound < 0 || listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int ipc_accept(int lfd, char *buf, size_t n) {
    int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
    if (cfd < 0) return -1;

    /* Only this user's own processes get an answer, even if the socket's permissions are ever wrong. */
    struct ucred cred;
    socklen_t cred_len = sizeof cred;
    if (getsockopt(cfd, SOL_SOCKET, SO_PEERCRED, &cred, &cred_len) < 0 || cred.uid != geteuid()) {
        close(cfd);
        return -1;
    }

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
    fprintf(stderr, "pacreact: %s\n", reply);
    return 1;
}
