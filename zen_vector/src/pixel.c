#include "zv_pixel.h"

#include <stdlib.h>
#include <string.h>

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
        for (size_t i = 0; i < count; i++)
            dst[i] = src;
        return;
    }
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

bool zv_surface_load(ZvSurface *surface, const Framebuffer *framebuffer)
{
    if (!surface || !framebuffer || !surface->pixels || !framebuffer->pixels || surface->width != framebuffer->width ||
        surface->height != framebuffer->height)
        return false;
    for (int y = 0; y < surface->height; y++)
    {
        const uint32_t *src = framebuffer->pixels + (size_t)y * (size_t)framebuffer->stride;
        ZvPixel *dst = surface->pixels + (size_t)y * (size_t)surface->stride;
        for (int x = 0; x < surface->width; x++)
            dst[x] = zv_premultiply(src[x]);
    }
    return true;
}

bool zv_surface_store(const ZvSurface *surface, Framebuffer *framebuffer)
{
    if (!surface || !framebuffer || !surface->pixels || !framebuffer->pixels || surface->width != framebuffer->width ||
        surface->height != framebuffer->height)
        return false;
    for (int y = 0; y < surface->height; y++)
    {
        const ZvPixel *src = surface->pixels + (size_t)y * (size_t)surface->stride;
        uint32_t *dst = framebuffer->pixels + (size_t)y * (size_t)framebuffer->stride;
        for (int x = 0; x < surface->width; x++)
            dst[x] = zv_unpremultiply(src[x]);
    }
    return true;
}
