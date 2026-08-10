/*
 * evo_blend — source-over pixel blending, in one place.
 *
 * This was `prospero_osd_blend_pixel`, a file-scope static in main.c that the
 * thumbnail module also needed. Extracting that module meant either exporting
 * a symbol out of main.c (backwards: a module would depend on the program) or
 * copying the arithmetic (a second copy of a colour-space assumption). It is
 * eight lines of pure function, so it becomes a header instead.
 *
 * Pixels are 0xAABBGGRR — R in the low byte, matching RR_BGRA, evo_icons.h and
 * the framebuffer. Output is always opaque.
 */
#ifndef EVO_BLEND_H
#define EVO_BLEND_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Blend source_pixel over destination at `opacity` (0–255).
 *
 * The two extremes are shortcuts rather than special cases: at 0 the result is
 * the destination exactly, at 255 the source exactly, with no rounding error
 * from the divide in between.
 */
static inline uint32_t evo_blend_pixel(uint32_t destination,
                                       uint32_t source_pixel,
                                       int opacity)
{
    int source_r, source_g, source_b;
    int destination_r, destination_g, destination_b;
    int inverse, output_r, output_g, output_b;

    if (opacity <= 0)
        return destination;
    if (opacity >= 255)
        return source_pixel;

    source_r = source_pixel & 0xFF;
    source_g = (source_pixel >> 8) & 0xFF;
    source_b = (source_pixel >> 16) & 0xFF;

    destination_r = destination & 0xFF;
    destination_g = (destination >> 8) & 0xFF;
    destination_b = (destination >> 16) & 0xFF;

    inverse = 255 - opacity;

    output_r = (source_r * opacity + destination_r * inverse) / 255;
    output_g = (source_g * opacity + destination_g * inverse) / 255;
    output_b = (source_b * opacity + destination_b * inverse) / 255;

    return 0xFF000000u |
           ((uint32_t)output_b << 16) |
           ((uint32_t)output_g << 8) |
           (uint32_t)output_r;
}

#ifdef __cplusplus
}
#endif

#endif /* EVO_BLEND_H */
