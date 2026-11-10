#include "image.h"
#include <string.h>

#define MAX_DIM 4096  /* beyond this it is not an icon */

static uint32_t premultiplied(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    return (uint32_t)a << 24 | (uint32_t)(r * a / 255) << 16 | (uint32_t)(g * a / 255) << 8 |
           (uint32_t)(b * a / 255);
}

bool image_from_hint(int32_t width, int32_t height, int32_t rowstride, bool has_alpha,
                     int32_t bits_per_sample, int32_t channels, const uint8_t *data, size_t len,
                     uint8_t *out) {
    if (bits_per_sample != 8 || (channels != 3 && channels != 4) || channels != (has_alpha ? 4 : 3))
        return false;
    if (width <= 0 || height <= 0 || width > MAX_DIM || height > MAX_DIM) return false;
    if (rowstride < width * channels) return false;
    /* the last row only needs its pixels, not a full stride */
    if ((size_t)rowstride * (size_t)(height - 1) + (size_t)width * channels > len) return false;

    memset(out, 0, IMAGE_BYTES);
    uint32_t *px = (uint32_t *)out;

    /* Fit inside IMAGE_PX x IMAGE_PX, keeping the aspect ratio, nearest-neighbour sampling
     * from the middle of each destination pixel's source area. */
    int dw = width >= height ? IMAGE_PX : (int)((int64_t)width * IMAGE_PX / height);
    int dh = height >= width ? IMAGE_PX : (int)((int64_t)height * IMAGE_PX / width);
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = (IMAGE_PX - dw) / 2, oy = (IMAGE_PX - dh) / 2;

    for (int y = 0; y < dh; y++) {
        int sy = (int)(((int64_t)y * 2 + 1) * height / (2 * dh));
        for (int x = 0; x < dw; x++) {
            int sx = (int)(((int64_t)x * 2 + 1) * width / (2 * dw));
            const uint8_t *p = data + (size_t)sy * rowstride + (size_t)sx * channels;
            px[(oy + y) * IMAGE_PX + ox + x] = premultiplied(p[0], p[1], p[2], channels == 4 ? p[3] : 255);
        }
    }
    return true;
}
