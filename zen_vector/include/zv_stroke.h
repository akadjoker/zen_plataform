#ifndef ZV_STROKE_H
#define ZV_STROKE_H

/*
 * Stroker: turns polylines into the outline of their stroke, to be filled
 * with the nonzero rule.
 *
 * The outline is built like the browser builds it: for each contour one
 * closed outline (open contour: left side forward, end cap, right side
 * backward, start cap) or two (closed contour: the left and the right
 * offsets, in opposite directions). Joins are built on the outer side of each
 * turn; the inner side goes through the vertex, so the outline overlaps
 * itself there and the nonzero fill removes the overlap.
 *
 * Stroking happens in the space the polyline is in. For the Canvas that is
 * user space: flatten there (with the tolerance divided by the largest scale
 * of the transform), stroke, then transform the outline to device space, so
 * a non-uniform scale gives a non-uniform stroke.
 */

#include "zv_geom.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        ZV_CAP_BUTT,
        ZV_CAP_ROUND,
        ZV_CAP_SQUARE
    } ZvLineCap;

    typedef enum
    {
        ZV_JOIN_MITER,
        ZV_JOIN_ROUND,
        ZV_JOIN_BEVEL
    } ZvLineJoin;

    typedef struct
    {
        float width;
        ZvLineCap cap;
        ZvLineJoin join;
        float miter_limit; /* ratio of the miter length to half the width, Canvas default 10 */
        const float *dashes; /* NULL or dash_count lengths, on/off/on..., all >= 0, not all 0 */
        int dash_count;
        float dash_offset;
    } ZvStrokeStyle;

    ZvStrokeStyle zv_stroke_style(float width);

    /* Splits every contour of in into dashes, appended to out as open
       contours. An odd dash count is repeated to make it even, like the
       Canvas. Returns false when memory runs out; with no dashes (NULL or all
       zero) copies in unchanged. */
    bool zv_dash_polyline(const ZvPolyline *in, const float *dashes, int dash_count, float offset, ZvPolyline *out);

    /* Appends the stroke outline of in to out. tolerance (same units as the
       polyline) bounds the error of round joins and caps. Dashes in the style
       are applied first. A contour with a single point, or whose points are
       all equal, gets a dot for round and square caps and nothing for butt.
       Returns false when memory runs out. */
    bool zv_stroke_polyline(const ZvPolyline *in, const ZvStrokeStyle *style, float tolerance, ZvPolyline *out);

#ifdef __cplusplus
}
#endif

#endif /* ZV_STROKE_H */
