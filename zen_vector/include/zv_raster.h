#ifndef ZV_RASTER_H
#define ZV_RASTER_H

/*
 * Scanline rasterizer with analytic anti-aliasing.
 *
 * Edges are accumulated as signed area and cover in sparse cells (one cell per
 * pixel an edge touches), in 24.8 fixed point. A sweep per scanline turns the
 * cells into coverage: runs of constant coverage between cells, and a mask of
 * per-pixel coverage where the cells are. Both fill rules come from the same
 * cells. Everything outside the clip box is discarded before it becomes a
 * cell, so the cost follows the visible edges, not the path.
 *
 * Coverage is exact for a single edge crossing a pixel; where edges of
 * opposite direction cross the same pixel the signed areas cancel instead of
 * being combined per sub-area. That is the approximation every accumulation
 * rasterizer makes (AGG, font-rs, stb_truetype) and it only shows on pixels
 * with two edges in them.
 */

#include "zv_geom.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        ZV_FILL_NONZERO,
        ZV_FILL_EVENODD
    } ZvFillRule;

    /* Where the coverage goes. solid gets runs of one coverage, mask gets a
       run of per-pixel coverage. The coverage is 1..255; pixels with 0 are
       never reported. The mask pointer is only valid during the call. */
    typedef struct
    {
        void (*solid)(void *context, int y, int x, int length, uint32_t coverage);
        void (*mask)(void *context, int y, int x, int length, const uint8_t *coverage);
        void *context;
    } ZvSpanSink;

    typedef struct
    {
        int x, y;
        int cover, area;
    } ZvCell;

    typedef struct
    {
        const ZvAllocator *allocator;
        ZvCell *cells;
        int cell_count, cell_capacity;
        ZvCell *sorted;
        int sorted_capacity;
        int *row_start; /* clip height + 1 entries once sorted */
        int row_capacity;
        uint8_t *mask;
        int mask_capacity;
        ZvCell current;
        int clip_x0, clip_y0, clip_x1, clip_y1; /* pixels, max exclusive */
        int min_y, max_y;                       /* rows touched, max inclusive */
        bool overflow;                           /* an allocation failed */
    } ZvRasterizer;

    void zv_rasterizer_init(ZvRasterizer *r, const ZvAllocator *allocator);
    void zv_rasterizer_release(ZvRasterizer *r);

    /* The clip box in pixels, max exclusive. Must be set before adding edges;
       it also resets the rasterizer. */
    void zv_rasterizer_set_clip(ZvRasterizer *r, int x0, int y0, int x1, int y1);
    /* Drops the edges, keeps the clip and the memory. */
    void zv_rasterizer_reset(ZvRasterizer *r);

    /* Adds one edge in device pixels. Edges may be given in any order; every
       contour must end where it starts for the fill to be well defined. */
    void zv_rasterizer_add_line(ZvRasterizer *r, float x0, float y0, float x1, float y1);
    /* Adds every contour of a polyline as a closed outline (fills always
       close contours). */
    void zv_rasterizer_add_polyline(ZvRasterizer *r, const ZvPolyline *poly);

    /* Sweeps the cells and reports the coverage to the sink, top to bottom,
       left to right. Returns false when an allocation failed while the edges
       were added (nothing is reported then). The edges stay until the next
       reset. */
    bool zv_rasterizer_sweep(ZvRasterizer *r, ZvFillRule rule, const ZvSpanSink *sink);

    /* Convenience: fills a polyline with one premultiplied colour on a
       surface, clipped to clip (NULL: the whole surface). */
    bool zv_fill_polyline_solid(ZvSurface *surface, const ZvPolyline *poly, ZvFillRule rule, ZvPixel color,
                                const ZvBounds *clip, ZvRasterizer *r);

#ifdef __cplusplus
}
#endif

#endif /* ZV_RASTER_H */
