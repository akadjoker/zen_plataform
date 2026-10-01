#include "scene.h"
#include "zv_raster.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 8

typedef struct
{
    ZvSurface surface;
    bool have_size;
    ZvPixel fill;
    ZvPath path;
    ZvPolyline poly;
    ZvRasterizer raster;
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
    if (len == 7)
        *out = 0xFF000000u | v;
    else
        *out = (v & 0xFFu) << 24 | v >> 8;
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

static bool parse_floats(char *tokens[], int count, int needed, float *v, int line, char *err, size_t errcap, const char *usage)
{
    if (count != needed + 1)
        return fail(err, errcap, line, "%s", usage);
    for (int i = 0; i < needed; i++)
    {
        if (!parse_float(tokens[i + 1], &v[i]))
            return fail(err, errcap, line, "'%s' is not a number", tokens[i + 1]);
    }
    return true;
}

static bool fill_path(Scene *sc, const ZvPath *path, ZvFillRule rule)
{
    zv_polyline_clear(&sc->poly);
    if (!zv_path_flatten(path, NULL, ZV_FLATTEN_TOLERANCE, &sc->poly))
        return false;
    return zv_fill_polyline_solid(&sc->surface, &sc->poly, rule, sc->fill, NULL, &sc->raster);
}

static bool run_line(Scene *sc, char *tokens[], int count, int line, char *err, size_t errcap)
{
    const char *cmd = tokens[0];
    float v[6];

    if (strcmp(cmd, "size") == 0)
    {
        int w, h;
        if (count != 3 || !parse_int(tokens[1], &w) || !parse_int(tokens[2], &h) || w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return fail(err, errcap, line, "size needs two positive integers up to 16384", NULL);
        if (sc->have_size)
            return fail(err, errcap, line, "size may appear only once", NULL);
        if (!zv_surface_init(&sc->surface, NULL, w, h))
            return fail(err, errcap, line, "out of memory", NULL);
        sc->have_size = true;
        return true;
    }
    if (!sc->have_size)
        return fail(err, errcap, line, "'%s' before size", cmd);

    if (strcmp(cmd, "fillStyle") == 0)
    {
        uint32_t straight;
        if (count != 2 || !parse_color(tokens[1], &straight))
            return fail(err, errcap, line, "fillStyle needs #rrggbb or #rrggbbaa", NULL);
        sc->fill = zv_premultiply(straight);
        return true;
    }
    if (strcmp(cmd, "fillRect") == 0)
    {
        if (!parse_floats(tokens, count, 4, v, line, err, errcap, "fillRect needs x y w h"))
            return false;
        ZvPath rect;
        zv_path_init(&rect, NULL);
        bool ok = zv_path_move_to(&rect, v[0], v[1]) && zv_path_line_to(&rect, v[0] + v[2], v[1]) &&
                  zv_path_line_to(&rect, v[0] + v[2], v[1] + v[3]) && zv_path_line_to(&rect, v[0], v[1] + v[3]) && zv_path_close(&rect) &&
                  fill_path(sc, &rect, ZV_FILL_NONZERO);
        zv_path_release(&rect);
        return ok || fail(err, errcap, line, "out of memory", NULL);
    }
    if (strcmp(cmd, "beginPath") == 0)
    {
        if (count != 1)
            return fail(err, errcap, line, "beginPath takes no arguments", NULL);
        zv_path_clear(&sc->path);
        return true;
    }
    if (strcmp(cmd, "moveTo") == 0)
    {
        if (!parse_floats(tokens, count, 2, v, line, err, errcap, "moveTo needs x y"))
            return false;
        return zv_path_move_to(&sc->path, v[0], v[1]) || fail(err, errcap, line, "out of memory", NULL);
    }
    if (strcmp(cmd, "lineTo") == 0)
    {
        if (!parse_floats(tokens, count, 2, v, line, err, errcap, "lineTo needs x y"))
            return false;
        return zv_path_line_to(&sc->path, v[0], v[1]) || fail(err, errcap, line, "out of memory", NULL);
    }
    if (strcmp(cmd, "quadraticCurveTo") == 0)
    {
        if (!parse_floats(tokens, count, 4, v, line, err, errcap, "quadraticCurveTo needs cx cy x y"))
            return false;
        return zv_path_quad_to(&sc->path, v[0], v[1], v[2], v[3]) || fail(err, errcap, line, "out of memory", NULL);
    }
    if (strcmp(cmd, "bezierCurveTo") == 0)
    {
        if (!parse_floats(tokens, count, 6, v, line, err, errcap, "bezierCurveTo needs c1x c1y c2x c2y x y"))
            return false;
        return zv_path_cubic_to(&sc->path, v[0], v[1], v[2], v[3], v[4], v[5]) || fail(err, errcap, line, "out of memory", NULL);
    }
    if (strcmp(cmd, "closePath") == 0)
    {
        if (count != 1)
            return fail(err, errcap, line, "closePath takes no arguments", NULL);
        return zv_path_close(&sc->path) || fail(err, errcap, line, "out of memory", NULL);
    }
    if (strcmp(cmd, "fill") == 0)
    {
        ZvFillRule rule = ZV_FILL_NONZERO;
        if (count == 2 && strcmp(tokens[1], "evenodd") == 0)
            rule = ZV_FILL_EVENODD;
        else if (count == 2 && strcmp(tokens[1], "nonzero") == 0)
            rule = ZV_FILL_NONZERO;
        else if (count != 1)
            return fail(err, errcap, line, "fill takes nonzero or evenodd", NULL);
        return fill_path(sc, &sc->path, rule) || fail(err, errcap, line, "out of memory", NULL);
    }
    return fail(err, errcap, line, "unknown command '%s'", cmd);
}

bool zv_scene_run(const char *text, Framebuffer *out, char *err, size_t errcap)
{
    if (err && errcap)
        err[0] = '\0';
    if (!text || !out)
        return fail(err, errcap, 0, "no scene", NULL);

    Scene sc;
    memset(&sc, 0, sizeof sc);
    sc.fill = 0xFF000000u;
    zv_path_init(&sc.path, NULL);
    zv_polyline_init(&sc.poly, NULL);
    zv_rasterizer_init(&sc.raster, NULL);

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
        if (!framebuffer_alloc(out, sc.surface.width, sc.surface.height))
            ok = fail(err, errcap, line_no, "out of memory", NULL);
        else
            zv_surface_store(&sc.surface, out);
    }
    zv_rasterizer_release(&sc.raster);
    zv_polyline_release(&sc.poly);
    zv_path_release(&sc.path);
    zv_surface_release(&sc.surface);
    return ok;
}
