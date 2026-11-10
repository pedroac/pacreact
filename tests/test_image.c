/* Raw notification pixels (the image-data hint) into the fixed ARGB icon buffer. */
#include "../src/image.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t at(const uint8_t *out, int x, int y) {
    return ((const uint32_t *)out)[y * IMAGE_PX + x];
}

int main(void) {
    uint8_t out[IMAGE_BYTES];

    /* A solid red square: opaque red everywhere */
    uint8_t red[4 * 4 * 3];
    for (int i = 0; i < 16; i++) { red[i * 3] = 255; red[i * 3 + 1] = 0; red[i * 3 + 2] = 0; }
    assert(image_from_hint(4, 4, 12, false, 8, 3, red, sizeof red, out));
    assert(at(out, 0, 0) == 0xFFFF0000u && at(out, 31, 31) == 0xFFFF0000u && at(out, 63, 63) == 0xFFFF0000u);

    /* RGBA: alpha is premultiplied, as Cairo expects */
    uint8_t half[2 * 2 * 4];
    for (int i = 0; i < 4; i++) { half[i * 4] = 200; half[i * 4 + 1] = 100; half[i * 4 + 2] = 50; half[i * 4 + 3] = 128; }
    assert(image_from_hint(2, 2, 8, true, 8, 4, half, sizeof half, out));
    assert(at(out, 10, 10) == (128u << 24 | (200u * 128 / 255) << 16 | (100u * 128 / 255) << 8 | (50u * 128 / 255)));

    /* A wide image keeps its aspect ratio: letterboxed top and bottom, transparent there */
    uint8_t wide[8 * 2 * 3];
    memset(wide, 255, sizeof wide);
    assert(image_from_hint(8, 2, 24, false, 8, 3, wide, sizeof wide, out));
    assert(at(out, 32, 0) == 0 && at(out, 32, 63) == 0);                 /* above and below the picture */
    assert(at(out, 32, 32) == 0xFFFFFFFFu && at(out, 0, 32) == 0xFFFFFFFFu && at(out, 63, 32) == 0xFFFFFFFFu);

    /* A tall one: letterboxed left and right */
    uint8_t tall[2 * 8 * 3];
    memset(tall, 255, sizeof tall);
    assert(image_from_hint(2, 8, 6, false, 8, 3, tall, sizeof tall, out));
    assert(at(out, 0, 32) == 0 && at(out, 63, 32) == 0 && at(out, 32, 32) == 0xFFFFFFFFu);

    /* Row padding (rowstride larger than the pixels) is skipped, and the last row needs no padding */
    uint8_t padded[2 * 8 - 2];  /* 2 rows of 2 RGB pixels with a stride of 8: 8 + 6 bytes */
    memset(padded, 0, sizeof padded);
    padded[0] = 10;   padded[1] = 20;  padded[2] = 30;     /* (0,0) */
    padded[8] = 90;   padded[9] = 80;  padded[10] = 70;    /* (0,1) */
    assert(image_from_hint(2, 2, 8, false, 8, 3, padded, 14, out));
    assert(at(out, 0, 0) == (0xFFu << 24 | 10u << 16 | 20u << 8 | 30u));
    assert(at(out, 0, 63) == (0xFFu << 24 | 90u << 16 | 80u << 8 | 70u));

    /* A tiny one still fills at least a pixel; a huge one is scaled down, not rejected */
    uint8_t one[3] = { 1, 2, 3 };
    assert(image_from_hint(1, 1, 3, false, 8, 3, one, 3, out) && at(out, 32, 32) == (0xFFu << 24 | 1u << 16 | 2u << 8 | 3u));

    /* Rejects: bad depth, bad channel counts, mismatched alpha flag, empty or absurd sizes,
     * a stride smaller than a row, and data that is too short */
    assert(!image_from_hint(4, 4, 12, false, 16, 3, red, sizeof red, out));
    assert(!image_from_hint(4, 4, 12, false, 8, 2, red, sizeof red, out));
    assert(!image_from_hint(4, 4, 12, true, 8, 3, red, sizeof red, out));
    assert(!image_from_hint(4, 4, 16, false, 8, 4, red, sizeof red, out));
    assert(!image_from_hint(0, 4, 12, false, 8, 3, red, sizeof red, out));
    assert(!image_from_hint(4, -1, 12, false, 8, 3, red, sizeof red, out));
    assert(!image_from_hint(5000, 1, 15000, false, 8, 3, red, sizeof red, out));
    assert(!image_from_hint(4, 4, 8, false, 8, 3, red, sizeof red, out));
    assert(!image_from_hint(4, 4, 12, false, 8, 3, red, sizeof red - 1, out));
    assert(!image_from_hint(2147483647, 2147483647, 2147483647, false, 8, 3, red, sizeof red, out));

    puts("image: ok");
    return 0;
}
