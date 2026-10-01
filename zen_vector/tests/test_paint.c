#include "bmpcmp.h"
#include "scene.h"
#include "zv_fill.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

static int channel(uint32_t p, int shift)
{
    return (int)((p >> shift) & 0xFF);
}

static bool pixel_near(uint32_t a, uint32_t b, int tol)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        if (abs(channel(a, shift) - channel(b, shift)) > tol)
            return false;
    }
    return true;
}

/* Reference colour of a gradient at t: straight interpolation, like the
   paint promises, then premultiplied. */
static uint32_t ref_gradient(const ZvPaint *p, float t)
{
    if (t <= p->stops[0].offset)
        return p->stops[0].color;
    for (int i = 1; i < p->stop_count; i++)
    {
        if (t <= p->stops[i].offset)
        {
            float f = (t - p->stops[i - 1].offset) / (p->stops[i].offset - p->stops[i - 1].offset);
            uint32_t out = 0;
            for (int shift = 0; shift < 32; shift += 8)
            {
                float a = (float)channel(p->stops[i - 1].color, shift), b = (float)channel(p->stops[i].color, shift);
                out |= (uint32_t)(a + (b - a) * f + 0.5f) << shift;
            }
            return out;
        }
    }
    return p->stops[p->stop_count - 1].color;
}

static void fill_rect_with(ZvSurface *s, const ZvPaint *paint, const ZvMatrix *m, float x, float y, float w, float h)
{
    ZvPolyline poly;
    zv_polyline_init(&poly, NULL);
    zv_polyline_add_contour(&poly, true);
    zv_polyline_add_point(&poly, x, y);
    zv_polyline_add_point(&poly, x + w, y);
    zv_polyline_add_point(&poly, x + w, y + h);
    zv_polyline_add_point(&poly, x, y + h);
    ZvPaintContext ctx;
    CHECK(zv_paint_prepare(&ctx, paint, m, NULL));
    CHECK(zv_fill_polyline(s, &poly, ZV_FILL_NONZERO, &ctx, NULL, NULL));
    zv_paint_release(&ctx);
    zv_polyline_release(&poly);
}

static void test_stops(void)
{
    ZvPaint p = zv_paint_linear(0, 0, 10, 0);
    CHECK(p.type == ZV_PAINT_LINEAR && p.alpha == 255 && p.stop_count == 0);
    CHECK(zv_paint_add_stop(&p, 0.5f, 0xFF00FF00u));
    CHECK(zv_paint_add_stop(&p, 0.0f, 0xFFFF0000u));
    CHECK(zv_paint_add_stop(&p, 1.0f, 0xFF0000FFu));
    CHECK(zv_paint_add_stop(&p, 0.5f, 0xFFFFFFFFu)); /* same offset: stays after */
    CHECK(p.stop_count == 4);
    CHECK(p.stops[0].offset == 0.0f && p.stops[1].color == 0xFF00FF00u && p.stops[2].color == 0xFFFFFFFFu && p.stops[3].offset == 1.0f);
    CHECK(!zv_paint_add_stop(&p, 1.5f, 0));
    CHECK(!zv_paint_add_stop(&p, -0.1f, 0));
    CHECK(!zv_paint_add_stop(&p, NAN, 0));
    for (int i = 0; i < 20; i++)
        zv_paint_add_stop(&p, 0.3f, 0);
    CHECK(p.stop_count == ZV_MAX_STOPS);
}

static void test_solid_and_degenerate(void)
{
    ZvPaint p = zv_paint_solid(0x80FF0000u);
    ZvPaintContext ctx;
    CHECK(zv_paint_prepare(&ctx, &p, NULL, NULL));
    CHECK(ctx.is_solid && ctx.solid == 0x80800000u && !ctx.opaque);
    p.alpha = 128;
    CHECK(zv_paint_prepare(&ctx, &p, NULL, NULL));
    CHECK(ctx.solid == zv_premultiply(0x40FF0000u));
    zv_paint_release(&ctx);

    /* no stops: transparent; one stop: solid; zero length: transparent */
    ZvPaint g = zv_paint_linear(0, 0, 10, 0);
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    CHECK(ctx.is_solid && ctx.solid == 0);
    zv_paint_release(&ctx);
    zv_paint_add_stop(&g, 0.3f, 0xFF123456u);
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    CHECK(ctx.is_solid && ctx.solid == 0xFF123456u && ctx.opaque);
    zv_paint_release(&ctx);
    zv_paint_add_stop(&g, 0.7f, 0xFF654321u);
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    CHECK(!ctx.is_solid && ctx.table != NULL && ctx.opaque);
    zv_paint_release(&ctx);
    ZvPaint z = zv_paint_linear(3, 3, 3, 3);
    zv_paint_add_stop(&z, 0, 0xFFFFFFFFu);
    zv_paint_add_stop(&z, 1, 0xFF000000u);
    CHECK(zv_paint_prepare(&ctx, &z, NULL, NULL));
    CHECK(ctx.is_solid && ctx.solid == 0);
    zv_paint_release(&ctx);
    /* a singular transform paints nothing */
    ZvMatrix flat = zv_matrix_make(1, 0, 0, 0, 0, 0);
    CHECK(zv_paint_prepare(&ctx, &g, &flat, NULL));
    CHECK(ctx.is_solid && ctx.solid == 0);
    zv_paint_release(&ctx);
    /* a pattern without a bitmap fails */
    ZvPaint pat = zv_paint_pattern(NULL, true, true, ZV_FILTER_NEAREST);
    CHECK(!zv_paint_prepare(&ctx, &pat, NULL, NULL));
    zv_paint_release(&ctx);
}

static void test_linear(void)
{
    ZvPaint g = zv_paint_linear(10, 0, 50, 0);
    zv_paint_add_stop(&g, 0, 0xFFFF0000u);
    zv_paint_add_stop(&g, 0.25f, 0x80000000u);
    zv_paint_add_stop(&g, 1, 0xFF0000FFu);
    ZvPaintContext ctx;
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    ZvPixel row[64];
    zv_paint_span(&ctx, 7, 0, 64, row);
    int worst = 0;
    for (int x = 0; x < 64; x++)
    {
        float t = ((float)x + 0.5f - 10.0f) / 40.0f;
        ZvPixel expect = zv_premultiply(ref_gradient(&g, t));
        for (int shift = 0; shift < 32; shift += 8)
        {
            int d = abs(channel(row[x], shift) - channel(expect, shift));
            if (d > worst)
                worst = d;
        }
    }
    printf("  linear against the straight interpolation: max %d/255\n", worst);
    CHECK(worst <= 1);
    CHECK(row[0] == 0xFFFF0000u && row[63] == 0xFF0000FFu); /* pad */
    zv_paint_release(&ctx);

    /* repeat and reflect */
    g.spread = ZV_SPREAD_REPEAT;
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    zv_paint_span(&ctx, 0, 0, 64, row);
    CHECK(pixel_near(row[52], row[12], 1));                /* t = 1.0625 -> 0.0625 */
    CHECK(pixel_near(row[2], row[42], 1));                 /* t = -0.1875 -> 0.8125 */
    zv_paint_release(&ctx);
    g.spread = ZV_SPREAD_REFLECT;
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    zv_paint_span(&ctx, 0, 0, 64, row);
    CHECK(pixel_near(row[52], row[47], 1));                /* 1.0625 -> 0.9375 */
    CHECK(pixel_near(row[2], row[17], 1));                 /* -0.1875 -> 0.1875 */
    zv_paint_release(&ctx);

    /* the transform moves the gradient: with a 2x scale t changes half as fast */
    g.spread = ZV_SPREAD_PAD;
    ZvMatrix m = zv_matrix_scaling(2, 2);
    CHECK(zv_paint_prepare(&ctx, &g, &m, NULL));
    ZvPixel scaled[128];
    zv_paint_span(&ctx, 0, 0, 128, scaled);
    zv_paint_release(&ctx);
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    zv_paint_span(&ctx, 0, 0, 64, row);
    CHECK(pixel_near(scaled[41], row[20], 2) && pixel_near(scaled[81], row[40], 2));
    zv_paint_release(&ctx);

    /* vertical and diagonal: t only depends on the projection */
    ZvPaint vg = zv_paint_linear(0, 10, 0, 30);
    zv_paint_add_stop(&vg, 0, 0xFF000000u);
    zv_paint_add_stop(&vg, 1, 0xFFFFFFFFu);
    CHECK(zv_paint_prepare(&ctx, &vg, NULL, NULL));
    ZvPixel a[4], b[4];
    zv_paint_span(&ctx, 20, 0, 4, a);
    zv_paint_span(&ctx, 20, 500, 4, b);
    CHECK(a[0] == b[3] && pixel_near(a[0], 0xFF868686u, 1)); /* t = 10.5 / 20 */
    zv_paint_release(&ctx);

    /* dithering: neighbouring pixels differ by at most one step and average
       to the undithered value */
    ZvPaint dg = zv_paint_linear(0, 0, 4096, 0);
    zv_paint_add_stop(&dg, 0, 0xFF000000u);
    zv_paint_add_stop(&dg, 1, 0xFFFFFFFFu);
    dg.dither = true;
    CHECK(zv_paint_prepare(&ctx, &dg, NULL, NULL));
    long sum = 0;
    int maxstep = 0, distinct = 0;
    for (int y = 0; y < 4; y++)
    {
        ZvPixel line[64];
        zv_paint_span(&ctx, y, 2000, 64, line);
        int prev = -1;
        for (int x = 0; x < 64; x++)
        {
            int v = channel(line[x], 0);
            sum += v;
            if (prev >= 0 && abs(v - prev) > maxstep)
                maxstep = abs(v - prev);
            if (prev >= 0 && v != prev)
                distinct++;
            prev = v;
        }
    }
    CHECK(maxstep <= 1);
    CHECK(distinct > 20); /* without dithering 64 pixels hold at most 5 steps */
    double mean = (double)sum / 256.0;
    double expect = ((2000.0 + 32.0) / 4096.0) * 255.0;
    CHECK(fabs(mean - expect) < 1.5);
    /* dithered pixels stay valid premultiplied colours */
    ZvPaint dt = zv_paint_linear(0, 0, 64, 0);
    zv_paint_add_stop(&dt, 0, 0x00FF0000u);
    zv_paint_add_stop(&dt, 1, 0xFFFF0000u);
    dt.dither = true;
    zv_paint_release(&ctx);
    CHECK(zv_paint_prepare(&ctx, &dt, NULL, NULL));
    bool valid = true;
    for (int y = 0; y < 4; y++)
    {
        ZvPixel line[64];
        zv_paint_span(&ctx, y, 0, 64, line);
        for (int x = 0; x < 64; x++)
            valid = valid && channel(line[x], 16) <= channel(line[x], 24) && channel(line[x], 0) == 0;
    }
    CHECK(valid);
    zv_paint_release(&ctx);
}

static void test_radial(void)
{
    /* concentric: t is the distance over the radius */
    ZvPaint g = zv_paint_radial(32, 32, 0, 32, 32, 20);
    zv_paint_add_stop(&g, 0, 0xFFFFFFFFu);
    zv_paint_add_stop(&g, 1, 0xFF000000u);
    ZvPaintContext ctx;
    CHECK(zv_paint_prepare(&ctx, &g, NULL, NULL));
    CHECK(ctx.radial_simple);
    ZvPixel row[64];
    zv_paint_span(&ctx, 32, 0, 64, row);
    int worst = 0;
    for (int x = 0; x < 64; x++)
    {
        float t = hypotf((float)x + 0.5f - 32.0f, 0.5f) / 20.0f; /* row 32 is centred at 32.5 */
        ZvPixel expect = zv_premultiply(ref_gradient(&g, t));
        int d = abs(channel(row[x], 8) - channel(expect, 8));
        if (d > worst)
            worst = d;
    }
    printf("  radial against the distance formula: max %d/255\n", worst);
    CHECK(worst <= 1);
    CHECK(row[0] == 0xFF000000u && row[63] == 0xFF000000u);
    /* symmetric in every direction */
    ZvPixel col[64];
    for (int y = 0; y < 64; y++)
        zv_paint_span(&ctx, y, 32, 1, &col[y]);
    bool sym = true;
    for (int i = 0; i < 64; i++)
        sym = sym && pixel_near(col[i], row[i], 1);
    CHECK(sym);
    zv_paint_release(&ctx);

    /* inner radius: inside r0 is the first colour, outside r1 the last */
    ZvPaint ring = zv_paint_radial(32, 32, 10, 32, 32, 20);
    zv_paint_add_stop(&ring, 0, 0xFFFF0000u);
    zv_paint_add_stop(&ring, 1, 0xFF0000FFu);
    CHECK(zv_paint_prepare(&ctx, &ring, NULL, NULL));
    zv_paint_span(&ctx, 32, 0, 64, row);
    CHECK(row[32] == 0xFFFF0000u && row[40] == 0xFFFF0000u && row[60] == 0xFF0000FFu);
    CHECK(pixel_near(row[47], 0xFF73008Cu, 3)); /* distance 15.5: t = 0.55 */
    zv_paint_release(&ctx);

    /* two circles, focal style: on the circle 1 boundary t = 1, at the focal
       point t = 0, and the largest root is chosen (Canvas) */
    ZvPaint focal = zv_paint_radial(44, 32, 0, 32, 32, 30);
    zv_paint_add_stop(&focal, 0, 0xFFFFFFFFu);
    zv_paint_add_stop(&focal, 1, 0xFF000000u);
    CHECK(zv_paint_prepare(&ctx, &focal, NULL, NULL));
    CHECK(!ctx.radial_simple);
    ZvPixel p;
    zv_paint_span(&ctx, 32, 44, 1, &p);
    CHECK(channel(p, 0) >= 240); /* 0.5 px from the focus on a ray of about 18 px */
    zv_paint_span(&ctx, 32, 2, 1, &p);  /* x = 2.5: distance 29.5 from the centre, 41.5 from the focus */
    CHECK(channel(p, 0) <= 6);
    zv_paint_span(&ctx, 32, 61, 1, &p); /* x = 61.5: just inside the circle on the focal side */
    /* ray from (44,32) through (61.5,32) hits the circle at x = 62: t = 17.5/18 */
    CHECK(abs(channel(p, 0) - (int)(255.0f * (1.0f - 17.5f / 18.0f) + 0.5f)) <= 2);
    /* a point outside the outer circle but on the far side still pads to the last colour */
    zv_paint_span(&ctx, 0, 32, 1, &p); /* (32.5, 0.5): 31.5 from the centre */
    CHECK(channel(p, 0) == 0 && (p >> 24) == 255);
    zv_paint_release(&ctx);

    /* a radial where the circles do not contain each other: outside the
       cone nothing is painted */
    ZvPaint cone = zv_paint_radial(10, 32, 2, 50, 32, 6);
    zv_paint_add_stop(&cone, 0, 0xFFFFFFFFu);
    zv_paint_add_stop(&cone, 1, 0xFF000000u);
    CHECK(zv_paint_prepare(&ctx, &cone, NULL, NULL));
    zv_paint_span(&ctx, 2, 30, 1, &p);
    CHECK(p == 0);
    zv_paint_span(&ctx, 32, 30, 1, &p);
    CHECK((p >> 24) == 255);
    zv_paint_release(&ctx);
}

static void test_pattern(void)
{
    ZvSurface img;
    zv_surface_init(&img, NULL, 4, 4);
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
            img.pixels[y * 4 + x] = 0xFF000000u | (uint32_t)(x * 60) << 16 | (uint32_t)(y * 60) << 8;
    }
    ZvPaint pat = zv_paint_pattern(&img, true, true, ZV_FILTER_NEAREST);
    ZvPaintContext ctx;
    CHECK(zv_paint_prepare(&ctx, &pat, NULL, NULL));
    ZvPixel row[12];
    zv_paint_span(&ctx, 1, -4, 12, row);
    bool periodic = true;
    for (int i = 0; i < 12; i++)
        periodic = periodic && row[i] == img.pixels[4 + (i % 4)];
    CHECK(periodic);
    zv_paint_release(&ctx);

    /* no repeat: outside the bitmap is transparent */
    ZvPaint once = zv_paint_pattern(&img, false, false, ZV_FILTER_NEAREST);
    CHECK(zv_paint_prepare(&ctx, &once, NULL, NULL));
    zv_paint_span(&ctx, 1, -4, 12, row);
    CHECK(row[0] == 0 && row[3] == 0 && row[4] == img.pixels[4] && row[7] == img.pixels[7] && row[8] == 0);
    zv_paint_span(&ctx, 5, 0, 4, row);
    CHECK(row[0] == 0);
    zv_paint_release(&ctx);
    /* repeat-x only */
    ZvPaint rx = zv_paint_pattern(&img, true, false, ZV_FILTER_NEAREST);
    CHECK(zv_paint_prepare(&ctx, &rx, NULL, NULL));
    zv_paint_span(&ctx, 1, 100, 1, row);
    CHECK(row[0] == img.pixels[4]);
    zv_paint_span(&ctx, 9, 1, 1, row);
    CHECK(row[0] == 0);
    zv_paint_release(&ctx);

    /* bilinear at the pixel centres returns the pixels, half way the mean */
    ZvPaint bl = zv_paint_pattern(&img, true, true, ZV_FILTER_BILINEAR);
    CHECK(zv_paint_prepare(&ctx, &bl, NULL, NULL));
    zv_paint_span(&ctx, 2, 0, 4, row);
    CHECK(row[1] == img.pixels[9] && row[2] == img.pixels[10]);
    ZvMatrix half = zv_matrix_translation(0.5f, 0);
    CHECK(zv_paint_prepare(&ctx, &bl, &half, NULL));
    zv_paint_span(&ctx, 2, 2, 1, row); /* device 2.5 -> bitmap 2.0: between pixels 1 and 2 */
    CHECK(channel(row[0], 16) == 90);
    zv_paint_release(&ctx);

    /* a scaled pattern: each bitmap pixel covers 3x3 device pixels with nearest */
    ZvPaint sc = zv_paint_pattern(&img, false, false, ZV_FILTER_NEAREST);
    sc.matrix = zv_matrix_scaling(3, 3);
    CHECK(zv_paint_prepare(&ctx, &sc, NULL, NULL));
    zv_paint_span(&ctx, 4, 0, 12, row);
    CHECK(row[0] == img.pixels[4] && row[2] == img.pixels[4] && row[3] == img.pixels[5] && row[11] == img.pixels[7]);
    zv_paint_release(&ctx);

    /* global alpha scales the samples */
    sc.alpha = 128;
    CHECK(zv_paint_prepare(&ctx, &sc, NULL, NULL));
    zv_paint_span(&ctx, 4, 3, 1, row);
    CHECK(row[0] == zv_scale(img.pixels[5], 128));
    zv_paint_release(&ctx);
    zv_surface_release(&img);
}

static void test_fill_with_paint(void)
{
    /* a gradient fill through the rasterizer: the colours follow the paint
       and the coverage multiplies them at the edges */
    ZvSurface s;
    zv_surface_init(&s, NULL, 40, 20);
    ZvPaint g = zv_paint_linear(0, 0, 40, 0);
    zv_paint_add_stop(&g, 0, 0xFFFF0000u);
    zv_paint_add_stop(&g, 1, 0xFF0000FFu);
    fill_rect_with(&s, &g, NULL, 4.5f, 2, 30, 10);
    CHECK(s.pixels[5 * 40 + 20] == zv_premultiply(ref_gradient(&g, 20.5f / 40.0f)));
    CHECK(s.pixels[5 * 40 + 4] == zv_scale(zv_premultiply(ref_gradient(&g, 4.5f / 40.0f)), 128));
    CHECK(s.pixels[5 * 40 + 35] == 0 && s.pixels[15 * 40 + 20] == 0);
    /* a solid paint takes the solid path and gives the same as the solid fill */
    ZvSurface a, b;
    zv_surface_init(&a, NULL, 40, 20);
    zv_surface_init(&b, NULL, 40, 20);
    ZvPaint solid = zv_paint_solid(0x80204060u);
    fill_rect_with(&a, &solid, NULL, 4.5f, 2, 30, 10);
    ZvPolyline poly;
    zv_polyline_init(&poly, NULL);
    zv_polyline_add_contour(&poly, true);
    zv_polyline_add_point(&poly, 4.5f, 2);
    zv_polyline_add_point(&poly, 34.5f, 2);
    zv_polyline_add_point(&poly, 34.5f, 12);
    zv_polyline_add_point(&poly, 4.5f, 12);
    zv_fill_polyline_solid(&b, &poly, ZV_FILL_NONZERO, zv_premultiply(0x80204060u), NULL, NULL);
    CHECK(memcmp(a.pixels, b.pixels, sizeof(ZvPixel) * 800) == 0);
    zv_polyline_release(&poly);
    zv_surface_release(&a);
    zv_surface_release(&b);
    zv_surface_release(&s);

    /* a wide fill exercises the span buffer in pieces */
    zv_surface_init(&s, NULL, 1000, 2);
    fill_rect_with(&s, &g, NULL, 0, 0, 1000, 2);
    CHECK(channel(s.pixels[0], 16) >= 250 && s.pixels[999] == 0xFF0000FFu && s.pixels[500] == 0xFF0000FFu);
    CHECK((s.pixels[300] >> 24) == 255);
    zv_surface_release(&s);
}

static char *scene_text(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, ZV_DIR "/scenes/%s.scene", name);
    return zv_io_read_text(path);
}

static bool load_reference(const char *name, ZvBitmap *out)
{
    char path[512];
    snprintf(path, sizeof path, ZV_DIR "/refs/%s.bmp", name);
    return zv_bitmap_load_bmp(out, path);
}

static void test_reference(const char *name, int tolerance, int max_over, int max_over_8_permille)
{
    char *text = scene_text(name);
    CHECK(text != NULL);
    if (!text)
        return;
    ZvBitmap mine, ref;
    char err[256];
    bool ok = zv_scene_run(text, &mine, err, sizeof err);
    if (!ok)
        printf("  %s: %s\n", name, err);
    CHECK(ok);
    CHECK(load_reference(name, &ref));
    ZvCompare r, r8;
    CHECK(zv_compare(&mine, &ref, tolerance, &r, NULL));
    CHECK(zv_compare(&mine, &ref, 8, &r8, NULL));
    long long permille = r8.pixels ? r8.over_tolerance * 1000 / r8.pixels : 0;
    printf("  %-18s max channel diff %3d, over %d: %lld, over 8: %lld of %lld (%lld permille)\n", name, r.max_channel, tolerance,
           r.over_tolerance, r8.over_tolerance, r8.pixels, permille);
    CHECK(r.over_tolerance <= max_over);
    CHECK(permille <= max_over_8_permille);
    zv_io_free(text);
    zv_bitmap_free(&mine);
    zv_bitmap_free(&ref);
}

int main(void)
{
    test_stops();
    test_solid_and_degenerate();
    test_linear();
    test_radial();
    test_pattern();
    test_fill_with_paint();
    printf("against the browser:\n");
    test_reference("grad_linear", 4, 0, 0);
    test_reference("grad_linear_alpha", 4, 0, 0);
    test_reference("grad_radial", 32, 0, 20);
    test_reference("grad_focal", 32, 0, 20);
    test_reference("grad_alpha_global", 4, 0, 0);
    test_reference("pattern_repeat", 4, 0, 0);
    test_reference("pattern_scaled", 32, 0, 20);
    test_reference("pattern_nearest", 4, 0, 0);
    test_reference("pattern_rotated", 48, 0, 50);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
