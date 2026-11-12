#include "pngload.h"
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FILE_MAX (4 << 20)
#define DIM_MAX 2048

typedef struct { int fd; size_t left; } Source;

static cairo_status_t read_png(void *closure, unsigned char *data, unsigned int length) {
    Source *src = closure;
    while (length) {
        if (!src->left) return CAIRO_STATUS_READ_ERROR;
        ssize_t n = read(src->fd, data, length < src->left ? length : src->left);
        if (n <= 0) return CAIRO_STATUS_READ_ERROR;
        data += n;
        length -= (unsigned)n;
        src->left -= (size_t)n;
    }
    return CAIRO_STATUS_SUCCESS;
}

/* The signature and the IHDR chunk, which comes first and holds the size. */
static int header_ok(int fd) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    unsigned char h[24];
    if (pread(fd, h, sizeof h, 0) != (ssize_t)sizeof h || memcmp(h, sig, sizeof sig) || memcmp(h + 12, "IHDR", 4))
        return 0;
    uint32_t w = (uint32_t)h[16] << 24 | (uint32_t)h[17] << 16 | (uint32_t)h[18] << 8 | h[19];
    uint32_t ht = (uint32_t)h[20] << 24 | (uint32_t)h[21] << 16 | (uint32_t)h[22] << 8 | h[23];
    return w >= 1 && ht >= 1 && w <= DIM_MAX && ht <= DIM_MAX;
}

cairo_surface_t *png_load(const char *path) {
    size_t len = strlen(path);
    if (path[0] != '/' || len < 5 || strcmp(path + len - 4, ".png")) return NULL;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) return NULL;
    struct stat st;
    cairo_surface_t *img = NULL;
    if (fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size <= FILE_MAX && header_ok(fd)) {
        Source src = { fd, (size_t)st.st_size };
        img = cairo_image_surface_create_from_png_stream(read_png, &src);
    }
    close(fd);
    if (img && cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(img);
        img = NULL;
    }
    return img;
}
