#ifndef ZV_PAINT_H
#define ZV_PAINT_H

/*
 * Paints: what colour each pixel of a fill gets.
 *
 * A paint is a description (solid colour, gradient, bitmap pattern). Before a
 * fill it is prepared for one device transform; the prepared paint then
 * produces premultiplied colours for runs of device pixels, which the fill
 * blends with the coverage from the rasterizer.
 *
 * Gradients: colours are interpolated with straight alpha, like the browser,
 * then premultiplied, through a lookup table. The radial gradient is the
 * Canvas two-circle one; the Flash radial with focalPointRatio f is the same
 * thing with the inner circle of radius 0 at (f, 0) in the gradient box.
 */

#include "zv_geom.h"

#ifdef __cplusplus
extern "C"
{
#endif

#define ZV_MAX_STOPS 16
#define ZV_GRADIENT_STEPS 1024

    typedef enum
    {
        ZV_PAINT_SOLID,
        ZV_PAINT_LINEAR,
        ZV_PAINT_RADIAL,
        ZV_PAINT_PATTERN
    } ZvPaintType;

    typedef enum
    {
        ZV_SPREAD_PAD,
        ZV_SPREAD_REFLECT,
        ZV_SPREAD_REPEAT
    } ZvSpread;

    typedef enum
    {
        ZV_FILTER_NEAREST,
        ZV_FILTER_BILINEAR
    } ZvFilter;

    typedef struct
    {
        float offset;   /* 0..1 */
        uint32_t color; /* straight 0xAARRGGBB */
    } ZvStop;

    typedef struct
    {
        ZvPaintType type;
        uint32_t color; /* solid: straight 0xAARRGGBB */
        uint32_t alpha; /* 0..255, multiplies everything (globalAlpha) */
        /* Gradients. Coordinates are in paint space, which matrix maps to
           user space. Linear: from (x0, y0) to (x1, y1). Radial: circle 0 at
           (x0, y0) radius r0, circle 1 at (x1, y1) radius r1. */
        float x0, y0, x1, y1, r0, r1;
        ZvStop stops[ZV_MAX_STOPS];
        int stop_count;
        ZvSpread spread;
        bool dither;
        /* Pattern: the bitmap (premultiplied) with its own matrix (bitmap
           pixels to user space), the sampling filter and whether it repeats
           in each direction. */
        const ZvSurface *bitmap;
        ZvFilter filter;
        bool repeat_x, repeat_y;
        ZvMatrix matrix; /* paint space to user space */
    } ZvPaint;

    ZvPaint zv_paint_solid(uint32_t straight_color);
    ZvPaint zv_paint_linear(float x0, float y0, float x1, float y1);
    ZvPaint zv_paint_radial(float x0, float y0, float r0, float x1, float y1, float r1);
    ZvPaint zv_paint_pattern(const ZvSurface *bitmap, bool repeat_x, bool repeat_y, ZvFilter filter);
    /* Adds a stop; stops are kept sorted by offset (stable). Returns false
       when the table is full or the offset is outside 0..1. */
    bool zv_paint_add_stop(ZvPaint *paint, float offset, uint32_t straight_color);

    /* A paint prepared for one transform, ready to produce colours. Memory
       for the lookup table and a span of colours comes from the allocator. */
    typedef struct
    {
        const ZvPaint *paint;
        const ZvAllocator *allocator;
        ZvPixel solid;      /* premultiplied, with alpha applied */
        bool is_solid;      /* the paint reduces to one colour */
        bool opaque;        /* every colour it produces is opaque */
        ZvMatrix inverse;   /* device to paint space */
        ZvPixel *table;     /* ZV_GRADIENT_STEPS premultiplied colours */
        uint16_t *table16;  /* same memory, 4 x 16 bits per entry, when dithering */
        /* linear: t = lx * x + ly * y + l0 in device space */
        float lx, ly, l0;
        /* radial, in paint space */
        float cdx, cdy, dr, a;
        bool radial_simple; /* concentric: t = (|p - c| - r0) / dr */
        ZvPixel *span;
        int span_capacity;
    } ZvPaintContext;

    /* user_to_device is the current transform of the fill. Returns false when
       memory runs out or the pattern bitmap is missing. */
    bool zv_paint_prepare(ZvPaintContext *ctx, const ZvPaint *paint, const ZvMatrix *user_to_device, const ZvAllocator *allocator);
    void zv_paint_release(ZvPaintContext *ctx);

    /* Writes the premultiplied colours of the device pixels (x..x+count-1, y)
       into out. */
    void zv_paint_span(const ZvPaintContext *ctx, int y, int x, int count, ZvPixel *out);

    /* Spans with a colour per pixel (the output of a paint). */
    void zv_span_pixels(ZvPixel *dst, const ZvPixel *src, size_t count);
    void zv_span_pixels_cover(ZvPixel *dst, const ZvPixel *src, size_t count, uint32_t cover);
    void zv_span_pixels_mask(ZvPixel *dst, const ZvPixel *src, size_t count, const uint8_t *cover);

#ifdef __cplusplus
}
#endif

#endif /* ZV_PAINT_H */
