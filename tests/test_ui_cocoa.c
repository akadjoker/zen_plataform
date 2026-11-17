/*
 * test_ui_cocoa.c - one frame of zen_ui through each renderer on macOS: the software one on a RENDER_PIXELS window and the GL
 * one (r2d) on a RENDER_GL window. Each frame is written out as a PNG for a person to look at (right side up? the right
 * colours? the right size?): what the library's buffer holds (or what glReadPixels says), and what the window server shows of
 * the window; the CI job keeps them as artifacts. The test also compares them: the window against the buffer, and the two
 * renderers' pictures of the same UI against each other (their colours, block by block, which a flip or a swap of red and
 * blue would fail). The folder is ZP_TEST_OUT, else the current one. Skips (77) without a graphical session, unless
 * ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

#include "zen_2d.h"
#include "zen_ui.h"
#include <glad/gl.h>

#define W 640
#define H 420

typedef struct
{
    UiContext *ui;
    UiRect panel, tools;
    float volume, gain;
    bool grid, snap;
    char name[48];
    int list_sel, combo_sel;
} Ui;

static void ui_init(Ui *u)
{
    memset(u, 0, sizeof *u);
    u->ui = zui_create();
    zui_set_font(u->ui, zui_font_default(14));
    UiStyle s;
    zui_style_dark(&s);
    zui_set_style(u->ui, &s);
    u->panel = zui_rect(20, 20, 290, 330);
    u->tools = zui_rect(330, 40, 280, 250);
    u->volume = 65.0f;
    u->gain = 30.0f;
    u->grid = true;
    u->list_sel = 1;
    u->combo_sel = 1;
    strcpy(u->name, "untitled");
}

static void ui_build(Ui *u, PlatformWindow *w)
{
    static const char *files[5] = {"main.c", "platform.h", "zen_ui.c", "README.md", "Makefile"};
    static const char *sizes[3] = {"Small", "Medium", "Large"};
    UiContext *ui = u->ui;
    zui_begin(ui, w);
    if (zui_window_begin(ui, "Controls", &u->panel))
    {
        zui_row(ui, 22, 1);
        zui_label(ui, "zen_ui on macOS");
        zui_row(ui, 24, 2);
        zui_checkbox(ui, "Grid", &u->grid);
        zui_checkbox(ui, "Snap", &u->snap);
        zui_row(ui, 24, 1);
        zui_label(ui, "Volume");
        zui_slider(ui, "##vol", &u->volume, 0.0f, 100.0f);
        zui_row(ui, 28, 1);
        zui_progress(ui, u->volume / 100.0f);
        zui_row(ui, 28, 1);
        zui_button(ui, "A button");
        zui_row(ui, 26, 1);
        zui_text_input(ui, "##name", u->name, sizeof u->name);
        zui_row(ui, 24, 1);
        zui_combo(ui, "##size", sizes, 3, &u->combo_sel);
        zui_row(ui, 90, 1);
        zui_listbox(ui, "##files", files, 5, &u->list_sel, 4);
        zui_window_end(ui);
    }
    if (zui_window_begin(ui, "Dial", &u->tools))
    {
        zui_row(ui, 110, 1);
        zui_knob(ui, "Gain", &u->gain, 0.0f, 100.0f);
        zui_row(ui, 24, 1);
        zui_label(ui, "Top left red, top right green");
        zui_window_end(ui);
    }
    zui_end(ui);
}

/* How alike two pictures of one size are: the share of 16x16 blocks whose mean colour is within `tol` of the other's. */
static double block_likeness(const Image *a, const Image *b, int tol)
{
    if (a->w != b->w || a->h != b->h)
        return 0.0;
    int bx = (a->w + 15) / 16, by = (a->h + 15) / 16, like = 0;
    for (int j = 0; j < by; j++)
        for (int i = 0; i < bx; i++)
        {
            long sa[3] = {0}, sb[3] = {0}, n = 0;
            for (int y = j * 16; y < (j + 1) * 16 && y < a->h; y++)
                for (int x = i * 16; x < (i + 1) * 16 && x < a->w; x++, n++)
                    for (int c = 0; c < 3; c++)
                    {
                        sa[c] += (a->px[(size_t)y * a->w + x] >> (8 * c)) & 0xFF;
                        sb[c] += (b->px[(size_t)y * b->w + x] >> (8 * c)) & 0xFF;
                    }
            int worst = 0;
            for (int c = 0; c < 3; c++)
            {
                int d = (int)(sa[c] / n - sb[c] / n);
                worst = abs(d) > worst ? abs(d) : worst;
            }
            like += worst <= tol;
        }
    return (double)like / (double)(bx * by);
}

static void save(const Image *im, const char *name)
{
    char path[512];
    out_path(path, sizeof path, name);
    bool ok = image_save_png(im, path);
    printf("  %s %s (%dx%d)\n", ok ? "wrote" : "could not write", path, im->w, im->h);
    CHECK(ok);
}

/* the whole window's content as one picture of one size */
static bool settle_and_capture(PlatformWindow *w, const Image *reference, int tolerance, Image *out)
{
    double t0 = time_seconds();
    memset(out, 0, sizeof *out);
    long best = -1;
    while (time_seconds() - t0 < 3.0)
    {
        pump_for(w, 60);
        Image got;
        if (!capture_window(w, &got))
            continue;
        long diff = image_diff(reference, &got, tolerance);
        if (best < 0 || diff < best)
        {
            best = diff;
            image_free(out);
            *out = got;
        }
        else
            image_free(&got);
        if (best == 0)
            break;
    }
    return out->px != NULL;
}

static Image software_frame(void)
{
    Image keep = {0};
    WindowConfig cfg = {.title = "zen_ui software", .width = W, .height = H, .x = 40, .y = 40, .render = RENDER_PIXELS, .resizable = true};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return keep;
    Ui u;
    ui_init(&u);
    for (int i = 0; i < 4; i++) /* the layout of the windows settles in a frame or two */
    {
        window_begin_frame(w);
        Framebuffer fb;
        if (!window_lock_pixels(w, &fb))
        {
            CHECK(false);
            break;
        }
        draw_clear(&fb, 0xFF1E1E1E);
        ui_build(&u, w);
        zui_render_software(&fb, zui_draw_list(u.ui));
        window_present_pixels(w);
        time_sleep(20);
    }
    pump_for(w, 200);
    Framebuffer fb;
    if (window_lock_pixels(w, &fb))
    {
        int fw, fh;
        window_get_framebuffer_size(w, &fw, &fh);
        printf("  software: framebuffer %dx%d pixels for a window of %dx%d points\n", fb.width, fb.height, W, H);
        CHECK(fb.width == fw && fb.height == fh);
        keep.w = fb.width;
        keep.h = fb.height;
        keep.px = malloc((size_t)fb.width * (size_t)fb.height * 4);
        for (int y = 0; keep.px && y < fb.height; y++)
            memcpy(keep.px + (size_t)y * fb.width, fb.pixels + (size_t)y * fb.stride, (size_t)fb.width * 4);
        save(&keep, "ui_software_buffer.png");

        Image shown;
        if (keep.px && settle_and_capture(w, &keep, 2, &shown))
        {
            long diff = image_diff(&keep, &shown, 2);
            printf("  software: the window server's picture differs from the buffer in %ld of %d pixels\n", diff, keep.w * keep.h);
            save(&shown, "ui_software_window.png");
            CHECK(diff == 0);
            image_free(&shown);
        }
        else
            NOTE("the window server's image of the software window is not available here");
    }
    zui_destroy(u.ui);
    window_destroy(w);
    return keep;
}

static Image gl_frame(void)
{
    Image keep = {0};
    WindowConfig cfg = {.title = "zen_ui gl", .width = W, .height = H, .x = 40, .y = 40, .render = RENDER_GL, .resizable = true};
    cfg.gl.major = 3;
    cfg.gl.minor = 3;
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
    {
        printf("  GL window: %s\n", platform_get_error());
        return keep;
    }
    window_make_current(w);
    CHECK(r2d_init());
    Ui u;
    ui_init(&u);
    int fw = 0, fh = 0;
    for (int i = 0; i < 4; i++)
    {
        window_begin_frame(w);
        window_get_framebuffer_size(w, &fw, &fh);
        glViewport(0, 0, fw, fh);
        glClearColor(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ui_build(&u, w);
        zui_render_gl(zui_draw_list(u.ui), fw, fh);
        if (i == 3)
        {
            /* what the frame holds, before it is presented: glReadPixels is bottom up and RGBA, the picture is top down */
            unsigned char *rgba = malloc((size_t)fw * (size_t)fh * 4);
            keep.w = fw;
            keep.h = fh;
            keep.px = malloc((size_t)fw * (size_t)fh * 4);
            if (rgba && keep.px)
            {
                glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
                for (int y = 0; y < fh; y++)
                    for (int x = 0; x < fw; x++)
                    {
                        const unsigned char *p = rgba + ((size_t)(fh - 1 - y) * fw + x) * 4;
                        keep.px[(size_t)y * fw + x] = 0xFF000000u | (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
                    }
            }
            free(rgba);
        }
        window_swap(w);
        time_sleep(20);
    }
    CHECK(glGetError() == 0);
    printf("  gl: framebuffer %dx%d pixels for a window of %dx%d points\n", fw, fh, W, H);
    if (keep.px)
    {
        save(&keep, "ui_gl_readback.png");
        Image shown;
        pump_for(w, 200);
        /* the same frame again, so that what is on screen is the last one drawn */
        window_swap(w);
        if (settle_and_capture(w, &keep, 2, &shown))
        {
            long diff = image_diff(&keep, &shown, 3);
            printf("  gl: the window server's picture differs from the read back frame in %ld of %d pixels\n", diff, keep.w * keep.h);
            save(&shown, "ui_gl_window.png");
            CHECK(diff * 100 <= (long)keep.w * keep.h); /* up to 1%: the frame that is shown may be the one before the last */
            image_free(&shown);
        }
        else
            NOTE("the window server's image of the GL window is not available here");
    }
    zui_destroy(u.ui);
    r2d_shutdown();
    window_destroy(w);
    return keep;
}

int main(void)
{
    alarm(110);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    printf("software renderer\n");
    Image sw = software_frame();
    printf("GL renderer\n");
    Image gl = gl_frame();
    if (sw.px && gl.px)
    {
        double like = block_likeness(&sw, &gl, 40);
        printf("  the two renderers' pictures agree in %.0f%% of their 16x16 blocks (mean colour within 40)\n", like * 100.0);
        CHECK(like >= 0.85);
    }
    else
        NOTE("a frame is missing, so the two renderers were not compared");
    image_free(&sw);
    image_free(&gl);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
