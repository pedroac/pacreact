#include "state.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void state_file(const char *name, char *out, size_t n, bool make_dir) {
    const char *base = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
    char dir[400];
    if (base && *base) snprintf(dir, sizeof dir, "%s", base);
    else snprintf(dir, sizeof dir, "%s/.local/state", home ? home : "/tmp");
    if (make_dir) mkdir(dir, 0700);
    strncat(dir, "/pacreact", sizeof dir - strlen(dir) - 1);
    if (make_dir) mkdir(dir, 0700);
    snprintf(out, n, "%s/%s", dir, name);
}

FILE *state_open_read(const char *path) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid() ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) {
        close(fd);
        return NULL;
    }
    FILE *f = fdopen(fd, "r");
    if (!f) close(fd);
    return f;
}

FILE *state_create(const char *path) {
    unlink(path);
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    FILE *f = fdopen(fd, "w");
    if (!f) close(fd);
    return f;
}
