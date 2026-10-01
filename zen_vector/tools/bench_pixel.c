/*
 * Cost of blending one translucent colour over a 1280x720 opaque destination, per
 * pixel, for the pixel core against a naive per-channel version and against the
 * draw2d of zen_platform measured in FASE1.md. Prints the fastest and the median
 * of RUNS timed runs.
 *
 *   bench_pixel          human readable table
 *   bench_pixel --csv    name,min_ms,median_ms,ns_per_pixel
 */
#include "zv_pixel.h"
#include "zv_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 1280
#define HEIGHT 720
#define PIXELS ((size_t)WIDTH * HEIGHT)
#define RUNS 15

#define TRANSLUCENT 0x80402010u /* alpha 0x80, valid premultiplied */
#define OPAQUE 0xFF3366CCu

static uint32_t g_seed = 7;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static uint32_t sink;

static ZvPixel naive_blend(ZvPixel dst, ZvPixel src)
{
    uint32_t inverse = 255u - (src >> 24);
    uint32_t a = (src >> 24) + ((dst >> 24) * inverse + 127u) / 255u;
    uint32_t r = ((src >> 16) & 0xFFu) + (((dst >> 16) & 0xFFu) * inverse + 127u) / 255u;
    uint32_t g = ((src >> 8) & 0xFFu) + (((dst >> 8) & 0xFFu) * inverse + 127u) / 255u;
    uint32_t b = (src & 0xFFu) + ((dst & 0xFFu) * inverse + 127u) / 255u;
    return a << 24 | r << 16 | g << 8 | b;
}

static void run_naive(ZvPixel *buf, ZvBitmap *fb, const uint8_t *cover)
{
    (void)fb;
    (void)cover;
    for (size_t i = 0; i < PIXELS; i++)
        buf[i] = naive_blend(buf[i], TRANSLUCENT);
}

static void run_blend_over(ZvPixel *buf, ZvBitmap *fb, const uint8_t *cover)
{
    (void)fb;
    (void)cover;
    for (size_t i = 0; i < PIXELS; i++)
        buf[i] = zv_blend_over(buf[i], TRANSLUCENT);
}

static void run_span_translucent(ZvPixel *buf, ZvBitmap *fb, const uint8_t *cover)
{
    (void)fb;
    (void)cover;
    for (int y = 0; y < HEIGHT; y++)
        zv_span_solid(buf + (size_t)y * WIDTH, WIDTH, TRANSLUCENT);
}

static void run_span_opaque(ZvPixel *buf, ZvBitmap *fb, const uint8_t *cover)
{
    (void)fb;
    (void)cover;
    for (int y = 0; y < HEIGHT; y++)
        zv_span_solid(buf + (size_t)y * WIDTH, WIDTH, OPAQUE);
}

static void run_span_cover(ZvPixel *buf, ZvBitmap *fb, const uint8_t *cover)
{
    (void)fb;
    for (int y = 0; y < HEIGHT; y++)
        zv_span_cover(buf + (size_t)y * WIDTH, WIDTH, TRANSLUCENT, cover);
}

typedef struct
{
    const char *name;
    void (*run)(ZvPixel *, ZvBitmap *, const uint8_t *);
} Case;

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    bool csv = argc > 1 && strcmp(argv[1], "--csv") == 0;
    static const Case cases[] = {
        {"naive_per_channel", run_naive},
        {"zv_blend_over", run_blend_over},
        {"zv_span_solid_translucent", run_span_translucent},
        {"zv_span_solid_opaque", run_span_opaque},
        {"zv_span_cover", run_span_cover},
    };


    ZvPixel *buf = malloc(PIXELS * sizeof *buf);
    uint8_t *cover = malloc(WIDTH);
    ZvBitmap fb;
    if (!buf || !cover || !zv_bitmap_alloc(&fb, WIDTH, HEIGHT))
        return 1;
    for (int i = 0; i < WIDTH; i++)
        cover[i] = (uint8_t)(i % 4 == 0 ? 0 : i % 4 == 1 ? 255
                                                         : 128);

    if (!csv)
    {
#if defined(__VERSION__)
        printf("compiler: %s\n", __VERSION__);
#endif
        printf("%dx%d = %zu pixels per run, %d timed runs after one warm-up\n\n", WIDTH, HEIGHT, PIXELS, RUNS);
        printf("%-28s %10s %10s %12s\n", "case", "min ms", "median ms", "ns / pixel");
    }
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++)
    {
        double ms[RUNS];
        for (int r = -1; r < RUNS; r++)
        {
            g_seed = 7;
            for (size_t i = 0; i < PIXELS; i++)
            {
                buf[i] = 0xFF000000u | (rnd() & 0x00FFFFFFu);
                fb.pixels[i] = buf[i];
            }
            uint64_t t0 = zv_time_nanos();
            cases[c].run(buf, &fb, cover);
            uint64_t dt = zv_time_nanos() - t0;
            if (r >= 0)
                ms[r] = (double)dt / 1e6;
            sink += buf[PIXELS / 2] + fb.pixels[PIXELS / 2];
        }
        qsort(ms, RUNS, sizeof ms[0], compare_double);
        double ns = ms[0] * 1e6 / (double)PIXELS;
        if (csv)
            printf("%s,%.3f,%.3f,%.3f\n", cases[c].name, ms[0], ms[RUNS / 2], ns);
        else
            printf("%-28s %10.3f %10.3f %12.3f\n", cases[c].name, ms[0], ms[RUNS / 2], ns);
    }
    if (sink == 0x12345678u)
        printf("\n");

    free(buf);
    free(cover);
    zv_bitmap_free(&fb);
    return 0;
}
