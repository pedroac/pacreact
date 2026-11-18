#ifndef IMAGE_H
#define IMAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tunables.h"


/* Converts the (width, height, rowstride, has_alpha, bits_per_sample, channels, data) of an
 * image-data hint into IMAGE_PX x IMAGE_PX premultiplied ARGB32 in native byte order (what
 * Cairo wants), scaled to fit and centred, transparent around it. Only 8-bit RGB and RGBA are
 * accepted. Returns false for anything malformed or implausibly large. */
bool image_from_hint(int32_t width, int32_t height, int32_t rowstride, bool has_alpha,
                     int32_t bits_per_sample, int32_t channels, const uint8_t *data, size_t len,
                     uint8_t *out);

#endif
