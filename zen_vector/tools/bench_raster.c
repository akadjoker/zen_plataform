/*
 * Benchmark of the zen_vector rasterizer, with scenes that match bench_draw2d:
 * 1280x720, fixed seed, one warm-up and RUNS timed runs.
 *
 *   bench_raster            table
 *   bench_raster --csv      scene,min_ms,median_ms
 */
#include "zv_raster.h"
#include "zv_io.h"

#include <math.h>
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

static float rnd_range(float lo, float hi)
{
    return lo + (hi - lo) * (float)(rnd() % 10000u) / 10000.0f;
}

static ZvPixel rnd_color(uint32_t alpha)
{
    return zv_premultiply((alpha << 24) | (rnd() & 0x00FFFFFFu));
}

typedef struct
{
    ZvSurface *surface;
    ZvPath path;
    ZvPolyline poly;
    ZvRasterizer raster;
} Ctx;

static void fill(Ctx *c, ZvPixel color)
{
    zv_polyline_clear(&c->poly);
    zv_path_flatten(&c->path, NULL, ZV_FLATTEN_TOLERANCE, &c->poly);
    zv_fill_polyline_solid(c->surface, &c->poly, ZV_FILL_NONZERO, color, NULL, &c->raster);
}

static void rect(Ctx *c, float x, float y, float w, float h)
{
    zv_path_clear(&c->path);
    zv_path_move_to(&c->path, x, y);
    zv_path_line_to(&c->path, x + w, y);
    zv_path_line_to(&c->path, x + w, y + h);
    zv_path_line_to(&c->path, x, y + h);
    zv_path_close(&c->path);
}

static void circle(Ctx *c, float cx, float cy, float r)
{
    const float k = 0.5522847f;
    zv_path_clear(&c->path);
    zv_path_move_to(&c->path, cx + r, cy);
    zv_path_cubic_to(&c->path, cx + r, cy + k * r, cx + k * r, cy + r, cx, cy + r);
    zv_path_cubic_to(&c->path, cx - k * r, cy + r, cx - r, cy + k * r, cx - r, cy);
    zv_path_cubic_to(&c->path, cx - r, cy - k * r, cx - k * r, cy - r, cx, cy - r);
    zv_path_cubic_to(&c->path, cx + k * r, cy - r, cx + r, cy - k * r, cx + r, cy);
    zv_path_close(&c->path);
}

static void scene_fill_rect_opaque(Ctx *c)
{
    g_seed = 1;
    for (int i = 0; i < 4000; i++)
    {
        float x = rnd_range(-50, WIDTH), y = rnd_range(-50, HEIGHT), w = rnd_range(8, 200), h = rnd_range(8, 200);
        rect(c, x, y, w, h);
        fill(c, rnd_color(0xFF));
    }
}

static void scene_fill_rect_alpha(Ctx *c)
{
    g_seed = 2;
    for (int i = 0; i < 4000; i++)
    {
        float x = rnd_range(-50, WIDTH), y = rnd_range(-50, HEIGHT), w = rnd_range(8, 200), h = rnd_range(8, 200);
        rect(c, x, y, w, h);
        fill(c, rnd_color(0x80));
    }
}

static void scene_fill_circle_alpha(Ctx *c)
{
    g_seed = 4;
    for (int i = 0; i < 1000; i++)
    {
        float x = rnd_range(0, WIDTH - 1), y = rnd_range(0, HEIGHT - 1), r = rnd_range(4, 60);
        circle(c, x, y, r);
        fill(c, rnd_color(0x80));
    }
}

static void scene_stars(Ctx *c)
{
    g_seed = 6;
    for (int i = 0; i < 500; i++)
    {
        float cx = rnd_range(0, WIDTH), cy = rnd_range(0, HEIGHT), r = rnd_range(10, 80);
        zv_path_clear(&c->path);
        for (int k = 0; k < 5; k++)
        {
            float a = (float)k * 4.0f * 3.14159265f / 5.0f - 1.5707963f;
            float x = cx + r * cosf(a), y = cy + r * sinf(a);
            if (k == 0)
                zv_path_move_to(&c->path, x, y);
            else
                zv_path_line_to(&c->path, x, y);
        }
        zv_path_close(&c->path);
        fill(c, rnd_color(0xC0));
    }
}

static void scene_full_screen_alpha(Ctx *c)
{
    g_seed = 7;
    for (int i = 0; i < 20; i++)
    {
        rect(c, -1, -1, WIDTH + 2, HEIGHT + 2);
        fill(c, rnd_color(0x80));
    }
}

typedef struct
{
    const char *name;
    void (*run)(Ctx *);
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
        {"fill_circle_alpha", scene_fill_circle_alpha},
        {"stars_alpha", scene_stars},
        {"full_screen_alpha", scene_full_screen_alpha},
    };

    ZvSurface surface;
    if (!zv_surface_init(&surface, NULL, WIDTH, HEIGHT))
        return 1;
    Ctx c;
    c.surface = &surface;
    zv_path_init(&c.path, NULL);
    zv_polyline_init(&c.poly, NULL);
    zv_rasterizer_init(&c.raster, NULL);

    if (!csv)
    {
        printf("%dx%d, %d timed runs after one warm-up, flatten tolerance %g\n\n", WIDTH, HEIGHT, RUNS, (double)ZV_FLATTEN_TOLERANCE);
        printf("%-20s %10s %10s\n", "scene", "min ms", "median ms");
    }
    for (size_t s = 0; s < sizeof scenes / sizeof scenes[0]; s++)
    {
        double ms[RUNS];
        scenes[s].run(&c);
        for (int r = 0; r < RUNS; r++)
        {
            for (int i = 0; i < WIDTH * HEIGHT; i++)
                surface.pixels[i] = 0xFF202020u;
            uint64_t t0 = zv_time_nanos();
            scenes[s].run(&c);
            ms[r] = (double)(zv_time_nanos() - t0) / 1e6;
        }
        qsort(ms, RUNS, sizeof ms[0], compare_double);
        if (csv)
            printf("%s,%.3f,%.3f\n", scenes[s].name, ms[0], ms[RUNS / 2]);
        else
            printf("%-20s %10.3f %10.3f\n", scenes[s].name, ms[0], ms[RUNS / 2]);
    }
    zv_rasterizer_release(&c.raster);
    zv_polyline_release(&c.poly);
    zv_path_release(&c.path);
    zv_surface_release(&surface);
    return 0;
}
