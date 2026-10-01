#include "zv_paint.h"
#include "zv_internal.h"

static ZvPaint paint_base(ZvPaintType type)
{
    ZvPaint p;
    memset(&p, 0, sizeof p);
    p.type = type;
    p.alpha = 255;
    p.matrix = zv_matrix_identity();
    p.filter = ZV_FILTER_BILINEAR;
    return p;
}

ZvPaint zv_paint_solid(uint32_t straight_color)
{
    ZvPaint p = paint_base(ZV_PAINT_SOLID);
    p.color = straight_color;
    return p;
}

ZvPaint zv_paint_linear(float x0, float y0, float x1, float y1)
{
    ZvPaint p = paint_base(ZV_PAINT_LINEAR);
    p.x0 = x0;
    p.y0 = y0;
    p.x1 = x1;
    p.y1 = y1;
    return p;
}

ZvPaint zv_paint_radial(float x0, float y0, float r0, float x1, float y1, float r1)
{
    ZvPaint p = paint_base(ZV_PAINT_RADIAL);
    p.x0 = x0;
    p.y0 = y0;
    p.r0 = r0;
    p.x1 = x1;
    p.y1 = y1;
    p.r1 = r1;
    return p;
}

ZvPaint zv_paint_pattern(const ZvSurface *bitmap, bool repeat_x, bool repeat_y, ZvFilter filter)
{
    ZvPaint p = paint_base(ZV_PAINT_PATTERN);
    p.bitmap = bitmap;
    p.repeat_x = repeat_x;
    p.repeat_y = repeat_y;
    p.filter = filter;
    return p;
}

bool zv_paint_add_stop(ZvPaint *paint, float offset, uint32_t straight_color)
{
    if (paint->stop_count >= ZV_MAX_STOPS || !(offset >= 0.0f && offset <= 1.0f))
        return false;
    int i = paint->stop_count;
    while (i > 0 && paint->stops[i - 1].offset > offset)
    {
        paint->stops[i] = paint->stops[i - 1];
        i--;
    }
    paint->stops[i].offset = offset;
    paint->stops[i].color = straight_color;
    paint->stop_count++;
    return true;
}

/* Gradient table */

static ZvPixel apply_alpha(uint32_t straight, uint32_t alpha)
{
    if (alpha < 255u)
        straight = (zv_div255((straight >> 24) * alpha) << 24) | (straight & 0x00FFFFFFu);
    return zv_premultiply(straight);
}

static void split_color(uint32_t c, float out[4])
{
    out[0] = (float)(c >> 24);
    out[1] = (float)((c >> 16) & 0xFFu);
    out[2] = (float)((c >> 8) & 0xFFu);
    out[3] = (float)(c & 0xFFu);
}

/* Straight colour at t, interpolated in float, as 4 channels a r g b. */
static void gradient_color(const ZvPaint *p, float t, float out[4])
{
    if (p->stop_count == 0)
    {
        out[0] = out[1] = out[2] = out[3] = 0.0f;
        return;
    }
    if (t <= p->stops[0].offset)
    {
        split_color(p->stops[0].color, out);
        return;
    }
    for (int i = 1; i < p->stop_count; i++)
    {
        if (t <= p->stops[i].offset)
        {
            float span = p->stops[i].offset - p->stops[i - 1].offset;
            if (span <= 0.0f)
            {
                split_color(p->stops[i].color, out);
                return;
            }
            float f = (t - p->stops[i - 1].offset) / span;
            float a[4], b[4];
            split_color(p->stops[i - 1].color, a);
            split_color(p->stops[i].color, b);
            for (int k = 0; k < 4; k++)
                out[k] = a[k] + (b[k] - a[k]) * f;
            return;
        }
    }
    split_color(p->stops[p->stop_count - 1].color, out);
}

static uint32_t pack_color(const float c[4])
{
    return (uint32_t)(c[0] + 0.5f) << 24 | (uint32_t)(c[1] + 0.5f) << 16 | (uint32_t)(c[2] + 0.5f) << 8 | (uint32_t)(c[3] + 0.5f);
}

static bool build_table(ZvPaintContext *ctx)
{
    const ZvPaint *p = ctx->paint;
    size_t bytes = ZV_GRADIENT_STEPS * (p->dither ? 4 * sizeof(uint16_t) : sizeof(ZvPixel));
    void *memory = ctx->allocator->allocate(ctx->allocator->context, bytes);
    if (!memory)
        return false;
    ctx->table = memory;
    ctx->table16 = memory;
    bool opaque = p->alpha == 255u;
    for (int i = 0; i < ZV_GRADIENT_STEPS; i++)
    {
        float t = ((float)i + 0.5f) / (float)ZV_GRADIENT_STEPS;
        float c[4];
        gradient_color(p, t, c);
        if (c[0] < 254.5f)
            opaque = false;
        if (p->dither)
        {
            float a = c[0] * (float)p->alpha / 255.0f;
            uint16_t *e = ctx->table16 + 4 * i;
            e[0] = (uint16_t)(a * 256.0f + 0.5f);
            for (int k = 1; k < 4; k++)
                e[k] = (uint16_t)(c[k] * a / 255.0f * 256.0f + 0.5f);
        }
        else
        {
            ctx->table[i] = apply_alpha(pack_color(c), p->alpha);
        }
    }
    ctx->opaque = opaque;
    return true;
}

bool zv_paint_prepare(ZvPaintContext *ctx, const ZvPaint *paint, const ZvMatrix *user_to_device, const ZvAllocator *allocator)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->paint = paint;
    ctx->allocator = allocator ? allocator : zv_default_allocator();
    ctx->is_solid = false;

    if (paint->type == ZV_PAINT_SOLID)
    {
        ctx->solid = apply_alpha(paint->color, paint->alpha);
        ctx->is_solid = true;
        ctx->opaque = (ctx->solid >> 24) == 255u;
        return true;
    }

    /* device -> user -> paint */
    ZvMatrix identity = zv_matrix_identity();
    if (!user_to_device)
        user_to_device = &identity;
    ZvMatrix paint_to_device = zv_matrix_concat(user_to_device, &paint->matrix);
    if (!zv_matrix_invert(&paint_to_device, &ctx->inverse))
    {
        /* degenerate transform: nothing visible; a transparent solid */
        ctx->solid = 0;
        ctx->is_solid = true;
        ctx->opaque = false;
        return true;
    }

    if (paint->type == ZV_PAINT_PATTERN)
    {
        if (!paint->bitmap || !paint->bitmap->pixels)
            return false;
        ctx->opaque = false;
        return true;
    }

    if (paint->stop_count == 0)
    {
        ctx->solid = 0;
        ctx->is_solid = true;
        return true;
    }
    if (paint->stop_count == 1)
    {
        ctx->solid = apply_alpha(paint->stops[0].color, paint->alpha);
        ctx->is_solid = true;
        ctx->opaque = (ctx->solid >> 24) == 255u;
        return true;
    }

    if (paint->type == ZV_PAINT_LINEAR)
    {
        float dx = paint->x1 - paint->x0;
        float dy = paint->y1 - paint->y0;
        float len2 = dx * dx + dy * dy;
        if (len2 <= 0.0f)
        {
            /* the Canvas paints nothing for a zero-length gradient */
            ctx->solid = 0;
            ctx->is_solid = true;
            return true;
        }
        /* t(p) = ((p - p0) . d) / |d|^2 with p = inverse(device) */
        float gx = dx / len2, gy = dy / len2;
        const ZvMatrix *m = &ctx->inverse;
        ctx->lx = gx * m->a + gy * m->b;
        ctx->ly = gx * m->c + gy * m->d;
        ctx->l0 = gx * (m->e - paint->x0) + gy * (m->f - paint->y0);
    }
    else
    {
        ctx->cdx = paint->x1 - paint->x0;
        ctx->cdy = paint->y1 - paint->y0;
        ctx->dr = paint->r1 - paint->r0;
        ctx->a = ctx->cdx * ctx->cdx + ctx->cdy * ctx->cdy - ctx->dr * ctx->dr;
        ctx->radial_simple = ctx->cdx == 0.0f && ctx->cdy == 0.0f;
        if (ctx->radial_simple && ctx->dr == 0.0f)
        {
            ctx->solid = 0;
            ctx->is_solid = true;
            return true;
        }
    }
    return build_table(ctx);
}

void zv_paint_release(ZvPaintContext *ctx)
{
    if (!ctx)
        return;
    if (ctx->table)
        ctx->allocator->release(ctx->allocator->context, ctx->table);
    if (ctx->span)
        ctx->allocator->release(ctx->allocator->context, ctx->span);
    ctx->table = NULL;
    ctx->span = NULL;
    ctx->span_capacity = 0;
}

/* Lookup */

static const uint8_t bayer4[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

static inline ZvPixel lookup(const ZvPaintContext *ctx, float t, int x, int y)
{
    const ZvPaint *p = ctx->paint;
    if (!(t == t))
        return 0;
    switch (p->spread)
    {
    case ZV_SPREAD_PAD:
        t = zv_clampf(t, 0.0f, 1.0f);
        break;
    case ZV_SPREAD_REPEAT:
        t = t - floorf(t);
        break;
    case ZV_SPREAD_REFLECT:
        t = fabsf(t);
        t = t - 2.0f * floorf(t * 0.5f);
        if (t > 1.0f)
            t = 2.0f - t;
        break;
    }
    int i = (int)(t * (float)ZV_GRADIENT_STEPS);
    if (i < 0)
        i = 0;
    if (i >= ZV_GRADIENT_STEPS)
        i = ZV_GRADIENT_STEPS - 1;
    if (!p->dither)
        return ctx->table[i];
    const uint16_t *e = ctx->table16 + 4 * i;
    int d = (int)bayer4[y & 3][x & 3] * 16 - 120; /* -120..120 of 256, zero mean */
    uint32_t a = (uint32_t)((e[0] + 128 + d) >> 8);
    uint32_t r = (uint32_t)((e[1] + 128 + d) >> 8);
    uint32_t g = (uint32_t)((e[2] + 128 + d) >> 8);
    uint32_t b = (uint32_t)((e[3] + 128 + d) >> 8);
    if (a > 255u)
        a = 255u;
    if (r > a)
        r = a;
    if (g > a)
        g = a;
    if (b > a)
        b = a;
    return a << 24 | r << 16 | g << 8 | b;
}

static void span_linear(const ZvPaintContext *ctx, int y, int x, int count, ZvPixel *out)
{
    float t = ctx->lx * ((float)x + 0.5f) + ctx->ly * ((float)y + 0.5f) + ctx->l0;
    for (int i = 0; i < count; i++)
    {
        out[i] = lookup(ctx, t, x + i, y);
        t += ctx->lx;
    }
}

/* Canvas radial: the largest t with |p - c(t)| = r(t) and r(t) >= 0, where
   c(t) = c0 + t (c1 - c0), r(t) = r0 + t (r1 - r0). */
static float radial_t(const ZvPaintContext *ctx, float px, float py)
{
    const ZvPaint *p = ctx->paint;
    float pdx = px - p->x0;
    float pdy = py - p->y0;
    if (ctx->radial_simple)
        return (sqrtf(pdx * pdx + pdy * pdy) - p->r0) / ctx->dr;
    float b = pdx * ctx->cdx + pdy * ctx->cdy + p->r0 * ctx->dr;
    float c = pdx * pdx + pdy * pdy - p->r0 * p->r0;
    if (ctx->a == 0.0f)
    {
        if (b == 0.0f)
            return NAN;
        float t = c / (2.0f * b);
        return p->r0 + t * ctx->dr >= 0.0f ? t : NAN;
    }
    float disc = b * b - ctx->a * c;
    if (disc < 0.0f)
        return NAN;
    float s = sqrtf(disc);
    float t1 = (b + s) / ctx->a;
    float t2 = (b - s) / ctx->a;
    if (t1 < t2)
    {
        float tmp = t1;
        t1 = t2;
        t2 = tmp;
    }
    if (p->r0 + t1 * ctx->dr >= 0.0f)
        return t1;
    if (p->r0 + t2 * ctx->dr >= 0.0f)
        return t2;
    return NAN;
}

static void span_radial(const ZvPaintContext *ctx, int y, int x, int count, ZvPixel *out)
{
    const ZvMatrix *m = &ctx->inverse;
    float dy = (float)y + 0.5f;
    float px = m->a * ((float)x + 0.5f) + m->c * dy + m->e;
    float py = m->b * ((float)x + 0.5f) + m->d * dy + m->f;
    for (int i = 0; i < count; i++)
    {
        out[i] = lookup(ctx, radial_t(ctx, px, py), x + i, y);
        px += m->a;
        py += m->b;
    }
}

/* Pattern */

static inline int wrap(int v, int n, bool repeat)
{
    if (repeat)
    {
        v %= n;
        return v < 0 ? v + n : v;
    }
    return v;
}

static inline ZvPixel sample_nearest(const ZvSurface *s, int ix, int iy, bool rx, bool ry)
{
    ix = wrap(ix, s->width, rx);
    iy = wrap(iy, s->height, ry);
    if (ix < 0 || iy < 0 || ix >= s->width || iy >= s->height)
        return 0;
    return s->pixels[(size_t)iy * (size_t)s->stride + (size_t)ix];
}

static inline ZvPixel lerp_pixel(ZvPixel a, ZvPixel b, uint32_t f)
{
    /* f in 0..256 */
    uint32_t rb = ((a & 0x00FF00FFu) * (256u - f) + (b & 0x00FF00FFu) * f) >> 8;
    uint32_t ag = (((a >> 8) & 0x00FF00FFu) * (256u - f) + ((b >> 8) & 0x00FF00FFu) * f) >> 8;
    return (rb & 0x00FF00FFu) | ((ag & 0x00FF00FFu) << 8);
}

static void span_pattern(const ZvPaintContext *ctx, int y, int x, int count, ZvPixel *out)
{
    const ZvPaint *p = ctx->paint;
    const ZvSurface *s = p->bitmap;
    const ZvMatrix *m = &ctx->inverse;
    float dy = (float)y + 0.5f;
    float u = m->a * ((float)x + 0.5f) + m->c * dy + m->e;
    float v = m->b * ((float)x + 0.5f) + m->d * dy + m->f;
    uint32_t alpha = p->alpha;
    bool rx = p->repeat_x, ry = p->repeat_y;
    for (int i = 0; i < count; i++)
    {
        ZvPixel c;
        if (p->filter == ZV_FILTER_NEAREST)
        {
            c = sample_nearest(s, zv_floor_int(u), zv_floor_int(v), rx, ry);
        }
        else
        {
            float fu = u - 0.5f, fv = v - 0.5f;
            int iu = zv_floor_int(fu), iv = zv_floor_int(fv);
            uint32_t fx = (uint32_t)((fu - (float)iu) * 256.0f);
            uint32_t fy = (uint32_t)((fv - (float)iv) * 256.0f);
            ZvPixel c00 = sample_nearest(s, iu, iv, rx, ry);
            ZvPixel c10 = sample_nearest(s, iu + 1, iv, rx, ry);
            ZvPixel c01 = sample_nearest(s, iu, iv + 1, rx, ry);
            ZvPixel c11 = sample_nearest(s, iu + 1, iv + 1, rx, ry);
            c = lerp_pixel(lerp_pixel(c00, c10, fx), lerp_pixel(c01, c11, fx), fy);
        }
        if (alpha < 255u)
            c = zv_scale(c, alpha);
        out[i] = c;
        u += m->a;
        v += m->b;
    }
}

void zv_paint_span(const ZvPaintContext *ctx, int y, int x, int count, ZvPixel *out)
{
    if (ctx->is_solid)
    {
        for (int i = 0; i < count; i++)
            out[i] = ctx->solid;
        return;
    }
    switch (ctx->paint->type)
    {
    case ZV_PAINT_LINEAR:
        span_linear(ctx, y, x, count, out);
        break;
    case ZV_PAINT_RADIAL:
        span_radial(ctx, y, x, count, out);
        break;
    case ZV_PAINT_PATTERN:
        span_pattern(ctx, y, x, count, out);
        break;
    default:
        for (int i = 0; i < count; i++)
            out[i] = ctx->solid;
        break;
    }
}

/* Spans of per-pixel colours */

void zv_span_pixels(ZvPixel *dst, const ZvPixel *src, size_t count)
{
    for (size_t i = 0; i < count; i++)
    {
        ZvPixel s = src[i];
        uint32_t a = s >> 24;
        if (a == 255u)
            dst[i] = s;
        else if (a != 0u)
            dst[i] = zv_blend_over(dst[i], s);
    }
}

void zv_span_pixels_cover(ZvPixel *dst, const ZvPixel *src, size_t count, uint32_t cover)
{
    if (cover == 0u)
        return;
    if (cover >= 255u)
    {
        zv_span_pixels(dst, src, count);
        return;
    }
    for (size_t i = 0; i < count; i++)
    {
        ZvPixel s = src[i];
        if ((s >> 24) != 0u)
            dst[i] = zv_blend_over(dst[i], zv_scale(s, cover));
    }
}

void zv_span_pixels_mask(ZvPixel *dst, const ZvPixel *src, size_t count, const uint8_t *cover)
{
    for (size_t i = 0; i < count; i++)
    {
        uint32_t k = cover[i];
        ZvPixel s = src[i];
        if (k == 0u || (s >> 24) == 0u)
            continue;
        dst[i] = zv_blend_over(dst[i], k == 255u ? s : zv_scale(s, k));
    }
}
