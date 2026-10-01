#ifndef ZV_PIXEL_H
#define ZV_PIXEL_H

/*
 * Pixel core of zen_vector.
 *
 * zen_vector depends on nothing but C11 and libm. The internal pixel is RGBA8
 * with premultiplied alpha, 0xAARRGGBB in a uint32_t (B, G, R, A in memory on
 * little endian: the ARGB8888 of SDL, the BGRA of OpenGL and Direct3D, the
 * Framebuffer of zen_platform), with every colour channel already multiplied
 * by alpha. A valid premultiplied pixel has each colour channel less than or
 * equal to its alpha; every blending function assumes that, and the result is
 * then valid too. Conversion from and to straight alpha is explicit:
 * zv_surface_load_pixels and zv_surface_store_pixels (zv_platform.h wraps them
 * for the zen_platform Framebuffer).
 *
 * To show a surface: SDL_UpdateTexture on an SDL_PIXELFORMAT_ARGB8888 texture,
 * or glTexImage2D with GL_BGRA / GL_UNSIGNED_BYTE; with the surface's alpha
 * use premultiplied blending (GL_ONE, GL_ONE_MINUS_SRC_ALPHA). An opaque
 * surface needs no conversion at all: its pixels are plain 0xFFRRGGBB.
 *
 * All the arithmetic is exact integer arithmetic, rounding to nearest.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef uint32_t ZvPixel;

    /* round(x / 255), exact for 0 <= x <= 65025 (255 * 255). */
    static inline uint32_t zv_div255(uint32_t x)
    {
        x += 128u;
        return (x + (x >> 8)) >> 8;
    }

    /* Source-over of a premultiplied src onto a premultiplied dst, all four
       channels, alpha included. Over an opaque destination the result stays
       opaque. Two channels are processed per multiplication. */
    static inline ZvPixel zv_blend_over(ZvPixel dst, ZvPixel src)
    {
        uint32_t inverse = 255u - (src >> 24);
        uint32_t rb = (dst & 0x00FF00FFu) * inverse + 0x00800080u;
        rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        uint32_t ag = ((dst >> 8) & 0x00FF00FFu) * inverse + 0x00800080u;
        ag = (ag + ((ag >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
        return src + (rb | ag);
    }

    /* Multiplies the four channels of a premultiplied pixel by k / 255,
       0 <= k <= 255. This is how a coverage applies to a source colour. */
    static inline ZvPixel zv_scale(ZvPixel pixel, uint32_t k)
    {
        uint32_t rb = (pixel & 0x00FF00FFu) * k + 0x00800080u;
        rb = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
        uint32_t ag = ((pixel >> 8) & 0x00FF00FFu) * k + 0x00800080u;
        ag = (ag + ((ag >> 8) & 0x00FF00FFu)) & 0xFF00FF00u;
        return rb | ag;
    }

    /* Straight 0xAARRGGBB to premultiplied. Alpha 0 gives 0: the colour of a
       fully transparent pixel is lost. */
    ZvPixel zv_premultiply(uint32_t straight);

    /* Premultiplied to straight 0xAARRGGBB. Alpha 0 gives 0. A round trip is
       exact for alpha 255; for lower alpha it can move a channel by at most
       255 / (2 * alpha) + 0.5. */
    uint32_t zv_unpremultiply(ZvPixel pixel);

    /* Spans: horizontal runs of destination pixels blended with one source.
       The source is premultiplied and valid. */

    /* Source-over of src over count pixels. Fast paths: an opaque source is a
       plain fill, a fully transparent one touches nothing. */
    void zv_span_solid(ZvPixel *dst, size_t count, ZvPixel src);

    /* Same with one coverage 0..255 for the whole run (larger values count as
       255). Coverage 0 touches nothing and 255 is zv_span_solid. */
    void zv_span_solid_cover(ZvPixel *dst, size_t count, ZvPixel src, uint32_t cover);

    /* Same with one coverage per pixel, the scanline of a rasterizer. */
    void zv_span_cover(ZvPixel *dst, size_t count, ZvPixel src, const uint8_t *cover);

    /* Memory is provided by the caller through an allocator, never behind your
       back. A NULL allocator means zv_default_allocator(), spelled out. */
    typedef struct
    {
        void *(*allocate)(void *context, size_t size);
        void (*release)(void *context, void *memory);
        void *context;
    } ZvAllocator;

    const ZvAllocator *zv_default_allocator(void); /* malloc and free */

    typedef struct
    {
        ZvPixel *pixels;
        int width, height;
        int stride; /* pixels per row, >= width */
        const ZvAllocator *allocator;
    } ZvSurface;

    /* Allocates width x height pixels, all transparent, in one allocation.
       Returns false, leaving *surface zeroed, for sizes that are not positive,
       whose byte size overflows, or when the allocator fails. */
    bool zv_surface_init(ZvSurface *surface, const ZvAllocator *allocator, int width, int height);
    void zv_surface_release(ZvSurface *surface);

    /* Converts between straight 0xAARRGGBB pixels (width x height, stride in
       pixels) and a surface of the same size. Return false when the sizes
       differ or a pointer is NULL. */
    bool zv_surface_load_pixels(ZvSurface *surface, const uint32_t *pixels, int width, int height, int stride);
    bool zv_surface_store_pixels(const ZvSurface *surface, uint32_t *pixels, int width, int height, int stride);

    /* Wraps external premultiplied pixels (an SDL surface locked, a texture
       staging buffer) as a surface that owns nothing: release does nothing. */
    ZvSurface zv_surface_wrap(uint32_t *pixels, int width, int height, int stride);

#ifdef __cplusplus
}
#endif

#endif /* ZV_PIXEL_H */
