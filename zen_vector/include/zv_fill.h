#ifndef ZV_FILL_H
#define ZV_FILL_H

/* Filling a polyline with a prepared paint: rasterizer coverage times paint
   colours, blended source-over on the surface. */

#include "zv_paint.h"
#include "zv_raster.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* Fills poly (device coordinates) on surface, clipped to clip (NULL: the
       whole surface), with the colours of paint. The rasterizer is optional
       scratch state to reuse between fills. Returns false when memory runs
       out. */
    bool zv_fill_polyline(ZvSurface *surface, const ZvPolyline *poly, ZvFillRule rule, ZvPaintContext *paint, const ZvBounds *clip,
                          ZvRasterizer *rasterizer);

#ifdef __cplusplus
}
#endif

#endif /* ZV_FILL_H */
