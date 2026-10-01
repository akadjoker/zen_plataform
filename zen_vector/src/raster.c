#include "zv_raster.h"
#include "zv_internal.h"

#include <stdlib.h>

#define SHIFT 8
#define SCALE (1 << SHIFT)
#define MASK (SCALE - 1)
/* Full coverage of one pixel in the units of cover * 2 * SCALE - area. */
#define FULL (SCALE * SCALE * 2)

void zv_rasterizer_init(ZvRasterizer *r, const ZvAllocator *allocator)
{
    memset(r, 0, sizeof *r);
    r->allocator = allocator ? allocator : zv_default_allocator();
    r->min_y = INT32_MAX;
    r->max_y = INT32_MIN;
    r->current.x = INT32_MAX;
    r->current.y = INT32_MAX;
}

void zv_rasterizer_release(ZvRasterizer *r)
{
    if (!r)
        return;
    const ZvAllocator *a = r->allocator;
    if (r->cells)
        a->release(a->context, r->cells);
    if (r->sorted)
        a->release(a->context, r->sorted);
    if (r->row_start)
        a->release(a->context, r->row_start);
    if (r->mask)
        a->release(a->context, r->mask);
    memset(r, 0, sizeof *r);
    r->allocator = a;
}

void zv_rasterizer_reset(ZvRasterizer *r)
{
    r->cell_count = 0;
    r->min_y = INT32_MAX;
    r->max_y = INT32_MIN;
    r->current.x = INT32_MAX;
    r->current.y = INT32_MAX;
    r->current.cover = 0;
    r->current.area = 0;
    r->overflow = false;
}

void zv_rasterizer_set_clip(ZvRasterizer *r, int x0, int y0, int x1, int y1)
{
    if (x1 < x0)
        x1 = x0;
    if (y1 < y0)
        y1 = y0;
    r->clip_x0 = x0;
    r->clip_y0 = y0;
    r->clip_x1 = x1;
    r->clip_y1 = y1;
    zv_rasterizer_reset(r);
}

/* Cells */

static void flush_cell(ZvRasterizer *r)
{
    if ((r->current.area | r->current.cover) == 0)
        return;
    if (r->cell_count == r->cell_capacity)
    {
        if (!zv_grow(r->allocator, (void **)&r->cells, &r->cell_capacity, r->cell_count, r->cell_count + 1, sizeof(ZvCell)))
        {
            r->overflow = true;
            return;
        }
    }
    r->cells[r->cell_count++] = r->current;
    if (r->current.y < r->min_y)
        r->min_y = r->current.y;
    if (r->current.y > r->max_y)
        r->max_y = r->current.y;
}

static void set_cell(ZvRasterizer *r, int x, int y)
{
    if (r->current.x != x || r->current.y != y)
    {
        flush_cell(r);
        r->current.x = x;
        r->current.y = y;
        r->current.cover = 0;
        r->current.area = 0;
    }
}

static void render_hline(ZvRasterizer *r, int ey, int x1, int y1, int x2, int y2)
{
    int ex1 = x1 >> SHIFT;
    int ex2 = x2 >> SHIFT;
    int fx1 = x1 & MASK;
    int fx2 = x2 & MASK;

    if (y1 == y2)
    {
        set_cell(r, ex2, ey);
        return;
    }
    if (ex1 == ex2)
    {
        int delta = y2 - y1;
        r->current.cover += delta;
        r->current.area += (fx1 + fx2) * delta;
        return;
    }

    long long p = (long long)(SCALE - fx1) * (y2 - y1);
    int first = SCALE;
    int incr = 1;
    long long dx = (long long)x2 - x1;
    if (dx < 0)
    {
        p = (long long)fx1 * (y2 - y1);
        first = 0;
        incr = -1;
        dx = -dx;
    }
    long long delta = p / dx;
    long long mod = p % dx;
    if (mod < 0)
    {
        delta--;
        mod += dx;
    }
    r->current.cover += (int)delta;
    r->current.area += (fx1 + first) * (int)delta;
    ex1 += incr;
    set_cell(r, ex1, ey);
    y1 += (int)delta;

    if (ex1 != ex2)
    {
        p = (long long)SCALE * (y2 - y1 + delta);
        long long lift = p / dx;
        long long rem = p % dx;
        if (rem < 0)
        {
            lift--;
            rem += dx;
        }
        mod -= dx;
        while (ex1 != ex2)
        {
            delta = lift;
            mod += rem;
            if (mod >= 0)
            {
                mod -= dx;
                delta++;
            }
            r->current.cover += (int)delta;
            r->current.area += SCALE * (int)delta;
            y1 += (int)delta;
            ex1 += incr;
            set_cell(r, ex1, ey);
        }
    }
    int delta2 = y2 - y1;
    r->current.cover += delta2;
    r->current.area += (fx2 + SCALE - first) * delta2;
}

static void render_line(ZvRasterizer *r, int x1, int y1, int x2, int y2)
{
    int ey1 = y1 >> SHIFT;
    int ey2 = y2 >> SHIFT;
    int fy1 = y1 & MASK;
    int fy2 = y2 & MASK;
    long long dx = (long long)x2 - x1;
    long long dy = (long long)y2 - y1;

    set_cell(r, x1 >> SHIFT, ey1);
    if (ey1 == ey2)
    {
        render_hline(r, ey1, x1, fy1, x2, fy2);
        return;
    }

    int incr = 1;
    if (dx == 0)
    {
        int ex = x1 >> SHIFT;
        int two_fx = (x1 - (ex << SHIFT)) << 1;
        int first = SCALE;
        if (dy < 0)
        {
            first = 0;
            incr = -1;
        }
        int delta = first - fy1;
        r->current.cover += delta;
        r->current.area += two_fx * delta;
        ey1 += incr;
        set_cell(r, ex, ey1);
        delta = first + first - SCALE;
        int area = two_fx * delta;
        while (ey1 != ey2)
        {
            r->current.cover = delta;
            r->current.area = area;
            ey1 += incr;
            set_cell(r, ex, ey1);
        }
        delta = fy2 - SCALE + first;
        r->current.cover += delta;
        r->current.area += two_fx * delta;
        return;
    }

    long long p = (SCALE - fy1) * dx;
    int first = SCALE;
    if (dy < 0)
    {
        p = fy1 * dx;
        first = 0;
        incr = -1;
        dy = -dy;
    }
    long long delta = p / dy;
    long long mod = p % dy;
    if (mod < 0)
    {
        delta--;
        mod += dy;
    }
    int x_from = x1 + (int)delta;
    render_hline(r, ey1, x1, fy1, x_from, first);
    ey1 += incr;
    set_cell(r, x_from >> SHIFT, ey1);

    if (ey1 != ey2)
    {
        p = SCALE * dx;
        long long lift = p / dy;
        long long rem = p % dy;
        if (rem < 0)
        {
            lift--;
            rem += dy;
        }
        mod -= dy;
        while (ey1 != ey2)
        {
            delta = lift;
            mod += rem;
            if (mod >= 0)
            {
                mod -= dy;
                delta++;
            }
            int x_to = x_from + (int)delta;
            render_hline(r, ey1, x_from, SCALE - first, x_to, first);
            x_from = x_to;
            ey1 += incr;
            set_cell(r, x_from >> SHIFT, ey1);
        }
    }
    render_hline(r, ey1, x_from, SCALE - first, x2, fy2);
}

static int to_fixed(float v)
{
    return (int)floorf(v * (float)SCALE + 0.5f);
}

/* An edge already inside the clip rows, with x inside [clip_x0, clip_x1]. */
static void add_clipped(ZvRasterizer *r, float x0, float y0, float x1, float y1)
{
    int fx0 = to_fixed(x0), fy0 = to_fixed(y0), fx1 = to_fixed(x1), fy1 = to_fixed(y1);
    if (fy0 == fy1)
        return;
    render_line(r, fx0, fy0, fx1, fy1);
}

/* Clips the edge in x: pieces left of the box are pushed to its left side,
   where they keep their winding effect; pieces right of it to its right side,
   where they close the runs. */
static void add_x_clipped(ZvRasterizer *r, float x0, float y0, float x1, float y1)
{
    float cx0 = (float)r->clip_x0;
    float cx1 = (float)r->clip_x1;
    if (x0 <= cx0 && x1 <= cx0)
    {
        add_clipped(r, cx0, y0, cx0, y1);
        return;
    }
    if (x0 >= cx1 && x1 >= cx1)
    {
        add_clipped(r, cx1, y0, cx1, y1);
        return;
    }
    if (x0 >= cx0 && x0 <= cx1 && x1 >= cx0 && x1 <= cx1)
    {
        add_clipped(r, x0, y0, x1, y1);
        return;
    }
    /* crosses one or both sides: split at the crossings, left to right */
    float dx = x1 - x0;
    float dy = y1 - y0;
    if (dx == 0.0f)
    {
        add_clipped(r, zv_clampf(x0, cx0, cx1), y0, zv_clampf(x1, cx0, cx1), y1);
        return;
    }
    float ta = (cx0 - x0) / dx;
    float tb = (cx1 - x0) / dx;
    float t1 = ta < tb ? ta : tb;
    float t2 = ta < tb ? tb : ta;
    t1 = zv_clampf(t1, 0.0f, 1.0f);
    t2 = zv_clampf(t2, 0.0f, 1.0f);
    float ya = y0 + dy * t1;
    float yb = y0 + dy * t2;
    float xa = zv_clampf(x0 + dx * t1, cx0, cx1);
    float xb = zv_clampf(x0 + dx * t2, cx0, cx1);
    add_clipped(r, zv_clampf(x0, cx0, cx1), y0, xa, ya);
    add_clipped(r, xa, ya, xb, yb);
    add_clipped(r, xb, yb, zv_clampf(x1, cx0, cx1), y1);
}

void zv_rasterizer_add_line(ZvRasterizer *r, float x0, float y0, float x1, float y1)
{
    if (!isfinite(x0) || !isfinite(y0) || !isfinite(x1) || !isfinite(y1))
        return;
    if (y0 == y1)
        return;
    float cy0 = (float)r->clip_y0;
    float cy1 = (float)r->clip_y1;
    if ((y0 <= cy0 && y1 <= cy0) || (y0 >= cy1 && y1 >= cy1))
        return;
    /* clip to the rows */
    if (y0 < cy0 || y1 < cy0 || y0 > cy1 || y1 > cy1)
    {
        float dx = x1 - x0;
        float dy = y1 - y0;
        if (y0 < cy0)
        {
            x0 += dx * (cy0 - y0) / dy;
            y0 = cy0;
        }
        else if (y0 > cy1)
        {
            x0 += dx * (cy1 - y0) / dy;
            y0 = cy1;
        }
        if (y1 < cy0)
        {
            x1 += dx * (cy0 - y1) / dy;
            y1 = cy0;
        }
        else if (y1 > cy1)
        {
            x1 += dx * (cy1 - y1) / dy;
            y1 = cy1;
        }
    }
    add_x_clipped(r, x0, y0, x1, y1);
}

void zv_rasterizer_add_polyline(ZvRasterizer *r, const ZvPolyline *poly)
{
    for (int c = 0; c < poly->contour_count; c++)
    {
        const ZvContour *contour = &poly->contours[c];
        if (contour->count < 2)
            continue;
        const ZvPoint *pts = poly->points + contour->first;
        for (int i = 0; i + 1 < contour->count; i++)
            zv_rasterizer_add_line(r, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y);
        ZvPoint last = pts[contour->count - 1];
        if (last.x != pts[0].x || last.y != pts[0].y)
            zv_rasterizer_add_line(r, last.x, last.y, pts[0].x, pts[0].y);
    }
}

/* Sweep */

static int compare_x(const void *a, const void *b)
{
    int xa = ((const ZvCell *)a)->x;
    int xb = ((const ZvCell *)b)->x;
    return (xa > xb) - (xa < xb);
}

static bool sort_cells(ZvRasterizer *r)
{
    int rows = r->max_y - r->min_y + 1;
    if (!zv_grow(r->allocator, (void **)&r->row_start, &r->row_capacity, 0, rows + 1, sizeof(int)))
        return false;
    if (!zv_grow(r->allocator, (void **)&r->sorted, &r->sorted_capacity, 0, r->cell_count, sizeof(ZvCell)))
        return false;
    memset(r->row_start, 0, (size_t)(rows + 1) * sizeof(int));
    for (int i = 0; i < r->cell_count; i++)
        r->row_start[r->cells[i].y - r->min_y + 1]++;
    for (int y = 0; y < rows; y++)
        r->row_start[y + 1] += r->row_start[y];
    for (int i = 0; i < r->cell_count; i++)
    {
        int row = r->cells[i].y - r->min_y;
        r->sorted[r->row_start[row]++] = r->cells[i];
    }
    for (int y = rows; y > 0; y--)
        r->row_start[y] = r->row_start[y - 1];
    r->row_start[0] = 0;
    for (int y = 0; y < rows; y++)
    {
        int start = r->row_start[y];
        int count = r->row_start[y + 1] - start;
        if (count < 2)
            continue;
        if (count <= 12)
        {
            ZvCell *c = r->sorted + start;
            for (int i = 1; i < count; i++)
            {
                ZvCell key = c[i];
                int j = i - 1;
                while (j >= 0 && c[j].x > key.x)
                {
                    c[j + 1] = c[j];
                    j--;
                }
                c[j + 1] = key;
            }
        }
        else
        {
            qsort(r->sorted + start, (size_t)count, sizeof(ZvCell), compare_x);
        }
    }
    return true;
}

static inline uint32_t coverage_nonzero(int value)
{
    uint32_t a = (uint32_t)(value < 0 ? -value : value);
    if (a > (uint32_t)FULL)
        a = FULL;
    return (a * 255u + (FULL / 2)) / FULL;
}

static inline uint32_t coverage_evenodd(int value)
{
    uint32_t a = (uint32_t)(value < 0 ? -value : value);
    a &= (uint32_t)(2 * FULL - 1);
    if (a > (uint32_t)FULL)
        a = 2 * FULL - a;
    return (a * 255u + (FULL / 2)) / FULL;
}

typedef struct
{
    const ZvSpanSink *sink;
    uint8_t *mask;
    int y;
    int mask_x; /* first x of the pending mask run, -1 for none */
    int mask_len;
} Sweep;

static void flush_mask(Sweep *s)
{
    if (s->mask_len > 0)
        s->sink->mask(s->sink->context, s->y, s->mask_x, s->mask_len, s->mask);
    s->mask_x = -1;
    s->mask_len = 0;
}

static void emit_pixel(Sweep *s, int x, uint32_t alpha)
{
    if (s->mask_len > 0 && s->mask_x + s->mask_len != x)
        flush_mask(s);
    if (s->mask_len == 0)
        s->mask_x = x;
    s->mask[s->mask_len++] = (uint8_t)alpha;
}

static void emit_run(Sweep *s, int x, int length, uint32_t alpha)
{
    if (length <= 4 && s->mask_len > 0 && s->mask_x + s->mask_len == x)
    {
        for (int i = 0; i < length; i++)
            s->mask[s->mask_len++] = (uint8_t)alpha;
        return;
    }
    flush_mask(s);
    s->sink->solid(s->sink->context, s->y, x, length, alpha);
}

bool zv_rasterizer_sweep(ZvRasterizer *r, ZvFillRule rule, const ZvSpanSink *sink)
{
    flush_cell(r);
    r->current.x = INT32_MAX;
    r->current.y = INT32_MAX;
    r->current.cover = 0;
    r->current.area = 0;
    if (r->overflow)
        return false;
    if (r->cell_count == 0)
        return true;
    int width = r->clip_x1 - r->clip_x0;
    if (width <= 0)
        return true;
    if (!zv_grow(r->allocator, (void **)&r->mask, &r->mask_capacity, 0, width, sizeof(uint8_t)))
        return false;
    if (!sort_cells(r))
        return false;

    Sweep s;
    s.sink = sink;
    s.mask = r->mask;
    s.mask_x = -1;
    s.mask_len = 0;

    int rows = r->max_y - r->min_y + 1;
    for (int row = 0; row < rows; row++)
    {
        int y = r->min_y + row;
        if (y < r->clip_y0 || y >= r->clip_y1)
            continue;
        const ZvCell *c = r->sorted + r->row_start[row];
        const ZvCell *end = r->sorted + r->row_start[row + 1];
        if (c == end)
            continue;
        s.y = y;
        int cover = 0;
        while (c < end)
        {
            int x = c->x;
            int area = 0;
            while (c < end && c->x == x)
            {
                area += c->area;
                cover += c->cover;
                c++;
            }
            if (area != 0)
            {
                int value = cover * (2 * SCALE) - area;
                uint32_t alpha = rule == ZV_FILL_NONZERO ? coverage_nonzero(value) : coverage_evenodd(value);
                if (alpha != 0 && x >= r->clip_x0 && x < r->clip_x1)
                    emit_pixel(&s, x, alpha);
                x++;
            }
            if (c < end && c->x > x)
            {
                int value = cover * (2 * SCALE);
                uint32_t alpha = rule == ZV_FILL_NONZERO ? coverage_nonzero(value) : coverage_evenodd(value);
                if (alpha != 0)
                {
                    int x0 = x > r->clip_x0 ? x : r->clip_x0;
                    int x1 = c->x < r->clip_x1 ? c->x : r->clip_x1;
                    if (x1 > x0)
                        emit_run(&s, x0, x1 - x0, alpha);
                }
            }
        }
        flush_mask(&s);
    }
    return true;
}

/* Solid fill */

typedef struct
{
    ZvSurface *surface;
    ZvPixel color;
} SolidFill;

static void solid_run(void *context, int y, int x, int length, uint32_t coverage)
{
    SolidFill *f = context;
    zv_span_solid_cover(f->surface->pixels + (size_t)y * (size_t)f->surface->stride + (size_t)x, (size_t)length, f->color, coverage);
}

static void solid_mask(void *context, int y, int x, int length, const uint8_t *coverage)
{
    SolidFill *f = context;
    zv_span_cover(f->surface->pixels + (size_t)y * (size_t)f->surface->stride + (size_t)x, (size_t)length, f->color, coverage);
}

bool zv_fill_polyline_solid(ZvSurface *surface, const ZvPolyline *poly, ZvFillRule rule, ZvPixel color, const ZvBounds *clip, ZvRasterizer *r)
{
    int x0 = 0, y0 = 0, x1 = surface->width, y1 = surface->height;
    if (clip)
    {
        x0 = zv_clampi(zv_floor_int(clip->min_x), 0, surface->width);
        y0 = zv_clampi(zv_floor_int(clip->min_y), 0, surface->height);
        x1 = zv_clampi(zv_ceil_int(clip->max_x), 0, surface->width);
        y1 = zv_clampi(zv_ceil_int(clip->max_y), 0, surface->height);
    }
    if (x1 <= x0 || y1 <= y0 || (color >> 24) == 0)
        return true;
    ZvRasterizer local;
    if (!r)
    {
        zv_rasterizer_init(&local, surface->allocator);
        r = &local;
    }
    zv_rasterizer_set_clip(r, x0, y0, x1, y1);
    zv_rasterizer_add_polyline(r, poly);
    SolidFill f = {surface, color};
    ZvSpanSink sink = {solid_run, solid_mask, &f};
    bool ok = zv_rasterizer_sweep(r, rule, &sink);
    if (r == &local)
        zv_rasterizer_release(&local);
    return ok;
}
