/*
 * Baseline of the zen_platform software rasterizer (draw2d). Five fixed scenes at
 * 1280x720 with a fixed random seed, so two runs on the same machine draw the same
 * pixels. Prints, for each scene, the fastest and the median of RUNS timed runs.
 *
 *   bench_draw2d            human readable table
 *   bench_draw2d --csv      scene,min_ms,median_ms
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIDTH 1280
#define HEIGHT 720
#define RUNS 9

static uint32_t g_seed;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static int rnd_range(int lo, int hi)
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

static uint32_t rnd_color(uint32_t alpha)
{
    return (alpha << 24) | (rnd() & 0x00FFFFFFu);
}

static void scene_fill_rect_opaque(Framebuffer *fb, const Framebuffer *src)
{
    (void)src;
    g_seed = 1;
    for (int i = 0; i < 4000; i++)
        draw_fill_rect(fb, rnd_range(-50, WIDTH), rnd_range(-50, HEIGHT), rnd_range(8, 200), rnd_range(8, 200), rnd_color(0xFF), BLEND_NONE);
}

static void scene_fill_rect_alpha(Framebuffer *fb, const Framebuffer *src)
{
    (void)src;
    g_seed = 2;
    for (int i = 0; i < 4000; i++)
        draw_fill_rect(fb, rnd_range(-50, WIDTH), rnd_range(-50, HEIGHT), rnd_range(8, 200), rnd_range(8, 200), rnd_color(0x80), BLEND_ALPHA);
}

static void scene_lines_alpha(Framebuffer *fb, const Framebuffer *src)
{
    (void)src;
    g_seed = 3;
    for (int i = 0; i < 8000; i++)
        draw_line(fb, rnd_range(0, WIDTH - 1), rnd_range(0, HEIGHT - 1), rnd_range(0, WIDTH - 1), rnd_range(0, HEIGHT - 1), rnd_color(0xC0), BLEND_ALPHA);
}

static void scene_fill_circle_alpha(Framebuffer *fb, const Framebuffer *src)
{
    (void)src;
    g_seed = 4;
    for (int i = 0; i < 1000; i++)
        draw_fill_circle(fb, rnd_range(0, WIDTH - 1), rnd_range(0, HEIGHT - 1), rnd_range(4, 60), rnd_color(0x80), BLEND_ALPHA);
}

static void scene_blit_scaled(Framebuffer *fb, const Framebuffer *src)
{
    g_seed = 5;
    for (int i = 0; i < 10; i++)
        draw_blit(fb, src, 0, 0, src->width, src->height, 0, 0, WIDTH, HEIGHT, BLEND_NONE, SCALE_BILINEAR);
    for (int i = 0; i < 2000; i++)
        draw_blit(fb, src, 0, 0, 128, 128, rnd_range(-20, WIDTH), rnd_range(-20, HEIGHT), 96, 96, BLEND_ALPHA, SCALE_BILINEAR);
}

typedef struct
{
    const char *name;
    void (*run)(Framebuffer *, const Framebuffer *);
} Scene;

static int compare_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return x < y ? -1 : x > y;
}

int main(int argc, char **argv)
{
    bool csv = argc > 1 && strcmp(argv[1], "--csv") == 0;
    static const Scene scenes[] = {
        {"fill_rect_opaque", scene_fill_rect_opaque},
        {"fill_rect_alpha", scene_fill_rect_alpha},
        {"lines_alpha", scene_lines_alpha},
        {"fill_circle_alpha", scene_fill_circle_alpha},
        {"blit_scaled", scene_blit_scaled},
    };

    if (!platform_init())
    {
        fprintf(stderr, "platform_init failed: %s\n", platform_get_error());
        return 1;
    }

    Framebuffer fb, src;
    if (!framebuffer_alloc(&fb, WIDTH, HEIGHT) || !framebuffer_alloc(&src, 256, 256))
        return 1;
    for (int y = 0; y < src.height; y++)
    {
        for (int x = 0; x < src.width; x++)
            src.pixels[y * src.stride + x] = 0xFF000000u | (uint32_t)(x << 16) | (uint32_t)(y << 8) | (uint32_t)((x ^ y) & 0xFF);
    }

    if (!csv)
    {
#if defined(__VERSION__)
        printf("compiler: %s\n", __VERSION__);
#endif
        printf("%dx%d, %d timed runs after one warm-up\n\n", WIDTH, HEIGHT, RUNS);
        printf("%-20s %10s %10s\n", "scene", "min ms", "median ms");
    }
    for (size_t s = 0; s < sizeof scenes / sizeof scenes[0]; s++)
    {
        double ms[RUNS];
        draw_clear(&fb, 0xFF202020);
        scenes[s].run(&fb, &src);
        for (int r = 0; r < RUNS; r++)
        {
            draw_clear(&fb, 0xFF202020);
            uint64_t t0 = time_nanos();
            scenes[s].run(&fb, &src);
            ms[r] = (double)(time_nanos() - t0) / 1e6;
        }
        qsort(ms, RUNS, sizeof ms[0], compare_double);
        if (csv)
            printf("%s,%.3f,%.3f\n", scenes[s].name, ms[0], ms[RUNS / 2]);
        else
            printf("%-20s %10.3f %10.3f\n", scenes[s].name, ms[0], ms[RUNS / 2]);
    }

    framebuffer_free(&fb);
    framebuffer_free(&src);
    platform_shutdown();
    return 0;
}
