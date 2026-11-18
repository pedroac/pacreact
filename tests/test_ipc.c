/* The IPC socket: a client talking to a listener over the real unix socket, single-instance
 * detection, stale-socket cleanup, and the reply protocol. */
#include "../src/ipc.c"   /* included so the statics are reachable */
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char dir[64];

/* Forks a client that sends msg and exits with ipc_send()'s result. */
static pid_t spawn_client(const char *msg) {
    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        int r = ipc_send(msg);
        _exit(r < 0 ? 100 : r);
    }
    return pid;
}

static int wait_client(pid_t pid) {
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* Accepts the pending client, checks what it sent and answers. */
static void serve_one_at(int line, int lfd, const char *expect, const char *reply) {
    struct pollfd p = { .fd = lfd, .events = POLLIN };
    assert(poll(&p, 1, 2000) == 1);
    char buf[256];
    int cfd = ipc_accept(lfd, buf, sizeof buf);
    if (cfd < 0) fprintf(stderr, "serve_one failed at line %d\n", line);
    assert(cfd >= 0);
    assert(!strcmp(buf, expect));
    ipc_reply(cfd, reply);
}

#define serve_one(...) serve_one_at(__LINE__, __VA_ARGS__)

int main(void) {
    snprintf(dir, sizeof dir, "/tmp/pacreact-ipc-XXXXXX");
    assert(mkdtemp(dir));
    setenv("XDG_RUNTIME_DIR", dir, 1);
    umask(0);   /* the socket must be private whatever the umask says */

    /* No daemon yet. */
    assert(ipc_send("toast") == -1);

    int lfd = ipc_listen();
    assert(lfd >= 0);

    /* The socket reaches the notification history: only its owner may use it. */
    {
        char path[96];
        struct stat st;
        snprintf(path, sizeof path, "%s/pacreact.sock", dir);
        assert(stat(path, &st) == 0 && S_ISSOCK(st.st_mode) && (st.st_mode & 0777) == 0600);
    }

    /* A second instance is refused while the first listens. */
    assert(ipc_listen() == -2);
    {   /* Its probe leaves a dead connection in our queue: accepting must shrug it off, not stall. */
        struct pollfd p = { .fd = lfd, .events = POLLIN };
        assert(poll(&p, 1, 1000) == 1);
        char buf[16];
        assert(ipc_accept(lfd, buf, sizeof buf) == -1);
    }

    /* "ok" is success; the message arrives intact, including tabs and UTF-8. */
    pid_t c = spawn_client("toast\tinfo\t3000\théllo wörld");
    serve_one(lfd, "toast\tinfo\t3000\théllo wörld", "ok");
    assert(wait_client(c) == 0);

    /* "ok" with data succeeds too (list prints it); anything else is an error. */
    c = spawn_client("list");
    serve_one(lfd, "list", "ok 1 timer 5m00s tea");
    assert(wait_client(c) == 0);

    c = spawn_client("timer\tbogus");
    serve_one(lfd, "timer\tbogus", "bad duration \"bogus\"");
    assert(wait_client(c) == 1);

    /* "okay" is not "ok". */
    c = spawn_client("x");
    serve_one(lfd, "x", "okay then");
    assert(wait_client(c) == 1);

    /* A client that connects and says nothing must not stall the daemon for long. */
    {
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        snprintf(a.sun_path, sizeof a.sun_path, "%s/pacreact.sock", dir);
        int silent = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        assert(silent >= 0 && connect(silent, (struct sockaddr *)&a, sizeof a) == 0);

        struct pollfd p = { .fd = lfd, .events = POLLIN };
        assert(poll(&p, 1, 2000) == 1);
        char buf[64];
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        assert(ipc_accept(lfd, buf, sizeof buf) == -1);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        assert((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000 < 1000);
        close(silent);

        /* and the listener still works afterwards */
        c = spawn_client("after the silent one");
        serve_one(lfd, "after the silent one", "ok");
        assert(wait_client(c) == 0);
    }

    /* The daemon dies leaving its socket file behind: the next one reclaims it. */
    close(lfd);
    assert(ipc_send("toast") == -1);          /* stale socket: nobody answers */
    lfd = ipc_listen();
    assert(lfd >= 0);

    c = spawn_client("dismiss");
    serve_one(lfd, "dismiss", "ok");
    assert(wait_client(c) == 0);

    close(lfd);

    /* The directory the socket falls back to without $XDG_RUNTIME_DIR: made private, and refused
     * when it is not (planted by someone else, open to others, or not a directory). */
    {
        char sub[96], link[96], file[96];
        struct stat st;
        snprintf(sub, sizeof sub, "%s/priv", dir);
        snprintf(link, sizeof link, "%s/link", dir);
        snprintf(file, sizeof file, "%s/file", dir);

        assert(private_dir(sub, false) == -1 && stat(sub, &st) == -1);   /* a client never makes it */
        assert(private_dir(sub, true) == 0 && stat(sub, &st) == 0 && (st.st_mode & 077) == 0);
        assert(private_dir(sub, false) == 0 && private_dir(sub, true) == 0);
        assert(chmod(sub, 0755) == 0 && private_dir(sub, true) == -1 && private_dir(sub, false) == -1);
        assert(chmod(sub, 0770) == 0 && private_dir(sub, true) == -1);
        assert(chmod(sub, 0700) == 0 && private_dir(sub, true) == 0);
        assert(symlink(sub, link) == 0 && private_dir(link, true) == -1);   /* a symlink is not it */
        FILE *f = fopen(file, "w");
        assert(f);
        fclose(f);
        assert(private_dir(file, true) == -1);
    }

    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    assert(system(cmd) == 0);
    puts("ipc: ok");
    return 0;
}
