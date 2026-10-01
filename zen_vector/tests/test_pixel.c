#include "zv_io.h"
#include "zv_pixel.h"

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

static uint32_t g_seed = 12345;

static uint32_t rnd(void)
{
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

/* Independent references: plain integer division, one channel at a time. */
static uint32_t ref_div255(uint32_t x)
{
    return (x + 127u) / 255u;
}

static uint32_t channel(uint32_t pixel, int shift)
{
    return (pixel >> shift) & 0xFFu;
}

static ZvPixel ref_blend(ZvPixel dst, ZvPixel src)
{
    uint32_t inverse = 255u - channel(src, 24);
    ZvPixel out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= (channel(src, shift) + ref_div255(channel(dst, shift) * inverse)) << shift;
    return out;
}

static ZvPixel ref_scale(ZvPixel pixel, uint32_t k)
{
    ZvPixel out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= ref_div255(channel(pixel, shift) * k) << shift;
    return out;
}

static bool is_valid(ZvPixel p)
{
    uint32_t a = channel(p, 24);
    return channel(p, 16) <= a && channel(p, 8) <= a && channel(p, 0) <= a;
}

static ZvPixel random_valid(void)
{
    uint32_t a = rnd() & 0xFFu;
    return a << 24 | (rnd() % (a + 1u)) << 16 | (rnd() % (a + 1u)) << 8 | (rnd() % (a + 1u));
}

static void test_div255(void)
{
    bool all = true;
    for (uint32_t x = 0; x <= 255u * 255u; x++)
    {
        if (zv_div255(x) != ref_div255(x))
            all = false;
    }
    CHECK(all);
    CHECK(zv_div255(0) == 0 && zv_div255(255) == 1 && zv_div255(127) == 0 && zv_div255(128) == 1 && zv_div255(255u * 255u) == 255);
}

static void test_premultiply(void)
{
    bool exact = true;
    for (uint32_t a = 0; a < 256; a++)
    {
        for (uint32_t c = 0; c < 256; c++)
        {
            uint32_t straight = a << 24 | c << 16 | c << 8 | c;
            ZvPixel p = zv_premultiply(straight);
            uint32_t expected_c = a == 0 ? 0 : ref_div255(c * a);
            if (channel(p, 24) != (a == 0 ? 0 : a) || channel(p, 16) != expected_c || channel(p, 8) != expected_c || channel(p, 0) != expected_c)
                exact = false;
            if (!is_valid(p))
                exact = false;
        }
    }
    CHECK(exact);

    CHECK(zv_premultiply(0x80FF8000u) == 0x80804000u);
    CHECK(zv_premultiply(0x00FFFFFFu) == 0u);
    CHECK(zv_premultiply(0xFF123456u) == 0xFF123456u);
    CHECK(zv_premultiply(0x01FFFFFFu) == 0x01010101u);

    CHECK(zv_unpremultiply(0x80804000u) == 0x80FF8000u);
    CHECK(zv_unpremultiply(0u) == 0u);
    CHECK(zv_unpremultiply(0xFF123456u) == 0xFF123456u);
    CHECK(zv_unpremultiply(0x01010101u) == 0x01FFFFFFu);
}

static void test_round_trip(void)
{
    bool opaque_identity = true;
    bool bound_holds = true;
    for (uint32_t a = 1; a < 256; a++)
    {
        for (uint32_t c = 0; c < 256; c++)
        {
            uint32_t straight = a << 24 | c << 16 | c << 8 | c;
            uint32_t back = zv_unpremultiply(zv_premultiply(straight));
            if (a == 255u && back != straight)
                opaque_identity = false;
            for (int shift = 0; shift < 24; shift += 8)
            {
                uint32_t got = channel(back, shift);
                uint32_t diff = got > c ? got - c : c - got;
                if (2u * a * diff > 255u + a)
                    bound_holds = false;
            }
            if (channel(back, 24) != a)
                bound_holds = false;
        }
    }
    CHECK(opaque_identity);
    CHECK(bound_holds);

    bool premul_stable = true;
    for (int i = 0; i < 200000; i++)
    {
        ZvPixel p = random_valid();
        if (zv_premultiply(zv_unpremultiply(p)) != p)
        {
            uint32_t a = channel(p, 24);
            /* going straight and back can only move a channel by one step */
            ZvPixel q = zv_premultiply(zv_unpremultiply(p));
            for (int shift = 0; shift < 24; shift += 8)
            {
                uint32_t x = channel(p, shift), y = channel(q, shift);
                if ((x > y ? x - y : y - x) > 1u || a == 0u)
                    premul_stable = false;
            }
        }
    }
    CHECK(premul_stable);
}

static void test_blend_cases(void)
{
    CHECK(zv_blend_over(0x00000000u, 0x00000000u) == 0x00000000u);
    CHECK(zv_blend_over(0xFF102030u, 0x00000000u) == 0xFF102030u);
    CHECK(zv_blend_over(0xFF102030u, 0xFF405060u) == 0xFF405060u);
    CHECK(zv_blend_over(0x00000000u, 0xFF405060u) == 0xFF405060u);
    CHECK(zv_blend_over(0x00000000u, 0x80402010u) == 0x80402010u);
    CHECK(zv_blend_over(0x80402010u, 0x80402010u) == ref_blend(0x80402010u, 0x80402010u));
    CHECK(zv_blend_over(0xFFFFFFFFu, 0x80000000u) == 0xFF7F7F7Fu);
    CHECK(zv_blend_over(0xFF000000u, 0x80800000u) == 0xFF800000u);
    CHECK(zv_blend_over(0xFF000000u, 0x01010101u) == 0xFF010101u);
    CHECK(zv_blend_over(0xFFFFFFFFu, 0xFEFEFEFEu) == 0xFFFFFFFFu);

    /* an opaque destination stays opaque, for every source alpha */
    bool opaque_stays = true;
    for (uint32_t a = 0; a < 256; a++)
    {
        ZvPixel out = zv_blend_over(0xFF336699u, a << 24 | (a / 3u) << 16 | (a / 2u) << 8 | a);
        if (channel(out, 24) != 255u)
            opaque_stays = false;
    }
    CHECK(opaque_stays);

    /* a transparent destination takes the source unchanged */
    bool transparent_takes = true;
    for (int i = 0; i < 100000; i++)
    {
        ZvPixel s = random_valid();
        if (zv_blend_over(0u, s) != s)
            transparent_takes = false;
    }
    CHECK(transparent_takes);
}

static void test_blend_exhaustive_lanes(void)
{
    /* Every destination channel value against every source alpha, in each of the
       four lanes, with random neighbours: a carry between lanes would show here. */
    bool exact = true;
    for (int shift = 0; shift < 32; shift += 8)
    {
        for (uint32_t d = 0; d < 256; d++)
        {
            for (uint32_t a = 0; a < 256; a++)
            {
                ZvPixel dst = (rnd() & ~(0xFFu << shift)) | d << shift;
                ZvPixel src = a << 24;
                if (zv_blend_over(dst, src) != ref_blend(dst, src))
                    exact = false;
            }
        }
    }
    CHECK(exact);
}

static void test_blend_random(void)
{
    bool exact = true;
    bool valid = true;
    for (int i = 0; i < 4000000; i++)
    {
        ZvPixel d = random_valid();
        ZvPixel s = random_valid();
        ZvPixel got = zv_blend_over(d, s);
        if (got != ref_blend(d, s))
            exact = false;
        if (!is_valid(got))
            valid = false;
    }
    CHECK(exact);
    CHECK(valid);

    static const uint32_t alphas[] = {0, 1, 2, 127, 128, 129, 253, 254, 255};
    bool edges = true;
    for (size_t i = 0; i < sizeof alphas / sizeof alphas[0]; i++)
    {
        for (size_t j = 0; j < sizeof alphas / sizeof alphas[0]; j++)
        {
            uint32_t da = alphas[i], sa = alphas[j];
            uint32_t dc[3] = {0, da, da / 2}, sc[3] = {0, sa, sa / 2};
            for (int x = 0; x < 3; x++)
            {
                for (int y = 0; y < 3; y++)
                {
                    ZvPixel d = da << 24 | dc[x] << 16 | dc[y] << 8 | dc[(x + y) % 3];
                    ZvPixel s = sa << 24 | sc[y] << 16 | sc[x] << 8 | sc[(x + 2 * y) % 3];
                    if (zv_blend_over(d, s) != ref_blend(d, s) || !is_valid(zv_blend_over(d, s)))
                        edges = false;
                }
            }
        }
    }
    CHECK(edges);
}

static void test_scale(void)
{
    bool exact = true;
    for (uint32_t k = 0; k < 256; k++)
    {
        for (int i = 0; i < 2000; i++)
        {
            ZvPixel p = random_valid();
            ZvPixel got = zv_scale(p, k);
            if (got != ref_scale(p, k) || !is_valid(got))
                exact = false;
        }
        if (zv_scale(0xFF804020u, 255) != 0xFF804020u || zv_scale(0xFF804020u, 0) != 0u)
            exact = false;
    }
    CHECK(exact);
}

#define GUARD 8
#define SENTINEL 0x5A5A5A5Au

static void check_guards(const ZvPixel *buffer, size_t count, bool *ok)
{
    for (size_t i = 0; i < GUARD; i++)
    {
        if (buffer[i] != SENTINEL || buffer[GUARD + count + i] != SENTINEL)
            *ok = false;
    }
}

static void test_spans(void)
{
    static const size_t counts[] = {0, 1, 2, 3, 4, 7, 8, 9, 15, 16, 17, 100, 1001};
    static const ZvPixel sources[] = {0x00000000u, 0xFF3366CCu, 0x80402010u, 0x01010101u, 0xFE80FE40u, 0xFF000000u};
    bool exact = true, guards = true;

    for (size_t c = 0; c < sizeof counts / sizeof counts[0]; c++)
    {
        size_t n = counts[c];
        ZvPixel *buffer = malloc((n + 2 * GUARD) * sizeof *buffer);
        ZvPixel *expected = malloc((n + 1) * sizeof *expected);
        uint8_t *cover = malloc(n + 1);
        for (size_t s = 0; s < sizeof sources / sizeof sources[0]; s++)
        {
            ZvPixel src = sources[s];
            for (size_t i = 0; i < n; i++)
            {
                buffer[GUARD + i] = random_valid();
                expected[i] = buffer[GUARD + i];
                cover[i] = (uint8_t)(i % 5 == 0 ? 0 : i % 5 == 1 ? 255
                                                                 : rnd());
            }
            for (size_t i = 0; i < GUARD; i++)
                buffer[i] = buffer[GUARD + n + i] = SENTINEL;

            for (size_t i = 0; i < n; i++)
                expected[i] = ref_blend(expected[i], src);
            zv_span_solid(buffer + GUARD, n, src);
            if (n && memcmp(buffer + GUARD, expected, n * sizeof *expected) != 0)
                exact = false;
            check_guards(buffer, n, &guards);

            static const uint32_t covers[] = {0, 1, 100, 254, 255, 256, 1000};
            for (size_t k = 0; k < sizeof covers / sizeof covers[0]; k++)
            {
                for (size_t i = 0; i < n; i++)
                    buffer[GUARD + i] = expected[i] = random_valid();
                uint32_t eff = covers[k] > 255u ? 255u : covers[k];
                for (size_t i = 0; i < n; i++)
                    expected[i] = eff == 0 ? expected[i] : ref_blend(expected[i], ref_scale(src, eff));
                zv_span_solid_cover(buffer + GUARD, n, src, covers[k]);
                if (n && memcmp(buffer + GUARD, expected, n * sizeof *expected) != 0)
                    exact = false;
                check_guards(buffer, n, &guards);
            }

            for (size_t i = 0; i < n; i++)
                buffer[GUARD + i] = expected[i] = random_valid();
            for (size_t i = 0; i < n; i++)
                expected[i] = cover[i] == 0 ? expected[i] : ref_blend(expected[i], ref_scale(src, cover[i]));
            zv_span_cover(buffer + GUARD, n, src, cover);
            if (n && memcmp(buffer + GUARD, expected, n * sizeof *expected) != 0)
                exact = false;
            check_guards(buffer, n, &guards);
        }
        free(buffer);
        free(expected);
        free(cover);
    }
    CHECK(exact);
    CHECK(guards);

    ZvPixel one[4] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    zv_span_solid(one, 4, 0xFFFFFFFFu);
    CHECK(one[0] == 0xFFFFFFFFu && one[3] == 0xFFFFFFFFu);
    uint8_t half[4] = {0, 255, 128, 0};
    ZvPixel dst[4] = {0xFF000000u, 0xFF000000u, 0xFF000000u, 0xFF000000u};
    zv_span_cover(dst, 4, 0xFFFFFFFFu, half);
    CHECK(dst[0] == 0xFF000000u && dst[1] == 0xFFFFFFFFu && dst[2] == 0xFF808080u && dst[3] == 0xFF000000u);
}

typedef struct
{
    int allocations, releases;
    size_t last_size;
    size_t limit; /* requests above this fail, like a real allocator running out */
    bool fail;
} Counting;

static void *counting_allocate(void *context, size_t size)
{
    Counting *c = context;
    c->allocations++;
    c->last_size = size;
    return c->fail || (c->limit && size > c->limit) ? NULL : malloc(size);
}

static void counting_release(void *context, void *memory)
{
    Counting *c = context;
    c->releases++;
    free(memory);
}

static void test_surface(void)
{
    Counting counts = {0};
    counts.limit = (size_t)1 << 30;
    ZvAllocator allocator = {counting_allocate, counting_release, &counts};
    ZvSurface s;

    CHECK(zv_surface_init(&s, &allocator, 7, 5));
    CHECK(counts.allocations == 1 && counts.last_size == 7u * 5u * 4u);
    CHECK(s.width == 7 && s.height == 5 && s.stride == 7 && s.allocator == &allocator);
    bool zeroed = true;
    for (int i = 0; i < 35; i++)
        zeroed = zeroed && s.pixels[i] == 0;
    CHECK(zeroed);
    zv_surface_release(&s);
    CHECK(counts.releases == 1 && s.pixels == NULL && s.width == 0);
    zv_surface_release(&s);
    CHECK(counts.releases == 1);

    counts.fail = true;
    CHECK(!zv_surface_init(&s, &allocator, 4, 4));
    CHECK(s.pixels == NULL);
    counts.fail = false;

    CHECK(!zv_surface_init(&s, &allocator, 0, 4));
    CHECK(!zv_surface_init(&s, &allocator, 4, -1));
    CHECK(!zv_surface_init(&s, &allocator, 0x7FFFFFFF, 0x7FFFFFFF));
    CHECK(s.pixels == NULL && s.width == 0);
    CHECK(!zv_surface_init(&s, &allocator, 0x7FFFFFFF, 0x3FFFFFFF));
    CHECK(s.pixels == NULL);
    CHECK(!zv_surface_init(&s, &allocator, 20000, 20000));
    CHECK(s.pixels == NULL);
    CHECK(!zv_surface_init(NULL, &allocator, 4, 4));

    CHECK(zv_surface_init(&s, NULL, 3, 3) && s.allocator == zv_default_allocator());
    zv_surface_release(&s);
    zv_surface_release(NULL);
}

static void test_conversion(void)
{
    ZvBitmap fb;
    CHECK(zv_bitmap_alloc(&fb, 256, 4));
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 256; x++)
            fb.pixels[y * fb.stride + x] = 0xFFu << 24 | (uint32_t)x << 16 | (uint32_t)(255 - x) << 8 | (uint32_t)((x * 7 + y) & 0xFF);
    }
    ZvSurface s;
    CHECK(zv_surface_init(&s, NULL, 256, 4));
    CHECK(zv_surface_load_pixels(&s, fb.pixels, fb.width, fb.height, fb.stride));
    ZvBitmap back;
    CHECK(zv_bitmap_alloc(&back, 256, 4));
    CHECK(zv_surface_store_pixels(&s, back.pixels, back.width, back.height, back.stride));
    CHECK(memcmp(back.pixels, fb.pixels, 256u * 4u * 4u) == 0);

    fb.pixels[0] = 0x80FF8000u;
    fb.pixels[1] = 0x00ABCDEFu;
    CHECK(zv_surface_load_pixels(&s, fb.pixels, fb.width, fb.height, fb.stride));
    CHECK(s.pixels[0] == 0x80804000u && s.pixels[1] == 0u);
    CHECK(zv_surface_store_pixels(&s, back.pixels, back.width, back.height, back.stride));
    CHECK(back.pixels[0] == 0x80FF8000u && back.pixels[1] == 0u);

    ZvBitmap wrong;
    CHECK(zv_bitmap_alloc(&wrong, 255, 4));
    CHECK(!zv_surface_load_pixels(&s, wrong.pixels, wrong.width, wrong.height, wrong.stride));
    CHECK(!zv_surface_store_pixels(&s, wrong.pixels, wrong.width, wrong.height, wrong.stride));
    CHECK(!zv_surface_load_pixels(NULL, fb.pixels, 256, 4, 256));
    CHECK(!zv_surface_load_pixels(&s, NULL, 256, 4, 256));
    CHECK(!zv_surface_load_pixels(&s, fb.pixels, 256, 4, 100)); /* stride below the width */
    ZvSurface wrapped = zv_surface_wrap(fb.pixels, 256, 4, fb.stride);
    CHECK(wrapped.pixels == fb.pixels && wrapped.allocator == NULL);
    zv_surface_release(&wrapped); /* owns nothing: fb stays valid */
    CHECK(fb.pixels[2] != 0);
    zv_bitmap_free(&wrong);

    /* a framebuffer with a larger stride than its width */
    ZvBitmap strided;
    CHECK(zv_bitmap_alloc(&strided, 40, 3));
    ZvBitmap view = {strided.pixels + 5, 30, 3, strided.stride};
    for (int y = 0; y < 3; y++)
    {
        for (int x = 0; x < 30; x++)
            view.pixels[y * view.stride + x] = 0xC0u << 24 | (uint32_t)(x * 8) << 16 | (uint32_t)(y * 60) << 8 | 0x10u;
    }
    ZvSurface s2;
    CHECK(zv_surface_init(&s2, NULL, 30, 3));
    CHECK(zv_surface_load_pixels(&s2, view.pixels, view.width, view.height, view.stride));
    bool consistent = true;
    for (int y = 0; y < 3; y++)
    {
        for (int x = 0; x < 30; x++)
        {
            if (s2.pixels[y * s2.stride + x] != zv_premultiply(view.pixels[y * view.stride + x]))
                consistent = false;
        }
    }
    CHECK(consistent);
    CHECK(strided.pixels[0] == 0u && strided.pixels[4] == 0u && strided.pixels[35] == 0u);
    zv_surface_release(&s2);
    zv_bitmap_free(&strided);

    zv_surface_release(&s);
    zv_bitmap_free(&fb);
    zv_bitmap_free(&back);
}

int main(void)
{
    test_div255();
    test_premultiply();
    test_round_trip();
    test_blend_cases();
    test_blend_exhaustive_lanes();
    test_blend_random();
    test_scale();
    test_spans();
    test_surface();
    test_conversion();

    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
