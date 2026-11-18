/*
 * test_ui.c - zen_ui below the widgets: the software renderer against a plain reference, fonts, the text arena of the
 * draw list and the state store. No window is needed for most of it; the draw list tests use the fake backend.
 */
#include "platform.h"
#include "backend_fake.h"
#include "zen_ui.h"
#include "ui/ui_utf8.h"

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

/* ------------------------------------------------------------------ the text of a frame -- */

/* The text of a frame's labels is copied into one arena that grows as the frame needs it, and the commands point into it
   ("the str pointers stay valid until the next zui_begin", zen_ui.h). When the arena grew it was reallocated, and the
   commands made before that kept pointing into the block that was freed: the label drawn from them was garbage (or a
   use after free). More than 1 KiB of text in one frame is an ordinary window. */
static PlatformWindow *make_window(void)
{
    WindowConfig cfg = {.title = "test_ui", .width = 400, .height = 300, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_PIXELS};
    return window_create(&cfg);
}

static void test_text_arena_survives_growth(void)
{
    PlatformWindow *w = make_window();
    CHECK(w != NULL);
    if (!w)
        return;
    UiFont *font = zui_font_builtin(1);
    UiContext *ui = zui_create();
    CHECK(font != NULL && ui != NULL);
    if (!font || !ui)
        return;
    zui_set_font(ui, font);

    enum { LABELS = 300 };
    char text[LABELS][48];
    for (int i = 0; i < LABELS; i++)
        snprintf(text[i], sizeof text[i], "Label number %d, with some padding text", i);

    for (int frame = 0; frame < 3; frame++) /* the first frame grows the arena, the next ones reuse it */
    {
        window_begin_frame(w);
        zui_begin(ui, w);
        for (int i = 0; i < LABELS; i++)
            zui_label(ui, text[i]);
        zui_end(ui);
        const UiDrawList *dl = zui_draw_list(ui);
        int texts = 0, wrong = 0, outside = 0;
        for (int i = 0; i < dl->count; i++)
        {
            const UiCmd *c = &dl->cmds[i];
            if (c->type != UI_CMD_TEXT)
                continue;
            /* what each command says, and that it points into the arena the frame ends with (a stale pointer is not there) */
            if (texts < LABELS && (c->u.text.len != (int)strlen(text[texts]) || strncmp(c->u.text.str, text[texts], (size_t)c->u.text.len) != 0))
                wrong++;
            if (c->u.text.str < dl->text_arena || c->u.text.str + c->u.text.len >= dl->text_arena + dl->text_len)
                outside++;
            texts++;
        }
        if (wrong || outside || texts != LABELS)
            printf("  frame %d: %d text commands of %d, %d say the wrong thing, %d point outside the arena\n", frame, texts, LABELS, wrong,
                   outside);
        CHECK(texts == LABELS && wrong == 0 && outside == 0);
    }
    zui_destroy(ui);
    zui_font_free(font);
    window_destroy(w);
}

/* ------------------------------------------------------------------ the state store -- */

/* zui_state_int and the others keep a value for each widget id across frames. The store held 64 entries, and from the 65th
   id on every new one was handed slot 0, the state of whichever widget came first: two tree nodes past the 64th opened
   and closed together, a menu's box height was another widget's. The entries now grow, and the pointers handed out stay
   where they are while more ids are added (a widget holds several at once). */
static void test_state_store_grows(void)
{
    UiContext *ui = zui_create();
    CHECK(ui != NULL);
    if (!ui)
        return;
    enum { N = 1500 };
    int *first = zui_state_int(ui, 12345, 7);
    CHECK(first != NULL && *first == 7);
    int bad = 0;
    for (int i = 0; i < N; i++)
    {
        int *iv = zui_state_int(ui, 100000u + (uint32_t)i * 37u, -1);
        if (*iv != -1)
            bad++; /* a new id starts at its default, not at what another widget left */
        *iv = i;
        float *fv = zui_state_float(ui, 900000u + (uint32_t)i, 0.5f);
        if (*fv != 0.5f)
            bad++;
        *fv = (float)i * 0.25f;
        zui_state_set_ptr(ui, 5000000u + (uint32_t)i, (void *)(uintptr_t)(0x1000 + i));
    }
    for (int i = 0; i < N; i++)
    {
        if (*zui_state_int(ui, 100000u + (uint32_t)i * 37u, -1) != i)
            bad++;
        if (*zui_state_float(ui, 900000u + (uint32_t)i, 0.5f) != (float)i * 0.25f)
            bad++;
        if (*(void **)zui_state_ptr(ui, 5000000u + (uint32_t)i) != (void *)(uintptr_t)(0x1000 + i))
            bad++;
    }
    if (bad)
        printf("  %d of %d state values are wrong\n", bad, 3 * N);
    CHECK(bad == 0);
    CHECK(*first == 7 && zui_state_int(ui, 12345, 0) == first); /* the entry did not move while 4500 more were added */
    *first = 99;
    CHECK(*zui_state_int(ui, 12345, 0) == 99);
    /* ids that are special for a hash table: 0 and the largest */
    *zui_state_int(ui, 0, 1) = 10;
    *zui_state_int(ui, 0xFFFFFFFFu, 2) = 20;
    CHECK(*zui_state_int(ui, 0, 1) == 10 && *zui_state_int(ui, 0xFFFFFFFFu, 2) == 20);
    zui_destroy(ui);
}

/* ------------------------------------------------------------------ every command, any geometry -- */

/* The renderer is handed rectangles, lines, triangles, icons, gradients and images by programs that compute them: sizes of
   zero or below, far outside the framebuffer, clips that are empty or reversed. Every command type with such numbers,
   into a small framebuffer with a border of canary pixels around it: nothing is written outside the framebuffer, nothing
   the sanitizers can see (an overflow, a bad index) happens, and the border stays as it was. */
static void test_render_any_geometry(void)
{
    enum { W = 37, H = 29, PAD = 64 };
    const size_t total = (size_t)(W + 2 * PAD) * (size_t)(H + 2 * PAD);
    uint32_t *mem = malloc(total * sizeof *mem);
    CHECK(mem != NULL);
    if (!mem)
        return;
    for (size_t i = 0; i < total; i++)
        mem[i] = 0xCAFE0000u | (uint32_t)(i & 0xFFFF);
    Framebuffer fb;
    memset(&fb, 0, sizeof fb);
    fb.width = W;
    fb.height = H;
    fb.stride = W + 2 * PAD;
    fb.pixels = mem + (size_t)PAD * (size_t)fb.stride + PAD;
    Framebuffer src;
    CHECK(framebuffer_alloc(&src, 7, 5));
    for (int i = 0; i < 35; i++)
        src.pixels[i] = rnd();
    UiFont *font = zui_font_builtin(2);
    static const char text[] = "Mixed 123 text: +-/.";

    static const int extremes[] = {0, 1, -1, 2, -2, 5, W - 1, W, W + 1, -W, 1000, -1000, 100000, -100000, 1 << 20, -(1 << 20)};
    const int ne = (int)(sizeof extremes / sizeof extremes[0]);
    int bad = 0;
    for (int n = 0; n < 6000; n++)
    {
        UiCmd c;
        memset(&c, 0, sizeof c);
        c.type = (UiCmdType)rnd_range(UI_CMD_CLIP, UI_CMD_GRADIENT);
        c.color = rnd();
        c.clip = rnd_range(0, 2) == 0 ? zui_rect(0, 0, W, H)
                                      : zui_rect(rnd_range(-80, 80), rnd_range(-80, 80), rnd_range(-10, 120), rnd_range(-10, 120));
#define G() (rnd_range(0, 2) == 0 ? extremes[rnd_range(0, ne - 1)] : rnd_range(-60, 90))
#define SZ() (rnd_range(0, 2) == 0 ? extremes[rnd_range(0, ne - 1)] : rnd_range(-5, 70))
        switch (c.type)
        {
        case UI_CMD_RECT:
            c.u.rect.rect = zui_rect(G(), G(), SZ(), SZ());
            c.u.rect.thickness = rnd_range(0, 1);
            c.u.rect.rounding = rnd_range(0, 3) == 0 ? extremes[rnd_range(0, ne - 1)] : rnd_range(-3, 30);
            break;
        case UI_CMD_LINE:
            c.u.line.x0 = rnd_range(-60, 90); c.u.line.y0 = rnd_range(-60, 90); /* coordinates near INT_MAX overflow in draw_line and draw_round_rect, and a line that long takes seconds: docs/SUSPEITAS.md */
            c.u.line.x1 = rnd_range(-60, 90); c.u.line.y1 = rnd_range(-60, 90);
            break;
        case UI_CMD_TRI:
            c.u.tri.x0 = rnd_range(-60, 90); c.u.tri.y0 = rnd_range(-60, 90); c.u.tri.x1 = rnd_range(-60, 90);
            c.u.tri.y1 = rnd_range(-60, 90); c.u.tri.x2 = rnd_range(-60, 90); c.u.tri.y2 = rnd_range(-60, 90);
            break;
        case UI_CMD_IMAGE:
            c.u.image.src = rnd_range(0, 9) == 0 ? NULL : &src;
            c.u.image.dst = zui_rect(rnd_range(-60, 90), rnd_range(-60, 90), rnd_range(-5, 120), rnd_range(-5, 120));
            break;
        case UI_CMD_TEXT:
            c.u.text.x = G(); c.u.text.y = G();
            c.u.text.str = text; c.u.text.len = rnd_range(0, (int)sizeof text - 1);
            c.u.text.font = rnd_range(0, 9) == 0 ? NULL : (const struct UiFont *)font;
            break;
        case UI_CMD_ICON:
            c.u.icon.box = zui_rect(rnd_range(-40, 70), rnd_range(-40, 70), rnd_range(-5, 60), rnd_range(-5, 60));
            c.u.icon.icon = rnd_range(-1, 12);
            break;
        case UI_CMD_GRADIENT:
            c.u.gradient.rect = zui_rect(rnd_range(-60, 90), rnd_range(-60, 90), rnd_range(-5, 120), rnd_range(0, 80));
            c.u.gradient.top = rnd();
            c.u.gradient.bottom = rnd();
            break;
        default:
            break;
        }
        UiDrawList list = {&c, 1, 1, NULL, 0, 0};
        zui_render_software(&fb, &list);
    }
    /* the canary border: the pixels outside the W by H area are what they were */
    for (int y = 0; y < H + 2 * PAD; y++)
        for (int x = 0; x < W + 2 * PAD; x++)
        {
            if (x >= PAD && x < PAD + W && y >= PAD && y < PAD + H)
                continue;
            size_t i = (size_t)y * (size_t)(W + 2 * PAD) + (size_t)x;
            if (mem[i] != (0xCAFE0000u | (uint32_t)(i & 0xFFFF)))
                bad++;
        }
    if (bad)
        printf("  %d pixels outside the framebuffer were written\n", bad);
    CHECK(bad == 0);
    framebuffer_free(&src);
    zui_font_free(font);
    free(mem);
}

/* ------------------------------------------------------------------ utf-8 -- */

/* Every code point that can be encoded comes back from the decoder whole, from a buffer of exactly its length (a read
   past the end is the sanitizer's to find); bytes that are not UTF-8 give U+FFFD and advance at least one byte, so a run of
   text always ends. */
static void test_utf8(void)
{
    int bad = 0;
    for (uint32_t cp = 0; cp <= 0x10FFFF; cp += (cp < 0x800 ? 1 : 7))
    {
        if (cp >= 0xD800 && cp <= 0xDFFF)
            continue;
        char enc[4];
        int n = ui_utf8_encode(cp, enc);
        char *exact = malloc((size_t)n);
        memcpy(exact, enc, (size_t)n);
        uint32_t back = 0;
        int used = ui_utf8_decode(exact, n, &back);
        if (used != n || back != cp)
            bad++;
        if (n > 1) /* one byte short: a replacement for the part, never a read past the end */
        {
            used = ui_utf8_decode(exact, n - 1, &back);
            if (used != 1 || back != UI_UTF8_REPLACEMENT)
                bad++;
        }
        free(exact);
    }
    CHECK(bad == 0);

    static const struct
    {
        const char *bytes;
        int len;
        int used;
    } broken[] = {
        {"\x80", 1, 1},              /* a continuation with no start */
        {"\xC0\x80", 2, 2},          /* overlong NUL */
        {"\xC1\xBF", 2, 2},          /* overlong 7F */
        {"\xE0\x80\x80", 3, 3},      /* overlong */
        {"\xED\xA0\x80", 3, 3},      /* a surrogate */
        {"\xF4\x90\x80\x80", 4, 4},  /* above U+10FFFF */
        {"\xF8\x88\x80\x80\x80", 5, 1}, /* five bytes: not UTF-8 */
        {"\xFF", 1, 1},
        {"\xE2\x82", 2, 1},          /* cut short */
        {"\xE2\x28\xA1", 3, 1},      /* a start and a byte that is not a continuation */
    };
    for (size_t i = 0; i < sizeof broken / sizeof broken[0]; i++)
    {
        uint32_t cp = 0;
        int used = ui_utf8_decode(broken[i].bytes, broken[i].len, &cp);
        if (cp != UI_UTF8_REPLACEMENT || used != broken[i].used)
            printf("  broken sequence %zu: U+%04X, %d bytes used (wanted U+FFFD, %d)\n", i, (unsigned)cp, used, broken[i].used);
        CHECK(cp == UI_UTF8_REPLACEMENT && used == broken[i].used);
    }
    uint32_t cp = 7;
    CHECK(ui_utf8_decode("", 0, &cp) == 0 && cp == 0);
    CHECK(ui_utf8_count("a\xC3\xA9\xE2\x82\xAC", 6) == 3);
    CHECK(ui_utf8_count("\xFF\xFF", 2) == 2);
}

/* ------------------------------------------------------------------ text input -- */

static char g_input[64];

static bool input_frame(PlatformWindow *w, UiContext *ui)
{
    window_begin_frame(w);
    zui_begin(ui, w);
    zui_row(ui, 28, 1);
    bool changed = zui_text_input(ui, "##t", g_input, (int)sizeof g_input);
    zui_end(ui);
    return changed;
}

static void press_key(PlatformWindow *w, UiContext *ui, int key)
{
    fake_key(w, key, true, false);
    input_frame(w, ui);
    fake_key(w, key, false, false);
    input_frame(w, ui);
}

/* A buffer that is not valid UTF-8 (a Latin-1 file name in the dialog's name field, a string cut in the middle of a
   character) has a byte that looks like the start of a longer character than there are bytes left. Delete then moved
   memory by a negative length (a huge one), and Right put the caret past the end for the next key of the same frame. */
static void test_text_input_invalid_utf8(void)
{
    PlatformWindow *w = make_window();
    UiContext *ui = zui_create();
    UiFont *font = zui_font_builtin(1);
    CHECK(w && ui && font);
    if (!w || !ui || !font)
        return;
    zui_set_font(ui, font);

    /* focus the field with a click in it (the window is 400 wide, the field spans it, the first row is 10 to 38) */
    snprintf(g_input, sizeof g_input, "caf\xE9");
    fake_mouse_move(w, 300, 24);
    input_frame(w, ui);
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_move(w, 300, 24);
    input_frame(w, ui);
    fake_mouse_button(w, MOUSE_LEFT, false);
    input_frame(w, ui);

    press_key(w, ui, KEY_END);
    press_key(w, ui, KEY_LEFT);   /* the caret from the end to before the last byte, 0xE9: a 3-byte start with no more bytes */
    press_key(w, ui, KEY_DELETE); /* deleted the byte, not the 3 it claims to start */
    CHECK(strcmp(g_input, "caf") == 0);

    snprintf(g_input, sizeof g_input, "a\xE2");
    press_key(w, ui, KEY_HOME);
    press_key(w, ui, KEY_RIGHT);  /* over the "a" */
    press_key(w, ui, KEY_RIGHT);  /* over the cut character: the end, not past it */
    press_key(w, ui, KEY_BACKSPACE);
    CHECK(strcmp(g_input, "a") == 0);
    press_key(w, ui, KEY_DELETE); /* at the end: nothing to delete */
    CHECK(strcmp(g_input, "a") == 0);

    /* a string that does start well still edits as one character: backspace takes the whole of "é" */
    snprintf(g_input, sizeof g_input, "x\xC3\xA9");
    press_key(w, ui, KEY_END);
    press_key(w, ui, KEY_BACKSPACE);
    CHECK(strcmp(g_input, "x") == 0);

    zui_destroy(ui);
    zui_font_free(font);
    window_destroy(w);
}

/* ------------------------------------------------------------------ the file dialog -- */

/* One frame of the dialog, over a window the test feeds with the fake backend. Returns what zui_dialog says. */
static UiDialogState dialog_frame(PlatformWindow *w, UiContext *ui, char *out, int cap)
{
    window_begin_frame(w);
    zui_begin(ui, w);
    UiDialogState st = zui_dialog(ui, out, cap);
    zui_end(ui);
    return st;
}

/* the text commands of the last frame, in drawing order */
static int text_pos(UiContext *ui, const char *text, int *x, int *y)
{
    const UiDrawList *dl = zui_draw_list(ui);
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].type == UI_CMD_TEXT && dl->cmds[i].u.text.len == (int)strlen(text) &&
            strncmp(dl->cmds[i].u.text.str, text, (size_t)dl->cmds[i].u.text.len) == 0)
        {
            *x = dl->cmds[i].u.text.x + 4;
            *y = dl->cmds[i].u.text.y + 4;
            return 1;
        }
    return 0;
}

static int text_index(UiContext *ui, const char *text) /* order among the text commands, -1 when it is not drawn */
{
    const UiDrawList *dl = zui_draw_list(ui);
    int n = 0;
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].type == UI_CMD_TEXT)
        {
            if (dl->cmds[i].u.text.len == (int)strlen(text) && strncmp(dl->cmds[i].u.text.str, text, (size_t)dl->cmds[i].u.text.len) == 0)
                return n;
            n++;
        }
    return -1;
}

/* 1 when a text command with this string is drawn where it can be seen: the clip it carries is not empty and holds the point
   text_pos gives (a command whose clip is empty is in the list and never reaches the screen) */
static int text_visible(UiContext *ui, const char *text)
{
    const UiDrawList *dl = zui_draw_list(ui);
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].type == UI_CMD_TEXT && dl->cmds[i].u.text.len == (int)strlen(text) &&
            strncmp(dl->cmds[i].u.text.str, text, (size_t)dl->cmds[i].u.text.len) == 0)
        {
            const UiRect c = dl->cmds[i].clip;
            const int x = dl->cmds[i].u.text.x + 4, y = dl->cmds[i].u.text.y + 4;
            return c.w > 0 && c.h > 0 && x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h;
        }
    return 0;
}

static void click_at(PlatformWindow *w, UiContext *ui, int x, int y, UiDialogState *st, char *out, int cap)
{
    fake_mouse_move(w, x, y);
    dialog_frame(w, ui, out, cap); /* hover */
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_move(w, x, y);
    dialog_frame(w, ui, out, cap); /* press */
    fake_mouse_button(w, MOUSE_LEFT, false);
    fake_mouse_move(w, x, y);
    *st = dialog_frame(w, ui, out, cap); /* release: the click */
}

#define DLG_ROOT "test_ui_dialog_tmp"

static void test_dialog_flow(void)
{
    remove(DLG_ROOT "/sub/inner.txt");
    remove(DLG_ROOT "/sub");
    CHECK(dir_make(DLG_ROOT "/sub"));
    CHECK(io_save_file(DLG_ROOT "/b.txt", "bbbb", 4));
    CHECK(io_save_file(DLG_ROOT "/A.txt", "a", 1));
    CHECK(io_save_file(DLG_ROOT "/c.png", "png", 3));
    CHECK(io_save_file(DLG_ROOT "/sub/inner.txt", "i", 1));

    PlatformWindow *w = make_window();
    CHECK(w != NULL);
    UiContext *ui = zui_create();
    UiFont *font = zui_font_builtin(1);
    CHECK(ui && font && w);
    if (!ui || !font || !w)
        return;
    zui_set_font(ui, font);
    char out[1024] = "";
    UiDialogState st;

    /* open: the folder's entries, the folder first, then by name without regard to the case, and the filter hides c.png */
    UiDialogConfig cfg = {.kind = ZUI_DIALOG_OPEN_FILE, .title = "Open", .filter = ".txt", .start_dir = DLG_ROOT};
    zui_dialog_open(ui, &cfg);
    st = dialog_frame(w, ui, out, sizeof out);
    CHECK(st == ZUI_DIALOG_RUNNING);
    int sub = text_index(ui, "sub"), a = text_index(ui, "A.txt"), b = text_index(ui, "b.txt");
    CHECK(sub >= 0 && a > sub && b > a);
    CHECK(text_index(ui, "c.png") < 0);

    /* The entries are on the screen, not only in the list of commands. The file area is a group, and a group cuts its clip by the
       one in force: the dialog set the clips of its sidebar (and of the header of the details view) by hand and left the last
       one in force, so the group's clip came out empty, and every view showed a file list with no files in it. The three views. */
    int vx, vy;
    CHECK(text_visible(ui, "sub") && text_visible(ui, "A.txt") && text_visible(ui, "b.txt"));
    CHECK(text_pos(ui, "Detalhes", &vx, &vy));
    click_at(w, ui, vx, vy, &st, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_visible(ui, "sub") && text_visible(ui, "A.txt") && text_visible(ui, "b.txt"));
    CHECK(text_pos(ui, "Icones", &vx, &vy));
    click_at(w, ui, vx, vy, &st, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_visible(ui, "sub") && text_visible(ui, "A.txt")); /* (a 400 x 300 window: the third cell is under the fold, as it should be) */
    CHECK(text_pos(ui, "Lista", &vx, &vy));
    click_at(w, ui, vx, vy, &st, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_visible(ui, "sub") && text_visible(ui, "A.txt") && text_visible(ui, "b.txt"));

    /* a click on the name selects it, a second click on it soon after opens it: the path comes back */
    int x, y;
    CHECK(text_pos(ui, "b.txt", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    CHECK(st == ZUI_DIALOG_RUNNING);
    click_at(w, ui, x, y, &st, out, sizeof out);
    CHECK(st == ZUI_DIALOG_OK);
    CHECK(strcmp(out, DLG_ROOT "/b.txt") == 0);

    /* Escape cancels */
    zui_dialog_open(ui, &cfg);
    dialog_frame(w, ui, out, sizeof out);
    fake_key(w, KEY_ESCAPE, true, false);
    st = dialog_frame(w, ui, out, sizeof out);
    CHECK(st == ZUI_DIALOG_CANCEL);

    /* a folder opens on a double click, and "Subir" goes back up */
    fake_key(w, KEY_ESCAPE, false, false);
    zui_dialog_open(ui, &cfg);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_pos(ui, "sub", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    click_at(w, ui, x, y, &st, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_index(ui, "inner.txt") >= 0);
    CHECK(text_index(ui, "b.txt") < 0);
    CHECK(text_pos(ui, "Subir", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_index(ui, "b.txt") >= 0);

    /* "Abrir" with nothing chosen does not answer OK with the path of the folder (an open that opens a folder as if it were
       the file), and with a file chosen it does; a save with no name is no save either */
    CHECK(text_pos(ui, "Abrir", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    if (st != ZUI_DIALOG_RUNNING)
        printf("  Abrir with nothing chosen answered %d with \"%s\"\n", (int)st, out);
    CHECK(st == ZUI_DIALOG_RUNNING);
    CHECK(text_pos(ui, "A.txt", &x, &y));
    int fx = x, fy = y;
    click_at(w, ui, fx, fy, &st, out, sizeof out);
    CHECK(st == ZUI_DIALOG_RUNNING);
    CHECK(text_pos(ui, "Abrir", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    CHECK(st == ZUI_DIALOG_OK && strcmp(out, DLG_ROOT "/A.txt") == 0);

    UiDialogConfig save = {.kind = ZUI_DIALOG_SAVE_FILE, .title = "Save", .filter = NULL, .start_dir = DLG_ROOT};
    zui_dialog_open(ui, &save);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_pos(ui, "Guardar", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    if (st != ZUI_DIALOG_RUNNING)
        printf("  Guardar with no name answered %d with \"%s\"\n", (int)st, out);
    CHECK(st == ZUI_DIALOG_RUNNING);

    /* a folder can be picked as it stands */
    UiDialogConfig pick = {.kind = ZUI_DIALOG_PICK_FOLDER, .title = "Pick", .filter = NULL, .start_dir = DLG_ROOT};
    zui_dialog_open(ui, &pick);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_pos(ui, "Escolher", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    CHECK(st == ZUI_DIALOG_OK && strcmp(out, DLG_ROOT) == 0);

    /* Cancelar */
    zui_dialog_open(ui, &cfg);
    dialog_frame(w, ui, out, sizeof out);
    CHECK(text_pos(ui, "Cancelar", &x, &y));
    click_at(w, ui, x, y, &st, out, sizeof out);
    CHECK(st == ZUI_DIALOG_CANCEL);

    zui_destroy(ui);
    zui_font_free(font);
    window_destroy(w);
    remove(DLG_ROOT "/sub/inner.txt");
    remove(DLG_ROOT "/sub");
    remove(DLG_ROOT "/b.txt");
    remove(DLG_ROOT "/A.txt");
    remove(DLG_ROOT "/c.png");
    remove(DLG_ROOT);
}

int main(void)
{
    if (!platform_init())
    {
        printf("platform_init failed\n");
        return 1;
    }

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
    test_text_arena_survives_growth();
    test_state_store_grows();
    test_dialog_flow();
    test_utf8();
    test_render_any_geometry();
    test_text_input_invalid_utf8();

    platform_shutdown();
    printf("%d checks passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
