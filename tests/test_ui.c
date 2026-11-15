/*
 * test_ui.c - zen_ui below the widgets: the software renderer against a plain reference, fonts, the text arena of the
 * draw list and the state store. No window is needed for most of it; the draw list tests use the fake backend.
 */
#include "platform.h"
#include "backend_fake.h"
#include "zen_ui.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The generated TTF data of the default font is only linked into the zen_ui library; this test compiles ui_font.c
   itself and loads the TTF from the source tree (ZEN_TEST_FONT) when it needs one. */
const unsigned char ui_font_default_data[1] = {0};
const unsigned int ui_font_default_size = 0;

static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
            g_pass++;                                              \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

static uint32_t g_rng = 0x2545F491u;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}
static int rnd_range(int lo, int hi) /* lo..hi inclusive */
{
    return lo + (int)(rnd() % (uint32_t)(hi - lo + 1));
}

/* ------------------------------------------------------------------ software renderer, text -- */

/* What the text of one command has to look like, written the plain way: every pixel of every glyph is tested against
   the framebuffer and the clip on its own, and blended with the same integer formula as the renderer. */
static void reference_text(Framebuffer *fb, const UiCmd *c)
{
    UiFont *font = (UiFont *)c->u.text.font;
    int pen = c->u.text.x;
    int baseline = c->u.text.y + zui_font_ascent(font);
    int cr = (c->color >> 16) & 0xFF, cg = (c->color >> 8) & 0xFF, cb = c->color & 0xFF, ca = (c->color >> 24) & 0xFF;
    for (int i = 0; i < c->u.text.len; i++)
    {
        UiGlyphInfo g;
        if (!zui_font_glyph(font, (uint8_t)c->u.text.str[i], &g))
            return;
        int gx = pen + g.xoff, gy = baseline + g.yoff;
        for (int y = 0; y < g.h; y++)
            for (int x = 0; x < g.w; x++)
            {
                long long fx = (long long)gx + x, fy = (long long)gy + y;
                if (fx < 0 || fy < 0 || fx >= fb->width || fy >= fb->height)
                    continue;
                if (fx < c->clip.x || fy < c->clip.y || fx >= (long long)c->clip.x + c->clip.w ||
                    fy >= (long long)c->clip.y + c->clip.h)
                    continue;
                int a = g.coverage[y * g.w + x] * ca / 255;
                if (!a)
                    continue;
                uint32_t *p = &fb->pixels[fy * fb->stride + fx];
                int dr = (*p >> 16) & 0xFF, dg = (*p >> 8) & 0xFF, db = *p & 0xFF;
                dr += (cr - dr) * a / 255;
                dg += (cg - dg) * a / 255;
                db += (cb - db) * a / 255;
                *p = 0xFF000000u | ((uint32_t)dr << 16) | ((uint32_t)dg << 8) | (uint32_t)db;
            }
        pen += g.advance;
    }
}

static void test_render_text_matches_reference(UiFont *font, const char *what)
{
    enum { W = 61, H = 43 }; /* odd sizes, a stride that is not a power of two */
    static const char text[] = "Hello, World! 0123 /+-.: gjpqy MWi";
    Framebuffer a, b;
    CHECK(framebuffer_alloc(&a, W, H));
    CHECK(framebuffer_alloc(&b, W, H));
    int bad = 0, cases = 4000;
    for (int n = 0; n < cases; n++)
    {
        for (int i = 0; i < W * H; i++)
            a.pixels[i] = b.pixels[i] = rnd();
        UiCmd c;
        memset(&c, 0, sizeof c);
        c.type = UI_CMD_TEXT;
        /* positions from far left of and above the buffer to past its right and bottom edge, so a glyph starts at a
           negative column on the first row now and then, the case that once formed a pointer before the buffer */
        c.u.text.x = rnd_range(-120, W + 20);
        c.u.text.y = rnd_range(-30, H + 10);
        int start = rnd_range(0, (int)sizeof text - 2);
        c.u.text.str = text + start;
        c.u.text.len = rnd_range(0, (int)sizeof text - 1 - start);
        c.u.text.font = (const struct UiFont *)font;
        c.color = rnd_range(0, 3) == 0 ? (rnd() | 0xFF000000u) : rnd(); /* opaque a quarter of the time */
        switch (rnd_range(0, 3))
        {
        case 0: c.clip = zui_rect(0, 0, W, H); break;
        case 1: c.clip = zui_rect(rnd_range(-10, W), rnd_range(-10, H), rnd_range(-5, W + 30), rnd_range(-5, H + 30)); break;
        case 2: c.clip = zui_rect(rnd_range(-30, 10), rnd_range(-30, 10), INT_MAX, INT_MAX); break; /* "as far as there is" */
        default: c.clip = zui_rect(INT_MIN / 2, INT_MIN / 2, INT_MAX, INT_MAX); break;
        }
        UiDrawList list = {&c, 1, 1, NULL, 0, 0};
        zui_render_software(&a, &list);
        reference_text(&b, &c);
        if (memcmp(a.pixels, b.pixels, sizeof(uint32_t) * W * H) != 0)
            bad++;
    }
    if (bad)
        printf("  %s: %d of %d text commands differ from the reference\n", what, bad, cases);
    CHECK(bad == 0);
    framebuffer_free(&a);
    framebuffer_free(&b);
}

/* ------------------------------------------------------------------ fonts that are not fonts -- */

/* A file that is not a font, or is a font cut short, is refused with NULL: it used to be freed twice (the loader gave the
   bytes to ttf_from_mem, which freed them when stb_truetype said no, and the caller freed them again), and a font whose
   tables run past the end of the data was handed to stb_truetype, which reads wherever the table directory says. */
#define BAD_FONT_FILE "test_ui_bad_font.ttf"

static bool save_bytes(const char *path, const void *data, size_t n)
{
    return io_save_file(path, data, n);
}

static void test_font_load_refuses_bad_files(void)
{
    size_t size = 0;
    uint8_t *good = file_read(ZEN_TEST_FONT, &size);
    CHECK(good != NULL && size > 100000);
    if (!good)
        return;

    /* text, empty, four bytes of a font's magic, a header that promises tables the file does not have */
    static const char text[] = "This is a text file, not a TrueType font. It is long enough to hold a table directory.";
    CHECK(save_bytes(BAD_FONT_FILE, text, sizeof text));
    CHECK(zui_font_load_ttf(BAD_FONT_FILE, 16) == NULL);
    CHECK(save_bytes(BAD_FONT_FILE, "", 0));
    CHECK(zui_font_load_ttf(BAD_FONT_FILE, 16) == NULL);
    CHECK(save_bytes(BAD_FONT_FILE, good, 4));
    CHECK(zui_font_load_ttf(BAD_FONT_FILE, 16) == NULL);
    CHECK(save_bytes(BAD_FONT_FILE, good, 200)); /* the directory is there, the tables are not */
    CHECK(zui_font_load_ttf(BAD_FONT_FILE, 16) == NULL);
    CHECK(save_bytes(BAD_FONT_FILE, good, size / 2)); /* cut in half: glyf and the rest run past the end */
    CHECK(zui_font_load_ttf(BAD_FONT_FILE, 16) == NULL);
    CHECK(save_bytes(BAD_FONT_FILE, good, size - 1)); /* one byte short */
    CHECK(zui_font_load_ttf(BAD_FONT_FILE, 16) == NULL);

    /* the same bytes from memory, in a buffer of exactly their size so that a read past the end is caught */
    uint8_t *cut = malloc(size / 2);
    CHECK(cut != NULL);
    if (cut)
    {
        memcpy(cut, good, size / 2);
        CHECK(zui_font_load_ttf_mem(cut, (int)(size / 2), 16) == NULL);
        free(cut);
    }
    CHECK(zui_font_load_ttf_mem(good, 0, 16) == NULL);
    CHECK(zui_font_load_ttf_mem(NULL, 100, 16) == NULL);

    /* the whole font still loads, from a file and from memory */
    UiFont *f = zui_font_load_ttf(ZEN_TEST_FONT, 16);
    CHECK(f != NULL);
    zui_font_free(f);
    f = zui_font_load_ttf_mem(good, (int)size, 16);
    CHECK(f != NULL);
    zui_font_free(f);
    remove(BAD_FONT_FILE);
    fs_free(good);
}

int main(void)
{
    UiFont *builtin = zui_font_builtin(3);
    CHECK(builtin != NULL);
    test_render_text_matches_reference(builtin, "builtin 5x7 x3");
    UiFont *ttf = zui_font_load_ttf(ZEN_TEST_FONT, 13);
    CHECK(ttf != NULL);
    if (ttf)
        test_render_text_matches_reference(ttf, "DejaVu Sans 13 px");
    zui_font_free(ttf);
    zui_font_free(builtin);

    test_font_load_refuses_bad_files();

    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
