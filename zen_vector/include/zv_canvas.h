#ifndef ZV_CANVAS_H
#define ZV_CANVAS_H

/*
 * Canvas 2D API over a ZvSurface. Names and semantics follow the HTML
 * canvas: a state stack, a current transform, a current path in user space,
 * fill and stroke styles, line styles, globalAlpha, globalCompositeOperation,
 * clip, drawImage, image data, text (zv_font.h) and shadows. No filters.
 *
 * Colours given to the API are straight 0xAARRGGBB; the surface is
 * premultiplied. Every drawing function returns false only when memory runs
 * out.
 */

#include "zv_fill.h"
#include "zv_font.h"
#include "zv_stroke.h"

#ifdef __cplusplus
extern "C"
{
#endif

    typedef enum
    {
        ZV_OP_SOURCE_OVER,
        ZV_OP_SOURCE_IN,
        ZV_OP_SOURCE_OUT,
        ZV_OP_SOURCE_ATOP,
        ZV_OP_DESTINATION_OVER,
        ZV_OP_DESTINATION_IN,
        ZV_OP_DESTINATION_OUT,
        ZV_OP_DESTINATION_ATOP,
        ZV_OP_LIGHTER,
        ZV_OP_COPY,
        ZV_OP_XOR,
        ZV_OP_MULTIPLY,
        ZV_OP_SCREEN,
        ZV_OP_DARKEN,
        ZV_OP_LIGHTEN,
        ZV_OP_COUNT
    } ZvCompositeOp;

    /* Parses the Canvas name ("source-over", "lighter", ...). Returns false
       for an unknown name, leaving *op untouched. */
    bool zv_composite_op_parse(const char *name, ZvCompositeOp *op);

    /* Composites one premultiplied src pixel onto dst with op. */
    ZvPixel zv_composite(ZvPixel dst, ZvPixel src, ZvCompositeOp op);

    typedef enum
    {
        ZV_ALIGN_START,
        ZV_ALIGN_END,
        ZV_ALIGN_LEFT,
        ZV_ALIGN_RIGHT,
        ZV_ALIGN_CENTER
    } ZvTextAlign;

    typedef enum
    {
        ZV_BASELINE_ALPHABETIC,
        ZV_BASELINE_TOP,
        ZV_BASELINE_MIDDLE,
        ZV_BASELINE_BOTTOM,
        ZV_BASELINE_HANGING,
        ZV_BASELINE_IDEOGRAPHIC
    } ZvTextBaseline;

#define ZV_MAX_DASHES 32
#define ZV_STATE_STACK 32
#define ZV_MAX_LAYERS 8

    typedef struct
    {
        ZvMatrix transform;
        ZvPaint fill;
        ZvPaint stroke;
        float line_width;
        ZvLineCap line_cap;
        ZvLineJoin line_join;
        float miter_limit;
        float dashes[ZV_MAX_DASHES];
        int dash_count;
        float dash_offset;
        float global_alpha; /* 0..1 */
        ZvCompositeOp op;
        bool image_smoothing;
        /* clip: bounds in device pixels plus an optional coverage mask */
        ZvBounds clip_bounds;
        uint8_t *clip_mask; /* NULL: rectangle only; else width*height, owned by the state */
        /* shadows */
        uint32_t shadow_color; /* straight; alpha 0 means no shadow */
        float shadow_blur, shadow_x, shadow_y;
        /* text */
        const ZvFont *font;
        float font_size;
        ZvTextAlign text_align;
        ZvTextBaseline text_baseline;
    } ZvCanvasState;

    typedef struct
    {
        ZvSurface *target;
        const ZvAllocator *allocator;
        ZvCanvasState state;
        ZvCanvasState stack[ZV_STATE_STACK];
        int depth;
        ZvPath path; /* current path, user space of the moment each segment was added */
        /* scratch */
        ZvPath scratch_path;
        ZvPolyline poly, poly2, poly3;
        ZvRasterizer raster;
        ZvSurface layer;  /* for composite operations that need one */
        uint8_t *cover;   /* target sized coverage scratch */
        uint8_t *shadow;  /* target sized shadow scratch */
        ZvSurface layers[ZV_MAX_LAYERS];
        ZvSurface *layer_targets[ZV_MAX_LAYERS];
        int layer_depth;
        bool oom;
    } ZvCanvas;

    /* The canvas draws on target, which the caller owns and keeps alive. */
    void zv_canvas_init(ZvCanvas *c, ZvSurface *target, const ZvAllocator *allocator);
    void zv_canvas_release(ZvCanvas *c);
    /* Everything back to the defaults, the surface untouched. */
    void zv_canvas_reset(ZvCanvas *c);

    /* State */
    void zv_canvas_save(ZvCanvas *c);
    void zv_canvas_restore(ZvCanvas *c);

    /* Transform */
    void zv_canvas_translate(ZvCanvas *c, float x, float y);
    void zv_canvas_rotate(ZvCanvas *c, float radians);
    void zv_canvas_scale(ZvCanvas *c, float x, float y);
    void zv_canvas_transform(ZvCanvas *c, float a, float b, float cc, float d, float e, float f);
    void zv_canvas_set_transform(ZvCanvas *c, float a, float b, float cc, float d, float e, float f);
    void zv_canvas_reset_transform(ZvCanvas *c);
    ZvMatrix zv_canvas_get_transform(const ZvCanvas *c);

    /* Styles */
    void zv_canvas_set_fill_color(ZvCanvas *c, uint32_t straight);
    void zv_canvas_set_stroke_color(ZvCanvas *c, uint32_t straight);
    void zv_canvas_set_fill_paint(ZvCanvas *c, const ZvPaint *paint);
    void zv_canvas_set_stroke_paint(ZvCanvas *c, const ZvPaint *paint);
    void zv_canvas_set_line_width(ZvCanvas *c, float width); /* ignored unless > 0 and finite */
    void zv_canvas_set_line_cap(ZvCanvas *c, ZvLineCap cap);
    void zv_canvas_set_line_join(ZvCanvas *c, ZvLineJoin join);
    void zv_canvas_set_miter_limit(ZvCanvas *c, float limit); /* ignored unless > 0 */
    /* Ignored when any value is negative or not finite; an odd count is repeated. */
    void zv_canvas_set_line_dash(ZvCanvas *c, const float *dashes, int count);
    void zv_canvas_set_line_dash_offset(ZvCanvas *c, float offset);
    void zv_canvas_set_global_alpha(ZvCanvas *c, float alpha); /* ignored outside 0..1 */
    void zv_canvas_set_composite_op(ZvCanvas *c, ZvCompositeOp op);
    void zv_canvas_set_image_smoothing(ZvCanvas *c, bool on);
    void zv_canvas_set_shadow(ZvCanvas *c, uint32_t straight_color, float blur, float x, float y);
    void zv_canvas_set_font(ZvCanvas *c, const ZvFont *font, float size);
    void zv_canvas_set_text_align(ZvCanvas *c, ZvTextAlign align);
    void zv_canvas_set_text_baseline(ZvCanvas *c, ZvTextBaseline baseline);

    /* Rectangles */
    bool zv_canvas_clear_rect(ZvCanvas *c, float x, float y, float w, float h);
    bool zv_canvas_fill_rect(ZvCanvas *c, float x, float y, float w, float h);
    bool zv_canvas_stroke_rect(ZvCanvas *c, float x, float y, float w, float h);

    /* Paths. Segments are transformed by the current transform as they are
       added, like the Canvas. */
    void zv_canvas_begin_path(ZvCanvas *c);
    bool zv_canvas_move_to(ZvCanvas *c, float x, float y);
    bool zv_canvas_line_to(ZvCanvas *c, float x, float y);
    bool zv_canvas_quadratic_curve_to(ZvCanvas *c, float cx, float cy, float x, float y);
    bool zv_canvas_bezier_curve_to(ZvCanvas *c, float c1x, float c1y, float c2x, float c2y, float x, float y);
    bool zv_canvas_close_path(ZvCanvas *c);
    bool zv_canvas_rect(ZvCanvas *c, float x, float y, float w, float h);
    /* One radius for all corners (the four-radius form takes an array of 4). */
    bool zv_canvas_round_rect(ZvCanvas *c, float x, float y, float w, float h, const float *radii, int radius_count);
    bool zv_canvas_arc(ZvCanvas *c, float x, float y, float r, float a0, float a1, bool anticlockwise);
    bool zv_canvas_arc_to(ZvCanvas *c, float x1, float y1, float x2, float y2, float r);
    bool zv_canvas_ellipse(ZvCanvas *c, float x, float y, float rx, float ry, float rotation, float a0, float a1, bool anticlockwise);

    bool zv_canvas_fill(ZvCanvas *c, ZvFillRule rule);
    bool zv_canvas_stroke(ZvCanvas *c);
    bool zv_canvas_clip(ZvCanvas *c, ZvFillRule rule);
    /* Fill, stroke and clip of an external path (Path2D), in user space. */
    bool zv_canvas_fill_path(ZvCanvas *c, const ZvPath *path, ZvFillRule rule);
    bool zv_canvas_stroke_path(ZvCanvas *c, const ZvPath *path);
    bool zv_canvas_clip_path(ZvCanvas *c, const ZvPath *path, ZvFillRule rule);
    bool zv_canvas_is_point_in_path(ZvCanvas *c, float x, float y, ZvFillRule rule);

    /* Images: a premultiplied surface. The 9-argument drawImage. */
    bool zv_canvas_draw_image(ZvCanvas *c, const ZvSurface *image, float sx, float sy, float sw, float sh, float dx, float dy, float dw, float dh);
    bool zv_canvas_draw_image_simple(ZvCanvas *c, const ZvSurface *image, float dx, float dy);

    /* Image data is straight 0xAARRGGBB in device pixels, like getImageData
       out must hold w*h pixels. */
    void zv_canvas_get_image_data(const ZvCanvas *c, int x, int y, int w, int h, uint32_t *out);
    void zv_canvas_put_image_data(ZvCanvas *c, const uint32_t *data, int w, int h, int x, int y);

    /* Text */
    bool zv_canvas_fill_text(ZvCanvas *c, const char *utf8, float x, float y);
    bool zv_canvas_stroke_text(ZvCanvas *c, const char *utf8, float x, float y);
    float zv_canvas_measure_text(const ZvCanvas *c, const char *utf8);

    /* Layers for group alpha (Flash): drawing goes to a fresh transparent
       surface until the matching end, which composites it with alpha and
       the current operation, optionally through a mask (coverage per pixel,
       target sized). */
    bool zv_canvas_begin_layer(ZvCanvas *c);
    bool zv_canvas_end_layer(ZvCanvas *c, float alpha, const uint8_t *mask);

#ifdef __cplusplus
}
#endif

#endif /* ZV_CANVAS_H */
