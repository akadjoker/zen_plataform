#include "scene.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_TOKENS 8

typedef struct
{
    Framebuffer fb;
    bool have_size;
    uint32_t fill;
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

static bool parse_color(const char *s, uint32_t *out)
{
    if (s[0] != '#' || strlen(s) != 7)
        return false;
    uint32_t v = 0;
    for (int i = 1; i < 7; i++)
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
    *out = 0xFF000000u | v;
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

static bool run_line(Scene *sc, char *tokens[], int count, int line, char *err, size_t errcap)
{
    const char *cmd = tokens[0];

    if (strcmp(cmd, "size") == 0)
    {
        int w, h;
        if (count != 3 || !parse_int(tokens[1], &w) || !parse_int(tokens[2], &h) || w <= 0 || h <= 0 || w > 16384 || h > 16384)
            return fail(err, errcap, line, "size needs two positive integers up to 16384", NULL);
        if (sc->have_size)
            return fail(err, errcap, line, "size may appear only once", NULL);
        if (!framebuffer_alloc(&sc->fb, w, h))
            return fail(err, errcap, line, "out of memory", NULL);
        sc->have_size = true;
        return true;
    }
    if (!sc->have_size)
        return fail(err, errcap, line, "'%s' before size", cmd);

    if (strcmp(cmd, "fillStyle") == 0)
    {
        if (count != 2 || !parse_color(tokens[1], &sc->fill))
            return fail(err, errcap, line, "fillStyle needs #rrggbb", NULL);
        return true;
    }
    if (strcmp(cmd, "fillRect") == 0)
    {
        int v[4];
        if (count != 5)
            return fail(err, errcap, line, "fillRect needs x y w h", NULL);
        for (int i = 0; i < 4; i++)
        {
            if (!parse_int(tokens[i + 1], &v[i]))
                return fail(err, errcap, line, "'%s' is not an integer", tokens[i + 1]);
        }
        if (v[2] < 0)
        {
            v[0] += v[2];
            v[2] = -v[2];
        }
        if (v[3] < 0)
        {
            v[1] += v[3];
            v[3] = -v[3];
        }
        if (v[2] > 0 && v[3] > 0)
            draw_fill_rect(&sc->fb, v[0], v[1], v[2], v[3], sc->fill, BLEND_NONE);
        return true;
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
    if (!ok)
    {
        if (sc.have_size)
            framebuffer_free(&sc.fb);
        return false;
    }
    *out = sc.fb;
    return true;
}
