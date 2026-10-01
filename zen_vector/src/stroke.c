#include "zv_stroke.h"
#include "zv_internal.h"

ZvStrokeStyle zv_stroke_style(float width)
{
    ZvStrokeStyle s;
    memset(&s, 0, sizeof s);
    s.width = width;
    s.cap = ZV_CAP_BUTT;
    s.join = ZV_JOIN_MITER;
    s.miter_limit = 10.0f;
    return s;
}

/* Dashes */

static float dash_total(const float *dashes, int count)
{
    float total = 0.0f;
    for (int i = 0; i < count; i++)
    {
        if (!(dashes[i] >= 0.0f) || !isfinite(dashes[i]))
            return -1.0f;
        total += dashes[i];
    }
    return total;
}

typedef struct
{
    const float *dashes;
    int count;  /* even */
    int index;  /* current dash entry */
    float left; /* length left in the current entry */
    bool on;
} DashState;

static void dash_start(DashState *d, const float *dashes, int count, float total, float offset)
{
    d->dashes = dashes;
    d->count = count;
    float pattern = total * (count & 1 ? 2.0f : 1.0f);
    if (!isfinite(offset))
        offset = 0.0f;
    offset = fmodf(offset, pattern);
    if (offset < 0.0f)
        offset += pattern;
    d->index = 0;
    d->on = true;
    d->left = dashes[0];
    /* skip whole entries covered by the offset */
    int guard = 0;
    while (offset > 0.0f && guard++ < 4 * count + 4)
    {
        if (offset >= d->left)
        {
            offset -= d->left;
            d->index = (d->index + 1) % (count & 1 ? 2 * count : count);
            d->left = dashes[d->index % count];
            d->on = !d->on;
        }
        else
        {
            d->left -= offset;
            offset = 0.0f;
        }
    }
}

static void dash_advance(DashState *d)
{
    int entries = d->count & 1 ? 2 * d->count : d->count;
    d->index = (d->index + 1) % entries;
    d->left = d->dashes[d->index % d->count];
    d->on = !d->on;
}

static bool dash_contour(const ZvPolyline *in, const ZvContour *c, const float *dashes, int count, float total, float offset, ZvPolyline *out)
{
    int n = c->count + (c->closed ? 1 : 0);
    if (c->count < 2)
        return true;
    DashState d;
    dash_start(&d, dashes, count, total, offset);
    const ZvPoint *pts = in->points + c->first;
    bool open = false;
    int first_contour = out->contour_count;
    bool started_on = d.on;
    if (d.on)
    {
        if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, pts[0].x, pts[0].y))
            return false;
        open = true;
    }
    for (int i = 0; i + 1 < n; i++)
    {
        ZvPoint a = pts[i];
        ZvPoint b = pts[(i + 1) % c->count];
        float dx = b.x - a.x, dy = b.y - a.y;
        float len = sqrtf(dx * dx + dy * dy);
        float pos = 0.0f;
        int guard = 0;
        while (len - pos > d.left && guard++ < 1000000)
        {
            pos += d.left;
            float t = pos / len;
            ZvPoint q = {a.x + dx * t, a.y + dy * t};
            if (d.on)
            {
                /* the dash ends here */
                if (!zv_polyline_add_point(out, q.x, q.y))
                    return false;
                open = false;
            }
            dash_advance(&d);
            if (d.on)
            {
                if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, q.x, q.y))
                    return false;
                open = true;
            }
            /* zero-length entries: skip them */
            int z = 0;
            while (d.left <= 0.0f && z++ < 2 * count)
            {
                if (d.on && open)
                {
                    open = false;
                }
                dash_advance(&d);
                if (d.on)
                {
                    if (!zv_polyline_add_contour(out, false) || !zv_polyline_add_point(out, q.x, q.y))
                        return false;
                    open = true;
                }
            }
        }
        d.left -= len - pos;
        if (d.on)
        {
            if (!zv_polyline_add_point(out, b.x, b.y))
                return false;
        }
    }
    /* a closed contour whose dash runs through the start point: merge the
       last dash into the first so the join at the start is drawn */
    if (c->closed && started_on && d.on && out->contour_count - first_contour >= 2)
    {
        ZvContour *first = &out->contours[first_contour];
        ZvContour *last = &out->contours[out->contour_count - 1];
        int first_count = first->count;
        int last_count = last->count;
        /* rotate: move the first contour's points after the last contour, dropping the shared point */
        int extra = first_count - 1;
        if (!zv_grow(out->allocator, (void **)&out->points, &out->point_capacity, out->point_count, out->point_count + extra, sizeof(ZvPoint)))
            return false;
        ZvPoint *p = out->points;
        for (int i = 1; i < first_count; i++)
            p[out->point_count + i - 1] = p[first->first + i];
        out->point_count += extra;
        last->count = last_count + extra;
        /* the first contour is now empty of meaning: keep its points but shrink it */
        first->count = 0;
    }
    return true;
}

bool zv_dash_polyline(const ZvPolyline *in, const float *dashes, int dash_count, float offset, ZvPolyline *out)
{
    float total = dash_count > 0 ? dash_total(dashes, dash_count) : 0.0f;
    if (dash_count <= 0 || total <= 0.0f)
    {
        for (int c = 0; c < in->contour_count; c++)
        {
            const ZvContour *k = &in->contours[c];
            if (!zv_polyline_add_contour(out, k->closed))
                return false;
            for (int i = 0; i < k->count; i++)
            {
                if (!zv_polyline_add_point(out, in->points[k->first + i].x, in->points[k->first + i].y))
                    return false;
            }
        }
        return true;
    }
    for (int c = 0; c < in->contour_count; c++)
    {
        if (!dash_contour(in, &in->contours[c], dashes, dash_count, total, offset, out))
            return false;
    }
    return true;
}

/* Outline */

typedef struct
{
    ZvPolyline *out;
    float hw;
    ZvLineCap cap;
    ZvLineJoin join;
    float miter_limit;
    float tolerance;
    bool ok;
} Stroker;

static void emit(Stroker *s, float x, float y)
{
    if (s->ok && !zv_polyline_add_point(s->out, x, y))
        s->ok = false;
}

static void contour(Stroker *s)
{
    if (s->ok && !zv_polyline_add_contour(s->out, true))
        s->ok = false;
}

/* Arc around c from angle a0 to a1 (radians, going the shorter signed way
   given), radius hw, without the end points. */
static void arc_points(Stroker *s, ZvPoint c, float a0, float sweep)
{
    float r = s->hw;
    float tol = s->tolerance;
    if (tol > r)
        tol = r;
    float step = tol >= r ? (float)M_PI : 2.0f * acosf(1.0f - tol / r);
    if (!(step > 1e-4f))
        step = 1e-4f;
    int n = (int)ceilf(fabsf(sweep) / step);
    if (n < 1)
        n = 1;
    if (n > 1024)
        n = 1024;
    for (int i = 1; i < n; i++)
    {
        float a = a0 + sweep * (float)i / (float)n;
        emit(s, c.x + r * cosf(a), c.y + r * sinf(a));
    }
}

/* Join on the outer side at vertex v, from the offset point of the previous
   segment (normal n0) to the offset point of the next (normal n1). The
   offset points are v + n0 * hw and v + n1 * hw. */
static void outer_join(Stroker *s, ZvPoint v, ZvPoint n0, ZvPoint n1)
{
    float hw = s->hw;
    ZvPoint p0 = {v.x + n0.x * hw, v.y + n0.y * hw};
    ZvPoint p1 = {v.x + n1.x * hw, v.y + n1.y * hw};
    emit(s, p0.x, p0.y);
    float dot = n0.x * n1.x + n0.y * n1.y;
    if (dot > 0.9999f)
    {
        emit(s, p1.x, p1.y);
        return;
    }
    switch (s->join)
    {
    case ZV_JOIN_MITER:
    {
        /* miter length over half width = 1 / cos(theta / 2) = sqrt(2 / (1 + dot)) */
        float d = 1.0f + dot;
        if (d > 1e-6f)
        {
            float ratio2 = 2.0f / d;
            if (ratio2 <= s->miter_limit * s->miter_limit)
            {
                float k = hw / d;
                emit(s, v.x + (n0.x + n1.x) * k, v.y + (n0.y + n1.y) * k);
            }
        }
        break;
    }
    case ZV_JOIN_ROUND:
    {
        float a0 = atan2f(n0.y, n0.x);
        float a1 = atan2f(n1.y, n1.x);
        float sweep = a1 - a0;
        while (sweep > (float)M_PI)
            sweep -= 2.0f * (float)M_PI;
        while (sweep < -(float)M_PI)
            sweep += 2.0f * (float)M_PI;
        arc_points(s, v, a0, sweep);
        break;
    }
    case ZV_JOIN_BEVEL:
        break;
    }
    emit(s, p1.x, p1.y);
}

static void cap(Stroker *s, ZvPoint end, ZvPoint dir, ZvPoint n)
{
    /* from end + n * hw around the end to end - n * hw, bulging along dir */
    float hw = s->hw;
    switch (s->cap)
    {
    case ZV_CAP_BUTT:
        emit(s, end.x + n.x * hw, end.y + n.y * hw);
        emit(s, end.x - n.x * hw, end.y - n.y * hw);
        break;
    case ZV_CAP_SQUARE:
        emit(s, end.x + n.x * hw, end.y + n.y * hw);
        emit(s, end.x + n.x * hw + dir.x * hw, end.y + n.y * hw + dir.y * hw);
        emit(s, end.x - n.x * hw + dir.x * hw, end.y - n.y * hw + dir.y * hw);
        emit(s, end.x - n.x * hw, end.y - n.y * hw);
        break;
    case ZV_CAP_ROUND:
    {
        emit(s, end.x + n.x * hw, end.y + n.y * hw);
        float a0 = atan2f(n.y, n.x);
        /* the arc goes through dir: from n to -n passing by dir */
        float cross = n.x * dir.y - n.y * dir.x;
        arc_points(s, end, a0, cross > 0.0f ? (float)M_PI : -(float)M_PI);
        emit(s, end.x - n.x * hw, end.y - n.y * hw);
        break;
    }
    }
}

static void dot(Stroker *s, ZvPoint c)
{
    float hw = s->hw;
    if (s->cap == ZV_CAP_BUTT)
        return;
    contour(s);
    if (s->cap == ZV_CAP_SQUARE)
    {
        emit(s, c.x - hw, c.y - hw);
        emit(s, c.x + hw, c.y - hw);
        emit(s, c.x + hw, c.y + hw);
        emit(s, c.x - hw, c.y + hw);
        return;
    }
    emit(s, c.x + hw, c.y);
    arc_points(s, c, 0.0f, 2.0f * (float)M_PI);
}

#define MAX_STACK 64

/* One side of the stroke, from vertex `from` to vertex `to` walking the
   points in the given direction. side = +1 uses the left normals, -1 the
   right ones (the normal is flipped so the same code serves both). */
static void walk_side(Stroker *s, const ZvPoint *pts, const ZvPoint *dirs, int count, bool closed, int side, bool forward)
{
    float hw = s->hw;
    int segs = closed ? count : count - 1;
    for (int k = 0; k < segs; k++)
    {
        int i = forward ? k : segs - 1 - k;
        ZvPoint d = dirs[i];
        if (!forward)
        {
            d.x = -d.x;
            d.y = -d.y;
        }
        ZvPoint n = {-d.y * (float)side, d.x * (float)side};
        ZvPoint a = forward ? pts[i] : pts[(i + 1) % count];
        ZvPoint b = forward ? pts[(i + 1) % count] : pts[i];
        bool first = k == 0;
        if (first)
            emit(s, a.x + n.x * hw, a.y + n.y * hw);
        /* the join at b with the next segment */
        bool has_next = k + 1 < segs || closed;
        if (!has_next)
        {
            emit(s, b.x + n.x * hw, b.y + n.y * hw);
            break;
        }
        int j = forward ? (i + 1) % segs : (i - 1 + segs) % segs;
        ZvPoint d2 = dirs[j];
        if (!forward)
        {
            d2.x = -d2.x;
            d2.y = -d2.y;
        }
        ZvPoint n2 = {-d2.y * (float)side, d2.x * (float)side};
        float cross = d.x * d2.y - d.y * d2.x;
        float turn = cross * (float)side;
        /* turn < 0: this side is the outer side of the turn */
        if (turn < -1e-7f)
        {
            outer_join(s, b, n, n2);
        }
        else if (turn > 1e-7f)
        {
            emit(s, b.x + n.x * hw, b.y + n.y * hw);
            emit(s, b.x, b.y);
            emit(s, b.x + n2.x * hw, b.y + n2.y * hw);
        }
        else
        {
            float dot = d.x * d2.x + d.y * d2.y;
            if (dot > 0.0f)
                emit(s, b.x + n.x * hw, b.y + n.y * hw);
            else
                outer_join(s, b, n, n2); /* reversal: a full turn-around */
        }
    }
}

static bool stroke_contour(Stroker *s, const ZvPolyline *in, const ZvContour *c)
{
    const ZvAllocator *alloc = in->allocator;
    /* drop repeated points */
    int n = c->count;
    if (n <= 0)
        return true;
    ZvPoint stack_pts[MAX_STACK], stack_dirs[MAX_STACK];
    ZvPoint *pts = stack_pts, *dirs = stack_dirs;
    if (n > MAX_STACK)
    {
        pts = alloc->allocate(alloc->context, sizeof(ZvPoint) * (size_t)n * 2);
        if (!pts)
            return false;
        dirs = pts + n;
    }
    int m = 0;
    for (int i = 0; i < n; i++)
    {
        ZvPoint p = in->points[c->first + i];
        if (!isfinite(p.x) || !isfinite(p.y))
            continue;
        if (m > 0 && p.x == pts[m - 1].x && p.y == pts[m - 1].y)
            continue;
        pts[m++] = p;
    }
    bool closed = c->closed;
    if (closed && m > 1 && pts[m - 1].x == pts[0].x && pts[m - 1].y == pts[0].y)
        m--;
    bool ok = true;
    if (m == 1)
    {
        dot(s, pts[0]);
    }
    else if (m >= 2)
    {
        if (closed && m == 2)
            closed = false; /* a closed two-point contour is a line there and back */
        int segs = closed ? m : m - 1;
        for (int i = 0; i < segs; i++)
        {
            ZvPoint a = pts[i], b = pts[(i + 1) % m];
            float dx = b.x - a.x, dy = b.y - a.y;
            float len = sqrtf(dx * dx + dy * dy);
            dirs[i].x = dx / len;
            dirs[i].y = dy / len;
        }
        if (closed)
        {
            contour(s);
            walk_side(s, pts, dirs, m, true, +1, true);
            contour(s);
            walk_side(s, pts, dirs, m, true, +1, false);
        }
        else
        {
            contour(s);
            walk_side(s, pts, dirs, m, false, +1, true);
            /* end cap: direction of the last segment */
            ZvPoint dl = dirs[m - 2];
            ZvPoint nl = {-dl.y, dl.x};
            cap(s, pts[m - 1], dl, nl);
            walk_side(s, pts, dirs, m, false, +1, false);
            ZvPoint d0 = {-dirs[0].x, -dirs[0].y};
            ZvPoint n0 = {-d0.y, d0.x};
            cap(s, pts[0], d0, n0);
        }
        ok = s->ok;
    }
    if (pts != stack_pts)
        alloc->release(alloc->context, pts);
    return ok && s->ok;
}

bool zv_stroke_polyline(const ZvPolyline *in, const ZvStrokeStyle *style, float tolerance, ZvPolyline *out)
{
    if (!(style->width > 0.0f) || !isfinite(style->width))
        return true;
    Stroker s;
    s.out = out;
    s.hw = style->width * 0.5f;
    s.cap = style->cap;
    s.join = style->join;
    s.miter_limit = style->miter_limit >= 1.0f ? style->miter_limit : 1.0f;
    s.tolerance = tolerance > 0.0f ? tolerance : ZV_FLATTEN_TOLERANCE;
    s.ok = true;

    if (style->dashes && style->dash_count > 0 && dash_total(style->dashes, style->dash_count) > 0.0f)
    {
        ZvPolyline dashed;
        zv_polyline_init(&dashed, in->allocator);
        bool ok = zv_dash_polyline(in, style->dashes, style->dash_count, style->dash_offset, &dashed);
        for (int c = 0; ok && c < dashed.contour_count; c++)
            ok = stroke_contour(&s, &dashed, &dashed.contours[c]);
        zv_polyline_release(&dashed);
        return ok;
    }
    for (int c = 0; c < in->contour_count; c++)
    {
        if (!stroke_contour(&s, in, &in->contours[c]))
            return false;
    }
    return true;
}
