#include "scene.h"
#include "zv_canvas.h"
#include "zv_io.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 40

#ifndef ZV_SCENE_FONT
#define ZV_SCENE_FONT "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
#endif

static const char *g_font_path = ZV_SCENE_FONT;

void zv_scene_set_font_path(const char *path)
{
    g_font_path = path ? path : ZV_SCENE_FONT;
}

typedef struct
{
    ZvSurface surface;
    ZvCanvas canvas;
    bool have_size;
    ZvPaint gradient;
    ZvSurface pattern_image;
    ZvPaint pattern;
    ZvFilter pattern_filter;
    ZvFont font;
    uint8_t *font_data;
    size_t font_size;
    bool font_loaded;
} Scene;

static bool fail(char *err, size_t errcap, int line, const char *fmt, const char *arg)
{
    if (err && errcap)
    {
        int n = snprintf(err, errcap, "line %d: ", line);
        if (n > 0 && (size_t)n < errcap)
            snprintf(err + n, errcap - (size_t)n, fmt, arg ? arg : "");
    }
    return false;
}

static bool parse_int(const char *s, int *out)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno != 0 || v < -1000000 || v > 1000000)
        return false;
    *out = (int)v;
    return true;
}

static bool parse_float(const char *s, float *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (end == s || *end != '\0' || errno != 0 || v < -1e7 || v > 1e7)
        return false;
    *out = (float)v;
    return true;
}

static bool parse_color(const char *s, uint32_t *out)
{
    size_t len = strlen(s);
    if (s[0] != '#' || (len != 7 && len != 9))
        return false;
    uint32_t v = 0;
    for (size_t i = 1; i < len; i++)
    {
        char c = s[i];
        int d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        v = v << 4 | (uint32_t)d;
    }
    *out = len == 7 ? 0xFF000000u | v : (v & 0xFFu) << 24 | v >> 8;
    return true;
}

static int tokenize(char *line, char *tokens[], int max)
{
    int n = 0;
    for (char *p = line; *p;)
    {
        while (*p == ' ' || *p == '\t' || *p == '\r')
            *p++ = '\0';
        if (!*p)
            break;
        if (n == max)
            return -1;
        tokens[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r')
            p++;
    }
    return n;
}

static bool args(char *tokens[], int count, int needed, float *v, int line, char *err, size_t errcap)
{
    if (count != needed + 1)
        return fail(err, errcap, line, "%s takes a different number of arguments", tokens[0]);
    for (int i = 0; i < needed; i++)
    {
        if (!parse_float(tokens[i + 1], &v[i]))
            return fail(err, errcap, line, "'%s' is not a number", tokens[i + 1]);
    }
    return true;
}

static bool make_pattern_image(ZvSurface *s, int w, int h)
{
    if (!zv_surface_init(s, NULL, w, h))
        return false;
    for (int y = 0; y < h; y++)
    {
        for (int x = 0; x < w; x++)
        {
            uint32_t r = w > 1 ? (uint32_t)(x * 255 / (w - 1)) : 0;
            uint32_t g = h > 1 ? (uint32_t)(y * 255 / (h - 1)) : 0;
            uint32_t b = (((x >> 2) + (y >> 2)) & 1) ? 255u : 0u;
            uint32_t a = (x < w / 2 && y < h / 2) ? 128u : 255u;
            s->pixels[y * s->stride + x] = zv_premultiply(a << 24 | r << 16 | g << 8 | b);
        }
    }
    return true;
}

static bool load_font(Scene *sc)
{
    if (sc->font_loaded)
        return true;
    size_t size = 0;
    uint8_t *data = zv_io_read(g_font_path, &size);
    if (!data)
        return false;
    sc->font_data = malloc(size);
    if (!sc->font_data)
    {
        zv_io_free(data);
        return false;
    }
    memcpy(sc->font_data, data, size);
    zv_io_free(data);
    sc->font_size = size;
    sc->font_loaded = zv_font_init(&sc->font, sc->font_data, sc->font_size, NULL);
    return sc->font_loaded;
}

static bool run_line(Scene *sc, char *tokens[], int count, int line, char *err, size_t errcap)
{
    const char *cmd = tokens[0];
    float v[8];
    ZvCanvas *c = &sc->canvas;

    if (strcmp(cmd, "size") == 0)
    {
        int w, h;
        if (count != 3 || !parse_int(tokens[1], &w) || !parse_int(tokens[2], &h) || w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return fail(err, errcap, line, "size needs two positive integers up to 16384", NULL);
        if (sc->have_size)
            return fail(err, errcap, line, "size may appear only once", NULL);
        if (!zv_surface_init(&sc->surface, NULL, w, h))
            return fail(err, errcap, line, "out of memory", NULL);
        zv_canvas_init(c, &sc->surface, NULL);
        sc->have_size = true;
        return true;
    }
    if (!sc->have_size)
        return fail(err, errcap, line, "'%s' before size", cmd);

#define NUM(n) \
    if (!args(tokens, count, n, v, line, err, errcap)) \
        return false;
#define OOM(x) ((x) || fail(err, errcap, line, "out of memory", NULL))

    if (strcmp(cmd, "fillStyle") == 0 || strcmp(cmd, "strokeStyle") == 0 || strcmp(cmd, "shadowColor") == 0)
    {
        uint32_t straight;
        if (count != 2 || !parse_color(tokens[1], &straight))
            return fail(err, errcap, line, "%s needs #rrggbb or #rrggbbaa", cmd);
        if (cmd[0] == 'f')
            zv_canvas_set_fill_color(c, straight);
        else if (cmd[1] == 't')
            zv_canvas_set_stroke_color(c, straight);
        else
            zv_canvas_set_shadow(c, straight, c->state.shadow_blur, c->state.shadow_x, c->state.shadow_y);
        return true;
    }
    if (strcmp(cmd, "shadowBlur") == 0)
    {
        NUM(1);
        zv_canvas_set_shadow(c, c->state.shadow_color, v[0], c->state.shadow_x, c->state.shadow_y);
        return true;
    }
    if (strcmp(cmd, "shadowOffsetX") == 0)
    {
        NUM(1);
        zv_canvas_set_shadow(c, c->state.shadow_color, c->state.shadow_blur, v[0], c->state.shadow_y);
        return true;
    }
    if (strcmp(cmd, "shadowOffsetY") == 0)
    {
        NUM(1);
        zv_canvas_set_shadow(c, c->state.shadow_color, c->state.shadow_blur, c->state.shadow_x, v[0]);
        return true;
    }
    if (strcmp(cmd, "lineWidth") == 0)
    {
        NUM(1);
        zv_canvas_set_line_width(c, v[0]);
        return true;
    }
    if (strcmp(cmd, "miterLimit") == 0)
    {
        NUM(1);
        zv_canvas_set_miter_limit(c, v[0]);
        return true;
    }
    if (strcmp(cmd, "lineDashOffset") == 0)
    {
        NUM(1);
        zv_canvas_set_line_dash_offset(c, v[0]);
        return true;
    }
    if (strcmp(cmd, "globalAlpha") == 0)
    {
        NUM(1);
        zv_canvas_set_global_alpha(c, v[0]);
        return true;
    }
    if (strcmp(cmd, "lineCap") == 0)
    {
        if (count != 2)
            return fail(err, errcap, line, "lineCap needs a value", NULL);
        if (strcmp(tokens[1], "butt") == 0)
            zv_canvas_set_line_cap(c, ZV_CAP_BUTT);
        else if (strcmp(tokens[1], "round") == 0)
            zv_canvas_set_line_cap(c, ZV_CAP_ROUND);
        else if (strcmp(tokens[1], "square") == 0)
            zv_canvas_set_line_cap(c, ZV_CAP_SQUARE);
        else
            return fail(err, errcap, line, "unknown lineCap '%s'", tokens[1]);
        return true;
    }
    if (strcmp(cmd, "lineJoin") == 0)
    {
        if (count != 2)
            return fail(err, errcap, line, "lineJoin needs a value", NULL);
        if (strcmp(tokens[1], "miter") == 0)
            zv_canvas_set_line_join(c, ZV_JOIN_MITER);
        else if (strcmp(tokens[1], "round") == 0)
            zv_canvas_set_line_join(c, ZV_JOIN_ROUND);
        else if (strcmp(tokens[1], "bevel") == 0)
            zv_canvas_set_line_join(c, ZV_JOIN_BEVEL);
        else
            return fail(err, errcap, line, "unknown lineJoin '%s'", tokens[1]);
        return true;
    }
    if (strcmp(cmd, "setLineDash") == 0)
    {
        float d[ZV_MAX_DASHES];
        if (count - 1 > ZV_MAX_DASHES / 2)
            return fail(err, errcap, line, "too many dashes", NULL);
        for (int i = 1; i < count; i++)
        {
            if (!parse_float(tokens[i], &d[i - 1]))
                return fail(err, errcap, line, "'%s' is not a number", tokens[i]);
        }
        zv_canvas_set_line_dash(c, d, count - 1);
        return true;
    }
    if (strcmp(cmd, "globalCompositeOperation") == 0)
    {
        ZvCompositeOp op;
        if (count != 2 || !zv_composite_op_parse(tokens[1], &op))
            return fail(err, errcap, line, "unknown composite operation", NULL);
        zv_canvas_set_composite_op(c, op);
        return true;
    }
    if (strcmp(cmd, "imageSmoothingEnabled") == 0)
    {
        if (count != 2 || (strcmp(tokens[1], "true") != 0 && strcmp(tokens[1], "false") != 0))
            return fail(err, errcap, line, "imageSmoothingEnabled needs true or false", NULL);
        zv_canvas_set_image_smoothing(c, tokens[1][0] == 't');
        sc->pattern_filter = tokens[1][0] == 't' ? ZV_FILTER_BILINEAR : ZV_FILTER_NEAREST;
        return true;
    }
    if (strcmp(cmd, "translate") == 0)
    {
        NUM(2);
        zv_canvas_translate(c, v[0], v[1]);
        return true;
    }
    if (strcmp(cmd, "scale") == 0)
    {
        NUM(2);
        zv_canvas_scale(c, v[0], v[1]);
        return true;
    }
    if (strcmp(cmd, "rotate") == 0)
    {
        NUM(1);
        zv_canvas_rotate(c, v[0]);
        return true;
    }
    if (strcmp(cmd, "transform") == 0)
    {
        NUM(6);
        zv_canvas_transform(c, v[0], v[1], v[2], v[3], v[4], v[5]);
        return true;
    }
    if (strcmp(cmd, "setTransform") == 0)
    {
        NUM(6);
        zv_canvas_set_transform(c, v[0], v[1], v[2], v[3], v[4], v[5]);
        return true;
    }
    if (strcmp(cmd, "resetTransform") == 0)
    {
        zv_canvas_reset_transform(c);
        return true;
    }
    if (strcmp(cmd, "save") == 0)
    {
        zv_canvas_save(c);
        return true;
    }
    if (strcmp(cmd, "restore") == 0)
    {
        zv_canvas_restore(c);
        return true;
    }
    if (strcmp(cmd, "fillRect") == 0)
    {
        NUM(4);
        return OOM(zv_canvas_fill_rect(c, v[0], v[1], v[2], v[3]));
    }
    if (strcmp(cmd, "strokeRect") == 0)
    {
        NUM(4);
        return OOM(zv_canvas_stroke_rect(c, v[0], v[1], v[2], v[3]));
    }
    if (strcmp(cmd, "clearRect") == 0)
    {
        NUM(4);
        return OOM(zv_canvas_clear_rect(c, v[0], v[1], v[2], v[3]));
    }
    if (strcmp(cmd, "beginPath") == 0)
    {
        zv_canvas_begin_path(c);
        return true;
    }
    if (strcmp(cmd, "closePath") == 0)
        return OOM(zv_canvas_close_path(c));
    if (strcmp(cmd, "moveTo") == 0)
    {
        NUM(2);
        return OOM(zv_canvas_move_to(c, v[0], v[1]));
    }
    if (strcmp(cmd, "lineTo") == 0)
    {
        NUM(2);
        return OOM(zv_canvas_line_to(c, v[0], v[1]));
    }
    if (strcmp(cmd, "quadraticCurveTo") == 0)
    {
        NUM(4);
        return OOM(zv_canvas_quadratic_curve_to(c, v[0], v[1], v[2], v[3]));
    }
    if (strcmp(cmd, "bezierCurveTo") == 0)
    {
        NUM(6);
        return OOM(zv_canvas_bezier_curve_to(c, v[0], v[1], v[2], v[3], v[4], v[5]));
    }
    if (strcmp(cmd, "rect") == 0)
    {
        NUM(4);
        return OOM(zv_canvas_rect(c, v[0], v[1], v[2], v[3]));
    }
    if (strcmp(cmd, "roundRect") == 0)
    {
        NUM(5);
        return OOM(zv_canvas_round_rect(c, v[0], v[1], v[2], v[3], &v[4], 1));
    }
    if (strcmp(cmd, "arc") == 0)
    {
        bool ccw = count == 7 && strcmp(tokens[6], "true") == 0;
        if (ccw)
            count--;
        NUM(5);
        return OOM(zv_canvas_arc(c, v[0], v[1], v[2], v[3], v[4], ccw));
    }
    if (strcmp(cmd, "arcTo") == 0)
    {
        NUM(5);
        return OOM(zv_canvas_arc_to(c, v[0], v[1], v[2], v[3], v[4]));
    }
    if (strcmp(cmd, "ellipse") == 0)
    {
        bool ccw = count == 9 && strcmp(tokens[8], "true") == 0;
        if (ccw)
            count--;
        NUM(7);
        return OOM(zv_canvas_ellipse(c, v[0], v[1], v[2], v[3], v[4], v[5], v[6], ccw));
    }
    if (strcmp(cmd, "fill") == 0 || strcmp(cmd, "clip") == 0)
    {
        ZvFillRule rule = ZV_FILL_NONZERO;
        if (count == 2 && strcmp(tokens[1], "evenodd") == 0)
            rule = ZV_FILL_EVENODD;
        else if (count == 2 && strcmp(tokens[1], "nonzero") == 0)
            rule = ZV_FILL_NONZERO;
        else if (count != 1)
            return fail(err, errcap, line, "%s takes nonzero or evenodd", cmd);
        return OOM(cmd[0] == 'f' ? zv_canvas_fill(c, rule) : zv_canvas_clip(c, rule));
    }
    if (strcmp(cmd, "stroke") == 0)
        return OOM(zv_canvas_stroke(c));
    if (strcmp(cmd, "gradientLinear") == 0)
    {
        NUM(4);
        sc->gradient = zv_paint_linear(v[0], v[1], v[2], v[3]);
        return true;
    }
    if (strcmp(cmd, "gradientRadial") == 0)
    {
        NUM(6);
        sc->gradient = zv_paint_radial(v[0], v[1], v[2], v[3], v[4], v[5]);
        return true;
    }
    if (strcmp(cmd, "gradientStop") == 0)
    {
        uint32_t straight;
        if (count != 3 || !parse_float(tokens[1], &v[0]) || !parse_color(tokens[2], &straight))
            return fail(err, errcap, line, "gradientStop needs offset #rrggbb[aa]", NULL);
        if (sc->gradient.type == ZV_PAINT_SOLID || !zv_paint_add_stop(&sc->gradient, v[0], straight))
            return fail(err, errcap, line, "bad gradient stop", NULL);
        return true;
    }
    if (strcmp(cmd, "fillGradient") == 0 || strcmp(cmd, "strokeGradient") == 0)
    {
        if (sc->gradient.type == ZV_PAINT_SOLID)
            return fail(err, errcap, line, "%s needs a gradient", cmd);
        if (cmd[0] == 'f')
            zv_canvas_set_fill_paint(c, &sc->gradient);
        else
            zv_canvas_set_stroke_paint(c, &sc->gradient);
        return true;
    }
    if (strcmp(cmd, "patternImage") == 0)
    {
        int w, h;
        if (count != 3 || !parse_int(tokens[1], &w) || !parse_int(tokens[2], &h) || w <= 0 || h <= 0 || w > 1024 || h > 1024)
            return fail(err, errcap, line, "patternImage needs w h", NULL);
        zv_surface_release(&sc->pattern_image);
        return OOM(make_pattern_image(&sc->pattern_image, w, h));
    }
    if (strcmp(cmd, "fillPattern") == 0)
    {
        if (count != 2 || !sc->pattern_image.pixels)
            return fail(err, errcap, line, "fillPattern needs a repetition after patternImage", NULL);
        bool rx, ry;
        if (strcmp(tokens[1], "repeat") == 0)
            rx = ry = true;
        else if (strcmp(tokens[1], "repeat-x") == 0)
        {
            rx = true;
            ry = false;
        }
        else if (strcmp(tokens[1], "repeat-y") == 0)
        {
            rx = false;
            ry = true;
        }
        else if (strcmp(tokens[1], "no-repeat") == 0)
            rx = ry = false;
        else
            return fail(err, errcap, line, "fillPattern needs repeat, repeat-x, repeat-y or no-repeat", NULL);
        sc->pattern = zv_paint_pattern(&sc->pattern_image, rx, ry, sc->pattern_filter);
        zv_canvas_set_fill_paint(c, &sc->pattern);
        return true;
    }
    if (strcmp(cmd, "patternTransform") == 0)
    {
        NUM(6);
        sc->pattern.matrix = zv_matrix_make(v[0], v[1], v[2], v[3], v[4], v[5]);
        if (c->state.fill.type == ZV_PAINT_PATTERN)
            c->state.fill.matrix = sc->pattern.matrix;
        return true;
    }
    if (strcmp(cmd, "drawImage") == 0)
    {
        NUM(8);
        if (!sc->pattern_image.pixels)
            return fail(err, errcap, line, "drawImage needs patternImage first", NULL);
        return OOM(zv_canvas_draw_image(c, &sc->pattern_image, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7]));
    }
    if (strcmp(cmd, "font") == 0)
    {
        NUM(1);
        if (!load_font(sc))
            return fail(err, errcap, line, "cannot load the font %s", g_font_path);
        zv_canvas_set_font(c, &sc->font, v[0]);
        return true;
    }
    if (strcmp(cmd, "textAlign") == 0)
    {
        static const char *const names[] = {"start", "end", "left", "right", "center"};
        for (int i = 0; i < 5; i++)
        {
            if (count == 2 && strcmp(tokens[1], names[i]) == 0)
            {
                zv_canvas_set_text_align(c, (ZvTextAlign)i);
                return true;
            }
        }
        return fail(err, errcap, line, "unknown textAlign", NULL);
    }
    if (strcmp(cmd, "textBaseline") == 0)
    {
        static const char *const names[] = {"alphabetic", "top", "middle", "bottom", "hanging", "ideographic"};
        for (int i = 0; i < 6; i++)
        {
            if (count == 2 && strcmp(tokens[1], names[i]) == 0)
            {
                zv_canvas_set_text_baseline(c, (ZvTextBaseline)i);
                return true;
            }
        }
        return fail(err, errcap, line, "unknown textBaseline", NULL);
    }
    if (strcmp(cmd, "fillText") == 0 || strcmp(cmd, "strokeText") == 0)
    {
        if (count < 4 || !parse_float(tokens[1], &v[0]) || !parse_float(tokens[2], &v[1]))
            return fail(err, errcap, line, "%s needs x y text", cmd);
        if (!c->state.font)
            return fail(err, errcap, line, "%s before font", cmd);
        char text[512];
        text[0] = '\0';
        for (int i = 3; i < count; i++)
        {
            if (i > 3)
                strncat(text, " ", sizeof text - strlen(text) - 1);
            strncat(text, tokens[i], sizeof text - strlen(text) - 1);
        }
        return OOM(cmd[0] == 'f' ? zv_canvas_fill_text(c, text, v[0], v[1]) : zv_canvas_stroke_text(c, text, v[0], v[1]));
    }
    return fail(err, errcap, line, "unknown command '%s'", cmd);
}

bool zv_scene_run(const char *text, ZvBitmap *out, char *err, size_t errcap)
{
    if (err && errcap)
        err[0] = '\0';
    if (!text || !out)
        return fail(err, errcap, 0, "no scene", NULL);

    Scene sc;
    memset(&sc, 0, sizeof sc);
    sc.gradient = zv_paint_solid(0);
    sc.pattern = zv_paint_solid(0);
    sc.pattern_filter = ZV_FILTER_BILINEAR;

    size_t len = strlen(text);
    char *copy = malloc(len + 1);
    if (!copy)
        return fail(err, errcap, 0, "out of memory", NULL);
    memcpy(copy, text, len + 1);

    bool ok = true;
    int line_no = 0;
    for (char *line = copy; ok && line;)
    {
        char *next = strchr(line, '\n');
        if (next)
            *next++ = '\0';
        line_no++;
        char *p = line;
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (*p && *p != '#')
        {
            char *tokens[MAX_TOKENS];
            int count = tokenize(p, tokens, MAX_TOKENS);
            if (count < 0)
                ok = fail(err, errcap, line_no, "too many arguments", NULL);
            else
                ok = run_line(&sc, tokens, count, line_no, err, errcap);
        }
        line = next;
    }
    free(copy);

    if (ok && !sc.have_size)
        ok = fail(err, errcap, line_no, "the scene has no size", NULL);
    if (ok)
    {
        if (!zv_bitmap_alloc(out, sc.surface.width, sc.surface.height))
            ok = fail(err, errcap, line_no, "out of memory", NULL);
        else
            zv_surface_store_pixels(&sc.surface, out->pixels, out->width, out->height, out->stride);
    }
    if (sc.have_size)
        zv_canvas_release(&sc.canvas);
    if (sc.font_loaded)
        zv_font_release(&sc.font);
    free(sc.font_data);
    zv_surface_release(&sc.pattern_image);
    zv_surface_release(&sc.surface);
    return ok;
}
