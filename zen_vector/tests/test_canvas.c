#include "bmpcmp.h"
#include "scene.h"
#include "zv_canvas.h"

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

static int alpha_at(const ZvSurface *s, int x, int y)
{
    return (int)(s->pixels[(size_t)y * (size_t)s->stride + (size_t)x] >> 24);
}

static ZvPixel at(const ZvSurface *s, int x, int y)
{
    return s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
}

static bool surfaces_near(const ZvSurface *a, const ZvSurface *b, int tol)
{
    for (int y = 0; y < a->height; y++)
    {
        for (int x = 0; x < a->width; x++)
        {
            ZvPixel p = at(a, x, y), q = at(b, x, y);
            for (int shift = 0; shift < 32; shift += 8)
            {
                if (abs((int)((p >> shift) & 0xFF) - (int)((q >> shift) & 0xFF)) > tol)
                    return false;
            }
        }
    }
    return true;
}

static void test_fast_rect_matches_path(void)
{
    /* the axis-aligned fast path gives the same pixels as the rasterizer */
    ZvSurface a, b;
    zv_surface_init(&a, NULL, 40, 30);
    zv_surface_init(&b, NULL, 40, 30);
    ZvCanvas ca, cb;
    zv_canvas_init(&ca, &a, NULL);
    zv_canvas_init(&cb, &b, NULL);
    static const float rects[][4] = {{2.25f, 3.5f, 20.5f, 10.75f}, {-5, -5, 20, 20}, {30, 20, 30, 30}, {5, 5, 0.3f, 8}, {10, 10, -6, -4}, {0, 0, 40, 30}};
    for (size_t i = 0; i < sizeof rects / sizeof rects[0]; i++)
    {
        zv_canvas_set_fill_color(&ca, 0x80FF4020u + (uint32_t)i);
        zv_canvas_set_fill_color(&cb, 0x80FF4020u + (uint32_t)i);
        zv_canvas_set_global_alpha(&ca, 0.7f);
        zv_canvas_set_global_alpha(&cb, 0.7f);
        CHECK(zv_canvas_fill_rect(&ca, rects[i][0], rects[i][1], rects[i][2], rects[i][3]));
        zv_canvas_begin_path(&cb);
        zv_canvas_rect(&cb, rects[i][0], rects[i][1], rects[i][2], rects[i][3]);
        CHECK(zv_canvas_fill(&cb, ZV_FILL_NONZERO));
    }
    CHECK(surfaces_near(&a, &b, 1));
    /* with a scale the fast path still applies */
    zv_canvas_scale(&ca, 2, 0.5f);
    zv_canvas_scale(&cb, 2, 0.5f);
    zv_canvas_fill_rect(&ca, 3.3f, 7.7f, 8, 20);
    zv_canvas_begin_path(&cb);
    zv_canvas_rect(&cb, 3.3f, 7.7f, 8, 20);
    zv_canvas_fill(&cb, ZV_FILL_NONZERO);
    CHECK(surfaces_near(&a, &b, 1));
    zv_canvas_release(&ca);
    zv_canvas_release(&cb);
    zv_surface_release(&a);
    zv_surface_release(&b);
}

static void test_state_and_clip(void)
{
    ZvSurface s;
    zv_surface_init(&s, NULL, 50, 50);
    ZvCanvas c;
    zv_canvas_init(&c, &s, NULL);
    zv_canvas_set_fill_color(&c, 0xFFFF0000u);
    zv_canvas_save(&c);
    zv_canvas_set_fill_color(&c, 0xFF00FF00u);
    zv_canvas_translate(&c, 10, 10);
    zv_canvas_begin_path(&c);
    zv_canvas_arc(&c, 15, 15, 10, 0, 6.2831853f, false);
    zv_canvas_clip(&c, ZV_FILL_NONZERO);
    CHECK(c.state.clip_mask != NULL);
    zv_canvas_fill_rect(&c, -100, -100, 300, 300);
    CHECK(at(&s, 25, 25) == 0xFF00FF00u);
    CHECK(at(&s, 5, 5) == 0 && at(&s, 45, 45) == 0);
    CHECK(alpha_at(&s, 15, 25) > 0 && alpha_at(&s, 15, 25) < 255); /* the circle's left edge at x = 15 */
    zv_canvas_restore(&c);
    CHECK(c.state.clip_mask == NULL && c.state.fill.color == 0xFFFF0000u);
    zv_canvas_fill_rect(&c, 0, 0, 5, 5);
    CHECK(at(&s, 2, 2) == 0xFFFF0000u);
    /* rectangular clip on whole pixels stays a rectangle */
    zv_canvas_begin_path(&c);
    zv_canvas_rect(&c, 40, 40, 10, 10);
    zv_canvas_clip(&c, ZV_FILL_NONZERO);
    CHECK(c.state.clip_mask == NULL && c.state.clip_bounds.min_x == 40);
    zv_canvas_fill_rect(&c, 0, 0, 50, 50);
    CHECK(at(&s, 45, 45) == 0xFFFF0000u && at(&s, 39, 45) == 0);
    /* point in path */
    zv_canvas_reset(&c);
    zv_canvas_begin_path(&c);
    zv_canvas_rect(&c, 10, 10, 10, 10);
    CHECK(zv_canvas_is_point_in_path(&c, 15, 15, ZV_FILL_NONZERO));
    CHECK(!zv_canvas_is_point_in_path(&c, 25, 15, ZV_FILL_NONZERO));
    zv_canvas_release(&c);
    zv_surface_release(&s);
}

static void test_composite_table(void)
{
    ZvPixel d = 0x80400000u; /* half red, premultiplied */
    ZvPixel sp = 0xFF0000FFu;
    CHECK(zv_composite(d, sp, ZV_OP_SOURCE_OVER) == sp);
    CHECK(zv_composite(d, sp, ZV_OP_COPY) == sp);
    CHECK(zv_composite(d, sp, ZV_OP_SOURCE_IN) == 0x80000080u);
    CHECK(zv_composite(d, sp, ZV_OP_SOURCE_OUT) == 0x7F00007Fu);
    CHECK(zv_composite(d, sp, ZV_OP_DESTINATION_OVER) == 0xFF40007Fu || zv_composite(d, sp, ZV_OP_DESTINATION_OVER) == 0xFF400080u);
    CHECK(zv_composite(d, sp, ZV_OP_DESTINATION_IN) == d);
    CHECK(zv_composite(d, sp, ZV_OP_DESTINATION_OUT) == 0);
    CHECK(zv_composite(d, 0, ZV_OP_DESTINATION_OUT) == d);
    CHECK(zv_composite(d, sp, ZV_OP_XOR) == 0x7F00007Fu);
    CHECK(zv_composite(0xFFFF0000u, 0xFFFFFFFFu, ZV_OP_LIGHTER) == 0xFFFFFFFFu);
    CHECK(zv_composite(0xFF808080u, 0xFF808080u, ZV_OP_MULTIPLY) == 0xFF404040u);
    CHECK(zv_composite(0xFF808080u, 0xFF808080u, ZV_OP_SCREEN) == 0xFFC0C0C0u);
    CHECK(zv_composite(0xFF80FF00u, 0xFFFF8000u, ZV_OP_DARKEN) == 0xFF808000u);
    CHECK(zv_composite(0xFF80FF00u, 0xFFFF8000u, ZV_OP_LIGHTEN) == 0xFFFFFF00u);
    ZvCompositeOp op;
    CHECK(zv_composite_op_parse("destination-atop", &op) && op == ZV_OP_DESTINATION_ATOP);
    CHECK(!zv_composite_op_parse("hue", &op));
}

static void test_layers_and_images(void)
{
    ZvSurface s;
    zv_surface_init(&s, NULL, 20, 20);
    ZvCanvas c;
    zv_canvas_init(&c, &s, NULL);
    zv_canvas_set_fill_color(&c, 0xFF0000FFu);
    zv_canvas_fill_rect(&c, 0, 0, 20, 20);
    CHECK(zv_canvas_begin_layer(&c));
    zv_canvas_set_fill_color(&c, 0xFFFF0000u);
    zv_canvas_fill_rect(&c, 0, 0, 10, 20);
    zv_canvas_fill_rect(&c, 0, 0, 10, 20); /* twice: inside the layer it stays opaque red */
    CHECK(zv_canvas_end_layer(&c, 0.5f, NULL));
    CHECK(at(&s, 5, 5) == zv_blend_over(0xFF0000FFu, 0x80800000u));
    CHECK(at(&s, 15, 5) == 0xFF0000FFu);

    /* image data round trip and drawImage */
    uint32_t data[4] = {0x80FF0000u, 0xFF00FF00u, 0x00000000u, 0xFF0000FFu};
    zv_canvas_put_image_data(&c, data, 2, 2, 0, 0);
    uint32_t back[4];
    zv_canvas_get_image_data(&c, 0, 0, 2, 2, back);
    CHECK(back[1] == 0xFF00FF00u && back[2] == 0 && back[3] == 0xFF0000FFu && (back[0] >> 24) == 0x80);
    ZvSurface img;
    zv_surface_init(&img, NULL, 2, 2);
    img.pixels[0] = 0xFFFF0000u;
    img.pixels[1] = 0xFF00FF00u;
    img.pixels[2] = 0xFF0000FFu;
    img.pixels[3] = 0xFFFFFFFFu;
    zv_canvas_set_image_smoothing(&c, false);
    CHECK(zv_canvas_draw_image(&c, &img, 0, 0, 2, 2, 10, 10, 8, 8));
    CHECK(at(&s, 11, 11) == 0xFFFF0000u && at(&s, 17, 11) == 0xFF00FF00u && at(&s, 11, 17) == 0xFF0000FFu && at(&s, 17, 17) == 0xFFFFFFFFu);
    CHECK(zv_canvas_draw_image(&c, &img, 1, 1, 1, 1, 0, 10, 4, 4));
    CHECK(at(&s, 1, 11) == 0xFFFFFFFFu);
    zv_surface_release(&img);
    zv_canvas_release(&c);
    zv_surface_release(&s);
}

static char *scene_text(const char *name)
{
    char path[512];
    snprintf(path, sizeof path, ZV_DIR "/scenes/%s.scene", name);
    return file_read_text(path);
}

static bool load_reference(const char *name, Framebuffer *out)
{
    char path[512];
    snprintf(path, sizeof path, ZV_DIR "/refs/%s.bmp", name);
    return framebuffer_load_bmp(out, path);
}

static void test_reference(const char *name, int tolerance, int max_over, int max_over_8_permille)
{
    char *text = scene_text(name);
    CHECK(text != NULL);
    if (!text)
        return;
    Framebuffer mine, ref;
    char err[256];
    bool ok = zv_scene_run(text, &mine, err, sizeof err);
    if (!ok)
        printf("  %s: %s\n", name, err);
    CHECK(ok);
    bool have_ref = load_reference(name, &ref);
    CHECK(have_ref);
    if (ok && have_ref)
    {
        ZvCompare r, r8;
        CHECK(zv_compare(&mine, &ref, tolerance, &r, NULL));
        CHECK(zv_compare(&mine, &ref, 8, &r8, NULL));
        long long permille = r8.pixels ? r8.over_tolerance * 1000 / r8.pixels : 0;
        printf("  %-20s max channel diff %3d, over %d: %lld, over 8: %lld of %lld (%lld permille)\n", name, r.max_channel, tolerance,
               r.over_tolerance, r8.over_tolerance, r8.pixels, permille);
        CHECK(r.over_tolerance <= max_over);
        CHECK(permille <= max_over_8_permille);
        framebuffer_free(&ref);
    }
    if (ok)
        framebuffer_free(&mine);
    fs_free(text);
}

int main(void)
{
    test_fast_rect_matches_path();
    test_state_and_clip();
    test_composite_table();
    test_layers_and_images();
    printf("against the browser:\n");
    /* the earlier scenes, now through the Canvas API */
    test_reference("rect_opaque", 0, 0, 0);
    test_reference("rect_subpixel", 1, 0, 0);
    test_reference("tri_subpixel", 32, 0, 10);
    test_reference("star_evenodd", 48, 2, 30);
    test_reference("circle_cubic", 48, 0, 20);
    test_reference("translucent", 8, 0, 0);
    test_reference("grad_radial", 32, 0, 20);
    test_reference("pattern_rotated", 48, 0, 50);
    /* strokes */
    test_reference("stroke_joins", 48, 0, 40);
    test_reference("stroke_caps", 64, 40, 40);
    test_reference("stroke_dash", 112, 30, 60);
    test_reference("stroke_curves", 112, 160, 80);
    test_reference("stroke_degenerate", 96, 6, 40);
    test_reference("stroke_transform", 48, 0, 60);
    /* canvas */
    test_reference("canvas_arcs", 64, 20, 40);
    test_reference("canvas_clip", 64, 0, 40);
    test_reference("canvas_composite", 255, 200, 60);
    test_reference("canvas_composite_in", 255, 100, 60);
    test_reference("canvas_shadow", 64, 20, 80);
    test_reference("canvas_image", 255, 600, 100);
    test_reference("canvas_text", 255, 2500, 150);
    test_reference("canvas_transform", 32, 0, 10);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
