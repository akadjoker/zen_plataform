#include "zv_font.h"
#include "zv_internal.h"

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static int16_t rds16(const uint8_t *p)
{
    return (int16_t)rd16(p);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool in_range(const ZvFont *f, uint32_t offset, uint32_t length)
{
    return offset <= f->size && length <= f->size - offset;
}

static uint32_t find_table(const ZvFont *f, const char *tag, uint32_t *length)
{
    if (f->size < 12)
        return 0;
    uint32_t count = rd16(f->data + 4);
    if (!in_range(f, 12, count * 16))
        return 0;
    for (uint32_t i = 0; i < count; i++)
    {
        const uint8_t *e = f->data + 12 + i * 16;
        if (memcmp(e, tag, 4) == 0)
        {
            uint32_t off = rd32(e + 8), len = rd32(e + 12);
            if (!in_range(f, off, len))
                return 0;
            if (length)
                *length = len;
            return off;
        }
    }
    return 0;
}

bool zv_font_init(ZvFont *font, const uint8_t *data, size_t size, const ZvAllocator *allocator)
{
    memset(font, 0, sizeof *font);
    font->data = data;
    font->size = size;
    font->allocator = allocator ? allocator : zv_default_allocator();
    if (!data || size < 12)
        return false;
    uint32_t tag = rd32(data);
    if (tag != 0x00010000u && tag != 0x74727565u) /* 'true' */
        return false;
    uint32_t head = find_table(font, "head", NULL);
    uint32_t hhea = find_table(font, "hhea", NULL);
    uint32_t maxp = find_table(font, "maxp", NULL);
    uint32_t loca_len = 0, glyf_len = 0;
    font->loca = find_table(font, "loca", &loca_len);
    font->glyf = find_table(font, "glyf", &glyf_len);
    font->hmtx = find_table(font, "hmtx", NULL);
    font->cmap = find_table(font, "cmap", NULL);
    font->kern = find_table(font, "kern", NULL);
    if (!head || !hhea || !maxp || !font->loca || !font->glyf || !font->hmtx || !font->cmap)
        return false;
    if (!in_range(font, head, 54) || !in_range(font, hhea, 36) || !in_range(font, maxp, 6))
        return false;
    font->units_per_em = rd16(data + head + 18);
    if (font->units_per_em == 0)
        return false;
    font->index_to_loc = rds16(data + head + 50);
    font->ascent = rds16(data + hhea + 4);
    font->descent = rds16(data + hhea + 6);
    font->line_gap = rds16(data + hhea + 8);
    font->hmetric_count = rd16(data + hhea + 34);
    font->glyph_count = rd16(data + maxp + 4);
    font->loca_count = loca_len / (font->index_to_loc ? 4 : 2);
    if (font->loca_count == 0)
        return false;

    /* cmap: prefer a Unicode subtable, format 12 over format 4 */
    uint32_t n = rd16(data + font->cmap + 2);
    if (!in_range(font, font->cmap + 4, n * 8))
        return false;
    int best = 0;
    for (uint32_t i = 0; i < n; i++)
    {
        const uint8_t *e = data + font->cmap + 4 + i * 8;
        uint16_t platform = rd16(e), encoding = rd16(e + 2);
        uint32_t off = rd32(e + 4);
        if (!in_range(font, font->cmap + off, 4))
            continue;
        uint16_t format = rd16(data + font->cmap + off);
        int score = 0;
        if (platform == 3 && (encoding == 1 || encoding == 10))
            score = format == 12 ? 3 : (format == 4 ? 2 : 0);
        else if (platform == 0)
            score = format == 12 ? 3 : (format == 4 ? 2 : 0);
        if (score > best)
        {
            best = score;
            font->cmap_sub = font->cmap + off;
            font->cmap_format = format;
        }
    }
    if (best == 0)
        return false;
    for (int i = 0; i < ZV_GLYPH_CACHE; i++)
        font->cache[i].glyph = -1;
    return true;
}

void zv_font_release(ZvFont *font)
{
    if (!font)
        return;
    for (int i = 0; i < ZV_GLYPH_CACHE; i++)
    {
        if (font->cache[i].glyph >= 0)
            zv_path_release(&font->cache[i].path);
    }
    memset(font, 0, sizeof *font);
}

int zv_font_glyph_index(const ZvFont *f, uint32_t cp)
{
    const uint8_t *d = f->data + f->cmap_sub;
    if (f->cmap_format == 4)
    {
        if (cp > 0xFFFF || !in_range(f, f->cmap_sub, 14))
            return 0;
        uint32_t segx2 = rd16(d + 6);
        if (!in_range(f, f->cmap_sub + 16, segx2 * 4))
            return 0;
        const uint8_t *ends = d + 14, *starts = ends + segx2 + 2, *deltas = starts + segx2, *ranges = deltas + segx2;
        for (uint32_t s = 0; s < segx2 / 2; s++)
        {
            uint32_t end = rd16(ends + s * 2);
            if (cp > end)
                continue;
            uint32_t start = rd16(starts + s * 2);
            if (cp < start)
                return 0;
            uint32_t delta = rd16(deltas + s * 2);
            uint32_t range = rd16(ranges + s * 2);
            if (range == 0)
                return (int)((cp + delta) & 0xFFFF);
            uint32_t off = (uint32_t)(ranges + s * 2 - f->data) + range + (cp - start) * 2;
            if (!in_range(f, off, 2))
                return 0;
            uint32_t g = rd16(f->data + off);
            return g ? (int)((g + delta) & 0xFFFF) : 0;
        }
        return 0;
    }
    if (f->cmap_format == 12)
    {
        if (!in_range(f, f->cmap_sub, 16))
            return 0;
        uint32_t groups = rd32(d + 12);
        if (!in_range(f, f->cmap_sub + 16, groups * 12))
            return 0;
        uint32_t lo = 0, hi = groups;
        while (lo < hi)
        {
            uint32_t mid = (lo + hi) / 2;
            const uint8_t *g = d + 16 + mid * 12;
            uint32_t start = rd32(g), end = rd32(g + 4);
            if (cp < start)
                hi = mid;
            else if (cp > end)
                lo = mid + 1;
            else
                return (int)(rd32(g + 8) + (cp - start));
        }
    }
    return 0;
}

int zv_font_advance(const ZvFont *f, int glyph)
{
    if (f->hmetric_count <= 0)
        return 0;
    int i = glyph < f->hmetric_count ? glyph : f->hmetric_count - 1;
    uint32_t off = f->hmtx + (uint32_t)i * 4;
    if (!in_range(f, off, 2))
        return 0;
    return rd16(f->data + off);
}

int zv_font_kerning(const ZvFont *f, int left, int right)
{
    if (!f->kern || !in_range(f, f->kern, 4))
        return 0;
    const uint8_t *d = f->data + f->kern;
    uint32_t tables = rd16(d + 2);
    uint32_t off = 4;
    for (uint32_t t = 0; t < tables; t++)
    {
        if (!in_range(f, f->kern + off, 6))
            return 0;
        uint32_t len = rd16(d + off + 2);
        uint16_t coverage = rd16(d + off + 4);
        if ((coverage >> 8) == 0 && (coverage & 1))
        {
            uint32_t pairs = rd16(d + off + 6);
            if (!in_range(f, f->kern + off + 14, pairs * 6))
                return 0;
            const uint8_t *p = d + off + 14;
            uint32_t key = (uint32_t)left << 16 | (uint32_t)right;
            uint32_t lo = 0, hi = pairs;
            while (lo < hi)
            {
                uint32_t mid = (lo + hi) / 2;
                uint32_t k = rd32(p + mid * 6);
                if (k < key)
                    lo = mid + 1;
                else if (k > key)
                    hi = mid;
                else
                    return rds16(p + mid * 6 + 4);
            }
        }
        if (len == 0)
            break;
        off += len;
    }
    return 0;
}

/* Outlines */

static bool glyph_range(const ZvFont *f, int glyph, uint32_t *off, uint32_t *len)
{
    if (glyph < 0 || (uint32_t)glyph + 1 >= f->loca_count)
        return false;
    uint32_t a, b;
    if (f->index_to_loc)
    {
        a = rd32(f->data + f->loca + (uint32_t)glyph * 4);
        b = rd32(f->data + f->loca + (uint32_t)glyph * 4 + 4);
    }
    else
    {
        a = (uint32_t)rd16(f->data + f->loca + (uint32_t)glyph * 2) * 2;
        b = (uint32_t)rd16(f->data + f->loca + (uint32_t)glyph * 2 + 2) * 2;
    }
    if (b < a || !in_range(f, f->glyf + a, b - a))
        return false;
    *off = f->glyf + a;
    *len = b - a;
    return true;
}

typedef struct
{
    float x, y;
    bool on;
} GlyphPoint;

static bool emit_contour(ZvPath *path, const GlyphPoint *pts, int n, const ZvMatrix *m)
{
    if (n == 0)
        return true;
    /* find a starting on-curve point, or make one between two off points */
    int start = -1;
    for (int i = 0; i < n; i++)
    {
        if (pts[i].on)
        {
            start = i;
            break;
        }
    }
    GlyphPoint first;
    if (start < 0)
    {
        first.x = (pts[0].x + pts[n - 1].x) * 0.5f;
        first.y = (pts[0].y + pts[n - 1].y) * 0.5f;
        first.on = true;
        start = 0;
    }
    else
    {
        first = pts[start];
    }
    ZvPoint fp = zv_matrix_apply(m, (ZvPoint){first.x, first.y});
    if (!zv_path_move_to(path, fp.x, fp.y))
        return false;
    GlyphPoint prev_off = first;
    bool have_off = false;
    int count = pts[start].on ? n : n + 1;
    for (int k = 1; k <= count; k++)
    {
        GlyphPoint p = (k == count) ? first : pts[(start + k) % n];
        if (p.on)
        {
            ZvPoint q = zv_matrix_apply(m, (ZvPoint){p.x, p.y});
            if (have_off)
            {
                ZvPoint c = zv_matrix_apply(m, (ZvPoint){prev_off.x, prev_off.y});
                if (!zv_path_quad_to(path, c.x, c.y, q.x, q.y))
                    return false;
                have_off = false;
            }
            else if (!zv_path_line_to(path, q.x, q.y))
                return false;
        }
        else
        {
            if (have_off)
            {
                GlyphPoint mid = {(prev_off.x + p.x) * 0.5f, (prev_off.y + p.y) * 0.5f, true};
                ZvPoint c = zv_matrix_apply(m, (ZvPoint){prev_off.x, prev_off.y});
                ZvPoint q = zv_matrix_apply(m, (ZvPoint){mid.x, mid.y});
                if (!zv_path_quad_to(path, c.x, c.y, q.x, q.y))
                    return false;
            }
            prev_off = p;
            have_off = true;
        }
    }
    if (have_off)
    {
        ZvPoint c = zv_matrix_apply(m, (ZvPoint){prev_off.x, prev_off.y});
        if (!zv_path_quad_to(path, c.x, c.y, fp.x, fp.y))
            return false;
    }
    return zv_path_close(path);
}

static bool read_glyph(ZvFont *f, int glyph, ZvPath *path, const ZvMatrix *m, int depth);

static bool read_simple(ZvFont *f, const uint8_t *g, uint32_t len, int contours, ZvPath *path, const ZvMatrix *m)
{
    if (len < 10 + (uint32_t)contours * 2 + 2)
        return false;
    const uint8_t *ends = g + 10;
    int npts = rd16(ends + (contours - 1) * 2) + 1;
    uint32_t ins = rd16(ends + contours * 2);
    uint32_t p = 10 + (uint32_t)contours * 2 + 2 + ins;
    if (p > len)
        return false;
    GlyphPoint stack[256];
    GlyphPoint *pts = stack;
    uint8_t flags_stack[256];
    uint8_t *flags = flags_stack;
    if (npts > 256)
    {
        pts = f->allocator->allocate(f->allocator->context, (size_t)npts * (sizeof(GlyphPoint) + 1));
        if (!pts)
            return false;
        flags = (uint8_t *)(pts + npts);
    }
    bool ok = true;
    for (int i = 0; i < npts && ok;)
    {
        if (p >= len)
        {
            ok = false;
            break;
        }
        uint8_t fl = g[p++];
        flags[i++] = fl;
        if (fl & 8)
        {
            if (p >= len)
            {
                ok = false;
                break;
            }
            int r = g[p++];
            while (r-- > 0 && i < npts)
                flags[i++] = fl;
        }
    }
    int v = 0;
    for (int i = 0; i < npts && ok; i++)
    {
        uint8_t fl = flags[i];
        if (fl & 2)
        {
            if (p >= len)
            {
                ok = false;
                break;
            }
            int d = g[p++];
            v += (fl & 16) ? d : -d;
        }
        else if (!(fl & 16))
        {
            if (p + 2 > len)
            {
                ok = false;
                break;
            }
            v += rds16(g + p);
            p += 2;
        }
        pts[i].x = (float)v;
    }
    v = 0;
    for (int i = 0; i < npts && ok; i++)
    {
        uint8_t fl = flags[i];
        if (fl & 4)
        {
            if (p >= len)
            {
                ok = false;
                break;
            }
            int d = g[p++];
            v += (fl & 32) ? d : -d;
        }
        else if (!(fl & 32))
        {
            if (p + 2 > len)
            {
                ok = false;
                break;
            }
            v += rds16(g + p);
            p += 2;
        }
        pts[i].y = (float)v;
        pts[i].on = (fl & 1) != 0;
    }
    int start = 0;
    for (int c = 0; c < contours && ok; c++)
    {
        int end = rd16(ends + c * 2);
        if (end < start || end >= npts)
        {
            ok = false;
            break;
        }
        ok = emit_contour(path, pts + start, end - start + 1, m);
        start = end + 1;
    }
    if (pts != stack)
        f->allocator->release(f->allocator->context, pts);
    return ok;
}

static bool read_composite(ZvFont *f, const uint8_t *g, uint32_t len, ZvPath *path, const ZvMatrix *m, int depth)
{
    uint32_t p = 10;
    for (;;)
    {
        if (p + 4 > len)
            return false;
        uint16_t flags = rd16(g + p);
        int index = rd16(g + p + 2);
        p += 4;
        float dx, dy;
        if (flags & 1)
        {
            if (p + 4 > len)
                return false;
            dx = (float)rds16(g + p);
            dy = (float)rds16(g + p + 2);
            p += 4;
        }
        else
        {
            if (p + 2 > len)
                return false;
            dx = (float)(int8_t)g[p];
            dy = (float)(int8_t)g[p + 1];
            p += 2;
        }
        float a = 1, b = 0, c = 0, d = 1;
        if (flags & 8)
        {
            if (p + 2 > len)
                return false;
            a = d = (float)rds16(g + p) / 16384.0f;
            p += 2;
        }
        else if (flags & 0x40)
        {
            if (p + 4 > len)
                return false;
            a = (float)rds16(g + p) / 16384.0f;
            d = (float)rds16(g + p + 2) / 16384.0f;
            p += 4;
        }
        else if (flags & 0x80)
        {
            if (p + 8 > len)
                return false;
            a = (float)rds16(g + p) / 16384.0f;
            b = (float)rds16(g + p + 2) / 16384.0f;
            c = (float)rds16(g + p + 4) / 16384.0f;
            d = (float)rds16(g + p + 6) / 16384.0f;
            p += 8;
        }
        if (!(flags & 2))
        {
            dx = dy = 0; /* point matching is not supported: no offset */
        }
        ZvMatrix sub = zv_matrix_make(a, b, c, d, dx, dy);
        ZvMatrix total = zv_matrix_concat(m, &sub);
        if (!read_glyph(f, index, path, &total, depth + 1))
            return false;
        if (!(flags & 0x20))
            break;
    }
    return true;
}

static bool read_glyph(ZvFont *f, int glyph, ZvPath *path, const ZvMatrix *m, int depth)
{
    if (depth > 8)
        return false;
    uint32_t off, len;
    if (!glyph_range(f, glyph, &off, &len))
        return false;
    if (len == 0)
        return true; /* empty glyph, like a space */
    if (len < 10)
        return false;
    const uint8_t *g = f->data + off;
    int contours = rds16(g);
    if (contours >= 0)
        return read_simple(f, g, len, contours, path, m);
    return read_composite(f, g, len, path, m, depth);
}

const ZvPath *zv_font_glyph_path(ZvFont *f, int glyph)
{
    if (glyph < 0)
        return NULL;
    ZvGlyphEntry *e = &f->cache[glyph % ZV_GLYPH_CACHE];
    if (e->glyph == glyph)
        return e->valid ? &e->path : NULL;
    if (e->glyph >= 0)
        zv_path_release(&e->path);
    zv_path_init(&e->path, f->allocator);
    e->glyph = glyph;
    ZvMatrix id = zv_matrix_identity();
    e->valid = read_glyph(f, glyph, &e->path, &id, 0);
    if (!e->valid)
        zv_path_clear(&e->path);
    return e->valid ? &e->path : NULL;
}

int zv_utf8_decode(const char *s, uint32_t *cp)
{
    const uint8_t *u = (const uint8_t *)s;
    if (u[0] < 0x80)
    {
        *cp = u[0];
        return 1;
    }
    int n = 0;
    uint32_t v = 0;
    if ((u[0] & 0xE0) == 0xC0)
    {
        n = 1;
        v = u[0] & 0x1F;
    }
    else if ((u[0] & 0xF0) == 0xE0)
    {
        n = 2;
        v = u[0] & 0x0F;
    }
    else if ((u[0] & 0xF8) == 0xF0)
    {
        n = 3;
        v = u[0] & 0x07;
    }
    else
    {
        *cp = 0xFFFD;
        return 1;
    }
    for (int i = 1; i <= n; i++)
    {
        if ((u[i] & 0xC0) != 0x80)
        {
            *cp = 0xFFFD;
            return 1;
        }
        v = v << 6 | (u[i] & 0x3F);
    }
    *cp = v;
    return n + 1;
}

float zv_font_text_path(ZvFont *f, const char *utf8, float x, float y, float size, ZvPath *path)
{
    float scale = size / (float)f->units_per_em;
    float pen = 0.0f;
    int prev = -1;
    for (const char *s = utf8; *s;)
    {
        uint32_t cp;
        s += zv_utf8_decode(s, &cp);
        int g = zv_font_glyph_index(f, cp);
        if (prev >= 0)
            pen += (float)zv_font_kerning(f, prev, g) * scale;
        const ZvPath *gp = zv_font_glyph_path(f, g);
        if (gp && path)
        {
            ZvMatrix m = zv_matrix_make(scale, 0, 0, -scale, x + pen, y);
            if (!zv_path_append(path, gp, &m))
                return pen;
        }
        pen += (float)zv_font_advance(f, g) * scale;
        prev = g;
    }
    return pen;
}

float zv_font_text_width(ZvFont *f, const char *utf8, float size)
{
    return zv_font_text_path(f, utf8, 0, 0, size, NULL);
}
