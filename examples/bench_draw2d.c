/*
 * bench_draw2d.c - how long the software rasterizer takes, headless (no window, no display). Each case is run five
 * times and the best is printed, in microseconds per call. An argument keeps only the cases whose name contains it:
 *   bench_draw2d blit
 * Compare two builds on the same machine, several runs each: a difference of a few percent is the alignment of the code.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *g_filter;

#define BENCH(name, iters, stmt)                                                     \
    do                                                                               \
    {                                                                                \
        if (g_filter && !strstr(name, g_filter))                                     \
            break;                                                                   \
        double best = 1e18;                                                          \
        for (int rep = 0; rep < 5; rep++)                                            \
        {                                                                            \
            uint64_t t0 = time_nanos();                                              \
            for (int it = 0; it < (iters); it++)                                     \
            {                                                                        \
                stmt;                                                                \
            }                                                                        \
            double t = (double)(time_nanos() - t0) / 1000.0 / (iters);               \
            if (t < best)                                                            \
                best = t;                                                            \
        }                                                                            \
        printf("%-40s %10.1f us\n", name, best);                                     \
    } while (0)

int main(int argc, char **argv)
{
    g_filter = argc > 1 ? argv[1] : NULL;
    Framebuffer fb, src;
    if (!framebuffer_alloc(&fb, 1920, 1080) || !framebuffer_alloc(&src, 256, 256))
        return 1;
    for (int i = 0; i < 256 * 256; i++)
        src.pixels[i] = (uint32_t)i * 2654435761u; /* every alpha, so the blends do their work */

    BENCH("clear 1920x1080", 200, draw_clear(&fb, 0xFF102030));
    BENCH("fill_rect 1920x1080 opaque", 200, draw_fill_rect(&fb, 0, 0, 1920, 1080, 0xFF405060, BLEND_NONE));
    BENCH("fill_rect 1920x1080 opaque, ALPHA", 200, draw_fill_rect(&fb, 0, 0, 1920, 1080, 0xFF405060, BLEND_ALPHA));
    BENCH("fill_rect 1920x1080 alpha", 50, draw_fill_rect(&fb, 0, 0, 1920, 1080, 0x80405060, BLEND_ALPHA));
    BENCH("fill_rect 200x30 alpha (x1000)", 20,
          for (int k = 0; k < 1000; k++) draw_fill_rect(&fb, (k * 13) % 1700, (k * 7) % 1000, 200, 30, 0x80405060, BLEND_ALPHA));
    BENCH("fill_round_rect 200x30 r8 (x1000)", 20,
          for (int k = 0; k < 1000; k++) draw_fill_round_rect(&fb, (k * 13) % 1700, (k * 7) % 1000, 200, 30, 8, 0xFF405060, BLEND_NONE));
    BENCH("fill_round_rect 200x30 alpha (x1000)", 20,
          for (int k = 0; k < 1000; k++) draw_fill_round_rect(&fb, (k * 13) % 1700, (k * 7) % 1000, 200, 30, 8, 0x80405060, BLEND_ALPHA));
    BENCH("fill_circle r50 (x1000)", 20,
          for (int k = 0; k < 1000; k++) draw_fill_circle(&fb, 100 + (k * 13) % 1700, 100 + (k * 7) % 900, 50, 0xFF405060, BLEND_NONE));
    BENCH("line 1000 long (x1000)", 20,
          for (int k = 0; k < 1000; k++) draw_line(&fb, 100, 100 + k % 800, 1100, 200 + k % 700, 0xFFFFFFFF, BLEND_NONE));
    BENCH("fill_triangle 200px (x200)", 20,
          for (int k = 0; k < 200; k++) draw_fill_triangle(&fb, 100 + k, 100, 300 + k, 150, 150 + k, 300, 0xFF405060, BLEND_NONE));
    BENCH("blit 256x256 nearest 1:1 (x100)", 20,
          for (int k = 0; k < 100; k++) draw_blit(&fb, &src, 0, 0, 256, 256, (k * 17) % 1600, (k * 11) % 800, 256, 256, BLEND_NONE, SCALE_NEAREST));
    BENCH("blit 256x256 nearest alpha (x100)", 20,
          for (int k = 0; k < 100; k++) draw_blit(&fb, &src, 0, 0, 256, 256, (k * 17) % 1600, (k * 11) % 800, 256, 256, BLEND_ALPHA, SCALE_NEAREST));
    BENCH("blit 256x256 bilinear 1:1 (x100)", 20,
          for (int k = 0; k < 100; k++) draw_blit(&fb, &src, 0, 0, 256, 256, (k * 17) % 1600, (k * 11) % 800, 256, 256, BLEND_ALPHA, SCALE_BILINEAR));
    BENCH("blit 256->512 nearest (x20)", 10,
          for (int k = 0; k < 20; k++) draw_blit(&fb, &src, 0, 0, 256, 256, (k * 17) % 1300, (k * 11) % 500, 512, 512, BLEND_ALPHA, SCALE_NEAREST));
    BENCH("blit 256->512 bilinear (x20)", 10,
          for (int k = 0; k < 20; k++) draw_blit(&fb, &src, 0, 0, 256, 256, (k * 17) % 1300, (k * 11) % 500, 512, 512, BLEND_NONE, SCALE_BILINEAR));
    BENCH("blit 256->1080 bilinear full", 5, draw_blit(&fb, &src, 0, 0, 256, 256, 0, 0, 1920, 1080, BLEND_NONE, SCALE_BILINEAR));
    BENCH("pixel 1M", 5, for (int k = 0; k < 1000000; k++) draw_pixel(&fb, k % 1920, (k / 1920) % 1080, 0xFFFFFFFF, BLEND_NONE));

    framebuffer_free(&fb);
    framebuffer_free(&src);
    return 0;
}
