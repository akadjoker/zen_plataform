/*
 * Renders the polygon subset of a scene with Cairo, as a second reference. Used
 * only to measure how far two mature renderers disagree on anti-aliased edges, so
 * the tolerance of the comparisons is chosen from data.
 *
 *   cairo_ref scene.txt out.bmp
 *
 * Commands: size, fillStyle #rrggbb, fillRect, beginPath, moveTo, lineTo,
 * closePath, fill [nonzero|evenodd].
 */
#include "zv_io.h"

#include <cairo.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_color(cairo_t *cr, const char *hex)
{
    unsigned v = (unsigned)strtoul(hex + 1, NULL, 16);
    cairo_set_source_rgb(cr, ((v >> 16) & 0xFF) / 255.0, ((v >> 8) & 0xFF) / 255.0, (v & 0xFF) / 255.0);
}

int main(int argc, char **argv)
{
    if (argc != 3)
    {
        fprintf(stderr, "usage: cairo_ref scene.txt out.bmp\n");
        return 2;
    }
    char *text = zv_io_read_text(argv[1]);
    if (!text)
        return 2;

    cairo_surface_t *surface = NULL;
    cairo_t *cr = NULL;
    int width = 0, height = 0;
    int status = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line && status == 0; line = strtok_r(NULL, "\n", &save))
    {
        char *tok[8];
        int n = 0;
        char *s2 = NULL;
        for (char *t = strtok_r(line, " \t\r", &s2); t && n < 8; t = strtok_r(NULL, " \t\r", &s2))
            tok[n++] = t;
        if (n == 0 || tok[0][0] == '#')
            continue;
        if (strcmp(tok[0], "size") == 0 && n == 3)
        {
            width = atoi(tok[1]);
            height = atoi(tok[2]);
            surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
            cr = cairo_create(surface);
            cairo_set_source_rgb(cr, 0, 0, 0);
        }
        else if (!cr)
            status = 1;
        else if (strcmp(tok[0], "fillStyle") == 0 && n == 2)
            set_color(cr, tok[1]);
        else if (strcmp(tok[0], "fillRect") == 0 && n == 5)
        {
            cairo_new_path(cr);
            cairo_rectangle(cr, atof(tok[1]), atof(tok[2]), atof(tok[3]), atof(tok[4]));
            cairo_set_fill_rule(cr, CAIRO_FILL_RULE_WINDING);
            cairo_fill(cr);
        }
        else if (strcmp(tok[0], "beginPath") == 0)
            cairo_new_path(cr);
        else if (strcmp(tok[0], "moveTo") == 0 && n == 3)
            cairo_move_to(cr, atof(tok[1]), atof(tok[2]));
        else if (strcmp(tok[0], "lineTo") == 0 && n == 3)
            cairo_line_to(cr, atof(tok[1]), atof(tok[2]));
        else if (strcmp(tok[0], "closePath") == 0)
            cairo_close_path(cr);
        else if (strcmp(tok[0], "fill") == 0)
        {
            cairo_set_fill_rule(cr, n == 2 && strcmp(tok[1], "evenodd") == 0 ? CAIRO_FILL_RULE_EVEN_ODD : CAIRO_FILL_RULE_WINDING);
            cairo_fill(cr);
        }
        else
            status = 1;
    }
    zv_io_free(text);
    if (status != 0 || !surface)
    {
        fprintf(stderr, "%s: unsupported or missing command\n", argv[1]);
        return 1;
    }

    cairo_surface_flush(surface);
    ZvBitmap fb;
    if (!zv_bitmap_alloc(&fb, width, height))
        return 2;
    const unsigned char *data = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    for (int y = 0; y < height; y++)
    {
        const uint32_t *row = (const uint32_t *)(data + (size_t)y * (size_t)stride);
        for (int x = 0; x < width; x++)
        {
            uint32_t p = row[x];
            uint32_t a = p >> 24;
            uint32_t r = (p >> 16) & 0xFF, g = (p >> 8) & 0xFF, b = p & 0xFF;
            if (a != 0 && a != 255)
            {
                r = (r * 255 + a / 2) / a;
                g = (g * 255 + a / 2) / a;
                b = (b * 255 + a / 2) / a;
            }
            fb.pixels[y * fb.stride + x] = a ? (a << 24 | (r > 255 ? 255 : r) << 16 | (g > 255 ? 255 : g) << 8 | (b > 255 ? 255 : b)) : 0;
        }
    }
    bool saved = zv_bitmap_save_bmp(&fb, argv[2]);
    zv_bitmap_free(&fb);
    cairo_destroy(cr);
    cairo_surface_destroy(surface);
    return saved ? 0 : 2;
}
