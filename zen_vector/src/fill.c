#include "zv_fill.h"
#include "zv_internal.h"

typedef struct
{
    ZvSurface *surface;
    ZvPaintContext *paint;
} PaintFill;

static ZvPixel *row(const PaintFill *f, int y, int x)
{
    return f->surface->pixels + (size_t)y * (size_t)f->surface->stride + (size_t)x;
}

static void paint_run(void *context, int y, int x, int length, uint32_t coverage)
{
    PaintFill *f = context;
    ZvPixel *span = f->paint->span;
    while (length > 0)
    {
        int n = length < f->paint->span_capacity ? length : f->paint->span_capacity;
        zv_paint_span(f->paint, y, x, n, span);
        zv_span_pixels_cover(row(f, y, x), span, (size_t)n, coverage);
        x += n;
        length -= n;
    }
}

static void paint_mask(void *context, int y, int x, int length, const uint8_t *coverage)
{
    PaintFill *f = context;
    ZvPixel *span = f->paint->span;
    while (length > 0)
    {
        int n = length < f->paint->span_capacity ? length : f->paint->span_capacity;
        zv_paint_span(f->paint, y, x, n, span);
        zv_span_pixels_mask(row(f, y, x), span, (size_t)n, coverage);
        x += n;
        length -= n;
        coverage += n;
    }
}

bool zv_fill_polyline(ZvSurface *surface, const ZvPolyline *poly, ZvFillRule rule, ZvPaintContext *paint, const ZvBounds *clip, ZvRasterizer *rasterizer)
{
    if (paint->is_solid)
        return zv_fill_polyline_solid(surface, poly, rule, paint->solid, clip, rasterizer);

    int x0 = 0, y0 = 0, x1 = surface->width, y1 = surface->height;
    if (clip)
    {
        x0 = zv_clampi(zv_floor_int(clip->min_x), 0, surface->width);
        y0 = zv_clampi(zv_floor_int(clip->min_y), 0, surface->height);
        x1 = zv_clampi(zv_ceil_int(clip->max_x), 0, surface->width);
        y1 = zv_clampi(zv_ceil_int(clip->max_y), 0, surface->height);
    }
    if (x1 <= x0 || y1 <= y0)
        return true;

    int width = x1 - x0;
    int want = width < 256 ? width : 256;
    if (paint->span_capacity < want)
    {
        if (paint->span)
            paint->allocator->release(paint->allocator->context, paint->span);
        paint->span = paint->allocator->allocate(paint->allocator->context, (size_t)want * sizeof(ZvPixel));
        paint->span_capacity = paint->span ? want : 0;
        if (!paint->span)
            return false;
    }

    ZvRasterizer local;
    ZvRasterizer *r = rasterizer;
    if (!r)
    {
        zv_rasterizer_init(&local, surface->allocator);
        r = &local;
    }
    zv_rasterizer_set_clip(r, x0, y0, x1, y1);
    zv_rasterizer_add_polyline(r, poly);
    PaintFill f = {surface, paint};
    ZvSpanSink sink = {paint_run, paint_mask, &f};
    bool ok = zv_rasterizer_sweep(r, rule, &sink);
    if (r == &local)
        zv_rasterizer_release(&local);
    return ok;
}
