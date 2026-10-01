#include "zv_geom.h"
#include "zv_internal.h"

#include <float.h>

/* Bounds */

ZvBounds zv_bounds_empty(void)
{
    ZvBounds b = {FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX};
    return b;
}

bool zv_bounds_is_empty(const ZvBounds *b)
{
    return b->max_x < b->min_x || b->max_y < b->min_y;
}

void zv_bounds_add(ZvBounds *b, float x, float y)
{
    if (x < b->min_x)
        b->min_x = x;
    if (x > b->max_x)
        b->max_x = x;
    if (y < b->min_y)
        b->min_y = y;
    if (y > b->max_y)
        b->max_y = y;
}

ZvBounds zv_bounds_union(const ZvBounds *a, const ZvBounds *b)
{
    if (zv_bounds_is_empty(a))
        return *b;
    if (zv_bounds_is_empty(b))
        return *a;
    ZvBounds r = *a;
    zv_bounds_add(&r, b->min_x, b->min_y);
    zv_bounds_add(&r, b->max_x, b->max_y);
    return r;
}

ZvBounds zv_bounds_intersect(const ZvBounds *a, const ZvBounds *b)
{
    ZvBounds r;
    r.min_x = a->min_x > b->min_x ? a->min_x : b->min_x;
    r.min_y = a->min_y > b->min_y ? a->min_y : b->min_y;
    r.max_x = a->max_x < b->max_x ? a->max_x : b->max_x;
    r.max_y = a->max_y < b->max_y ? a->max_y : b->max_y;
    return r;
}

/* Matrix */

ZvMatrix zv_matrix_identity(void)
{
    ZvMatrix m = {1, 0, 0, 1, 0, 0};
    return m;
}

ZvMatrix zv_matrix_make(float a, float b, float c, float d, float e, float f)
{
    ZvMatrix m = {a, b, c, d, e, f};
    return m;
}

ZvMatrix zv_matrix_translation(float tx, float ty)
{
    ZvMatrix m = {1, 0, 0, 1, tx, ty};
    return m;
}

ZvMatrix zv_matrix_scaling(float sx, float sy)
{
    ZvMatrix m = {sx, 0, 0, sy, 0, 0};
    return m;
}

ZvMatrix zv_matrix_rotation(float radians)
{
    float c = cosf(radians);
    float s = sinf(radians);
    ZvMatrix m = {c, s, -s, c, 0, 0};
    return m;
}

ZvMatrix zv_matrix_concat(const ZvMatrix *o, const ZvMatrix *i)
{
    ZvMatrix m;
    m.a = o->a * i->a + o->c * i->b;
    m.b = o->b * i->a + o->d * i->b;
    m.c = o->a * i->c + o->c * i->d;
    m.d = o->b * i->c + o->d * i->d;
    m.e = o->a * i->e + o->c * i->f + o->e;
    m.f = o->b * i->e + o->d * i->f + o->f;
    return m;
}

void zv_matrix_translate(ZvMatrix *m, float tx, float ty)
{
    m->e += m->a * tx + m->c * ty;
    m->f += m->b * tx + m->d * ty;
}

void zv_matrix_scale(ZvMatrix *m, float sx, float sy)
{
    m->a *= sx;
    m->b *= sx;
    m->c *= sy;
    m->d *= sy;
}

void zv_matrix_rotate(ZvMatrix *m, float radians)
{
    ZvMatrix r = zv_matrix_rotation(radians);
    *m = zv_matrix_concat(m, &r);
}

float zv_matrix_determinant(const ZvMatrix *m)
{
    return m->a * m->d - m->b * m->c;
}

bool zv_matrix_invert(const ZvMatrix *m, ZvMatrix *out)
{
    float det = zv_matrix_determinant(m);
    if (det == 0.0f || !isfinite(det))
        return false;
    float inv = 1.0f / det;
    ZvMatrix r;
    r.a = m->d * inv;
    r.b = -m->b * inv;
    r.c = -m->c * inv;
    r.d = m->a * inv;
    r.e = -(r.a * m->e + r.c * m->f);
    r.f = -(r.b * m->e + r.d * m->f);
    if (!isfinite(r.a) || !isfinite(r.b) || !isfinite(r.c) || !isfinite(r.d) || !isfinite(r.e) || !isfinite(r.f))
        return false;
    *out = r;
    return true;
}

ZvPoint zv_matrix_apply(const ZvMatrix *m, ZvPoint p)
{
    ZvPoint r;
    r.x = m->a * p.x + m->c * p.y + m->e;
    r.y = m->b * p.x + m->d * p.y + m->f;
    return r;
}

ZvPoint zv_matrix_apply_vector(const ZvMatrix *m, ZvPoint v)
{
    ZvPoint r;
    r.x = m->a * v.x + m->c * v.y;
    r.y = m->b * v.x + m->d * v.y;
    return r;
}

bool zv_matrix_is_identity(const ZvMatrix *m)
{
    return m->a == 1.0f && m->b == 0.0f && m->c == 0.0f && m->d == 1.0f && m->e == 0.0f && m->f == 0.0f;
}

bool zv_matrix_is_axis_aligned(const ZvMatrix *m)
{
    return m->b == 0.0f && m->c == 0.0f;
}

float zv_matrix_max_scale(const ZvMatrix *m)
{
    /* Larger singular value of [a c; b d]. */
    float s = m->a * m->a + m->b * m->b + m->c * m->c + m->d * m->d;
    float det = zv_matrix_determinant(m);
    float disc = s * s - 4.0f * det * det;
    if (disc < 0.0f)
        disc = 0.0f;
    float sigma2 = 0.5f * (s + sqrtf(disc));
    return sqrtf(sigma2);
}

ZvBounds zv_matrix_apply_bounds(const ZvMatrix *m, const ZvBounds *b)
{
    if (zv_bounds_is_empty(b))
        return *b;
    ZvBounds r = zv_bounds_empty();
    ZvPoint corners[4] = {{b->min_x, b->min_y}, {b->max_x, b->min_y}, {b->min_x, b->max_y}, {b->max_x, b->max_y}};
    for (int i = 0; i < 4; i++)
    {
        ZvPoint p = zv_matrix_apply(m, corners[i]);
        zv_bounds_add(&r, p.x, p.y);
    }
    return r;
}

/* Path */

void zv_path_init(ZvPath *path, const ZvAllocator *allocator)
{
    memset(path, 0, sizeof *path);
    path->allocator = allocator ? allocator : zv_default_allocator();
}

void zv_path_release(ZvPath *path)
{
    if (!path)
        return;
    if (path->verbs)
        path->allocator->release(path->allocator->context, path->verbs);
    if (path->points)
        path->allocator->release(path->allocator->context, path->points);
    const ZvAllocator *allocator = path->allocator;
    memset(path, 0, sizeof *path);
    path->allocator = allocator;
}

void zv_path_clear(ZvPath *path)
{
    path->verb_count = 0;
    path->point_count = 0;
    path->has_current = false;
    path->after_close = false;
}

bool zv_path_is_empty(const ZvPath *path)
{
    return path->verb_count == 0;
}

static bool reserve(ZvPath *path, int verbs, int points)
{
    if (!zv_grow(path->allocator, (void **)&path->verbs, &path->verb_capacity, path->verb_count, path->verb_count + verbs, sizeof(uint8_t)))
        return false;
    return zv_grow(path->allocator, (void **)&path->points, &path->point_capacity, path->point_count, path->point_count + points, sizeof(ZvPoint));
}

static void push_verb(ZvPath *path, ZvVerb verb)
{
    path->verbs[path->verb_count++] = (uint8_t)verb;
}

static void push_point(ZvPath *path, float x, float y)
{
    path->points[path->point_count].x = x;
    path->points[path->point_count].y = y;
    path->point_count++;
}

bool zv_path_move_to(ZvPath *path, float x, float y)
{
    if (!reserve(path, 1, 1))
        return false;
    push_verb(path, ZV_VERB_MOVE);
    push_point(path, x, y);
    path->start.x = path->current.x = x;
    path->start.y = path->current.y = y;
    path->has_current = true;
    path->after_close = false;
    return true;
}

/* Before a segment: without a current point the segment starts at its own
   end; after a close it starts a fresh subpath at the start point. Returns
   the number of verbs and points that will be needed for that start. */
static bool begin_segment(ZvPath *path, float x, float y, int verbs, int points)
{
    if (!path->has_current)
    {
        if (!reserve(path, verbs + 1, points + 1))
            return false;
        push_verb(path, ZV_VERB_MOVE);
        push_point(path, x, y);
        path->start.x = x;
        path->start.y = y;
        path->has_current = true;
        path->after_close = false;
        return true;
    }
    if (path->after_close)
    {
        if (!reserve(path, verbs + 1, points + 1))
            return false;
        push_verb(path, ZV_VERB_MOVE);
        push_point(path, path->start.x, path->start.y);
        path->after_close = false;
        return true;
    }
    return reserve(path, verbs, points);
}

bool zv_path_line_to(ZvPath *path, float x, float y)
{
    if (!begin_segment(path, x, y, 1, 1))
        return false;
    push_verb(path, ZV_VERB_LINE);
    push_point(path, x, y);
    path->current.x = x;
    path->current.y = y;
    return true;
}

bool zv_path_quad_to(ZvPath *path, float cx, float cy, float x, float y)
{
    if (!begin_segment(path, cx, cy, 1, 2))
        return false;
    push_verb(path, ZV_VERB_QUAD);
    push_point(path, cx, cy);
    push_point(path, x, y);
    path->current.x = x;
    path->current.y = y;
    return true;
}

bool zv_path_cubic_to(ZvPath *path, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    if (!begin_segment(path, c1x, c1y, 1, 3))
        return false;
    push_verb(path, ZV_VERB_CUBIC);
    push_point(path, c1x, c1y);
    push_point(path, c2x, c2y);
    push_point(path, x, y);
    path->current.x = x;
    path->current.y = y;
    return true;
}

bool zv_path_close(ZvPath *path)
{
    if (!path->has_current || path->after_close)
        return true;
    if (!reserve(path, 1, 0))
        return false;
    push_verb(path, ZV_VERB_CLOSE);
    path->current = path->start;
    path->after_close = true;
    return true;
}

static int verb_points(ZvVerb verb)
{
    switch (verb)
    {
    case ZV_VERB_MOVE:
    case ZV_VERB_LINE:
        return 1;
    case ZV_VERB_QUAD:
        return 2;
    case ZV_VERB_CUBIC:
        return 3;
    default:
        return 0;
    }
}

bool zv_path_append(ZvPath *dst, const ZvPath *src, const ZvMatrix *m)
{
    if (src->verb_count == 0)
        return true;
    if (!reserve(dst, src->verb_count, src->point_count))
        return false;
    int p = 0;
    for (int v = 0; v < src->verb_count; v++)
    {
        ZvVerb verb = (ZvVerb)src->verbs[v];
        int n = verb_points(verb);
        push_verb(dst, verb);
        for (int k = 0; k < n; k++)
        {
            ZvPoint pt = src->points[p++];
            if (m)
                pt = zv_matrix_apply(m, pt);
            push_point(dst, pt.x, pt.y);
        }
        switch (verb)
        {
        case ZV_VERB_MOVE:
            dst->start = dst->points[dst->point_count - 1];
            dst->current = dst->start;
            dst->has_current = true;
            dst->after_close = false;
            break;
        case ZV_VERB_CLOSE:
            dst->current = dst->start;
            dst->after_close = true;
            break;
        default:
            dst->current = dst->points[dst->point_count - 1];
            dst->after_close = false;
            break;
        }
    }
    return true;
}

bool zv_path_copy(ZvPath *dst, const ZvPath *src)
{
    zv_path_clear(dst);
    return zv_path_append(dst, src, NULL);
}

void zv_path_transform(ZvPath *path, const ZvMatrix *m)
{
    for (int i = 0; i < path->point_count; i++)
        path->points[i] = zv_matrix_apply(m, path->points[i]);
    path->start = zv_matrix_apply(m, path->start);
    path->current = zv_matrix_apply(m, path->current);
}

ZvBounds zv_path_control_bounds(const ZvPath *path)
{
    ZvBounds b = zv_bounds_empty();
    for (int i = 0; i < path->point_count; i++)
        zv_bounds_add(&b, path->points[i].x, path->points[i].y);
    return b;
}

/* Curve evaluation and extrema */

ZvPoint zv_quad_eval(ZvPoint p0, ZvPoint p1, ZvPoint p2, float t)
{
    float u = 1.0f - t;
    float a = u * u, b = 2.0f * u * t, c = t * t;
    ZvPoint r;
    r.x = a * p0.x + b * p1.x + c * p2.x;
    r.y = a * p0.y + b * p1.y + c * p2.y;
    return r;
}

ZvPoint zv_cubic_eval(ZvPoint p0, ZvPoint p1, ZvPoint p2, ZvPoint p3, float t)
{
    float u = 1.0f - t;
    float a = u * u * u, b = 3.0f * u * u * t, c = 3.0f * u * t * t, d = t * t * t;
    ZvPoint r;
    r.x = a * p0.x + b * p1.x + c * p2.x + d * p3.x;
    r.y = a * p0.y + b * p1.y + c * p2.y + d * p3.y;
    return r;
}

/* Adds the extrema of one coordinate of a quadratic, inside (0, 1). */
static void quad_extrema(float p0, float p1, float p2, float *ts, int *count)
{
    float denom = p0 - 2.0f * p1 + p2;
    if (denom == 0.0f)
        return;
    float t = (p0 - p1) / denom;
    if (t > 0.0f && t < 1.0f)
        ts[(*count)++] = t;
}

static void cubic_extrema(float p0, float p1, float p2, float p3, float *ts, int *count)
{
    /* derivative: 3 (a t^2 + b t + c) */
    float a = -p0 + 3.0f * p1 - 3.0f * p2 + p3;
    float b = 2.0f * (p0 - 2.0f * p1 + p2);
    float c = p1 - p0;
    if (a == 0.0f)
    {
        if (b != 0.0f)
        {
            float t = -c / b;
            if (t > 0.0f && t < 1.0f)
                ts[(*count)++] = t;
        }
        return;
    }
    float disc = b * b - 4.0f * a * c;
    if (disc < 0.0f)
        return;
    float s = sqrtf(disc);
    float t1 = (-b + s) / (2.0f * a);
    float t2 = (-b - s) / (2.0f * a);
    if (t1 > 0.0f && t1 < 1.0f)
        ts[(*count)++] = t1;
    if (t2 > 0.0f && t2 < 1.0f)
        ts[(*count)++] = t2;
}

ZvBounds zv_path_bounds(const ZvPath *path)
{
    ZvBounds b = zv_bounds_empty();
    ZvPoint current = {0, 0};
    int p = 0;
    for (int v = 0; v < path->verb_count; v++)
    {
        ZvVerb verb = (ZvVerb)path->verbs[v];
        switch (verb)
        {
        case ZV_VERB_MOVE:
        case ZV_VERB_LINE:
            current = path->points[p++];
            zv_bounds_add(&b, current.x, current.y);
            break;
        case ZV_VERB_QUAD:
        {
            ZvPoint c = path->points[p++];
            ZvPoint e = path->points[p++];
            zv_bounds_add(&b, e.x, e.y);
            float ts[2];
            int n = 0;
            quad_extrema(current.x, c.x, e.x, ts, &n);
            quad_extrema(current.y, c.y, e.y, ts, &n);
            for (int i = 0; i < n; i++)
            {
                ZvPoint q = zv_quad_eval(current, c, e, ts[i]);
                zv_bounds_add(&b, q.x, q.y);
            }
            current = e;
            break;
        }
        case ZV_VERB_CUBIC:
        {
            ZvPoint c1 = path->points[p++];
            ZvPoint c2 = path->points[p++];
            ZvPoint e = path->points[p++];
            zv_bounds_add(&b, e.x, e.y);
            float ts[4];
            int n = 0;
            cubic_extrema(current.x, c1.x, c2.x, e.x, ts, &n);
            cubic_extrema(current.y, c1.y, c2.y, e.y, ts, &n);
            for (int i = 0; i < n; i++)
            {
                ZvPoint q = zv_cubic_eval(current, c1, c2, e, ts[i]);
                zv_bounds_add(&b, q.x, q.y);
            }
            current = e;
            break;
        }
        case ZV_VERB_CLOSE:
            break;
        }
    }
    return b;
}

/* Polyline */

void zv_polyline_init(ZvPolyline *poly, const ZvAllocator *allocator)
{
    memset(poly, 0, sizeof *poly);
    poly->allocator = allocator ? allocator : zv_default_allocator();
}

void zv_polyline_release(ZvPolyline *poly)
{
    if (!poly)
        return;
    if (poly->points)
        poly->allocator->release(poly->allocator->context, poly->points);
    if (poly->contours)
        poly->allocator->release(poly->allocator->context, poly->contours);
    const ZvAllocator *allocator = poly->allocator;
    memset(poly, 0, sizeof *poly);
    poly->allocator = allocator;
}

void zv_polyline_clear(ZvPolyline *poly)
{
    poly->point_count = 0;
    poly->contour_count = 0;
}

bool zv_polyline_add_contour(ZvPolyline *poly, bool closed)
{
    if (!zv_grow(poly->allocator, (void **)&poly->contours, &poly->contour_capacity, poly->contour_count, poly->contour_count + 1, sizeof(ZvContour)))
        return false;
    ZvContour *c = &poly->contours[poly->contour_count++];
    c->first = poly->point_count;
    c->count = 0;
    c->closed = closed;
    return true;
}

bool zv_polyline_add_point(ZvPolyline *poly, float x, float y)
{
    if (poly->contour_count == 0 && !zv_polyline_add_contour(poly, false))
        return false;
    if (!zv_grow(poly->allocator, (void **)&poly->points, &poly->point_capacity, poly->point_count, poly->point_count + 1, sizeof(ZvPoint)))
        return false;
    poly->points[poly->point_count].x = x;
    poly->points[poly->point_count].y = y;
    poly->point_count++;
    poly->contours[poly->contour_count - 1].count++;
    return true;
}

ZvBounds zv_polyline_bounds(const ZvPolyline *poly)
{
    ZvBounds b = zv_bounds_empty();
    for (int i = 0; i < poly->point_count; i++)
        zv_bounds_add(&b, poly->points[i].x, poly->points[i].y);
    return b;
}

/* Flattening */

#define MAX_SEGMENTS 4096

static float second_difference(ZvPoint p0, ZvPoint p1, ZvPoint p2)
{
    float dx = p0.x - 2.0f * p1.x + p2.x;
    float dy = p0.y - 2.0f * p1.y + p2.y;
    return sqrtf(dx * dx + dy * dy);
}

static int segments_from_bound(float bound, float tolerance)
{
    if (!(tolerance > 0.0f))
        tolerance = ZV_FLATTEN_TOLERANCE;
    if (!isfinite(bound))
        return MAX_SEGMENTS;
    float n = ceilf(sqrtf(bound / tolerance));
    if (!(n >= 1.0f))
        return 1;
    if (n > (float)MAX_SEGMENTS)
        return MAX_SEGMENTS;
    return (int)n;
}

/* Wang's formula: n >= sqrt(k * max |second difference| / tolerance) with
   k = degree * (degree - 1) / 8 guarantees the chord error stays within the
   tolerance. */
int zv_quad_segments(ZvPoint p0, ZvPoint p1, ZvPoint p2, float tolerance)
{
    return segments_from_bound(0.25f * second_difference(p0, p1, p2), tolerance);
}

int zv_cubic_segments(ZvPoint p0, ZvPoint p1, ZvPoint p2, ZvPoint p3, float tolerance)
{
    float d1 = second_difference(p0, p1, p2);
    float d2 = second_difference(p1, p2, p3);
    return segments_from_bound(0.75f * (d1 > d2 ? d1 : d2), tolerance);
}

static bool flatten_quad(ZvPolyline *out, ZvPoint p0, ZvPoint p1, ZvPoint p2, float tolerance)
{
    int n = zv_quad_segments(p0, p1, p2, tolerance);
    for (int i = 1; i < n; i++)
    {
        ZvPoint q = zv_quad_eval(p0, p1, p2, (float)i / (float)n);
        if (!zv_polyline_add_point(out, q.x, q.y))
            return false;
    }
    return zv_polyline_add_point(out, p2.x, p2.y);
}

static bool flatten_cubic(ZvPolyline *out, ZvPoint p0, ZvPoint p1, ZvPoint p2, ZvPoint p3, float tolerance)
{
    int n = zv_cubic_segments(p0, p1, p2, p3, tolerance);
    for (int i = 1; i < n; i++)
    {
        ZvPoint q = zv_cubic_eval(p0, p1, p2, p3, (float)i / (float)n);
        if (!zv_polyline_add_point(out, q.x, q.y))
            return false;
    }
    return zv_polyline_add_point(out, p3.x, p3.y);
}

bool zv_path_flatten(const ZvPath *path, const ZvMatrix *m, float tolerance, ZvPolyline *out)
{
    ZvMatrix identity = zv_matrix_identity();
    if (!m)
        m = &identity;
    bool transform = !zv_matrix_is_identity(m);

    ZvPoint current = {0, 0};
    ZvPoint start = {0, 0};
    bool open = false;
    int p = 0;
    for (int v = 0; v < path->verb_count; v++)
    {
        ZvVerb verb = (ZvVerb)path->verbs[v];
        int n = verb_points(verb);
        ZvPoint pts[3];
        for (int k = 0; k < n; k++)
        {
            pts[k] = path->points[p++];
            if (transform)
                pts[k] = zv_matrix_apply(m, pts[k]);
        }
        switch (verb)
        {
        case ZV_VERB_MOVE:
            if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, pts[0].x, pts[0].y))
                return false;
            current = start = pts[0];
            open = true;
            break;
        case ZV_VERB_LINE:
            if (!open)
            {
                if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, start.x, start.y))
                    return false;
                open = true;
            }
            if (!zv_polyline_add_point(out, pts[0].x, pts[0].y))
                return false;
            current = pts[0];
            break;
        case ZV_VERB_QUAD:
            if (!open)
            {
                if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, start.x, start.y))
                    return false;
                open = true;
            }
            if (!flatten_quad(out, current, pts[0], pts[1], tolerance))
                return false;
            current = pts[1];
            break;
        case ZV_VERB_CUBIC:
            if (!open)
            {
                if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, start.x, start.y))
                    return false;
                open = true;
            }
            if (!flatten_cubic(out, current, pts[0], pts[1], pts[2], tolerance))
                return false;
            current = pts[2];
            break;
        case ZV_VERB_CLOSE:
            if (open)
            {
                out->contours[out->contour_count - 1].closed = true;
                current = start;
                open = false;
            }
            break;
        }
    }
    return true;
}
