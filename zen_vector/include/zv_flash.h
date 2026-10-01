#ifndef ZV_FLASH_H
#define ZV_FLASH_H

/*
 * Flash-style retained graphics and display list over the canvas.
 *
 * ZvGraphics records drawing commands (beginFill, lineStyle, moveTo, lineTo,
 * curveTo, drawRect, ...) and replays them on a ZvCanvas. Flash semantics:
 * fills use the even-odd rule and close automatically, a lineStyle applies
 * to the segments drawn after it, lineStyle with no width removes the line.
 *
 * ZvSprite is a display object: matrix, alpha, visible, blend mode, children
 * in drawing order, optional mask (another sprite whose drawing clips this
 * one) and cacheAsBitmap (the sprite is drawn once into its own surface and
 * reused while nothing changes). Dirty rectangles: every change marks the
 * sprite, and zv_stage_render only redraws the union of the old and new
 * bounds of the changed sprites.
 */

#include "zv_canvas.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        ZV_SCALE_NORMAL,     /* line width scales with the object */
        ZV_SCALE_NONE,       /* line width stays in screen pixels */
        ZV_SCALE_HORIZONTAL, /* scales with x only */
        ZV_SCALE_VERTICAL
    } ZvScaleMode;

    typedef enum
    {
        ZV_G_BEGIN_FILL,
        ZV_G_BEGIN_GRADIENT_FILL,
        ZV_G_BEGIN_BITMAP_FILL,
        ZV_G_END_FILL,
        ZV_G_LINE_STYLE,
        ZV_G_MOVE_TO,
        ZV_G_LINE_TO,
        ZV_G_CURVE_TO,
        ZV_G_CUBIC_CURVE_TO
    } ZvGraphicsOp;

    typedef struct
    {
        ZvGraphicsOp op;
        float v[6];
        ZvPaint paint; /* fills and line paints */
        bool has_paint;
        /* line style */
        float width;
        ZvLineCap cap;
        ZvLineJoin join;
        float miter_limit;
        ZvScaleMode scale_mode;
        bool pixel_hinting;
    } ZvGraphicsCommand;

    typedef struct
    {
        ZvGraphicsCommand *commands;
        int count, capacity;
        const ZvAllocator *allocator;
        ZvBounds bounds; /* control-point bounds in local space, fill and line */
        float max_line_width;
        bool dirty;
    } ZvGraphics;

    void zv_graphics_init(ZvGraphics *g, const ZvAllocator *allocator);
    void zv_graphics_release(ZvGraphics *g);
    void zv_graphics_clear(ZvGraphics *g);

    bool zv_graphics_begin_fill(ZvGraphics *g, uint32_t rgb, float alpha);
    /* A gradient in Flash form: type linear or radial, colours and alphas
       and ratios 0..255 per stop, a matrix mapping the unit box
       (-1..1 in Flash is -819.2..819.2 user units, scaled here to -1..1) to
       the object, spread, and focal point ratio for radial. */
    bool zv_graphics_begin_gradient_fill(ZvGraphics *g, bool radial, const uint32_t *rgbs, const float *alphas, const uint8_t *ratios, int count,
                                         const ZvMatrix *matrix, ZvSpread spread, float focal_ratio);
    bool zv_graphics_begin_bitmap_fill(ZvGraphics *g, const ZvSurface *bitmap, const ZvMatrix *matrix, bool repeat, bool smooth);
    bool zv_graphics_end_fill(ZvGraphics *g);
    /* width <= 0 or NaN removes the line */
    bool zv_graphics_line_style(ZvGraphics *g, float width, uint32_t rgb, float alpha, bool pixel_hinting, ZvScaleMode scale_mode, ZvLineCap cap,
                                ZvLineJoin join, float miter_limit);
    bool zv_graphics_line_gradient_style(ZvGraphics *g, bool radial, const uint32_t *rgbs, const float *alphas, const uint8_t *ratios, int count,
                                        const ZvMatrix *matrix, ZvSpread spread, float focal_ratio);
    bool zv_graphics_move_to(ZvGraphics *g, float x, float y);
    bool zv_graphics_line_to(ZvGraphics *g, float x, float y);
    bool zv_graphics_curve_to(ZvGraphics *g, float cx, float cy, float x, float y);
    bool zv_graphics_cubic_curve_to(ZvGraphics *g, float c1x, float c1y, float c2x, float c2y, float x, float y);
    bool zv_graphics_draw_rect(ZvGraphics *g, float x, float y, float w, float h);
    bool zv_graphics_draw_round_rect(ZvGraphics *g, float x, float y, float w, float h, float ellipse_w, float ellipse_h);
    bool zv_graphics_draw_circle(ZvGraphics *g, float x, float y, float r);
    bool zv_graphics_draw_ellipse(ZvGraphics *g, float x, float y, float w, float h);
    /* drawPath: commands 1 moveTo, 2 lineTo, 3 curveTo, 6 cubicCurveTo
       (Flash GraphicsPathCommand), data in pairs. */
    bool zv_graphics_draw_path(ZvGraphics *g, const uint8_t *commands, int command_count, const float *data, int data_count);
    /* drawTriangles: vertices in pairs, indices in triples (NULL: in order). */
    bool zv_graphics_draw_triangles(ZvGraphics *g, const float *vertices, int vertex_count, const int *indices, int index_count);

    /* Replays the graphics on a canvas with the canvas' current transform. */
    bool zv_graphics_render(const ZvGraphics *g, ZvCanvas *canvas);

    typedef enum
    {
        ZV_BLEND_NORMAL,
        ZV_BLEND_LAYER,
        ZV_BLEND_MULTIPLY,
        ZV_BLEND_SCREEN,
        ZV_BLEND_LIGHTEN,
        ZV_BLEND_DARKEN,
        ZV_BLEND_ADD,
        ZV_BLEND_ERASE,
        ZV_BLEND_ALPHA
    } ZvBlendMode;

    typedef struct ZvSprite ZvSprite;

    struct ZvSprite
    {
        ZvGraphics graphics;
        float x, y, scale_x, scale_y, rotation; /* rotation in degrees, like Flash */
        ZvMatrix *custom_matrix;                 /* NULL: built from x, y, scale, rotation */
        ZvMatrix matrix_storage;
        float alpha;
        bool visible;
        ZvBlendMode blend;
        bool cache_as_bitmap;
        ZvSprite *mask;
        ZvSprite *parent;
        ZvSprite **children;
        int child_count, child_capacity;
        const ZvAllocator *allocator;
        /* internals */
        ZvSurface cache;
        ZvMatrix cache_matrix;
        bool cache_valid;
        ZvBounds last_bounds; /* device bounds at the last render */
        bool dirty;
    };

    void zv_sprite_init(ZvSprite *s, const ZvAllocator *allocator);
    void zv_sprite_release(ZvSprite *s); /* does not release the children */
    bool zv_sprite_add_child(ZvSprite *parent, ZvSprite *child);
    bool zv_sprite_add_child_at(ZvSprite *parent, ZvSprite *child, int index);
    bool zv_sprite_remove_child(ZvSprite *parent, ZvSprite *child);
    void zv_sprite_set_position(ZvSprite *s, float x, float y);
    void zv_sprite_set_scale(ZvSprite *s, float sx, float sy);
    void zv_sprite_set_rotation(ZvSprite *s, float degrees);
    void zv_sprite_set_matrix(ZvSprite *s, const ZvMatrix *m); /* NULL goes back to x, y, scale, rotation */
    void zv_sprite_set_alpha(ZvSprite *s, float alpha);
    void zv_sprite_set_visible(ZvSprite *s, bool visible);
    void zv_sprite_set_blend(ZvSprite *s, ZvBlendMode blend);
    void zv_sprite_set_mask(ZvSprite *s, ZvSprite *mask);
    void zv_sprite_set_cache_as_bitmap(ZvSprite *s, bool on);
    /* Marks the sprite changed (after drawing in its graphics, for example). */
    void zv_sprite_invalidate(ZvSprite *s);
    ZvMatrix zv_sprite_matrix(const ZvSprite *s);
    /* Bounds of the sprite and its children in the space of its parent. */
    ZvBounds zv_sprite_bounds(const ZvSprite *s);

    /* Draws the sprite tree on a canvas, with the canvas transform as the
       stage transform. */
    bool zv_sprite_render(ZvSprite *s, ZvCanvas *canvas);

    typedef struct
    {
        ZvSprite root;
        ZvCanvas canvas;
        ZvSurface *target;
        uint32_t background; /* straight; 0 keeps the surface */
        ZvBounds dirty;      /* device rectangle to redraw next */
        bool full;
        int frames;
    } ZvStage;

    void zv_stage_init(ZvStage *stage, ZvSurface *target, const ZvAllocator *allocator);
    void zv_stage_release(ZvStage *stage);
    /* Redraws what changed since the last frame (the dirty rectangle: the
       union of the previous and current bounds of every changed sprite),
       or everything the first time or after zv_stage_invalidate_all. Returns
       the rectangle redrawn (empty when nothing changed). */
    ZvBounds zv_stage_render(ZvStage *stage);
    void zv_stage_invalidate_all(ZvStage *stage);

#ifdef __cplusplus
}
#endif

#endif /* ZV_FLASH_H */
