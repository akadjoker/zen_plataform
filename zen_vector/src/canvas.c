#include "zv_canvas.h"
#include "zv_filter.h"
#include "zv_internal.h"

/* Composite operations */

static const char *const op_names[ZV_OP_COUNT] = {
    "source-over", "source-in", "source-out", "source-atop", "destination-over", "destination-in", "destination-out", "destination-atop",
    "lighter", "copy", "xor", "multiply", "screen", "darken", "lighten",
};

bool zv_composite_op_parse(const char *name, ZvCompositeOp *op)
{
    for (int i = 0; i < ZV_OP_COUNT; i++)
    {
        if (strcmp(name, op_names[i]) == 0)
        {
            *op = (ZvCompositeOp)i;
            return true;
        }
    }
    return false;
}

static inline uint32_t ch(ZvPixel p, int shift)
{
    return (p >> shift) & 0xFFu;
}

static inline uint32_t clamp255(uint32_t v)
{
    return v > 255u ? 255u : v;
}

ZvPixel zv_composite(ZvPixel d, ZvPixel s, ZvCompositeOp op)
{
    uint32_t sa = s >> 24, da = d >> 24;
    uint32_t fa, fb; /* factors in 0..255 */
    switch (op)
    {
    case ZV_OP_SOURCE_OVER:
        return zv_blend_over(d, s);
    case ZV_OP_SOURCE_IN:
        fa = da;
        fb = 0;
        break;
    case ZV_OP_SOURCE_OUT:
        fa = 255u - da;
        fb = 0;
        break;
    case ZV_OP_SOURCE_ATOP:
        fa = da;
        fb = 255u - sa;
        break;
    case ZV_OP_DESTINATION_OVER:
        fa = 255u - da;
        fb = 255u;
        break;
    case ZV_OP_DESTINATION_IN:
        fa = 0;
        fb = sa;
        break;
    case ZV_OP_DESTINATION_OUT:
        fa = 0;
        fb = 255u - sa;
        break;
    case ZV_OP_DESTINATION_ATOP:
        fa = 255u - da;
        fb = sa;
        break;
    case ZV_OP_LIGHTER:
        fa = 255u;
        fb = 255u;
        break;
    case ZV_OP_COPY:
        return s;
    case ZV_OP_XOR:
        fa = 255u - da;
        fb = 255u - sa;
        break;
    case ZV_OP_MULTIPLY:
    case ZV_OP_SCREEN:
    case ZV_OP_DARKEN:
    case ZV_OP_LIGHTEN:
    {
        ZvPixel out = 0;
        for (int shift = 0; shift < 32; shift += 8)
        {
            uint32_t cs = ch(s, shift), cb = ch(d, shift);
            uint32_t v;
            if (shift == 24)
                v = sa + da - zv_div255(sa * da);
            else
            {
                uint32_t base = zv_div255(cs * (255u - da)) + zv_div255(cb * (255u - sa));
                uint32_t blend;
                switch (op)
                {
                case ZV_OP_MULTIPLY:
                    blend = zv_div255(cs * cb);
                    break;
                case ZV_OP_SCREEN:
                    blend = zv_div255(cs * da) + zv_div255(cb * sa) - zv_div255(cs * cb);
                    break;
                case ZV_OP_DARKEN:
                {
                    uint32_t x = zv_div255(cs * da), y = zv_div255(cb * sa);
                    blend = x < y ? x : y;
                    break;
                }
                default:
                {
                    uint32_t x = zv_div255(cs * da), y = zv_div255(cb * sa);
                    blend = x > y ? x : y;
                    break;
                }
                }
                v = clamp255(base + blend);
            }
            out |= v << shift;
        }
        return out;
    }
    default:
        return zv_blend_over(d, s);
    }
    ZvPixel out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= clamp255(zv_div255(ch(s, shift) * fa) + zv_div255(ch(d, shift) * fb)) << shift;
    return out;
}

/* Lerp between dst and composite by coverage k (0..255). */
static inline ZvPixel composite_cover(ZvPixel d, ZvPixel s, ZvCompositeOp op, uint32_t k)
{
    if (k == 0)
        return d;
    if (op == ZV_OP_SOURCE_OVER)
        return zv_blend_over(d, k == 255u ? s : zv_scale(s, k));
    ZvPixel r = zv_composite(d, s, op);
    if (k == 255u)
        return r;
    ZvPixel out = 0;
    for (int shift = 0; shift < 32; shift += 8)
        out |= zv_div255(ch(r, shift) * k + ch(d, shift) * (255u - k)) << shift;
    return out;
}

/* State */

static void state_defaults(ZvCanvasState *s, int width, int height)
{
    memset(s, 0, sizeof *s);
    s->transform = zv_matrix_identity();
    s->fill = zv_paint_solid(0xFF000000u);
    s->stroke = zv_paint_solid(0xFF000000u);
    s->line_width = 1.0f;
    s->line_cap = ZV_CAP_BUTT;
    s->line_join = ZV_JOIN_MITER;
    s->miter_limit = 10.0f;
    s->global_alpha = 1.0f;
    s->op = ZV_OP_SOURCE_OVER;
    s->image_smoothing = true;
    s->clip_bounds.min_x = 0;
    s->clip_bounds.min_y = 0;
    s->clip_bounds.max_x = (float)width;
    s->clip_bounds.max_y = (float)height;
    s->font_size = 10.0f;
    s->text_align = ZV_ALIGN_START;
    s->text_baseline = ZV_BASELINE_ALPHABETIC;
    s->shadow_color = 0x00000000u;
}

static void release_mask(ZvCanvas *c, ZvCanvasState *s)
{
    if (s->clip_mask)
        c->allocator->release(c->allocator->context, s->clip_mask);
    s->clip_mask = NULL;
}

void zv_canvas_init(ZvCanvas *c, ZvSurface *target, const ZvAllocator *allocator)
{
    memset(c, 0, sizeof *c);
    c->target = target;
    c->allocator = allocator ? allocator : zv_default_allocator();
    zv_path_init(&c->path, c->allocator);
    zv_path_init(&c->scratch_path, c->allocator);
    zv_polyline_init(&c->poly, c->allocator);
    zv_polyline_init(&c->poly2, c->allocator);
    zv_polyline_init(&c->poly3, c->allocator);
    zv_rasterizer_init(&c->raster, c->allocator);
    state_defaults(&c->state, target->width, target->height);
}

void zv_canvas_reset(ZvCanvas *c)
{
    while (c->depth > 0)
        zv_canvas_restore(c);
    while (c->layer_depth > 0)
        zv_canvas_end_layer(c, 0.0f, NULL);
    release_mask(c, &c->state);
    state_defaults(&c->state, c->target->width, c->target->height);
    zv_path_clear(&c->path);
    c->oom = false;
}

void zv_canvas_release(ZvCanvas *c)
{
    if (!c)
        return;
    zv_canvas_reset(c);
    zv_path_release(&c->path);
    zv_path_release(&c->scratch_path);
    zv_polyline_release(&c->poly);
    zv_polyline_release(&c->poly2);
    zv_polyline_release(&c->poly3);
    zv_rasterizer_release(&c->raster);
    zv_surface_release(&c->layer);
    if (c->cover)
        c->allocator->release(c->allocator->context, c->cover);
    if (c->shadow)
        c->allocator->release(c->allocator->context, c->shadow);
    c->cover = NULL;
    c->shadow = NULL;
}

static size_t target_pixels(const ZvCanvas *c)
{
    return (size_t)c->target->width * (size_t)c->target->height;
}

void zv_canvas_save(ZvCanvas *c)
{
    if (c->depth >= ZV_STATE_STACK)
        return;
    c->stack[c->depth] = c->state;
    if (c->state.clip_mask)
    {
        size_t n = target_pixels(c);
        uint8_t *copy = c->allocator->allocate(c->allocator->context, n);
        if (copy)
            memcpy(copy, c->state.clip_mask, n);
        else
            c->oom = true;
        c->stack[c->depth].clip_mask = copy;
    }
    c->depth++;
}

void zv_canvas_restore(ZvCanvas *c)
{
    if (c->depth == 0)
        return;
    release_mask(c, &c->state);
    c->depth--;
    c->state = c->stack[c->depth];
    c->stack[c->depth].clip_mask = NULL;
}

/* Transform */

void zv_canvas_translate(ZvCanvas *c, float x, float y)
{
    if (isfinite(x) && isfinite(y))
        zv_matrix_translate(&c->state.transform, x, y);
}

void zv_canvas_rotate(ZvCanvas *c, float radians)
{
    if (isfinite(radians))
        zv_matrix_rotate(&c->state.transform, radians);
}

void zv_canvas_scale(ZvCanvas *c, float x, float y)
{
    if (isfinite(x) && isfinite(y))
        zv_matrix_scale(&c->state.transform, x, y);
}

void zv_canvas_transform(ZvCanvas *c, float a, float b, float cc, float d, float e, float f)
{
    ZvMatrix m = zv_matrix_make(a, b, cc, d, e, f);
    c->state.transform = zv_matrix_concat(&c->state.transform, &m);
}

void zv_canvas_set_transform(ZvCanvas *c, float a, float b, float cc, float d, float e, float f)
{
    c->state.transform = zv_matrix_make(a, b, cc, d, e, f);
}

void zv_canvas_reset_transform(ZvCanvas *c)
{
    c->state.transform = zv_matrix_identity();
}

ZvMatrix zv_canvas_get_transform(const ZvCanvas *c)
{
    return c->state.transform;
}

/* Styles */

void zv_canvas_set_fill_color(ZvCanvas *c, uint32_t straight)
{
    c->state.fill = zv_paint_solid(straight);
}

void zv_canvas_set_stroke_color(ZvCanvas *c, uint32_t straight)
{
    c->state.stroke = zv_paint_solid(straight);
}

void zv_canvas_set_fill_paint(ZvCanvas *c, const ZvPaint *paint)
{
    c->state.fill = *paint;
}

void zv_canvas_set_stroke_paint(ZvCanvas *c, const ZvPaint *paint)
{
    c->state.stroke = *paint;
}

void zv_canvas_set_line_width(ZvCanvas *c, float width)
{
    if (width > 0.0f && isfinite(width))
        c->state.line_width = width;
}

void zv_canvas_set_line_cap(ZvCanvas *c, ZvLineCap cap)
{
    c->state.line_cap = cap;
}

void zv_canvas_set_line_join(ZvCanvas *c, ZvLineJoin join)
{
    c->state.line_join = join;
}

void zv_canvas_set_miter_limit(ZvCanvas *c, float limit)
{
    if (limit > 0.0f && isfinite(limit))
        c->state.miter_limit = limit;
}

void zv_canvas_set_line_dash(ZvCanvas *c, const float *dashes, int count)
{
    if (count < 0 || count > ZV_MAX_DASHES / 2)
        return;
    for (int i = 0; i < count; i++)
    {
        if (!(dashes[i] >= 0.0f) || !isfinite(dashes[i]))
            return;
    }
    int n = count & 1 ? count * 2 : count;
    for (int i = 0; i < n; i++)
        c->state.dashes[i] = dashes[i % count];
    c->state.dash_count = n;
}

void zv_canvas_set_line_dash_offset(ZvCanvas *c, float offset)
{
    if (isfinite(offset))
        c->state.dash_offset = offset;
}

void zv_canvas_set_global_alpha(ZvCanvas *c, float alpha)
{
    if (alpha >= 0.0f && alpha <= 1.0f)
        c->state.global_alpha = alpha;
}

void zv_canvas_set_composite_op(ZvCanvas *c, ZvCompositeOp op)
{
    if (op >= 0 && op < ZV_OP_COUNT)
        c->state.op = op;
}

void zv_canvas_set_image_smoothing(ZvCanvas *c, bool on)
{
    c->state.image_smoothing = on;
}

void zv_canvas_set_shadow(ZvCanvas *c, uint32_t straight_color, float blur, float x, float y)
{
    c->state.shadow_color = straight_color;
    c->state.shadow_blur = blur >= 0.0f && isfinite(blur) ? blur : 0.0f;
    c->state.shadow_x = isfinite(x) ? x : 0.0f;
    c->state.shadow_y = isfinite(y) ? y : 0.0f;
}

void zv_canvas_set_font(ZvCanvas *c, const ZvFont *font, float size)
{
    c->state.font = font;
    if (size > 0.0f && isfinite(size))
        c->state.font_size = size;
}

void zv_canvas_set_text_align(ZvCanvas *c, ZvTextAlign align)
{
    c->state.text_align = align;
}

void zv_canvas_set_text_baseline(ZvCanvas *c, ZvTextBaseline baseline)
{
    c->state.text_baseline = baseline;
}

/* Drawing core */

static bool ensure_cover(ZvCanvas *c)
{
    if (c->cover)
        return true;
    c->cover = c->allocator->allocate(c->allocator->context, target_pixels(c));
    if (!c->cover)
        return false;
    memset(c->cover, 0, target_pixels(c));
    return true;
}

static bool ensure_layer(ZvCanvas *c)
{
    if (c->layer.pixels && c->layer.width == c->target->width && c->layer.height == c->target->height)
        return true;
    zv_surface_release(&c->layer);
    return zv_surface_init(&c->layer, c->allocator, c->target->width, c->target->height);
}

typedef struct
{
    ZvCanvas *c;
    ZvPaintContext *paint;
    const uint8_t *clip; /* NULL or target sized */
    ZvCompositeOp op;
    bool to_layer; /* write unscaled colours into the layer and coverage into cover */
    uint8_t tmp[256];
    ZvPixel colors[256];
} Draw;

static void draw_pixels(Draw *d, int y, int x, int length, const uint8_t *cov)
{
    ZvCanvas *c = d->c;
    size_t at = (size_t)y * (size_t)c->target->stride + (size_t)x;
    while (length > 0)
    {
        int n = length < 256 ? length : 256;
        const uint8_t *k = cov;
        if (d->clip)
        {
            const uint8_t *m = d->clip + (size_t)y * (size_t)c->target->width + (size_t)x;
            for (int i = 0; i < n; i++)
                d->tmp[i] = (uint8_t)zv_div255((uint32_t)cov[i] * m[i]);
            k = d->tmp;
        }
        zv_paint_span(d->paint, y, x, n, d->colors);
        if (d->to_layer)
        {
            ZvPixel *lp = c->layer.pixels + (size_t)y * (size_t)c->layer.stride + (size_t)x;
            uint8_t *cp = c->cover + (size_t)y * (size_t)c->target->width + (size_t)x;
            for (int i = 0; i < n; i++)
            {
                if (k[i])
                {
                    lp[i] = d->colors[i];
                    cp[i] = k[i];
                }
            }
        }
        else
        {
            zv_span_pixels_mask(c->target->pixels + at, d->colors, (size_t)n, k);
        }
        at += (size_t)n;
        x += n;
        length -= n;
        cov += n;
    }
}

static void draw_run(void *context, int y, int x, int length, uint32_t coverage)
{
    Draw *d = context;
    if (!d->clip && !d->to_layer && d->paint->is_solid)
    {
        ZvCanvas *c = d->c;
        zv_span_solid_cover(c->target->pixels + (size_t)y * (size_t)c->target->stride + (size_t)x, (size_t)length, d->paint->solid, coverage);
        return;
    }
    uint8_t cov[256];
    memset(cov, (int)coverage, sizeof cov);
    while (length > 0)
    {
        int n = length < 256 ? length : 256;
        draw_pixels(d, y, x, n, cov);
        x += n;
        length -= n;
    }
}

static void draw_mask(void *context, int y, int x, int length, const uint8_t *coverage)
{
    Draw *d = context;
    if (!d->clip && !d->to_layer && d->paint->is_solid)
    {
        ZvCanvas *c = d->c;
        zv_span_cover(c->target->pixels + (size_t)y * (size_t)c->target->stride + (size_t)x, (size_t)length, d->paint->solid, coverage);
        return;
    }
    draw_pixels(d, y, x, length, coverage);
}

static void clip_box(const ZvCanvas *c, int *x0, int *y0, int *x1, int *y1)
{
    const ZvBounds *b = &c->state.clip_bounds;
    *x0 = zv_clampi(zv_floor_int(b->min_x), 0, c->target->width);
    *y0 = zv_clampi(zv_floor_int(b->min_y), 0, c->target->height);
    *x1 = zv_clampi(zv_ceil_int(b->max_x), 0, c->target->width);
    *y1 = zv_clampi(zv_ceil_int(b->max_y), 0, c->target->height);
}

/* Composites the layer (colours) with cover (coverage) onto the target over
   the clip box, then clears both. */
static void flush_layer(ZvCanvas *c, ZvCompositeOp op)
{
    int x0, y0, x1, y1;
    clip_box(c, &x0, &y0, &x1, &y1);
    bool unbounded = op == ZV_OP_SOURCE_IN || op == ZV_OP_SOURCE_OUT || op == ZV_OP_DESTINATION_IN || op == ZV_OP_DESTINATION_ATOP || op == ZV_OP_COPY;
    for (int y = y0; y < y1; y++)
    {
        ZvPixel *dst = c->target->pixels + (size_t)y * (size_t)c->target->stride;
        ZvPixel *src = c->layer.pixels + (size_t)y * (size_t)c->layer.stride;
        uint8_t *cov = c->cover + (size_t)y * (size_t)c->target->width;
        const uint8_t *clip = c->state.clip_mask ? c->state.clip_mask + (size_t)y * (size_t)c->target->width : NULL;
        for (int x = x0; x < x1; x++)
        {
            uint32_t k = cov[x];
            if (k == 0 && !unbounded)
                continue;
            if (k == 0)
            {
                /* outside the shape: the operation sees a transparent source */
                uint32_t m = clip ? clip[x] : 255u;
                dst[x] = composite_cover(dst[x], 0, op, m);
            }
            else
            {
                dst[x] = composite_cover(dst[x], src[x], op, k);
                cov[x] = 0;
                src[x] = 0;
            }
        }
    }
}

/* Draws a shadow of the coverage in cover (already offset) and clears it. */
static bool draw_shadow(ZvCanvas *c, int x0, int y0, int x1, int y1)
{
    const ZvCanvasState *s = &c->state;
    if (!c->shadow)
    {
        c->shadow = c->allocator->allocate(c->allocator->context, target_pixels(c));
        if (!c->shadow)
            return false;
    }
    int w = c->target->width, h = c->target->height;
    float sigma = s->shadow_blur * 0.5f;
    int pad = (int)ceilf(sigma * 3.0f) + 1;
    int bx0 = zv_clampi(x0 - pad, 0, w), by0 = zv_clampi(y0 - pad, 0, h), bx1 = zv_clampi(x1 + pad, 0, w), by1 = zv_clampi(y1 + pad, 0, h);
    if (bx1 <= bx0 || by1 <= by0)
        return true;
    int bw = bx1 - bx0, bh = by1 - by0;
    /* copy the coverage into a tight buffer, blur it there */
    uint8_t *buf = c->shadow;
    for (int y = 0; y < bh; y++)
        memcpy(buf + (size_t)y * (size_t)bw, c->cover + (size_t)(by0 + y) * (size_t)w + (size_t)bx0, (size_t)bw);
    if (!zv_blur_mask(buf, bw, bh, sigma, c->allocator))
        return false;
    ZvPixel color = zv_premultiply(s->shadow_color);
    uint32_t ga = (uint32_t)(s->global_alpha * 255.0f + 0.5f);
    if (ga < 255u)
        color = zv_scale(color, ga);
    int cx0, cy0, cx1, cy1;
    clip_box(c, &cx0, &cy0, &cx1, &cy1);
    for (int y = by0; y < by1; y++)
    {
        if (y < cy0 || y >= cy1)
            continue;
        ZvPixel *dst = c->target->pixels + (size_t)y * (size_t)c->target->stride;
        const uint8_t *row = buf + (size_t)(y - by0) * (size_t)bw;
        const uint8_t *clip = s->clip_mask ? s->clip_mask + (size_t)y * (size_t)w : NULL;
        for (int x = bx0; x < bx1; x++)
        {
            if (x < cx0 || x >= cx1)
                continue;
            uint32_t k = row[x - bx0];
            if (clip)
                k = zv_div255(k * clip[x]);
            if (k)
                dst[x] = composite_cover(dst[x], color, s->op, k);
        }
    }
    for (int y = y0; y < y1; y++)
        memset(c->cover + (size_t)y * (size_t)w + (size_t)x0, 0, (size_t)(x1 - x0));
    return true;
}

typedef struct
{
    ZvCanvas *c;
} CoverOnly;

static void cover_run(void *context, int y, int x, int length, uint32_t coverage)
{
    ZvCanvas *c = ((CoverOnly *)context)->c;
    memset(c->cover + (size_t)y * (size_t)c->target->width + (size_t)x, (int)coverage, (size_t)length);
}

static void cover_mask(void *context, int y, int x, int length, const uint8_t *coverage)
{
    ZvCanvas *c = ((CoverOnly *)context)->c;
    memcpy(c->cover + (size_t)y * (size_t)c->target->width + (size_t)x, coverage, (size_t)length);
}

static bool has_shadow(const ZvCanvasState *s)
{
    return (s->shadow_color >> 24) != 0 && (s->shadow_blur > 0.0f || s->shadow_x != 0.0f || s->shadow_y != 0.0f);
}

/* Fills poly (device space) with paint under the current state. */
static bool fill_device(ZvCanvas *c, const ZvPolyline *poly, ZvFillRule rule, const ZvPaint *paint_in)
{
    const ZvCanvasState *s = &c->state;
    int x0, y0, x1, y1;
    clip_box(c, &x0, &y0, &x1, &y1);
    if (x1 <= x0 || y1 <= y0)
        return true;
    ZvPaint paint = *paint_in;
    paint.alpha = zv_div255(paint.alpha * (uint32_t)(s->global_alpha * 255.0f + 0.5f));
    ZvPaintContext ctx;
    if (!zv_paint_prepare(&ctx, &paint, &s->transform, c->allocator))
        return false;
    bool ok = true;

    if (has_shadow(s))
    {
        /* coverage of the shape shifted by the offset in device space */
        ZvPoint off = zv_matrix_apply_vector(&s->transform, (ZvPoint){s->shadow_x, s->shadow_y});
        if (ensure_cover(c))
        {
            ZvBounds b = zv_polyline_bounds(poly);
            int sx0 = zv_clampi(zv_floor_int(b.min_x + off.x) - 1, 0, c->target->width);
            int sy0 = zv_clampi(zv_floor_int(b.min_y + off.y) - 1, 0, c->target->height);
            int sx1 = zv_clampi(zv_ceil_int(b.max_x + off.x) + 1, 0, c->target->width);
            int sy1 = zv_clampi(zv_ceil_int(b.max_y + off.y) + 1, 0, c->target->height);
            if (sx1 > sx0 && sy1 > sy0)
            {
                for (int y = sy0; y < sy1; y++)
                    memset(c->cover + (size_t)y * (size_t)c->target->width + (size_t)sx0, 0, (size_t)(sx1 - sx0));
                zv_rasterizer_set_clip(&c->raster, sx0, sy0, sx1, sy1);
                for (int k = 0; k < poly->contour_count; k++)
                {
                    const ZvContour *ct = &poly->contours[k];
                    if (ct->count < 2)
                        continue;
                    const ZvPoint *p = poly->points + ct->first;
                    for (int i = 0; i < ct->count; i++)
                    {
                        ZvPoint a = p[i], bb = p[(i + 1) % ct->count];
                        zv_rasterizer_add_line(&c->raster, a.x + off.x, a.y + off.y, bb.x + off.x, bb.y + off.y);
                    }
                }
                CoverOnly co = {c};
                ZvSpanSink sink = {cover_run, cover_mask, &co};
                ok = zv_rasterizer_sweep(&c->raster, rule, &sink) && draw_shadow(c, sx0, sy0, sx1, sy1);
            }
        }
        else
            ok = false;
    }

    Draw d;
    d.c = c;
    d.paint = &ctx;
    d.clip = s->clip_mask;
    d.op = s->op;
    d.to_layer = s->op != ZV_OP_SOURCE_OVER;
    if (ok && d.to_layer)
    {
        if (!ensure_layer(c) || !ensure_cover(c))
            ok = false;
        else
        {
            for (int y = y0; y < y1; y++)
                memset(c->cover + (size_t)y * (size_t)c->target->width + (size_t)x0, 0, (size_t)(x1 - x0));
        }
    }
    if (ok)
    {
        zv_rasterizer_set_clip(&c->raster, x0, y0, x1, y1);
        zv_rasterizer_add_polyline(&c->raster, poly);
        ZvSpanSink sink = {draw_run, draw_mask, &d};
        ok = zv_rasterizer_sweep(&c->raster, rule, &sink);
        if (ok && d.to_layer)
            flush_layer(c, s->op);
    }
    zv_paint_release(&ctx);
    if (!ok)
        c->oom = true;
    return ok;
}

/* The current path is kept in device space; stroking needs user space. */
static bool stroke_device_path(ZvCanvas *c, const ZvPath *device_path, const ZvPaint *paint)
{
    const ZvCanvasState *s = &c->state;
    ZvMatrix inverse;
    if (!zv_matrix_invert(&s->transform, &inverse))
        return true;
    float scale = zv_matrix_max_scale(&s->transform);
    if (!(scale > 0.0f))
        return true;
    float tol = ZV_FLATTEN_TOLERANCE / scale;
    zv_polyline_clear(&c->poly);
    zv_polyline_clear(&c->poly2);
    if (!zv_path_flatten(device_path, &inverse, tol, &c->poly))
        return false;
    ZvStrokeStyle st = zv_stroke_style(s->line_width);
    st.cap = s->line_cap;
    st.join = s->line_join;
    st.miter_limit = s->miter_limit;
    st.dashes = s->dash_count ? s->dashes : NULL;
    st.dash_count = s->dash_count;
    st.dash_offset = s->dash_offset;
    if (!zv_stroke_polyline(&c->poly, &st, tol, &c->poly2))
        return false;
    for (int i = 0; i < c->poly2.point_count; i++)
        c->poly2.points[i] = zv_matrix_apply(&s->transform, c->poly2.points[i]);
    return fill_device(c, &c->poly2, ZV_FILL_NONZERO, paint);
}

static bool fill_device_path(ZvCanvas *c, const ZvPath *device_path, ZvFillRule rule, const ZvPaint *paint)
{
    zv_polyline_clear(&c->poly);
    if (!zv_path_flatten(device_path, NULL, ZV_FLATTEN_TOLERANCE, &c->poly))
        return false;
    return fill_device(c, &c->poly, rule, paint);
}

/* Rectangles */

static bool rect_path(ZvCanvas *c, ZvPath *path, float x, float y, float w, float h)
{
    const ZvMatrix *m = &c->state.transform;
    ZvPoint p[4] = {zv_matrix_apply(m, (ZvPoint){x, y}), zv_matrix_apply(m, (ZvPoint){x + w, y}), zv_matrix_apply(m, (ZvPoint){x + w, y + h}),
                    zv_matrix_apply(m, (ZvPoint){x, y + h})};
    return zv_path_move_to(path, p[0].x, p[0].y) && zv_path_line_to(path, p[1].x, p[1].y) && zv_path_line_to(path, p[2].x, p[2].y) &&
           zv_path_line_to(path, p[3].x, p[3].y) && zv_path_close(path);
}

static bool finite4(float a, float b, float c, float d)
{
    return isfinite(a) && isfinite(b) && isfinite(c) && isfinite(d);
}

/* Axis-aligned rectangle, solid colour, source-over, rectangular clip, no
   shadow: coverage is separable, no rasterizer needed. */
static bool fast_rect(ZvCanvas *c, float x, float y, float w, float h)
{
    const ZvCanvasState *s = &c->state;
    ZvPoint a = zv_matrix_apply(&s->transform, (ZvPoint){x, y});
    ZvPoint b = zv_matrix_apply(&s->transform, (ZvPoint){x + w, y + h});
    float x0 = a.x < b.x ? a.x : b.x, x1 = a.x < b.x ? b.x : a.x;
    float y0 = a.y < b.y ? a.y : b.y, y1 = a.y < b.y ? b.y : a.y;
    const ZvBounds *cb = &s->clip_bounds;
    if (x0 < cb->min_x)
        x0 = cb->min_x;
    if (y0 < cb->min_y)
        y0 = cb->min_y;
    if (x1 > cb->max_x)
        x1 = cb->max_x;
    if (y1 > cb->max_y)
        y1 = cb->max_y;
    if (x0 < 0.0f)
        x0 = 0.0f;
    if (y0 < 0.0f)
        y0 = 0.0f;
    if (x1 > (float)c->target->width)
        x1 = (float)c->target->width;
    if (y1 > (float)c->target->height)
        y1 = (float)c->target->height;
    if (x1 <= x0 || y1 <= y0)
        return true;
    uint32_t straight = s->fill.color;
    uint32_t alpha = zv_div255((straight >> 24) * zv_div255(s->fill.alpha * (uint32_t)(s->global_alpha * 255.0f + 0.5f)));
    ZvPixel color = zv_premultiply((alpha << 24) | (straight & 0x00FFFFFFu));
    if (alpha == 0)
        return true;
    int ix0 = zv_floor_int(x0), ix1 = zv_ceil_int(x1), iy0 = zv_floor_int(y0), iy1 = zv_ceil_int(y1);
    /* column coverage */
    int cols = ix1 - ix0;
    uint8_t stack_cov[1024];
    uint8_t *cov = stack_cov;
    if (cols > 1024)
    {
        cov = c->allocator->allocate(c->allocator->context, (size_t)cols);
        if (!cov)
            return false;
    }
    bool solid_cols = true;
    for (int i = 0; i < cols; i++)
    {
        float px0 = (float)(ix0 + i), px1 = px0 + 1.0f;
        float l = x0 > px0 ? x0 : px0, r = x1 < px1 ? x1 : px1;
        float f = r - l;
        cov[i] = (uint8_t)(f * 255.0f + 0.5f);
        if (cov[i] != 255)
            solid_cols = false;
    }
    for (int yy = iy0; yy < iy1; yy++)
    {
        float py0 = (float)yy, py1 = py0 + 1.0f;
        float t = y0 > py0 ? y0 : py0, bt = y1 < py1 ? y1 : py1;
        uint32_t rowk = (uint32_t)((bt - t) * 255.0f + 0.5f);
        if (rowk == 0)
            continue;
        ZvPixel *dst = c->target->pixels + (size_t)yy * (size_t)c->target->stride + (size_t)ix0;
        if (solid_cols)
            zv_span_solid_cover(dst, (size_t)cols, color, rowk);
        else if (rowk == 255)
            zv_span_cover(dst, (size_t)cols, color, cov);
        else
        {
            ZvPixel rc = zv_scale(color, rowk);
            zv_span_cover(dst, (size_t)cols, rc, cov);
        }
    }
    if (cov != stack_cov)
        c->allocator->release(c->allocator->context, cov);
    return true;
}

bool zv_canvas_fill_rect(ZvCanvas *c, float x, float y, float w, float h)
{
    if (!finite4(x, y, w, h) || w == 0.0f || h == 0.0f)
        return true;
    const ZvCanvasState *s = &c->state;
    if (zv_matrix_is_axis_aligned(&s->transform) && !s->clip_mask && s->op == ZV_OP_SOURCE_OVER && s->fill.type == ZV_PAINT_SOLID && !has_shadow(s))
        return fast_rect(c, x, y, w, h);
    zv_path_clear(&c->scratch_path);
    if (!rect_path(c, &c->scratch_path, x, y, w, h))
        return false;
    return fill_device_path(c, &c->scratch_path, ZV_FILL_NONZERO, &c->state.fill);
}

bool zv_canvas_stroke_rect(ZvCanvas *c, float x, float y, float w, float h)
{
    if (!finite4(x, y, w, h))
        return true;
    zv_path_clear(&c->scratch_path);
    if (w == 0.0f && h == 0.0f)
    {
        ZvPoint p = zv_matrix_apply(&c->state.transform, (ZvPoint){x, y});
        if (!zv_path_move_to(&c->scratch_path, p.x, p.y) || !zv_path_close(&c->scratch_path))
            return false;
    }
    else if (!rect_path(c, &c->scratch_path, x, y, w, h))
        return false;
    return stroke_device_path(c, &c->scratch_path, &c->state.stroke);
}

bool zv_canvas_clear_rect(ZvCanvas *c, float x, float y, float w, float h)
{
    if (!finite4(x, y, w, h) || w == 0.0f || h == 0.0f)
        return true;
    ZvCanvasState saved = c->state;
    c->state.op = ZV_OP_COPY;
    c->state.fill = zv_paint_solid(0);
    c->state.global_alpha = 1.0f;
    c->state.shadow_color = 0;
    zv_path_clear(&c->scratch_path);
    bool ok = rect_path(c, &c->scratch_path, x, y, w, h) && fill_device_path(c, &c->scratch_path, ZV_FILL_NONZERO, &c->state.fill);
    c->state = saved;
    return ok;
}

/* Paths */

void zv_canvas_begin_path(ZvCanvas *c)
{
    zv_path_clear(&c->path);
}

static ZvPoint tp(const ZvCanvas *c, float x, float y)
{
    return zv_matrix_apply(&c->state.transform, (ZvPoint){x, y});
}

bool zv_canvas_move_to(ZvCanvas *c, float x, float y)
{
    if (!isfinite(x) || !isfinite(y))
        return true;
    ZvPoint p = tp(c, x, y);
    return zv_path_move_to(&c->path, p.x, p.y);
}

bool zv_canvas_line_to(ZvCanvas *c, float x, float y)
{
    if (!isfinite(x) || !isfinite(y))
        return true;
    ZvPoint p = tp(c, x, y);
    return zv_path_line_to(&c->path, p.x, p.y);
}

bool zv_canvas_quadratic_curve_to(ZvCanvas *c, float cx, float cy, float x, float y)
{
    if (!finite4(cx, cy, x, y))
        return true;
    ZvPoint a = tp(c, cx, cy), b = tp(c, x, y);
    return zv_path_quad_to(&c->path, a.x, a.y, b.x, b.y);
}

bool zv_canvas_bezier_curve_to(ZvCanvas *c, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    if (!finite4(c1x, c1y, c2x, c2y) || !isfinite(x) || !isfinite(y))
        return true;
    ZvPoint a = tp(c, c1x, c1y), b = tp(c, c2x, c2y), e = tp(c, x, y);
    return zv_path_cubic_to(&c->path, a.x, a.y, b.x, b.y, e.x, e.y);
}

bool zv_canvas_close_path(ZvCanvas *c)
{
    return zv_path_close(&c->path);
}

bool zv_canvas_rect(ZvCanvas *c, float x, float y, float w, float h)
{
    if (!finite4(x, y, w, h))
        return true;
    if (!rect_path(c, &c->path, x, y, w, h))
        return false;
    /* the Canvas leaves the current point at (x, y) with a new subpath */
    ZvPoint p = tp(c, x, y);
    return zv_path_move_to(&c->path, p.x, p.y);
}

/* An elliptical arc as cubics, in user space, appended through the
   transform. The first point is joined with a line (or starts a subpath). */
static bool arc_segments(ZvCanvas *c, ZvPath *path, float cx, float cy, float rx, float ry, float rotation, float a0, float a1, bool ccw, bool line_first)
{
    const ZvMatrix *m = &c->state.transform;
    float sweep = a1 - a0;
    const float two_pi = 2.0f * (float)M_PI;
    if (!ccw)
    {
        if (sweep >= two_pi)
            sweep = two_pi;
        else
        {
            sweep = fmodf(sweep, two_pi);
            if (sweep < 0.0f)
                sweep += two_pi;
        }
    }
    else
    {
        if (sweep <= -two_pi)
            sweep = -two_pi;
        else
        {
            sweep = fmodf(sweep, two_pi);
            if (sweep > 0.0f)
                sweep -= two_pi;
        }
    }
    float cr = cosf(rotation), sr = sinf(rotation);
    int n = (int)ceilf(fabsf(sweep) / ((float)M_PI / 2.0f) - 1e-4f);
    if (n < 1)
        n = 1;
    float step = sweep / (float)n;
    float k = 4.0f / 3.0f * tanf(step / 4.0f);
    float a = a0;
    for (int i = 0; i < n; i++)
    {
        float b = a + step;
        float c0 = cosf(a), s0 = sinf(a), c1 = cosf(b), s1 = sinf(b);
        /* unit circle points and tangents, scaled and rotated */
        float px[4] = {c0, c0 - k * s0, c1 + k * s1, c1};
        float py[4] = {s0, s0 + k * c0, s1 - k * c1, s1};
        ZvPoint q[4];
        for (int j = 0; j < 4; j++)
        {
            float ux = px[j] * rx, uy = py[j] * ry;
            q[j] = zv_matrix_apply(m, (ZvPoint){cx + ux * cr - uy * sr, cy + ux * sr + uy * cr});
        }
        if (i == 0)
        {
            bool ok = line_first ? zv_path_line_to(path, q[0].x, q[0].y) : zv_path_move_to(path, q[0].x, q[0].y);
            if (!ok)
                return false;
        }
        if (!zv_path_cubic_to(path, q[1].x, q[1].y, q[2].x, q[2].y, q[3].x, q[3].y))
            return false;
        a = b;
    }
    return true;
}

bool zv_canvas_ellipse(ZvCanvas *c, float x, float y, float rx, float ry, float rotation, float a0, float a1, bool ccw)
{
    if (!finite4(x, y, rx, ry) || !finite4(rotation, a0, a1, 0) || rx < 0.0f || ry < 0.0f)
        return true;
    return arc_segments(c, &c->path, x, y, rx, ry, rotation, a0, a1, ccw, true);
}

bool zv_canvas_arc(ZvCanvas *c, float x, float y, float r, float a0, float a1, bool ccw)
{
    return zv_canvas_ellipse(c, x, y, r, r, 0.0f, a0, a1, ccw);
}

bool zv_canvas_arc_to(ZvCanvas *c, float x1, float y1, float x2, float y2, float r)
{
    if (!finite4(x1, y1, x2, y2) || !isfinite(r) || r < 0.0f)
        return true;
    if (!c->path.has_current)
        return zv_canvas_move_to(c, x1, y1);
    /* current point in user space */
    ZvMatrix inv;
    if (!zv_matrix_invert(&c->state.transform, &inv))
        return true;
    ZvPoint p0 = zv_matrix_apply(&inv, c->path.current);
    float d0x = p0.x - x1, d0y = p0.y - y1;
    float d1x = x2 - x1, d1y = y2 - y1;
    float l0 = sqrtf(d0x * d0x + d0y * d0y), l1 = sqrtf(d1x * d1x + d1y * d1y);
    if (l0 == 0.0f || l1 == 0.0f || r == 0.0f)
        return zv_canvas_line_to(c, x1, y1);
    d0x /= l0;
    d0y /= l0;
    d1x /= l1;
    d1y /= l1;
    float cross = d0x * d1y - d0y * d1x;
    if (fabsf(cross) < 1e-6f)
        return zv_canvas_line_to(c, x1, y1);
    float cos_t = d0x * d1x + d0y * d1y;
    float half = acosf(zv_clampf(cos_t, -1.0f, 1.0f)) * 0.5f;
    float tangent = r / tanf(half);
    ZvPoint t0 = {x1 + d0x * tangent, y1 + d0y * tangent};
    ZvPoint t1 = {x1 + d1x * tangent, y1 + d1y * tangent};
    float bx = d0x + d1x, by = d0y + d1y;
    float bl = sqrtf(bx * bx + by * by);
    bx /= bl;
    by /= bl;
    float to_centre = r / sinf(half);
    ZvPoint centre = {x1 + bx * to_centre, y1 + by * to_centre};
    float a0 = atan2f(t0.y - centre.y, t0.x - centre.x);
    float a1 = atan2f(t1.y - centre.y, t1.x - centre.x);
    if (!zv_canvas_line_to(c, t0.x, t0.y))
        return false;
    return arc_segments(c, &c->path, centre.x, centre.y, r, r, 0.0f, a0, a1, cross > 0.0f, true);
}

bool zv_canvas_round_rect(ZvCanvas *c, float x, float y, float w, float h, const float *radii, int count)
{
    if (!finite4(x, y, w, h))
        return true;
    float r[4] = {0, 0, 0, 0}; /* top-left, top-right, bottom-right, bottom-left */
    if (count == 1)
        r[0] = r[1] = r[2] = r[3] = radii[0];
    else if (count == 2)
    {
        r[0] = r[2] = radii[0];
        r[1] = r[3] = radii[1];
    }
    else if (count == 3)
    {
        r[0] = radii[0];
        r[1] = r[3] = radii[1];
        r[2] = radii[2];
    }
    else if (count >= 4)
        for (int i = 0; i < 4; i++)
            r[i] = radii[i];
    for (int i = 0; i < 4; i++)
    {
        if (!(r[i] >= 0.0f) || !isfinite(r[i]))
            return true;
    }
    float aw = fabsf(w), ah = fabsf(h);
    float scale = 1.0f;
    float sums[4] = {r[0] + r[1], r[2] + r[3], r[0] + r[3], r[1] + r[2]};
    float lims[4] = {aw, aw, ah, ah};
    for (int i = 0; i < 4; i++)
    {
        if (sums[i] > lims[i] && sums[i] > 0.0f)
        {
            float s = lims[i] / sums[i];
            if (s < scale)
                scale = s;
        }
    }
    for (int i = 0; i < 4; i++)
        r[i] *= scale;
    if (w < 0.0f)
    {
        x += w;
        w = -w;
        float t = r[0];
        r[0] = r[1];
        r[1] = t;
        t = r[2];
        r[2] = r[3];
        r[3] = t;
    }
    if (h < 0.0f)
    {
        y += h;
        h = -h;
        float t = r[0];
        r[0] = r[3];
        r[3] = t;
        t = r[1];
        r[1] = r[2];
        r[2] = t;
    }
    const float pi = (float)M_PI;
    ZvPoint start = tp(c, x + r[0], y);
    bool ok = zv_path_move_to(&c->path, start.x, start.y);
    ZvPoint p = tp(c, x + w - r[1], y);
    ok = ok && zv_path_line_to(&c->path, p.x, p.y);
    if (r[1] > 0.0f)
        ok = ok && arc_segments(c, &c->path, x + w - r[1], y + r[1], r[1], r[1], 0, -pi / 2, 0, false, true);
    p = tp(c, x + w, y + h - r[2]);
    ok = ok && zv_path_line_to(&c->path, p.x, p.y);
    if (r[2] > 0.0f)
        ok = ok && arc_segments(c, &c->path, x + w - r[2], y + h - r[2], r[2], r[2], 0, 0, pi / 2, false, true);
    p = tp(c, x + r[3], y + h);
    ok = ok && zv_path_line_to(&c->path, p.x, p.y);
    if (r[3] > 0.0f)
        ok = ok && arc_segments(c, &c->path, x + r[3], y + h - r[3], r[3], r[3], 0, pi / 2, pi, false, true);
    p = tp(c, x, y + r[0]);
    ok = ok && zv_path_line_to(&c->path, p.x, p.y);
    if (r[0] > 0.0f)
        ok = ok && arc_segments(c, &c->path, x + r[0], y + r[0], r[0], r[0], 0, pi, 3 * pi / 2, false, true);
    ok = ok && zv_path_close(&c->path);
    ZvPoint again = tp(c, x, y);
    return ok && zv_path_move_to(&c->path, again.x, again.y);
}

bool zv_canvas_fill(ZvCanvas *c, ZvFillRule rule)
{
    return fill_device_path(c, &c->path, rule, &c->state.fill);
}

bool zv_canvas_stroke(ZvCanvas *c)
{
    return stroke_device_path(c, &c->path, &c->state.stroke);
}

static bool user_path_to_device(ZvCanvas *c, const ZvPath *path)
{
    zv_path_clear(&c->scratch_path);
    return zv_path_append(&c->scratch_path, path, &c->state.transform);
}

bool zv_canvas_fill_path(ZvCanvas *c, const ZvPath *path, ZvFillRule rule)
{
    return user_path_to_device(c, path) && fill_device_path(c, &c->scratch_path, rule, &c->state.fill);
}

bool zv_canvas_stroke_path(ZvCanvas *c, const ZvPath *path)
{
    return user_path_to_device(c, path) && stroke_device_path(c, &c->scratch_path, &c->state.stroke);
}

/* Clip */

typedef struct
{
    uint8_t *mask;
    int width;
} ClipSink;

static void clip_run(void *context, int y, int x, int length, uint32_t coverage)
{
    ClipSink *k = context;
    memset(k->mask + (size_t)y * (size_t)k->width + (size_t)x, (int)coverage, (size_t)length);
}

static void clip_mask_cb(void *context, int y, int x, int length, const uint8_t *coverage)
{
    ClipSink *k = context;
    memcpy(k->mask + (size_t)y * (size_t)k->width + (size_t)x, coverage, (size_t)length);
}

static bool clip_device_path(ZvCanvas *c, const ZvPath *device_path, ZvFillRule rule)
{
    ZvCanvasState *s = &c->state;
    zv_polyline_clear(&c->poly);
    if (!zv_path_flatten(device_path, NULL, ZV_FLATTEN_TOLERANCE, &c->poly))
        return false;
    ZvBounds b = zv_polyline_bounds(&c->poly);
    ZvBounds nb = zv_bounds_intersect(&s->clip_bounds, &b);
    int w = c->target->width, h = c->target->height;
    /* an axis-aligned rectangle on whole pixels keeps the clip a rectangle
       (zv_canvas_rect leaves a one-point subpath behind it) */
    int real = 0, rect_contour = -1;
    for (int k = 0; k < c->poly.contour_count; k++)
    {
        if (c->poly.contours[k].count > 1)
        {
            real++;
            rect_contour = k;
        }
    }
    bool rect = real == 1 && c->poly.contours[rect_contour].count == 4 && !s->clip_mask;
    if (rect)
    {
        const ZvPoint *p = c->poly.points + c->poly.contours[rect_contour].first;
        bool aligned = (p[0].x == p[1].x && p[1].y == p[2].y && p[2].x == p[3].x && p[3].y == p[0].y) ||
                       (p[0].y == p[1].y && p[1].x == p[2].x && p[2].y == p[3].y && p[3].x == p[0].x);
        rect = aligned && b.min_x == floorf(b.min_x) && b.min_y == floorf(b.min_y) && b.max_x == floorf(b.max_x) && b.max_y == floorf(b.max_y);
    }
    if (rect)
    {
        s->clip_bounds = nb;
        return true;
    }
    size_t n = (size_t)w * (size_t)h;
    uint8_t *mask = c->allocator->allocate(c->allocator->context, n);
    if (!mask)
        return false;
    memset(mask, 0, n);
    int x0 = zv_clampi(zv_floor_int(nb.min_x), 0, w), y0 = zv_clampi(zv_floor_int(nb.min_y), 0, h);
    int x1 = zv_clampi(zv_ceil_int(nb.max_x), 0, w), y1 = zv_clampi(zv_ceil_int(nb.max_y), 0, h);
    if (x1 > x0 && y1 > y0)
    {
        zv_rasterizer_set_clip(&c->raster, x0, y0, x1, y1);
        zv_rasterizer_add_polyline(&c->raster, &c->poly);
        ClipSink k = {mask, w};
        ZvSpanSink sink = {clip_run, clip_mask_cb, &k};
        if (!zv_rasterizer_sweep(&c->raster, rule, &sink))
        {
            c->allocator->release(c->allocator->context, mask);
            return false;
        }
        if (s->clip_mask)
        {
            for (size_t i = 0; i < n; i++)
                mask[i] = (uint8_t)zv_div255((uint32_t)mask[i] * s->clip_mask[i]);
        }
    }
    release_mask(c, s);
    s->clip_mask = mask;
    s->clip_bounds = nb;
    return true;
}

bool zv_canvas_clip(ZvCanvas *c, ZvFillRule rule)
{
    return clip_device_path(c, &c->path, rule);
}

bool zv_canvas_clip_path(ZvCanvas *c, const ZvPath *path, ZvFillRule rule)
{
    return user_path_to_device(c, path) && clip_device_path(c, &c->scratch_path, rule);
}

bool zv_canvas_is_point_in_path(ZvCanvas *c, float x, float y, ZvFillRule rule)
{
    zv_polyline_clear(&c->poly);
    if (!zv_path_flatten(&c->path, NULL, ZV_FLATTEN_TOLERANCE, &c->poly))
        return false;
    ZvPoint p = tp(c, x, y);
    int winding = 0;
    for (int k = 0; k < c->poly.contour_count; k++)
    {
        const ZvContour *ct = &c->poly.contours[k];
        const ZvPoint *pts = c->poly.points + ct->first;
        for (int i = 0; i < ct->count; i++)
        {
            ZvPoint a = pts[i], b = pts[(i + 1) % ct->count];
            if ((a.y <= p.y) != (b.y <= p.y))
            {
                float t = (p.y - a.y) / (b.y - a.y);
                float ix = a.x + t * (b.x - a.x);
                if (ix > p.x)
                    winding += b.y > a.y ? 1 : -1;
            }
        }
    }
    return rule == ZV_FILL_NONZERO ? winding != 0 : (winding & 1) != 0;
}

/* Images */

bool zv_canvas_draw_image(ZvCanvas *c, const ZvSurface *image, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh)
{
    if (!image || !image->pixels || !finite4(sx, sy, sw, sh) || !finite4(dx, dy, dw, dh) || sw == 0.0f || sh == 0.0f || dw == 0.0f || dh == 0.0f)
        return true;
    if (sw < 0.0f)
    {
        sx += sw;
        sw = -sw;
    }
    if (sh < 0.0f)
    {
        sy += sh;
        sh = -sh;
    }
    /* clip the source rectangle to the image, moving the destination with it */
    float scale_x = dw / sw, scale_y = dh / sh;
    float cx0 = sx < 0.0f ? 0.0f : sx, cy0 = sy < 0.0f ? 0.0f : sy;
    float cx1 = sx + sw > (float)image->width ? (float)image->width : sx + sw;
    float cy1 = sy + sh > (float)image->height ? (float)image->height : sy + sh;
    if (cx1 <= cx0 || cy1 <= cy0)
        return true;
    dx += (cx0 - sx) * scale_x;
    dy += (cy0 - sy) * scale_y;
    dw = (cx1 - cx0) * scale_x;
    dh = (cy1 - cy0) * scale_y;
    /* a view of the whole-pixel source rectangle */
    int ix0 = zv_floor_int(cx0), iy0 = zv_floor_int(cy0), ix1 = zv_ceil_int(cx1), iy1 = zv_ceil_int(cy1);
    ZvSurface view;
    view.pixels = image->pixels + (size_t)iy0 * (size_t)image->stride + (size_t)ix0;
    view.width = ix1 - ix0;
    view.height = iy1 - iy0;
    view.stride = image->stride;
    view.allocator = NULL;
    ZvPaint paint = zv_paint_pattern(&view, false, false, c->state.image_smoothing ? ZV_FILTER_BILINEAR : ZV_FILTER_NEAREST);
    /* bitmap pixel (u, v) -> user: dx + (u + ix0 - cx0) * scale_x */
    paint.matrix = zv_matrix_make(scale_x, 0, 0, scale_y, dx + ((float)ix0 - cx0) * scale_x, dy + ((float)iy0 - cy0) * scale_y);
    zv_path_clear(&c->scratch_path);
    if (!rect_path(c, &c->scratch_path, dx, dy, dw, dh))
        return false;
    return fill_device_path(c, &c->scratch_path, ZV_FILL_NONZERO, &paint);
}

bool zv_canvas_draw_image_simple(ZvCanvas *c, const ZvSurface *image, float dx, float dy)
{
    if (!image)
        return true;
    return zv_canvas_draw_image(c, image, 0, 0, (float)image->width, (float)image->height, dx, dy, (float)image->width, (float)image->height);
}

void zv_canvas_get_image_data(const ZvCanvas *c, int x, int y, int w, int h, uint32_t *out)
{
    for (int j = 0; j < h; j++)
    {
        for (int i = 0; i < w; i++)
        {
            int px = x + i, py = y + j;
            uint32_t v = 0;
            if (px >= 0 && py >= 0 && px < c->target->width && py < c->target->height)
                v = zv_unpremultiply(c->target->pixels[(size_t)py * (size_t)c->target->stride + (size_t)px]);
            out[(size_t)j * (size_t)w + (size_t)i] = v;
        }
    }
}

void zv_canvas_put_image_data(ZvCanvas *c, const uint32_t *data, int w, int h, int x, int y)
{
    for (int j = 0; j < h; j++)
    {
        int py = y + j;
        if (py < 0 || py >= c->target->height)
            continue;
        for (int i = 0; i < w; i++)
        {
            int px = x + i;
            if (px < 0 || px >= c->target->width)
                continue;
            c->target->pixels[(size_t)py * (size_t)c->target->stride + (size_t)px] = zv_premultiply(data[(size_t)j * (size_t)w + (size_t)i]);
        }
    }
}

/* Text */

static bool text_path(ZvCanvas *c, const char *utf8, float x, float y, ZvPath *out)
{
    const ZvCanvasState *s = &c->state;
    if (!s->font || !utf8)
        return false;
    ZvFont *font = (ZvFont *)s->font;
    float size = s->font_size;
    float width = zv_font_text_width(font, utf8, size);
    switch (s->text_align)
    {
    case ZV_ALIGN_CENTER:
        x -= width * 0.5f;
        break;
    case ZV_ALIGN_RIGHT:
    case ZV_ALIGN_END:
        x -= width;
        break;
    default:
        break;
    }
    float scale = size / (float)font->units_per_em;
    switch (s->text_baseline)
    {
    case ZV_BASELINE_TOP:
        y += (float)font->ascent * scale;
        break;
    case ZV_BASELINE_HANGING:
        y += (float)font->ascent * scale * 0.8f;
        break;
    case ZV_BASELINE_MIDDLE:
        y += (float)(font->ascent + font->descent) * 0.5f * scale;
        break;
    case ZV_BASELINE_BOTTOM:
    case ZV_BASELINE_IDEOGRAPHIC:
        y += (float)font->descent * scale;
        break;
    default:
        break;
    }
    zv_path_clear(out);
    zv_font_text_path(font, utf8, x, y, size, out);
    return true;
}

bool zv_canvas_fill_text(ZvCanvas *c, const char *utf8, float x, float y)
{
    if (!text_path(c, utf8, x, y, &c->scratch_path))
        return true;
    ZvPath device;
    zv_path_init(&device, c->allocator);
    bool ok = zv_path_append(&device, &c->scratch_path, &c->state.transform) && fill_device_path(c, &device, ZV_FILL_NONZERO, &c->state.fill);
    zv_path_release(&device);
    return ok;
}

bool zv_canvas_stroke_text(ZvCanvas *c, const char *utf8, float x, float y)
{
    if (!text_path(c, utf8, x, y, &c->scratch_path))
        return true;
    ZvPath device;
    zv_path_init(&device, c->allocator);
    bool ok = zv_path_append(&device, &c->scratch_path, &c->state.transform) && stroke_device_path(c, &device, &c->state.stroke);
    zv_path_release(&device);
    return ok;
}

float zv_canvas_measure_text(const ZvCanvas *c, const char *utf8)
{
    if (!c->state.font || !utf8)
        return 0.0f;
    return zv_font_text_width((ZvFont *)c->state.font, utf8, c->state.font_size);
}

/* Layers */

bool zv_canvas_begin_layer(ZvCanvas *c)
{
    if (c->layer_depth >= ZV_MAX_LAYERS)
        return false;
    ZvSurface *layer = &c->layers[c->layer_depth];
    if (!zv_surface_init(layer, c->allocator, c->target->width, c->target->height))
        return false;
    c->layer_targets[c->layer_depth] = c->target;
    c->layer_depth++;
    c->target = layer;
    return true;
}

bool zv_canvas_end_layer(ZvCanvas *c, float alpha, const uint8_t *mask)
{
    if (c->layer_depth == 0)
        return false;
    c->layer_depth--;
    ZvSurface *layer = &c->layers[c->layer_depth];
    c->target = c->layer_targets[c->layer_depth];
    uint32_t a = (uint32_t)(zv_clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    int x0, y0, x1, y1;
    clip_box(c, &x0, &y0, &x1, &y1);
    int w = c->target->width;
    for (int y = y0; y < y1; y++)
    {
        ZvPixel *dst = c->target->pixels + (size_t)y * (size_t)c->target->stride;
        const ZvPixel *src = layer->pixels + (size_t)y * (size_t)layer->stride;
        const uint8_t *m = mask ? mask + (size_t)y * (size_t)w : NULL;
        const uint8_t *clip = c->state.clip_mask ? c->state.clip_mask + (size_t)y * (size_t)w : NULL;
        for (int x = x0; x < x1; x++)
        {
            ZvPixel s = src[x];
            if (s == 0 && c->state.op == ZV_OP_SOURCE_OVER)
                continue;
            uint32_t k = a;
            if (m)
                k = zv_div255(k * m[x]);
            if (clip)
                k = zv_div255(k * clip[x]);
            if (k == 0)
                continue;
            dst[x] = c->state.op == ZV_OP_SOURCE_OVER ? zv_blend_over(dst[x], zv_scale(s, k)) : composite_cover(dst[x], s, c->state.op, k);
        }
    }
    zv_surface_release(layer);
    return true;
}
