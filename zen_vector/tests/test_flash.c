#include "zv_flash.h"

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

static ZvPixel at(const ZvSurface *s, int x, int y)
{
    return s->pixels[(size_t)y * (size_t)s->stride + (size_t)x];
}

static bool near_pixel(ZvPixel a, ZvPixel b, int tol)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        if (abs((int)((a >> shift) & 0xFF) - (int)((b >> shift) & 0xFF)) > tol)
            return false;
    }
    return true;
}

static bool same_surface(const ZvSurface *a, const ZvSurface *b, int tol)
{
    for (int y = 0; y < a->height; y++)
        for (int x = 0; x < a->width; x++)
            if (!near_pixel(at(a, x, y), at(b, x, y), tol))
                return false;
    return true;
}

static void test_graphics(void)
{
    ZvSurface s;
    zv_surface_init(&s, NULL, 60, 60);
    ZvCanvas c;
    zv_canvas_init(&c, &s, NULL);
    ZvGraphics g;
    zv_graphics_init(&g, NULL);
    /* even-odd by default: a square with a square inside is a frame */
    CHECK(zv_graphics_begin_fill(&g, 0xFF0000, 1.0f));
    CHECK(zv_graphics_draw_rect(&g, 5, 5, 40, 40));
    CHECK(zv_graphics_draw_rect(&g, 15, 15, 20, 20));
    CHECK(zv_graphics_end_fill(&g));
    /* a line without a fill, automatically closed fill not involved */
    CHECK(zv_graphics_line_style(&g, 4, 0x0000FF, 1.0f, false, ZV_SCALE_NORMAL, ZV_CAP_BUTT, ZV_JOIN_MITER, 3));
    CHECK(zv_graphics_move_to(&g, 50, 5));
    CHECK(zv_graphics_line_to(&g, 50, 55));
    /* a fill left open is closed automatically */
    CHECK(zv_graphics_line_style(&g, 0, 0, 0, false, ZV_SCALE_NORMAL, ZV_CAP_BUTT, ZV_JOIN_MITER, 3));
    CHECK(zv_graphics_begin_fill(&g, 0x00FF00, 0.5f));
    CHECK(zv_graphics_move_to(&g, 5, 50));
    CHECK(zv_graphics_line_to(&g, 25, 50));
    CHECK(zv_graphics_line_to(&g, 15, 58));
    CHECK(zv_graphics_render(&g, &c));
    CHECK(at(&s, 10, 10) == 0xFFFF0000u);
    CHECK(at(&s, 25, 25) == 0);
    CHECK(at(&s, 49, 30) == 0xFF0000FFu && at(&s, 51, 30) == 0xFF0000FFu && at(&s, 47, 30) == 0 && at(&s, 53, 30) == 0);
    CHECK(near_pixel(at(&s, 15, 53), 0x80008000u, 1));
    CHECK(g.bounds.min_x == 5 && g.bounds.max_x == 50 && g.bounds.max_y == 58);
    zv_graphics_release(&g);

    /* circle and round rect come out where expected */
    zv_graphics_init(&g, NULL);
    memset(s.pixels, 0, sizeof(ZvPixel) * 3600);
    zv_graphics_begin_fill(&g, 0x123456, 1);
    zv_graphics_draw_circle(&g, 30, 30, 20);
    zv_graphics_end_fill(&g);
    zv_graphics_render(&g, &c);
    CHECK(at(&s, 30, 30) == 0xFF123456u && at(&s, 30, 11) == 0xFF123456u && at(&s, 30, 9) == 0 && at(&s, 12, 12) == 0);
    zv_graphics_clear(&g);
    memset(s.pixels, 0, sizeof(ZvPixel) * 3600);
    zv_graphics_begin_fill(&g, 0x123456, 1);
    zv_graphics_draw_round_rect(&g, 5, 5, 50, 30, 20, 20);
    zv_graphics_end_fill(&g);
    zv_graphics_render(&g, &c);
    CHECK(at(&s, 30, 20) == 0xFF123456u && at(&s, 6, 6) == 0 && at(&s, 30, 6) == 0xFF123456u && at(&s, 6, 20) == 0xFF123456u);
    /* drawPath and drawTriangles */
    zv_graphics_clear(&g);
    memset(s.pixels, 0, sizeof(ZvPixel) * 3600);
    uint8_t cmds[4] = {1, 2, 2, 2};
    float data[8] = {5, 5, 25, 5, 25, 25, 5, 25};
    zv_graphics_begin_fill(&g, 0xFFFFFF, 1);
    CHECK(zv_graphics_draw_path(&g, cmds, 4, data, 8));
    zv_graphics_end_fill(&g);
    float verts[6] = {30, 30, 55, 30, 30, 55};
    zv_graphics_begin_fill(&g, 0xFF00FF, 1);
    CHECK(zv_graphics_draw_triangles(&g, verts, 3, NULL, 0));
    zv_graphics_end_fill(&g);
    zv_graphics_render(&g, &c);
    CHECK(at(&s, 10, 10) == 0xFFFFFFFFu && at(&s, 35, 35) == 0xFFFF00FFu && at(&s, 50, 50) == 0);
    /* a gradient fill through the Flash box */
    zv_graphics_clear(&g);
    memset(s.pixels, 0, sizeof(ZvPixel) * 3600);
    uint32_t rgbs[2] = {0xFF0000, 0x0000FF};
    float alphas[2] = {1, 1};
    uint8_t ratios[2] = {0, 255};
    ZvMatrix m = zv_matrix_make(60.0f / 1638.4f, 0, 0, 60.0f / 1638.4f, 30, 30);
    zv_graphics_begin_gradient_fill(&g, false, rgbs, alphas, ratios, 2, &m, ZV_SPREAD_PAD, 0);
    zv_graphics_draw_rect(&g, 0, 0, 60, 60);
    zv_graphics_end_fill(&g);
    zv_graphics_render(&g, &c);
    CHECK(near_pixel(at(&s, 0, 30), 0xFFFF0000u, 8) && near_pixel(at(&s, 59, 30), 0xFF0000FFu, 8) && near_pixel(at(&s, 30, 30), 0xFF800080u, 8));
    zv_graphics_release(&g);
    zv_canvas_release(&c);
    zv_surface_release(&s);
}

static void test_sprites(void)
{
    ZvSurface s;
    zv_surface_init(&s, NULL, 80, 80);
    ZvStage stage;
    zv_stage_init(&stage, &s, NULL);
    stage.background = 0xFF202020u;
    ZvSprite a, b, mask;
    zv_sprite_init(&a, NULL);
    zv_sprite_init(&b, NULL);
    zv_sprite_init(&mask, NULL);
    zv_graphics_begin_fill(&a.graphics, 0xFF0000, 1);
    zv_graphics_draw_rect(&a.graphics, 0, 0, 20, 20);
    zv_graphics_end_fill(&a.graphics);
    zv_graphics_begin_fill(&b.graphics, 0x00FF00, 1);
    zv_graphics_draw_rect(&b.graphics, -10, -10, 20, 20);
    zv_graphics_end_fill(&b.graphics);
    CHECK(zv_sprite_add_child(&stage.root, &a));
    CHECK(zv_sprite_add_child(&a, &b));
    zv_sprite_set_position(&a, 10, 10);
    zv_sprite_set_position(&b, 30, 30);
    ZvBounds r = zv_stage_render(&stage);
    CHECK(r.min_x == 0 && r.max_x == 80); /* first frame: everything */
    CHECK(at(&s, 15, 15) == 0xFFFF0000u);
    CHECK(at(&s, 45, 45) == 0xFF00FF00u); /* b at 10 + 30 */
    CHECK(at(&s, 70, 70) == 0xFF202020u);
    /* nothing changed: nothing redrawn */
    r = zv_stage_render(&stage);
    CHECK(zv_bounds_is_empty(&r));
    /* move b: the dirty rectangle covers old and new places only */
    zv_sprite_set_position(&b, 50, 50);
    r = zv_stage_render(&stage);
    CHECK(r.min_x >= 28 && r.max_x <= 72 && r.min_y >= 28);
    CHECK(at(&s, 45, 45) == 0xFF202020u && at(&s, 65, 65) == 0xFF00FF00u && at(&s, 15, 15) == 0xFFFF0000u);
    /* alpha and a rotation through the matrix */
    zv_sprite_set_alpha(&b, 0.5f);
    zv_stage_render(&stage);
    CHECK(near_pixel(at(&s, 65, 65), zv_blend_over(0xFF202020u, 0x80008000u), 1));
    zv_sprite_set_rotation(&a, 90);
    zv_stage_render(&stage);
    /* a's square 0..20 rotated 90 degrees around (10,10) lands at x in -10..10 */
    CHECK(at(&s, 5, 15) == 0xFFFF0000u && at(&s, 15, 15) == 0xFF202020u);
    zv_sprite_set_rotation(&a, 0);
    zv_sprite_set_alpha(&b, 1.0f);
    /* a mask: only the masked part of a shows */
    zv_graphics_begin_fill(&mask.graphics, 0xFFFFFF, 1);
    zv_graphics_draw_rect(&mask.graphics, 10, 10, 10, 10);
    zv_graphics_end_fill(&mask.graphics);
    zv_sprite_set_mask(&a, &mask);
    zv_stage_invalidate_all(&stage);
    zv_stage_render(&stage);
    CHECK(at(&s, 15, 15) == 0xFFFF0000u && at(&s, 25, 25) == 0xFF202020u);
    CHECK(at(&s, 65, 65) == 0xFF202020u); /* b is a child of a: masked too */
    zv_sprite_set_mask(&a, NULL);
    /* cacheAsBitmap draws the same thing */
    ZvSurface plain;
    zv_surface_init(&plain, NULL, 80, 80);
    zv_stage_invalidate_all(&stage);
    zv_stage_render(&stage);
    memcpy(plain.pixels, s.pixels, sizeof(ZvPixel) * 6400);
    zv_sprite_set_cache_as_bitmap(&a, true);
    zv_stage_invalidate_all(&stage);
    zv_stage_render(&stage);
    CHECK(a.cache_valid && a.cache.pixels != NULL);
    CHECK(same_surface(&plain, &s, 1));
    zv_stage_render(&stage);
    CHECK(same_surface(&plain, &s, 1));
    /* blend modes: add on an overlapping sprite */
    zv_sprite_set_cache_as_bitmap(&a, false);
    zv_sprite_set_position(&b, 5, 5);
    zv_sprite_set_blend(&b, ZV_BLEND_ADD);
    zv_stage_invalidate_all(&stage);
    zv_stage_render(&stage);
    CHECK(at(&s, 15, 15) == 0xFFFFFF00u);
    zv_sprite_set_visible(&b, false);
    zv_stage_render(&stage);
    CHECK(at(&s, 15, 15) == 0xFFFF0000u);
    CHECK(zv_sprite_remove_child(&a, &b));
    CHECK(!zv_sprite_remove_child(&a, &b));
    zv_surface_release(&plain);
    zv_sprite_release(&mask);
    zv_sprite_release(&b);
    zv_sprite_release(&a);
    zv_stage_release(&stage);
    zv_surface_release(&s);
}

int main(void)
{
    test_graphics();
    test_sprites();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
