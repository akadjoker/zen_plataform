#include "bmpcmp.h"
#include "scene.h"

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

static void test_compare(void)
{
    ZvBitmap a, b, diff;
    CHECK(zv_bitmap_alloc(&a, 4, 3));
    CHECK(zv_bitmap_alloc(&b, 4, 3));
    zv_bitmap_clear(&a, 0xFF102030);
    zv_bitmap_clear(&b, 0xFF102030);

    ZvCompare r;
    CHECK(zv_compare(&a, &b, 0, &r, NULL));
    CHECK(r.pixels == 12 && r.over_tolerance == 0 && r.max_channel == 0);

    b.pixels[5] = 0xFF102333;
    CHECK(zv_compare(&a, &b, 0, &r, &diff));
    CHECK(r.over_tolerance == 1 && r.max_green == 3 && r.max_blue == 3 && r.max_red == 0 && r.max_alpha == 0 && r.max_channel == 3);
    CHECK(diff.width == 4 && diff.height == 3);
    CHECK(diff.pixels[0] == 0xFF000000u);
    CHECK((diff.pixels[5] >> 16 & 0xFF) > 0 && (diff.pixels[5] & 0xFFFF) == 0);
    zv_bitmap_free(&diff);

    CHECK(zv_compare(&a, &b, 2, &r, &diff));
    CHECK(r.over_tolerance == 1);
    zv_bitmap_free(&diff);
    CHECK(zv_compare(&a, &b, 3, &r, &diff));
    CHECK(r.over_tolerance == 0 && r.max_channel == 3);
    CHECK(diff.pixels[5] == 0xFF006000u);
    zv_bitmap_free(&diff);

    b.pixels[5] = 0x00102030;
    CHECK(zv_compare(&a, &b, 100, &r, NULL));
    CHECK(r.max_alpha == 255 && r.over_tolerance == 1);

    ZvBitmap c;
    CHECK(zv_bitmap_alloc(&c, 5, 3));
    CHECK(!zv_compare(&a, &c, 0, &r, NULL));
    CHECK(!zv_compare(NULL, &a, 0, &r, NULL));
    zv_bitmap_free(&a);
    zv_bitmap_free(&b);
    zv_bitmap_free(&c);
}

static void test_scene_errors(void)
{
    static const struct
    {
        const char *text;
        const char *needle;
    } cases[] = {
        {"", "no size"},
        {"# only a comment\n", "no size"},
        {"fillRect 0 0 1 1\n", "before size"},
        {"size 4\n", "size needs"},
        {"size 0 4\n", "size needs"},
        {"size 4 4\nsize 4 4\n", "only once"},
        {"size 4 4\nfillStyle red\n", "#rrggbb"},
        {"size 4 4\nfillStyle #12345\n", "#rrggbb"},
        {"size 4 4\nfillStyle #12345g\n", "#rrggbb"},
        {"size 4 4\nfillRect 1 2 3\n", "fillRect takes"},
        {"size 4 4\nfillRect 1 2 3 x\n", "not a number"},
        {"size 4 4\n\nfoo\n", "line 3: unknown command 'foo'"},
        {"size 4 4\nfill both\n", "nonzero or evenodd"},
        {"size 4 4\nmoveTo 1\n", "moveTo takes"},
        {"size 4 4\nlineCap flat\n", "unknown lineCap"},
        {"size 4 4\nfillText 1 1 a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a a\n", "too many"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        ZvBitmap fb = {0};
        char err[256];
        bool ok = zv_scene_run(cases[i].text, &fb, err, sizeof err);
        if (ok || strstr(err, cases[i].needle) == NULL)
            printf("  case %zu: ok=%d err='%s'\n", i, ok, err);
        CHECK(!ok);
        CHECK(strstr(err, cases[i].needle) != NULL);
        CHECK(fb.pixels == NULL);
    }
    CHECK(!zv_scene_run(NULL, NULL, NULL, 0));
}

static void test_scene_pixels(void)
{
    ZvBitmap fb;
    char err[256];
    CHECK(zv_scene_run("size 6 4\nfillStyle #ff0000\nfillRect 1 1 2 2\nfillStyle #00ff00\nfillRect 5 3 -2 -1\nfillRect 0 0 0 5\nfillRect -3 -3 5 5\n", &fb, err, sizeof err));
    CHECK(fb.width == 6 && fb.height == 4);
    CHECK(zv_bitmap_get(&fb, 0, 3) == 0x00000000u);
    CHECK(zv_bitmap_get(&fb, 2, 1) == 0xFFFF0000u && zv_bitmap_get(&fb, 1, 2) == 0xFFFF0000u && zv_bitmap_get(&fb, 2, 2) == 0xFFFF0000u);
    CHECK(zv_bitmap_get(&fb, 3, 2) == 0xFF00FF00u && zv_bitmap_get(&fb, 4, 2) == 0xFF00FF00u);
    CHECK(zv_bitmap_get(&fb, 3, 3) == 0x00000000u && zv_bitmap_get(&fb, 5, 3) == 0x00000000u && zv_bitmap_get(&fb, 5, 2) == 0x00000000u);
    CHECK(zv_bitmap_get(&fb, 0, 0) == 0xFF00FF00u && zv_bitmap_get(&fb, 1, 1) == 0xFF00FF00u);
    zv_bitmap_free(&fb);

    CHECK(zv_scene_run("size 2 2\n# comment\n   \n\tfillRect 0 0 2 2\r\n", &fb, err, sizeof err));
    CHECK(zv_bitmap_get(&fb, 1, 1) == 0xFF000000u);
    zv_bitmap_free(&fb);
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

static void test_reference(const char *name, int tolerance)
{
    char *text = scene_text(name);
    CHECK(text != NULL);
    ZvBitmap mine, ref;
    char err[256];
    CHECK(text && zv_scene_run(text, &mine, err, sizeof err));
    CHECK(load_reference(name, &ref));

    ZvCompare r;
    CHECK(zv_compare(&mine, &ref, tolerance, &r, NULL));
    if (r.over_tolerance != 0)
        printf("  %s: %lld pixels over tolerance %d, max channel %d\n", name, r.over_tolerance, tolerance, r.max_channel);
    CHECK(r.over_tolerance == 0);
    CHECK(r.max_channel == 0);
    zv_io_free(text);
    zv_bitmap_free(&mine);
    zv_bitmap_free(&ref);
}

static void test_detects_difference(void)
{
    ZvBitmap ref, mine;
    char err[256];
    CHECK(load_reference("rect_opaque", &ref));
    CHECK(zv_scene_run("size 64 48\nfillStyle #3366cc\nfillRect 8 8 33 24\n", &mine, err, sizeof err));
    ZvCompare r;
    CHECK(zv_compare(&mine, &ref, 0, &r, NULL));
    CHECK(r.over_tolerance == 24);
    zv_bitmap_free(&ref);
    zv_bitmap_free(&mine);

    CHECK(load_reference("rect_opaque", &ref));
    CHECK(zv_scene_run("size 64 48\nfillStyle #3366cd\nfillRect 8 8 32 24\n", &mine, err, sizeof err));
    CHECK(zv_compare(&mine, &ref, 0, &r, NULL));
    CHECK(r.over_tolerance == 32 * 24 && r.max_blue == 1);
    zv_bitmap_free(&ref);
    zv_bitmap_free(&mine);
}

int main(void)
{
    test_compare();
    test_scene_errors();
    test_scene_pixels();
    test_reference("rect_opaque", 0);
    test_detects_difference();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
