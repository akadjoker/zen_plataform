#ifndef ZV_GEOM_H
#define ZV_GEOM_H

/*
 * Geometry of zen_vector: affine matrices, paths, flattening and bounds.
 *
 * Coordinates are floats in user space. A matrix maps user space to device
 * space (pixels). Paths hold move, line, quadratic, cubic and close verbs for
 * any number of subpaths; flattening turns a path, transformed to device
 * space, into polylines whose distance to the true curves stays within a
 * tolerance given in pixels.
 */

#include "zv_pixel.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

    typedef struct
    {
        float x, y;
    } ZvPoint;

    /* Axis-aligned box, min inclusive, max inclusive. Empty when max < min. */
    typedef struct
    {
        float min_x, min_y, max_x, max_y;
    } ZvBounds;

    ZvBounds zv_bounds_empty(void);
    bool zv_bounds_is_empty(const ZvBounds *b);
    void zv_bounds_add(ZvBounds *b, float x, float y);
    ZvBounds zv_bounds_union(const ZvBounds *a, const ZvBounds *b);
    ZvBounds zv_bounds_intersect(const ZvBounds *a, const ZvBounds *b);

    /* 2x3 affine matrix with the Canvas 2D meaning:
         x' = a * x + c * y + e
         y' = b * x + d * y + f */
    typedef struct
    {
        float a, b, c, d, e, f;
    } ZvMatrix;

    ZvMatrix zv_matrix_identity(void);
    ZvMatrix zv_matrix_make(float a, float b, float c, float d, float e, float f);
    ZvMatrix zv_matrix_translation(float tx, float ty);
    ZvMatrix zv_matrix_scaling(float sx, float sy);
    ZvMatrix zv_matrix_rotation(float radians);

    /* Composition: the result applies inner first, then outer.
       zv_matrix_concat(outer, inner) applied to p == outer(inner(p)). */
    ZvMatrix zv_matrix_concat(const ZvMatrix *outer, const ZvMatrix *inner);

    /* Convenience: m = m * (translation / scaling / rotation), the Canvas
       translate, scale and rotate (the new operation applies first). */
    void zv_matrix_translate(ZvMatrix *m, float tx, float ty);
    void zv_matrix_scale(ZvMatrix *m, float sx, float sy);
    void zv_matrix_rotate(ZvMatrix *m, float radians);

    /* Returns false, leaving *out untouched, when the matrix is singular. */
    bool zv_matrix_invert(const ZvMatrix *m, ZvMatrix *out);
    float zv_matrix_determinant(const ZvMatrix *m);

    ZvPoint zv_matrix_apply(const ZvMatrix *m, ZvPoint p);
    ZvPoint zv_matrix_apply_vector(const ZvMatrix *m, ZvPoint v); /* no translation */

    bool zv_matrix_is_identity(const ZvMatrix *m);
    /* True when axes stay axis aligned (b == 0 and c == 0): rectangles stay
       rectangles. */
    bool zv_matrix_is_axis_aligned(const ZvMatrix *m);
    /* Largest factor by which the matrix can stretch a length (the larger
       singular value). It converts a tolerance in pixels to user space. */
    float zv_matrix_max_scale(const ZvMatrix *m);

    /* Bounds of a box after transformation (the box of the four corners). */
    ZvBounds zv_matrix_apply_bounds(const ZvMatrix *m, const ZvBounds *b);

    typedef enum
    {
        ZV_VERB_MOVE,  /* 1 point */
        ZV_VERB_LINE,  /* 1 point */
        ZV_VERB_QUAD,  /* 2 points: control, end */
        ZV_VERB_CUBIC, /* 3 points: control 1, control 2, end */
        ZV_VERB_CLOSE  /* 0 points */
    } ZvVerb;

    /* A path, growing through its allocator. The Canvas rules apply: a line or
       curve on a path without a current point starts a subpath at its own end
       point; after a close the next segment starts a new subpath at the point
       the closed subpath began. Every function that grows the path returns
       false when memory runs out and leaves the path as it was. */
    typedef struct
    {
        uint8_t *verbs;
        int verb_count, verb_capacity;
        ZvPoint *points;
        int point_count, point_capacity;
        const ZvAllocator *allocator;
        ZvPoint start;   /* first point of the current subpath */
        ZvPoint current; /* current point */
        bool has_current;
        bool after_close;
    } ZvPath;

    void zv_path_init(ZvPath *path, const ZvAllocator *allocator); /* NULL: default */
    void zv_path_release(ZvPath *path);
    void zv_path_clear(ZvPath *path); /* keeps the memory */
    bool zv_path_is_empty(const ZvPath *path);

    bool zv_path_move_to(ZvPath *path, float x, float y);
    bool zv_path_line_to(ZvPath *path, float x, float y);
    bool zv_path_quad_to(ZvPath *path, float cx, float cy, float x, float y);
    bool zv_path_cubic_to(ZvPath *path, float c1x, float c1y, float c2x, float c2y, float x, float y);
    bool zv_path_close(ZvPath *path);

    /* Appends src to dst, transformed by m when m is not NULL. */
    bool zv_path_append(ZvPath *dst, const ZvPath *src, const ZvMatrix *m);
    bool zv_path_copy(ZvPath *dst, const ZvPath *src);
    void zv_path_transform(ZvPath *path, const ZvMatrix *m);

    /* Tight bounds of the path (the curves themselves, not their control
       points). Empty for a path without points. */
    ZvBounds zv_path_bounds(const ZvPath *path);
    /* Bounds of all the points, control points included. Cheaper. */
    ZvBounds zv_path_control_bounds(const ZvPath *path);

    /* A polyline per subpath. Points of all the contours are in one array. */
    typedef struct
    {
        int first;  /* index of the first point */
        int count;  /* points in this contour */
        bool closed;
    } ZvContour;

    typedef struct
    {
        ZvPoint *points;
        int point_count, point_capacity;
        ZvContour *contours;
        int contour_count, contour_capacity;
        const ZvAllocator *allocator;
    } ZvPolyline;

    void zv_polyline_init(ZvPolyline *poly, const ZvAllocator *allocator);
    void zv_polyline_release(ZvPolyline *poly);
    void zv_polyline_clear(ZvPolyline *poly);
    bool zv_polyline_add_contour(ZvPolyline *poly, bool closed);
    bool zv_polyline_add_point(ZvPolyline *poly, float x, float y);
    ZvBounds zv_polyline_bounds(const ZvPolyline *poly);

    /* Default flattening tolerance in pixels: the distance between a curve and
       its polyline never exceeds this. 0.05 was chosen from data (FASE3.md):
       coarser values visibly shrink curves against the browser, finer ones
       gain nothing. */
#define ZV_FLATTEN_TOLERANCE 0.05f

    /* Flattens path, transformed by m (NULL for identity), into out, appending
       to it. Each curve is cut in the number of pieces that keeps the chord
       error within tolerance (Wang's formula, so the bound is guaranteed, not
       estimated). Subpaths with a single point are kept as one-point contours
       (round caps draw dots there). Returns false when memory runs out. */
    bool zv_path_flatten(const ZvPath *path, const ZvMatrix *m, float tolerance, ZvPolyline *out);

    /* Number of line segments the flattener uses for one curve. Exposed for
       the tests and the benchmark. */
    int zv_quad_segments(ZvPoint p0, ZvPoint p1, ZvPoint p2, float tolerance);
    int zv_cubic_segments(ZvPoint p0, ZvPoint p1, ZvPoint p2, ZvPoint p3, float tolerance);

    ZvPoint zv_quad_eval(ZvPoint p0, ZvPoint p1, ZvPoint p2, float t);
    ZvPoint zv_cubic_eval(ZvPoint p0, ZvPoint p1, ZvPoint p2, ZvPoint p3, float t);

#ifdef __cplusplus
}
#endif

#endif /* ZV_GEOM_H */
