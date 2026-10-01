#include "zv_flash.h"
#include "zv_internal.h"

/* Graphics */

void zv_graphics_init(ZvGraphics *g, const ZvAllocator *allocator)
{
    memset(g, 0, sizeof *g);
    g->allocator = allocator ? allocator : zv_default_allocator();
    g->bounds = zv_bounds_empty();
}

void zv_graphics_release(ZvGraphics *g)
{
    if (!g)
        return;
    if (g->commands)
        g->allocator->release(g->allocator->context, g->commands);
    const ZvAllocator *a = g->allocator;
    memset(g, 0, sizeof *g);
    g->allocator = a;
    g->bounds = zv_bounds_empty();
}

void zv_graphics_clear(ZvGraphics *g)
{
    g->count = 0;
    g->bounds = zv_bounds_empty();
    g->max_line_width = 0.0f;
    g->dirty = true;
}

static ZvGraphicsCommand *push(ZvGraphics *g, ZvGraphicsOp op)
{
    if (!zv_grow(g->allocator, (void **)&g->commands, &g->capacity, g->count, g->count + 1, sizeof(ZvGraphicsCommand)))
        return NULL;
    ZvGraphicsCommand *c = &g->commands[g->count++];
    memset(c, 0, sizeof *c);
    c->op = op;
    g->dirty = true;
    return c;
}

static uint32_t straight(uint32_t rgb, float alpha)
{
    uint32_t a = (uint32_t)(zv_clampf(alpha, 0.0f, 1.0f) * 255.0f + 0.5f);
    return a << 24 | (rgb & 0x00FFFFFFu);
}

bool zv_graphics_begin_fill(ZvGraphics *g, uint32_t rgb, float alpha)
{
    ZvGraphicsCommand *c = push(g, ZV_G_BEGIN_FILL);
    if (!c)
        return false;
    c->paint = zv_paint_solid(straight(rgb, alpha));
    c->has_paint = true;
    return true;
}

static ZvPaint flash_gradient(bool radial, const uint32_t *rgbs, const float *alphas, const uint8_t *ratios, int count, const ZvMatrix *matrix,
                              ZvSpread spread, float focal)
{
    /* the Flash gradient box is -819.2..819.2 twips of user units mapped by
       the matrix; the paint works in a -1..1 box scaled by 819.2 */
    ZvPaint p = radial ? zv_paint_radial(zv_clampf(focal, -1.0f, 1.0f), 0, 0, 0, 0, 1) : zv_paint_linear(-1, 0, 1, 0);
    for (int i = 0; i < count && i < ZV_MAX_STOPS; i++)
        zv_paint_add_stop(&p, (float)ratios[i] / 255.0f, straight(rgbs[i], alphas[i]));
    p.spread = spread;
    ZvMatrix box = zv_matrix_scaling(819.2f, 819.2f);
    p.matrix = matrix ? zv_matrix_concat(matrix, &box) : box;
    return p;
}

bool zv_graphics_begin_gradient_fill(ZvGraphics *g, bool radial, const uint32_t *rgbs, const float *alphas, const uint8_t *ratios, int count,
                                     const ZvMatrix *matrix, ZvSpread spread, float focal)
{
    ZvGraphicsCommand *c = push(g, ZV_G_BEGIN_GRADIENT_FILL);
    if (!c)
        return false;
    c->paint = flash_gradient(radial, rgbs, alphas, ratios, count, matrix, spread, focal);
    c->has_paint = true;
    return true;
}

bool zv_graphics_begin_bitmap_fill(ZvGraphics *g, const ZvSurface *bitmap, const ZvMatrix *matrix, bool repeat, bool smooth)
{
    ZvGraphicsCommand *c = push(g, ZV_G_BEGIN_BITMAP_FILL);
    if (!c)
        return false;
    c->paint = zv_paint_pattern(bitmap, repeat, repeat, smooth ? ZV_FILTER_BILINEAR : ZV_FILTER_NEAREST);
    if (matrix)
        c->paint.matrix = *matrix;
    c->has_paint = true;
    return true;
}

bool zv_graphics_end_fill(ZvGraphics *g)
{
    return push(g, ZV_G_END_FILL) != NULL;
}

bool zv_graphics_line_style(ZvGraphics *g, float width, uint32_t rgb, float alpha, bool hinting, ZvScaleMode scale_mode, ZvLineCap cap, ZvLineJoin join, float miter)
{
    ZvGraphicsCommand *c = push(g, ZV_G_LINE_STYLE);
    if (!c)
        return false;
    if (!(width > 0.0f) || !isfinite(width))
    {
        c->width = 0.0f;
        return true;
    }
    c->width = width;
    c->paint = zv_paint_solid(straight(rgb, alpha));
    c->has_paint = true;
    c->cap = cap;
    c->join = join;
    c->miter_limit = miter > 0.0f ? miter : 3.0f;
    c->scale_mode = scale_mode;
    c->pixel_hinting = hinting;
    if (width > g->max_line_width)
        g->max_line_width = width;
    return true;
}

bool zv_graphics_line_gradient_style(ZvGraphics *g, bool radial, const uint32_t *rgbs, const float *alphas, const uint8_t *ratios, int count,
                                     const ZvMatrix *matrix, ZvSpread spread, float focal)
{
    /* applies to the last line style */
    for (int i = g->count - 1; i >= 0; i--)
    {
        if (g->commands[i].op == ZV_G_LINE_STYLE)
        {
            g->commands[i].paint = flash_gradient(radial, rgbs, alphas, ratios, count, matrix, spread, focal);
            g->commands[i].has_paint = true;
            g->dirty = true;
            return true;
        }
    }
    return false;
}

static bool point_command(ZvGraphics *g, ZvGraphicsOp op, const float *v, int n)
{
    ZvGraphicsCommand *c = push(g, op);
    if (!c)
        return false;
    for (int i = 0; i < n; i++)
    {
        c->v[i] = v[i];
        if (i % 2 == 1)
            zv_bounds_add(&g->bounds, v[i - 1], v[i]);
    }
    return true;
}

bool zv_graphics_move_to(ZvGraphics *g, float x, float y)
{
    float v[2] = {x, y};
    return point_command(g, ZV_G_MOVE_TO, v, 2);
}

bool zv_graphics_line_to(ZvGraphics *g, float x, float y)
{
    float v[2] = {x, y};
    return point_command(g, ZV_G_LINE_TO, v, 2);
}

bool zv_graphics_curve_to(ZvGraphics *g, float cx, float cy, float x, float y)
{
    float v[4] = {cx, cy, x, y};
    return point_command(g, ZV_G_CURVE_TO, v, 4);
}

bool zv_graphics_cubic_curve_to(ZvGraphics *g, float c1x, float c1y, float c2x, float c2y, float x, float y)
{
    float v[6] = {c1x, c1y, c2x, c2y, x, y};
    return point_command(g, ZV_G_CUBIC_CURVE_TO, v, 6);
}

bool zv_graphics_draw_rect(ZvGraphics *g, float x, float y, float w, float h)
{
    return zv_graphics_move_to(g, x, y) && zv_graphics_line_to(g, x + w, y) && zv_graphics_line_to(g, x + w, y + h) && zv_graphics_line_to(g, x, y + h) &&
           zv_graphics_line_to(g, x, y);
}

/* An ellipse quadrant as a quadratic pair, like Flash does it (8 curves). */
static bool ellipse_path(ZvGraphics *g, float x, float y, float w, float h)
{
    float rx = w * 0.5f, ry = h * 0.5f, cx = x + rx, cy = y + ry;
    const float k = 0.41421356f; /* tan(pi/8) */
    const float c = 0.70710678f;
    if (!zv_graphics_move_to(g, cx + rx, cy))
        return false;
    float pts[8][4] = {
        {cx + rx, cy + ry * k, cx + rx * c, cy + ry * c}, {cx + rx * k, cy + ry, cx, cy + ry},   {cx - rx * k, cy + ry, cx - rx * c, cy + ry * c},
        {cx - rx, cy + ry * k, cx - rx, cy},             {cx - rx, cy - ry * k, cx - rx * c, cy - ry * c}, {cx - rx * k, cy - ry, cx, cy - ry},
        {cx + rx * k, cy - ry, cx + rx * c, cy - ry * c}, {cx + rx, cy - ry * k, cx + rx, cy},
    };
    for (int i = 0; i < 8; i++)
    {
        if (!zv_graphics_curve_to(g, pts[i][0], pts[i][1], pts[i][2], pts[i][3]))
            return false;
    }
    return true;
}

bool zv_graphics_draw_circle(ZvGraphics *g, float x, float y, float r)
{
    return ellipse_path(g, x - r, y - r, 2 * r, 2 * r);
}

bool zv_graphics_draw_ellipse(ZvGraphics *g, float x, float y, float w, float h)
{
    return ellipse_path(g, x, y, w, h);
}

bool zv_graphics_draw_round_rect(ZvGraphics *g, float x, float y, float w, float h, float ew, float eh)
{
    float rx = ew * 0.5f, ry = eh * 0.5f;
    if (rx > w * 0.5f)
        rx = w * 0.5f;
    if (ry > h * 0.5f)
        ry = h * 0.5f;
    if (rx <= 0.0f || ry <= 0.0f)
        return zv_graphics_draw_rect(g, x, y, w, h);
    const float k = 0.41421356f, c = 0.70710678f;
    bool ok = zv_graphics_move_to(g, x + w - rx, y);
    /* top right corner, centre (x+w-rx, y+ry) from angle -90 to 0 */
    float corners[4][2] = {{x + w - rx, y + ry}, {x + w - rx, y + h - ry}, {x + rx, y + h - ry}, {x + rx, y + ry}};
    float starts[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    float lines[4][2] = {{x + w, y + h - ry}, {x + rx, y + h}, {x, y + ry}, {x + w - rx, y}};
    for (int i = 0; i < 4 && ok; i++)
    {
        float cx = corners[i][0], cy = corners[i][1];
        float sx = starts[i][0], sy = starts[i][1];
        /* rotate the start direction by 45 and 90 degrees clockwise (y down) */
        float mx = (sx - sy) * c, my = (sx + sy) * c;
        float ex = -sy, ey = sx;
        ok = zv_graphics_curve_to(g, cx + (sx + ex * k) * rx, cy + (sy + ey * k) * ry, cx + mx * rx, cy + my * ry) &&
             zv_graphics_curve_to(g, cx + (ex + sx * k) * rx, cy + (ey + sy * k) * ry, cx + ex * rx, cy + ey * ry) &&
             zv_graphics_line_to(g, lines[i][0], lines[i][1]);
    }
    return ok;
}

bool zv_graphics_draw_path(ZvGraphics *g, const uint8_t *commands, int command_count, const float *data, int data_count)
{
    int d = 0;
    for (int i = 0; i < command_count; i++)
    {
        switch (commands[i])
        {
        case 1:
            if (d + 2 > data_count || !zv_graphics_move_to(g, data[d], data[d + 1]))
                return false;
            d += 2;
            break;
        case 2:
            if (d + 2 > data_count || !zv_graphics_line_to(g, data[d], data[d + 1]))
                return false;
            d += 2;
            break;
        case 3:
            if (d + 4 > data_count || !zv_graphics_curve_to(g, data[d], data[d + 1], data[d + 2], data[d + 3]))
                return false;
            d += 4;
            break;
        case 6:
            if (d + 6 > data_count || !zv_graphics_cubic_curve_to(g, data[d], data[d + 1], data[d + 2], data[d + 3], data[d + 4], data[d + 5]))
                return false;
            d += 6;
            break;
        default:
            break;
        }
    }
    return true;
}

bool zv_graphics_draw_triangles(ZvGraphics *g, const float *vertices, int vertex_count, const int *indices, int index_count)
{
    int tris = indices ? index_count / 3 : vertex_count / 3;
    for (int t = 0; t < tris; t++)
    {
        int a = indices ? indices[t * 3] : t * 3, b = indices ? indices[t * 3 + 1] : t * 3 + 1, k = indices ? indices[t * 3 + 2] : t * 3 + 2;
        if (a < 0 || b < 0 || k < 0 || a >= vertex_count || b >= vertex_count || k >= vertex_count)
            return false;
        if (!zv_graphics_move_to(g, vertices[a * 2], vertices[a * 2 + 1]) || !zv_graphics_line_to(g, vertices[b * 2], vertices[b * 2 + 1]) ||
            !zv_graphics_line_to(g, vertices[k * 2], vertices[k * 2 + 1]) || !zv_graphics_line_to(g, vertices[a * 2], vertices[a * 2 + 1]))
            return false;
    }
    return true;
}

/* Replay: fills collect every segment between beginFill and endFill into
   one path (even-odd, closed); the line is stroked per run of segments
   with the same style. */

typedef struct
{
    ZvCanvas *canvas;
    ZvPath fill_path, line_path;
    bool filling;
    ZvPaint fill_paint;
    const ZvGraphicsCommand *line; /* current line style, NULL for none */
    ZvPoint current;
    bool has_current;
    bool ok;
} Replay;

static void flush_fill(Replay *r)
{
    if (!r->filling)
        return;
    if (!zv_path_is_empty(&r->fill_path))
    {
        zv_canvas_set_fill_paint(r->canvas, &r->fill_paint);
        r->ok = r->ok && zv_canvas_fill_path(r->canvas, &r->fill_path, ZV_FILL_EVENODD);
    }
    zv_path_clear(&r->fill_path);
    r->filling = false;
}

static void flush_line(Replay *r)
{
    if (!r->line || zv_path_is_empty(&r->line_path))
    {
        zv_path_clear(&r->line_path);
        return;
    }
    ZvCanvas *c = r->canvas;
    const ZvGraphicsCommand *l = r->line;
    zv_canvas_save(c);
    zv_canvas_set_stroke_paint(c, &l->paint);
    zv_canvas_set_line_cap(c, l->cap);
    zv_canvas_set_line_join(c, l->join);
    zv_canvas_set_miter_limit(c, l->miter_limit);
    float width = l->width;
    ZvMatrix m = zv_canvas_get_transform(c);
    if (l->scale_mode != ZV_SCALE_NORMAL)
    {
        /* keep the width in device pixels along the chosen axis: stroke in
           device space by transforming the path first */
        float sx = sqrtf(m.a * m.a + m.b * m.b), sy = sqrtf(m.c * m.c + m.d * m.d);
        float s = l->scale_mode == ZV_SCALE_HORIZONTAL ? sx : (l->scale_mode == ZV_SCALE_VERTICAL ? sy : 1.0f);
        ZvPath device;
        zv_path_init(&device, c->allocator);
        if (zv_path_append(&device, &r->line_path, &m))
        {
            zv_canvas_reset_transform(c);
            zv_canvas_set_line_width(c, width * s);
            r->ok = r->ok && zv_canvas_stroke_path(c, &device);
        }
        else
            r->ok = false;
        zv_path_release(&device);
    }
    else
    {
        zv_canvas_set_line_width(c, width);
        r->ok = r->ok && zv_canvas_stroke_path(c, &r->line_path);
    }
    zv_canvas_restore(c);
    zv_path_clear(&r->line_path);
}

static float hint(float v, bool on)
{
    return on ? floorf(v) + 0.5f : v;
}

bool zv_graphics_render(const ZvGraphics *g, ZvCanvas *canvas)
{
    Replay r;
    memset(&r, 0, sizeof r);
    r.canvas = canvas;
    r.ok = true;
    zv_path_init(&r.fill_path, canvas->allocator);
    zv_path_init(&r.line_path, canvas->allocator);
    for (int i = 0; i < g->count; i++)
    {
        const ZvGraphicsCommand *c = &g->commands[i];
        bool h = r.line && r.line->pixel_hinting;
        switch (c->op)
        {
        case ZV_G_BEGIN_FILL:
        case ZV_G_BEGIN_GRADIENT_FILL:
        case ZV_G_BEGIN_BITMAP_FILL:
            flush_fill(&r);
            r.filling = true;
            r.fill_paint = c->paint;
            if (r.has_current)
                r.ok = r.ok && zv_path_move_to(&r.fill_path, r.current.x, r.current.y);
            break;
        case ZV_G_END_FILL:
            flush_fill(&r);
            break;
        case ZV_G_LINE_STYLE:
            flush_line(&r);
            r.line = c->width > 0.0f ? c : NULL;
            if (r.line && r.has_current)
                r.ok = r.ok && zv_path_move_to(&r.line_path, hint(r.current.x, r.line->pixel_hinting), hint(r.current.y, r.line->pixel_hinting));
            break;
        case ZV_G_MOVE_TO:
            r.current.x = c->v[0];
            r.current.y = c->v[1];
            r.has_current = true;
            if (r.filling)
                r.ok = r.ok && zv_path_move_to(&r.fill_path, c->v[0], c->v[1]);
            if (r.line)
                r.ok = r.ok && zv_path_move_to(&r.line_path, hint(c->v[0], h), hint(c->v[1], h));
            break;
        case ZV_G_LINE_TO:
            if (r.filling)
                r.ok = r.ok && zv_path_line_to(&r.fill_path, c->v[0], c->v[1]);
            if (r.line)
            {
                if (!r.has_current)
                    r.ok = r.ok && zv_path_move_to(&r.line_path, 0, 0);
                r.ok = r.ok && zv_path_line_to(&r.line_path, hint(c->v[0], h), hint(c->v[1], h));
            }
            r.current.x = c->v[0];
            r.current.y = c->v[1];
            r.has_current = true;
            break;
        case ZV_G_CURVE_TO:
            if (r.filling)
                r.ok = r.ok && zv_path_quad_to(&r.fill_path, c->v[0], c->v[1], c->v[2], c->v[3]);
            if (r.line)
                r.ok = r.ok && zv_path_quad_to(&r.line_path, c->v[0], c->v[1], hint(c->v[2], h), hint(c->v[3], h));
            r.current.x = c->v[2];
            r.current.y = c->v[3];
            r.has_current = true;
            break;
        case ZV_G_CUBIC_CURVE_TO:
            if (r.filling)
                r.ok = r.ok && zv_path_cubic_to(&r.fill_path, c->v[0], c->v[1], c->v[2], c->v[3], c->v[4], c->v[5]);
            if (r.line)
                r.ok = r.ok && zv_path_cubic_to(&r.line_path, c->v[0], c->v[1], c->v[2], c->v[3], hint(c->v[4], h), hint(c->v[5], h));
            r.current.x = c->v[4];
            r.current.y = c->v[5];
            r.has_current = true;
            break;
        }
    }
    flush_fill(&r);
    flush_line(&r);
    zv_path_release(&r.fill_path);
    zv_path_release(&r.line_path);
    return r.ok;
}

/* Sprites */

void zv_sprite_init(ZvSprite *s, const ZvAllocator *allocator)
{
    memset(s, 0, sizeof *s);
    s->allocator = allocator ? allocator : zv_default_allocator();
    zv_graphics_init(&s->graphics, s->allocator);
    s->scale_x = s->scale_y = 1.0f;
    s->alpha = 1.0f;
    s->visible = true;
    s->last_bounds = zv_bounds_empty();
    s->dirty = true;
}

void zv_sprite_release(ZvSprite *s)
{
    if (!s)
        return;
    zv_graphics_release(&s->graphics);
    if (s->children)
        s->allocator->release(s->allocator->context, s->children);
    zv_surface_release(&s->cache);
    s->children = NULL;
    s->child_count = s->child_capacity = 0;
}

static void mark(ZvSprite *s)
{
    s->dirty = true;
    for (ZvSprite *p = s; p; p = p->parent)
        p->cache_valid = false;
}

void zv_sprite_invalidate(ZvSprite *s)
{
    mark(s);
}

bool zv_sprite_add_child_at(ZvSprite *parent, ZvSprite *child, int index)
{
    if (child->parent)
        zv_sprite_remove_child(child->parent, child);
    if (!zv_grow(parent->allocator, (void **)&parent->children, &parent->child_capacity, parent->child_count, parent->child_count + 1, sizeof(ZvSprite *)))
        return false;
    if (index < 0 || index > parent->child_count)
        index = parent->child_count;
    for (int i = parent->child_count; i > index; i--)
        parent->children[i] = parent->children[i - 1];
    parent->children[index] = child;
    parent->child_count++;
    child->parent = parent;
    mark(child);
    return true;
}

bool zv_sprite_add_child(ZvSprite *parent, ZvSprite *child)
{
    return zv_sprite_add_child_at(parent, child, -1);
}

bool zv_sprite_remove_child(ZvSprite *parent, ZvSprite *child)
{
    for (int i = 0; i < parent->child_count; i++)
    {
        if (parent->children[i] == child)
        {
            for (int j = i; j + 1 < parent->child_count; j++)
                parent->children[j] = parent->children[j + 1];
            parent->child_count--;
            child->parent = NULL;
            mark(parent);
            return true;
        }
    }
    return false;
}

void zv_sprite_set_position(ZvSprite *s, float x, float y)
{
    if (s->x != x || s->y != y)
    {
        s->x = x;
        s->y = y;
        mark(s);
    }
}

void zv_sprite_set_scale(ZvSprite *s, float sx, float sy)
{
    s->scale_x = sx;
    s->scale_y = sy;
    mark(s);
}

void zv_sprite_set_rotation(ZvSprite *s, float degrees)
{
    s->rotation = degrees;
    mark(s);
}

void zv_sprite_set_matrix(ZvSprite *s, const ZvMatrix *m)
{
    if (m)
    {
        s->matrix_storage = *m;
        s->custom_matrix = &s->matrix_storage;
    }
    else
        s->custom_matrix = NULL;
    mark(s);
}

void zv_sprite_set_alpha(ZvSprite *s, float alpha)
{
    s->alpha = zv_clampf(alpha, 0.0f, 1.0f);
    mark(s);
}

void zv_sprite_set_visible(ZvSprite *s, bool visible)
{
    if (s->visible != visible)
    {
        s->visible = visible;
        mark(s);
    }
}

void zv_sprite_set_blend(ZvSprite *s, ZvBlendMode blend)
{
    s->blend = blend;
    mark(s);
}

void zv_sprite_set_mask(ZvSprite *s, ZvSprite *mask)
{
    s->mask = mask;
    mark(s);
}

void zv_sprite_set_cache_as_bitmap(ZvSprite *s, bool on)
{
    s->cache_as_bitmap = on;
    if (!on)
        zv_surface_release(&s->cache);
    mark(s);
}

ZvMatrix zv_sprite_matrix(const ZvSprite *s)
{
    if (s->custom_matrix)
        return *s->custom_matrix;
    ZvMatrix m = zv_matrix_translation(s->x, s->y);
    zv_matrix_rotate(&m, s->rotation * (float)M_PI / 180.0f);
    zv_matrix_scale(&m, s->scale_x, s->scale_y);
    return m;
}

static ZvBounds local_bounds(const ZvSprite *s)
{
    ZvBounds b = s->graphics.bounds;
    if (!zv_bounds_is_empty(&b))
    {
        float pad = s->graphics.max_line_width * 0.5f * 1.5f;
        b.min_x -= pad;
        b.min_y -= pad;
        b.max_x += pad;
        b.max_y += pad;
    }
    for (int i = 0; i < s->child_count; i++)
    {
        ZvBounds cb = zv_sprite_bounds(s->children[i]);
        b = zv_bounds_union(&b, &cb);
    }
    return b;
}

ZvBounds zv_sprite_bounds(const ZvSprite *s)
{
    ZvBounds b = local_bounds(s);
    if (zv_bounds_is_empty(&b))
        return b;
    ZvMatrix m = zv_sprite_matrix(s);
    return zv_matrix_apply_bounds(&m, &b);
}

static ZvCompositeOp blend_op(ZvBlendMode b)
{
    switch (b)
    {
    case ZV_BLEND_MULTIPLY:
        return ZV_OP_MULTIPLY;
    case ZV_BLEND_SCREEN:
        return ZV_OP_SCREEN;
    case ZV_BLEND_LIGHTEN:
        return ZV_OP_LIGHTEN;
    case ZV_BLEND_DARKEN:
        return ZV_OP_DARKEN;
    case ZV_BLEND_ADD:
        return ZV_OP_LIGHTER;
    case ZV_BLEND_ERASE:
        return ZV_OP_DESTINATION_OUT;
    case ZV_BLEND_ALPHA:
        return ZV_OP_DESTINATION_IN;
    default:
        return ZV_OP_SOURCE_OVER;
    }
}

static bool render_tree(ZvSprite *s, ZvCanvas *canvas);

/* Renders the mask sprite's shapes as coverage, in the stage space. */
static uint8_t *render_mask(ZvSprite *mask, ZvCanvas *canvas, const ZvMatrix *parent_space)
{
    ZvSurface *target = canvas->target;
    size_t n = (size_t)target->width * (size_t)target->height;
    ZvSurface tmp;
    if (!zv_surface_init(&tmp, canvas->allocator, target->width, target->height))
        return NULL;
    uint8_t *cov = canvas->allocator->allocate(canvas->allocator->context, n);
    if (!cov)
    {
        zv_surface_release(&tmp);
        return NULL;
    }
    ZvSurface *saved = canvas->target;
    canvas->target = &tmp;
    zv_canvas_save(canvas);
    canvas->state.transform = *parent_space;
    canvas->state.op = ZV_OP_SOURCE_OVER;
    bool visible = mask->visible;
    float alpha = mask->alpha;
    mask->visible = true;
    mask->alpha = 1.0f;
    render_tree(mask, canvas);
    mask->visible = visible;
    mask->alpha = alpha;
    zv_canvas_restore(canvas);
    canvas->target = saved;
    for (size_t i = 0; i < n; i++)
        cov[i] = (uint8_t)(tmp.pixels[i] >> 24);
    zv_surface_release(&tmp);
    return cov;
}

static bool render_content(ZvSprite *s, ZvCanvas *canvas)
{
    bool ok = zv_graphics_render(&s->graphics, canvas);
    for (int i = 0; i < s->child_count && ok; i++)
        ok = render_tree(s->children[i], canvas);
    return ok;
}

static bool render_tree(ZvSprite *s, ZvCanvas *canvas)
{
    if (!s->visible || s->alpha <= 0.0f)
        return true;
    ZvMatrix parent_space = zv_canvas_get_transform(canvas);
    ZvMatrix local = zv_sprite_matrix(s);
    ZvMatrix world = zv_matrix_concat(&parent_space, &local);
    bool needs_layer = s->alpha < 1.0f || s->mask || s->blend == ZV_BLEND_LAYER || s->blend != ZV_BLEND_NORMAL;
    bool ok = true;
    uint8_t *mask = NULL;
    if (s->mask)
    {
        mask = render_mask(s->mask, canvas, &parent_space);
        if (!mask)
            return false;
    }

    if (s->cache_as_bitmap)
    {
        /* draw once into a surface covering the bounds in device space */
        ZvBounds b = zv_sprite_bounds(s);
        ZvBounds db = zv_matrix_apply_bounds(&parent_space, &b);
        if (!zv_bounds_is_empty(&db))
        {
            int x0 = zv_floor_int(db.min_x) - 1, y0 = zv_floor_int(db.min_y) - 1;
            int w = zv_ceil_int(db.max_x) + 1 - x0, h = zv_ceil_int(db.max_y) + 1 - y0;
            bool same = s->cache_valid && s->cache.pixels && s->cache.width == w && s->cache.height == h &&
                        memcmp(&s->cache_matrix, &world, sizeof world) == 0;
            if (!same)
            {
                zv_surface_release(&s->cache);
                if (!zv_surface_init(&s->cache, s->allocator, w, h))
                    ok = false;
                else
                {
                    ZvCanvas sub;
                    zv_canvas_init(&sub, &s->cache, canvas->allocator);
                    ZvMatrix shift = zv_matrix_translation((float)-x0, (float)-y0);
                    sub.state.transform = zv_matrix_concat(&shift, &world);
                    ok = render_content(s, &sub);
                    zv_canvas_release(&sub);
                    s->cache_matrix = world;
                    s->cache_valid = ok;
                }
            }
            if (ok)
            {
                zv_canvas_save(canvas);
                zv_canvas_reset_transform(canvas);
                zv_canvas_set_global_alpha(canvas, s->alpha);
                zv_canvas_set_composite_op(canvas, blend_op(s->blend));
                if (mask)
                {
                    ok = zv_canvas_begin_layer(canvas);
                    zv_canvas_set_global_alpha(canvas, 1.0f);
                    zv_canvas_set_composite_op(canvas, ZV_OP_SOURCE_OVER);
                    ok = ok && zv_canvas_draw_image_simple(canvas, &s->cache, (float)x0, (float)y0);
                    zv_canvas_set_composite_op(canvas, blend_op(s->blend));
                    ok = ok && zv_canvas_end_layer(canvas, s->alpha, mask);
                }
                else
                    ok = zv_canvas_draw_image_simple(canvas, &s->cache, (float)x0, (float)y0);
                zv_canvas_restore(canvas);
            }
        }
    }
    else
    {
        zv_canvas_save(canvas);
        canvas->state.transform = world;
        if (needs_layer)
        {
            ZvCompositeOp op = blend_op(s->blend);
            ok = zv_canvas_begin_layer(canvas);
            zv_canvas_set_composite_op(canvas, ZV_OP_SOURCE_OVER);
            ok = ok && render_content(s, canvas);
            zv_canvas_set_composite_op(canvas, op);
            ok = zv_canvas_end_layer(canvas, s->alpha, mask) && ok;
        }
        else
            ok = render_content(s, canvas);
        zv_canvas_restore(canvas);
    }
    if (mask)
        canvas->allocator->release(canvas->allocator->context, mask);
    s->dirty = false;
    s->graphics.dirty = false;
    return ok;
}

bool zv_sprite_render(ZvSprite *s, ZvCanvas *canvas)
{
    return render_tree(s, canvas);
}

/* Stage with dirty rectangles */

void zv_stage_init(ZvStage *stage, ZvSurface *target, const ZvAllocator *allocator)
{
    memset(stage, 0, sizeof *stage);
    stage->target = target;
    zv_sprite_init(&stage->root, allocator);
    zv_canvas_init(&stage->canvas, target, allocator);
    stage->full = true;
    stage->dirty = zv_bounds_empty();
}

void zv_stage_release(ZvStage *stage)
{
    zv_canvas_release(&stage->canvas);
    zv_sprite_release(&stage->root);
}

void zv_stage_invalidate_all(ZvStage *stage)
{
    stage->full = true;
}

/* Collects the union of the old and new device bounds of changed sprites,
   and records the new bounds. */
static void collect_dirty(ZvSprite *s, const ZvMatrix *parent_space, ZvBounds *dirty, bool inherited)
{
    ZvMatrix local = zv_sprite_matrix(s);
    ZvMatrix world = zv_matrix_concat(parent_space, &local);
    ZvBounds lb = s->graphics.bounds;
    if (!zv_bounds_is_empty(&lb))
    {
        float pad = s->graphics.max_line_width * 0.75f;
        lb.min_x -= pad;
        lb.min_y -= pad;
        lb.max_x += pad;
        lb.max_y += pad;
    }
    ZvBounds now = zv_bounds_is_empty(&lb) || !s->visible ? zv_bounds_empty() : zv_matrix_apply_bounds(&world, &lb);
    bool changed = inherited || s->dirty || s->graphics.dirty;
    if (changed)
    {
        *dirty = zv_bounds_union(dirty, &s->last_bounds);
        *dirty = zv_bounds_union(dirty, &now);
    }
    s->last_bounds = now;
    for (int i = 0; i < s->child_count; i++)
        collect_dirty(s->children[i], &world, dirty, changed || !s->visible);
}

ZvBounds zv_stage_render(ZvStage *stage)
{
    ZvCanvas *c = &stage->canvas;
    ZvBounds area;
    ZvMatrix id = zv_matrix_identity();
    if (stage->full)
    {
        area.min_x = 0;
        area.min_y = 0;
        area.max_x = (float)stage->target->width;
        area.max_y = (float)stage->target->height;
        ZvBounds ignored = zv_bounds_empty();
        collect_dirty(&stage->root, &id, &ignored, true);
        stage->full = false;
    }
    else
    {
        ZvBounds d = zv_bounds_empty();
        collect_dirty(&stage->root, &id, &d, false);
        if (zv_bounds_is_empty(&d))
            return d;
        ZvBounds screen = {0, 0, (float)stage->target->width, (float)stage->target->height};
        d.min_x = floorf(d.min_x) - 1;
        d.min_y = floorf(d.min_y) - 1;
        d.max_x = ceilf(d.max_x) + 1;
        d.max_y = ceilf(d.max_y) + 1;
        area = zv_bounds_intersect(&d, &screen);
        if (zv_bounds_is_empty(&area))
            return area;
    }
    zv_canvas_reset(c);
    zv_canvas_save(c);
    zv_canvas_rect(c, area.min_x, area.min_y, area.max_x - area.min_x, area.max_y - area.min_y);
    zv_canvas_clip(c, ZV_FILL_NONZERO);
    zv_canvas_begin_path(c);
    if (stage->background)
    {
        zv_canvas_set_composite_op(c, ZV_OP_COPY);
        zv_canvas_set_fill_color(c, stage->background);
        zv_canvas_fill_rect(c, area.min_x, area.min_y, area.max_x - area.min_x, area.max_y - area.min_y);
        zv_canvas_set_composite_op(c, ZV_OP_SOURCE_OVER);
    }
    else
        zv_canvas_clear_rect(c, area.min_x, area.min_y, area.max_x - area.min_x, area.max_y - area.min_y);
    render_tree(&stage->root, c);
    zv_canvas_restore(c);
    stage->frames++;
    return area;
}
