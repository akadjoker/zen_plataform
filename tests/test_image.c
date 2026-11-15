/*
 * test_image.c - Framebuffer ownership (alloc/free/from_pixels) and BMP round-trip,
 * headless. No window backend needed: these are pure pixel operations.
 */
#include "platform.h"

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

#define TMP "test_image_tmp.bmp"

static void test_from_pixels(void)
{
    uint32_t px[2] = {0xFF112233, 0xFFAABBCC};
    Framebuffer fb;
    CHECK(framebuffer_from_pixels(&fb, px, 2, 1));
    CHECK(draw_get_pixel(&fb, 0, 0) == 0xFF112233);
    CHECK(draw_get_pixel(&fb, 1, 0) == 0xFFAABBCC);

    px[0] = 0; /* it must be a copy, not a view */
    CHECK(draw_get_pixel(&fb, 0, 0) == 0xFF112233);
    framebuffer_free(&fb);
    CHECK(fb.pixels == NULL && fb.width == 0);
}

static void test_bmp_roundtrip(void)
{
    Framebuffer a;
    CHECK(framebuffer_alloc(&a, 5, 3));
    draw_clear(&a, 0xFF204060);
    draw_pixel(&a, 0, 0, 0xFF010203, BLEND_NONE);
    draw_pixel(&a, 4, 2, 0xFFFFFFFF, BLEND_NONE);
    draw_pixel(&a, 2, 1, 0x80AB12CD, BLEND_NONE); /* alpha preserved through 32-bit BMP */

    CHECK(framebuffer_save_bmp(&a, TMP));

    Framebuffer b;
    CHECK(framebuffer_load_bmp(&b, TMP));
    CHECK(b.width == 5 && b.height == 3);

    bool same = true;
    for (int y = 0; y < a.height; y++)
        for (int x = 0; x < a.width; x++)
            if (draw_get_pixel(&a, x, y) != draw_get_pixel(&b, x, y))
                same = false;
    CHECK(same);

    framebuffer_free(&a);
    framebuffer_free(&b);
    remove(TMP);
}

/* A BMP whose height is -2147483648: the file says top-down and the height is the negated value, which does not fit
   an int (undefined behaviour; with UBSan halting, a failure of the test). It is a file to refuse, not to crash on. */
static void test_bmp_height_int_min(void)
{
    Framebuffer a;
    CHECK(framebuffer_alloc(&a, 3, 2));
    CHECK(framebuffer_save_bmp(&a, TMP));
    framebuffer_free(&a);

    size_t size = 0;
    uint8_t *data = file_read(TMP, &size);
    CHECK(data != NULL && size > 54);
    if (data)
    {
        const uint8_t min[4] = {0x00, 0x00, 0x00, 0x80}; /* little-endian INT_MIN */
        memcpy(data + 22, min, 4);
        CHECK(file_write(TMP, data, size));
        fs_free(data);
        Framebuffer b;
        CHECK(!framebuffer_load_bmp(&b, TMP));
    }
    remove(TMP);
}

static void test_bad_input(void)
{
    Framebuffer fb;
    CHECK(!framebuffer_alloc(&fb, 0, 10));
    CHECK(!framebuffer_load_bmp(&fb, "does_not_exist.bmp"));
}

int main(void)
{
    test_from_pixels();
    test_bmp_roundtrip();
    test_bad_input();
    test_bmp_height_int_min();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
