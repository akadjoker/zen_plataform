/*
 * draw2d.c - software rasterizer over a Framebuffer. Single format (0xAARRGGBB),
 * integer math only (no libm), every primitive clips to the framebuffer. The API
 * is declared in platform.h.
 */
#include "platform.h"

#include <stdlib.h>
#include <string.h>

/* ---- optional scissor clip ------------------------------------------------ */

static struct { bool on; int x, y, w, h; } g_clip;

void draw_set_clip(int x, int y, int w, int h)
{
    g_clip.on = true;
    g_clip.x = x;
    g_clip.y = y;
    g_clip.w = w;
    g_clip.h = h;
}

void draw_reset_clip(void)
{
    g_clip.on = false;
}

/* The far edges are summed in 64 bits: an extent of INT_MAX means "as far as there is", and x + w would wrap round to a
   negative number and reject everything. */
static inline bool clip_reject(int x, int y)
{
    return g_clip.on && (x < g_clip.x || y < g_clip.y || x >= (int64_t)g_clip.x + g_clip.w ||
                         y >= (int64_t)g_clip.y + g_clip.h);
}

/* ---- framebuffer ownership ---- */

bool framebuffer_alloc(Framebuffer *fb, int width, int height)
{
    if (!fb || width <= 0 || height <= 0)
        return false;
    fb->pixels = calloc((size_t)width * height, sizeof *fb->pixels);
    if (!fb->pixels)
        return false;
    fb->width = width;
    fb->height = height;
    fb->stride = width;
    return true;
}

bool framebuffer_from_pixels(Framebuffer *fb, const uint32_t *pixels, int width, int height)
{
    if (!pixels || !framebuffer_alloc(fb, width, height))
        return false;
    memcpy(fb->pixels, pixels, (size_t)width * height * sizeof *fb->pixels);
    return true;
}

void framebuffer_free(Framebuffer *fb)
{
    if (!fb)
        return;
    free(fb->pixels);
    fb->pixels = NULL;
    fb->width = fb->height = fb->stride = 0;
}

/* ---- color helpers ---- */

static inline uint32_t blend_over(uint32_t dst, uint32_t src)
{
    uint32_t sa = src >> 24;
    if (sa == 0xFF)
        return src | 0xFF000000u;
    if (sa == 0)
        return dst;
    uint32_t ia = 255 - sa;
    uint32_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint32_t dr = (dst >> 16) & 0xFF, dg = (dst >> 8) & 0xFF, db = dst & 0xFF;
    uint32_t r = (sr * sa + dr * ia + 127) / 255;
    uint32_t g = (sg * sa + dg * ia + 127) / 255;
    uint32_t b = (sb * sa + db * ia + 127) / 255;
    return 0xFF000000u | (r << 16) | (g << 8) | b;
}

static long isqrt_l(long v)
{
    if (v <= 0)
        return 0;
    long x = v, y = (x + 1) / 2;
    while (y < x)
    {
        x = y;
        y = (x + v / x) / 2;
    }
    return x;
}

/* ---- pixel ---- */

void draw_pixel(Framebuffer *fb, int x, int y, uint32_t color, BlendMode blend)
{
    if (!fb || !fb->pixels || x < 0 || y < 0 || x >= fb->width || y >= fb->height)
        return;
    if (clip_reject(x, y))
        return;
    uint32_t *p = &fb->pixels[(size_t)y * fb->stride + x];
    *p = blend == BLEND_ALPHA ? blend_over(*p, color) : color;
}

uint32_t draw_get_pixel(const Framebuffer *fb, int x, int y)
{
    if (!fb || !fb->pixels || x < 0 || y < 0 || x >= fb->width || y >= fb->height)
        return 0;
    return fb->pixels[(size_t)y * fb->stride + x];
}

void draw_clear(Framebuffer *fb, uint32_t color)
{
    if (!fb || !fb->pixels)
        return;
    for (int y = 0; y < fb->height; y++)
    {
        uint32_t *row = fb->pixels + (size_t)y * fb->stride;
        for (int x = 0; x < fb->width; x++)
            row[x] = color;
    }
}

/* ---- lines, rects ---- */

void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t color, BlendMode blend)
{
    int dx = x1 - x0, dy = y1 - y0;
    int sx = dx < 0 ? -1 : 1, sy = dy < 0 ? -1 : 1;
    dx = dx < 0 ? -dx : dx;
    dy = dy < 0 ? -dy : dy;
    int err = (dx > dy ? dx : -dy) / 2;
    for (;;)
    {
        draw_pixel(fb, x0, y0, color, blend);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = err;
        if (e2 > -dx)
        {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dy)
        {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color, BlendMode blend)
{
    if (!fb || !fb->pixels || w <= 0 || h <= 0)
        return;
    /* 64 bits for the far edges, see clip_reject: x + w wraps for a rectangle of width INT_MAX */
    int64_t x0 = x < 0 ? 0 : x;
    int64_t y0 = y < 0 ? 0 : y;
    int64_t x1 = (int64_t)x + w > fb->width ? fb->width : (int64_t)x + w;
    int64_t y1 = (int64_t)y + h > fb->height ? fb->height : (int64_t)y + h;
    if (g_clip.on) /* honour the scissor like the per-pixel primitives do */
    {
        if (x0 < g_clip.x) x0 = g_clip.x;
        if (y0 < g_clip.y) y0 = g_clip.y;
        if (x1 > (int64_t)g_clip.x + g_clip.w) x1 = (int64_t)g_clip.x + g_clip.w;
        if (y1 > (int64_t)g_clip.y + g_clip.h) y1 = (int64_t)g_clip.y + g_clip.h;
    }
    /* all four now lie inside the framebuffer (or the range is empty): they fit an int again */
    for (int yy = (int)y0; yy < (int)y1; yy++)
    {
        uint32_t *row = fb->pixels + (size_t)yy * fb->stride;
        for (int xx = (int)x0; xx < (int)x1; xx++)
            row[xx] = blend == BLEND_ALPHA ? blend_over(row[xx], color) : color;
    }
}

void draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color, BlendMode blend)
{
    if (w <= 0 || h <= 0)
        return;
    draw_line(fb, x, y, x + w - 1, y, color, blend);
    draw_line(fb, x, y + h - 1, x + w - 1, y + h - 1, color, blend);
    draw_line(fb, x, y, x, y + h - 1, color, blend);
    draw_line(fb, x + w - 1, y, x + w - 1, y + h - 1, color, blend);
}

/* ---- rounded rects ---- */

static int round_radius(int w, int h, int r)
{
    int m = (w < h ? w : h) / 2;
    if (r > m)
        r = m;
    return r;
}

void draw_fill_round_rect(Framebuffer *fb, int x, int y, int w, int h, int radius, uint32_t color, BlendMode blend)
{
    if (!fb || !fb->pixels || w <= 0 || h <= 0)
        return;
    int r = round_radius(w, h, radius);
    if (r <= 0)
    {
        draw_fill_rect(fb, x, y, w, h, color, blend);
        return;
    }
    /* Middle band spans the full width; the two end bands inset each row by the
       corner arc. vd is the vertical distance from the corner centre. */
    draw_fill_rect(fb, x, y + r, w, h - 2 * r, color, blend);
    for (int dy = 0; dy < r; dy++)
    {
        int vd = r - dy;
        int chord = (int)isqrt_l((long)r * r - (long)vd * vd);
        int inset = r - chord;
        draw_fill_rect(fb, x + inset, y + dy, w - 2 * inset, 1, color, blend);
        draw_fill_rect(fb, x + inset, y + h - 1 - dy, w - 2 * inset, 1, color, blend);
    }
}

void draw_round_rect(Framebuffer *fb, int x, int y, int w, int h, int radius, uint32_t color, BlendMode blend)
{
    if (w <= 0 || h <= 0)
        return;
    int r = round_radius(w, h, radius);
    if (r <= 0)
    {
        draw_rect(fb, x, y, w, h, color, blend);
        return;
    }
    /* Straight edges between the corner arcs. */
    draw_line(fb, x + r, y, x + w - 1 - r, y, color, blend);
    draw_line(fb, x + r, y + h - 1, x + w - 1 - r, y + h - 1, color, blend);
    draw_line(fb, x, y + r, x, y + h - 1 - r, color, blend);
    draw_line(fb, x + w - 1, y + r, x + w - 1, y + h - 1 - r, color, blend);
    /* Four quarter arcs; plotting both (i,j) and (j,i) leaves no gaps. */
    int cxl = x + r, cxr = x + w - 1 - r, cyt = y + r, cyb = y + h - 1 - r;
    for (int i = 0; i <= r; i++)
    {
        int j = (int)isqrt_l((long)r * r - (long)i * i);
        draw_pixel(fb, cxl - i, cyt - j, color, blend);
        draw_pixel(fb, cxl - j, cyt - i, color, blend);
        draw_pixel(fb, cxr + i, cyt - j, color, blend);
        draw_pixel(fb, cxr + j, cyt - i, color, blend);
        draw_pixel(fb, cxl - i, cyb + j, color, blend);
        draw_pixel(fb, cxl - j, cyb + i, color, blend);
        draw_pixel(fb, cxr + i, cyb + j, color, blend);
        draw_pixel(fb, cxr + j, cyb + i, color, blend);
    }
}

/* ---- circles ---- */

void draw_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color, BlendMode blend)
{
    if (radius < 0)
        return;
    int x = radius, y = 0, err = 0;
    while (x >= y)
    {
        draw_pixel(fb, cx + x, cy + y, color, blend);
        draw_pixel(fb, cx + y, cy + x, color, blend);
        draw_pixel(fb, cx - y, cy + x, color, blend);
        draw_pixel(fb, cx - x, cy + y, color, blend);
        draw_pixel(fb, cx - x, cy - y, color, blend);
        draw_pixel(fb, cx - y, cy - x, color, blend);
        draw_pixel(fb, cx + y, cy - x, color, blend);
        draw_pixel(fb, cx + x, cy - y, color, blend);
        y++;
        if (err <= 0)
            err += 2 * y + 1;
        if (err > 0)
        {
            x--;
            err -= 2 * x + 1;
        }
    }
}

void draw_fill_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color, BlendMode blend)
{
    if (radius < 0)
        return;
    for (int dy = -radius; dy <= radius; dy++)
    {
        int dx = (int)isqrt_l((long)radius * radius - (long)dy * dy);
        draw_fill_rect(fb, cx - dx, cy + dy, 2 * dx + 1, 1, color, blend);
    }
}

/* ---- triangle (edge function over the bounding box, either winding) ---- */

static long edge(int ax, int ay, int bx, int by, int px, int py)
{
    return (long)(bx - ax) * (py - ay) - (long)(by - ay) * (px - ax);
}

void draw_fill_triangle(Framebuffer *fb, int x0, int y0, int x1, int y1, int x2, int y2,
                        uint32_t color, BlendMode blend)
{
    if (!fb || !fb->pixels)
        return;
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2);
    int maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    int maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    if (minx < 0)
        minx = 0;
    if (miny < 0)
        miny = 0;
    if (maxx >= fb->width)
        maxx = fb->width - 1;
    if (maxy >= fb->height)
        maxy = fb->height - 1;

    for (int y = miny; y <= maxy; y++)
        for (int x = minx; x <= maxx; x++)
        {
            long w0 = edge(x1, y1, x2, y2, x, y);
            long w1 = edge(x2, y2, x0, y0, x, y);
            long w2 = edge(x0, y0, x1, y1, x, y);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                draw_pixel(fb, x, y, color, blend);
        }
}

/* ---- blit ---- */

static inline int clamp_index(int64_t v, int count)
{
    return v < 0 ? 0 : v >= count ? count - 1 : (int)v;
}

/* Two texels of a row mixed by tx (0..255), channel by channel. */
static inline uint32_t mix_row(uint32_t p0, uint32_t p1, int tx)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
    {
        int c0 = (p0 >> s) & 0xFF, c1 = (p1 >> s) & 0xFF;
        int v = c0 + (((c1 - c0) * tx) >> 8);
        out |= (uint32_t)(v & 0xFF) << s;
    }
    return out;
}

/* The four texels around a point: two rows mixed by tx, then by ty. */
static inline uint32_t mix_quad(uint32_t p00, uint32_t p10, uint32_t p01, uint32_t p11, int tx, int ty)
{
    uint32_t out = 0;
    for (int s = 0; s < 32; s += 8)
    {
        int c00 = (p00 >> s) & 0xFF, c10 = (p10 >> s) & 0xFF;
        int c01 = (p01 >> s) & 0xFF, c11 = (p11 >> s) & 0xFF;
        int top = c00 + (((c10 - c00) * tx) >> 8);
        int bot = c01 + (((c11 - c01) * tx) >> 8);
        int v = top + (((bot - top) * ty) >> 8);
        out |= (uint32_t)(v & 0xFF) << s;
    }
    return out;
}

void draw_blit(Framebuffer *dst, const Framebuffer *src,
               int sx, int sy, int sw, int sh,
               int dx, int dy, int dw, int dh,
               BlendMode blend, ScaleMode scale)
{
    if (!dst || !dst->pixels || !src || !src->pixels || sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0)
        return;

    /* 64 bits for the fixed-point coordinates: a source rectangle at 32768 or more, or a negative one (a left shift of a
       negative number is undefined), does not fit 16.16 in an int. A coordinate outside the source reads its nearest
       edge, in both scale modes, the neighbour texel of the bilinear one included. */
    const int64_t stepx = ((int64_t)sw * 65536) / dw;
    const int64_t stepy = ((int64_t)sh * 65536) / dh;
    const bool bilinear = scale == SCALE_BILINEAR, alpha = blend == BLEND_ALPHA;

    /* The destination pixels this call can touch: inside the framebuffer and, when it is on, the scissor (the tests
       draw_pixel made for each one). Columns i0..i1 and rows j0..j1 of the destination rectangle. */
    int64_t i0 = dx < 0 ? -(int64_t)dx : 0, i1 = dw;
    int64_t j0 = dy < 0 ? -(int64_t)dy : 0, j1 = dh;
    if (i1 > dst->width - (int64_t)dx)
        i1 = dst->width - (int64_t)dx;
    if (j1 > dst->height - (int64_t)dy)
        j1 = dst->height - (int64_t)dy;
    if (g_clip.on)
    {
        if (i0 < (int64_t)g_clip.x - dx)
            i0 = (int64_t)g_clip.x - dx;
        if (i1 > (int64_t)g_clip.x + g_clip.w - dx)
            i1 = (int64_t)g_clip.x + g_clip.w - dx;
        if (j0 < (int64_t)g_clip.y - dy)
            j0 = (int64_t)g_clip.y - dy;
        if (j1 > (int64_t)g_clip.y + g_clip.h - dy)
            j1 = (int64_t)g_clip.y + g_clip.h - dy;
    }
    if (i0 >= i1 || j0 >= j1)
        return;

    const int64_t xoff = bilinear ? stepx / 2 - 32768 : 0;
    const int64_t yoff = bilinear ? stepy / 2 - 32768 : 0;
    const int64_t fx0 = (int64_t)sx * 65536 + i0 * stepx + xoff;
    /* One destination pixel per source texel across, and every one of them inside the source: no mixing along the row
       (the weight tx is 0) and no clamping, so a row is a plain copy. The common case: an image drawn at its size. */
    const bool unit_x = stepx == 65536 && xoff == 0 && (int64_t)sx + i0 >= 0 && (int64_t)sx + i1 <= src->width;

    for (int64_t j = j0; j < j1; j++)
    {
        uint32_t *drow = dst->pixels + (size_t)(dy + j) * dst->stride;
        uint32_t *out = drow + (dx + i0);
        const int64_t fy = (int64_t)sy * 65536 + j * stepy + yoff;
        const int64_t iy = fy >> 16;
        const uint32_t *r0 = src->pixels + (size_t)clamp_index(iy, src->height) * src->stride;
        const uint32_t *r1 = src->pixels + (size_t)clamp_index(iy + 1, src->height) * src->stride;
        const int ty = (int)((fy >> 8) & 0xFF);
        int64_t fx = fx0;

        if (unit_x && (!bilinear || ty == 0))
        {
            const uint32_t *in = r0 + (sx + i0);
            if (alpha)
                for (int64_t k = 0; k < i1 - i0; k++)
                    out[k] = blend_over(out[k], in[k]);
            else
                for (int64_t k = 0; k < i1 - i0; k++)
                    out[k] = in[k];
            continue;
        }

        for (int64_t i = i0; i < i1; i++, fx += stepx, out++)
        {
            uint32_t c;
            if (!bilinear)
                c = r0[clamp_index(fx >> 16, src->width)];
            else
            {
                const int64_t ix = fx >> 16;
                const int tx = (int)((fx >> 8) & 0xFF);
                const int x0 = clamp_index(ix, src->width), x1 = clamp_index(ix + 1, src->width);
                if (ty == 0) /* on a texel row: the second row has a weight of 0, and the whole mix is the first */
                    c = tx == 0 ? r0[x0] : mix_row(r0[x0], r0[x1], tx);
                else
                    c = mix_quad(r0[x0], r0[x1], r1[x0], r1[x1], tx, ty);
            }
            *out = alpha ? blend_over(*out, c) : c;
        }
    }
}
