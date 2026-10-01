#include "zv_pixel.h"

#include <stdlib.h>
#include <string.h>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#define ZV_SSE2 1
#include <emmintrin.h>
#elif defined(__ARM_NEON) || defined(__aarch64__)
#define ZV_NEON 1
#include <arm_neon.h>
#endif


static void *default_allocate(void *context, size_t size)
{
    (void)context;
    return malloc(size);
}

static void default_release(void *context, void *memory)
{
    (void)context;
    free(memory);
}

const ZvAllocator *zv_default_allocator(void)
{
    static const ZvAllocator allocator = {default_allocate, default_release, NULL};
    return &allocator;
}

ZvPixel zv_premultiply(uint32_t straight)
{
    uint32_t a = straight >> 24;
    if (a == 255u)
        return straight;
    if (a == 0u)
        return 0u;
    uint32_t r = zv_div255(((straight >> 16) & 0xFFu) * a);
    uint32_t g = zv_div255(((straight >> 8) & 0xFFu) * a);
    uint32_t b = zv_div255((straight & 0xFFu) * a);
    return a << 24 | r << 16 | g << 8 | b;
}

static uint32_t unpremultiply_channel(uint32_t c, uint32_t a)
{
    uint32_t v = (c * 255u + a / 2u) / a;
    return v > 255u ? 255u : v;
}

uint32_t zv_unpremultiply(ZvPixel pixel)
{
    uint32_t a = pixel >> 24;
    if (a == 255u)
        return pixel;
    if (a == 0u)
        return 0u;
    uint32_t r = unpremultiply_channel((pixel >> 16) & 0xFFu, a);
    uint32_t g = unpremultiply_channel((pixel >> 8) & 0xFFu, a);
    uint32_t b = unpremultiply_channel(pixel & 0xFFu, a);
    return a << 24 | r << 16 | g << 8 | b;
}

void zv_span_solid(ZvPixel *dst, size_t count, ZvPixel src)
{
    uint32_t alpha = src >> 24;
    if (alpha == 0u)
        return;
    if (alpha == 255u)
    {
#ifdef ZV_SSE2
        __m128i v = _mm_set1_epi32((int)src);
        while (count >= 4)
        {
            _mm_storeu_si128((__m128i *)dst, v);
            dst += 4;
            count -= 4;
        }
#endif
        for (size_t i = 0; i < count; i++)
            dst[i] = src;
        return;
    }
#ifdef ZV_SSE2
    {
        uint16_t inv = (uint16_t)(255u - alpha);
        __m128i inverse = _mm_set1_epi16((short)inv);
        __m128i s = _mm_set1_epi32((int)src);
        __m128i zero = _mm_setzero_si128();
        __m128i round = _mm_set1_epi16(128);
        while (count >= 4)
        {
            __m128i d = _mm_loadu_si128((const __m128i *)dst);
            __m128i lo = _mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), inverse);
            __m128i hi = _mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), inverse);
            lo = _mm_add_epi16(lo, round);
            hi = _mm_add_epi16(hi, round);
            lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
            hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
            _mm_storeu_si128((__m128i *)dst, _mm_add_epi8(_mm_packus_epi16(lo, hi), s));
            dst += 4;
            count -= 4;
        }
    }
#elif defined(ZV_NEON)
    {
        uint8x8_t inverse = vdup_n_u8((uint8_t)(255u - alpha));
        uint32x2_t s = vdup_n_u32(src);
        while (count >= 2)
        {
            uint8x8_t d = vreinterpret_u8_u32(vld1_u32(dst));
            uint16x8_t m = vmull_u8(d, inverse);
            m = vaddq_u16(m, vdupq_n_u16(128));
            m = vaddq_u16(m, vshrq_n_u16(m, 8));
            uint8x8_t r = vshrn_n_u16(m, 8);
            vst1_u32(dst, vadd_u32(vreinterpret_u32_u8(r), s));
            dst += 2;
            count -= 2;
        }
    }
#endif
    for (size_t i = 0; i < count; i++)
        dst[i] = zv_blend_over(dst[i], src);
}

void zv_span_solid_cover(ZvPixel *dst, size_t count, ZvPixel src, uint32_t cover)
{
    if (cover == 0u)
        return;
    if (cover >= 255u)
    {
        zv_span_solid(dst, count, src);
        return;
    }
    zv_span_solid(dst, count, zv_scale(src, cover));
}

void zv_span_cover(ZvPixel *dst, size_t count, ZvPixel src, const uint8_t *cover)
{
    if ((src >> 24) == 0u)
        return;
#ifdef ZV_SSE2
    {
        /* per pixel: scaled = src * k / 255; dst = dst * (255 - scaled.a) / 255 + scaled */
        __m128i zero = _mm_setzero_si128();
        __m128i round = _mm_set1_epi16(128);
        __m128i s = _mm_set1_epi32((int)src);
        __m128i s_lo = _mm_unpacklo_epi8(s, zero); /* 2 pixels as 16-bit */
        while (count >= 4)
        {
            uint32_t k0 = cover[0], k1 = cover[1], k2 = cover[2], k3 = cover[3];
            if ((k0 | k1 | k2 | k3) == 0u)
            {
                dst += 4;
                cover += 4;
                count -= 4;
                continue;
            }
            __m128i kk = _mm_set_epi16((short)k3, (short)k3, (short)k3, (short)k3, (short)k2, (short)k2, (short)k2, (short)k2);
            __m128i kl = _mm_set_epi16((short)k1, (short)k1, (short)k1, (short)k1, (short)k0, (short)k0, (short)k0, (short)k0);
            __m128i sl = _mm_mullo_epi16(s_lo, kl);
            __m128i sh = _mm_mullo_epi16(s_lo, kk);
            sl = _mm_add_epi16(sl, round);
            sh = _mm_add_epi16(sh, round);
            sl = _mm_srli_epi16(_mm_add_epi16(sl, _mm_srli_epi16(sl, 8)), 8);
            sh = _mm_srli_epi16(_mm_add_epi16(sh, _mm_srli_epi16(sh, 8)), 8);
            __m128i scaled = _mm_packus_epi16(sl, sh);
            /* inverse alpha per pixel, broadcast to the 4 channels */
            __m128i a = _mm_srli_epi32(scaled, 24);
            __m128i inv = _mm_sub_epi32(_mm_set1_epi32(255), a);
            inv = _mm_or_si128(inv, _mm_slli_epi32(inv, 16));
            __m128i d = _mm_loadu_si128((const __m128i *)dst);
            __m128i lo = _mm_mullo_epi16(_mm_unpacklo_epi8(d, zero), _mm_unpacklo_epi32(inv, inv));
            __m128i hi = _mm_mullo_epi16(_mm_unpackhi_epi8(d, zero), _mm_unpackhi_epi32(inv, inv));
            lo = _mm_add_epi16(lo, round);
            hi = _mm_add_epi16(hi, round);
            lo = _mm_srli_epi16(_mm_add_epi16(lo, _mm_srli_epi16(lo, 8)), 8);
            hi = _mm_srli_epi16(_mm_add_epi16(hi, _mm_srli_epi16(hi, 8)), 8);
            _mm_storeu_si128((__m128i *)dst, _mm_add_epi8(_mm_packus_epi16(lo, hi), scaled));
            dst += 4;
            cover += 4;
            count -= 4;
        }
    }
#endif
    for (size_t i = 0; i < count; i++)
    {
        uint32_t k = cover[i];
        if (k == 0u)
            continue;
        dst[i] = zv_blend_over(dst[i], k == 255u ? src : zv_scale(src, k));
    }
}

bool zv_surface_init(ZvSurface *surface, const ZvAllocator *allocator, int width, int height)
{
    if (!surface)
        return false;
    memset(surface, 0, sizeof *surface);
    if (width <= 0 || height <= 0)
        return false;
    if (!allocator)
        allocator = zv_default_allocator();

    size_t w = (size_t)width;
    size_t h = (size_t)height;
    if (w > SIZE_MAX / sizeof(ZvPixel) / h)
        return false;
    size_t bytes = w * h * sizeof(ZvPixel);
    ZvPixel *pixels = allocator->allocate(allocator->context, bytes);
    if (!pixels)
        return false;
    memset(pixels, 0, bytes);

    surface->pixels = pixels;
    surface->width = width;
    surface->height = height;
    surface->stride = width;
    surface->allocator = allocator;
    return true;
}

void zv_surface_release(ZvSurface *surface)
{
    if (!surface)
        return;
    if (surface->pixels && surface->allocator)
        surface->allocator->release(surface->allocator->context, surface->pixels);
    memset(surface, 0, sizeof *surface);
}

bool zv_surface_load_pixels(ZvSurface *surface, const uint32_t *pixels, int width, int height, int stride)
{
    if (!surface || !pixels || !surface->pixels || surface->width != width || surface->height != height || stride < width)
        return false;
    for (int y = 0; y < height; y++)
    {
        const uint32_t *src = pixels + (size_t)y * (size_t)stride;
        ZvPixel *dst = surface->pixels + (size_t)y * (size_t)surface->stride;
        for (int x = 0; x < width; x++)
            dst[x] = zv_premultiply(src[x]);
    }
    return true;
}

bool zv_surface_store_pixels(const ZvSurface *surface, uint32_t *pixels, int width, int height, int stride)
{
    if (!surface || !pixels || !surface->pixels || surface->width != width || surface->height != height || stride < width)
        return false;
    for (int y = 0; y < height; y++)
    {
        const ZvPixel *src = surface->pixels + (size_t)y * (size_t)surface->stride;
        uint32_t *dst = pixels + (size_t)y * (size_t)stride;
        for (int x = 0; x < width; x++)
            dst[x] = zv_unpremultiply(src[x]);
    }
    return true;
}

ZvSurface zv_surface_wrap(uint32_t *pixels, int width, int height, int stride)
{
    ZvSurface s;
    s.pixels = pixels;
    s.width = width;
    s.height = height;
    s.stride = stride;
    s.allocator = NULL;
    return s;
}
