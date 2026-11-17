/*
 * test_pixels_cocoa.c - RENDER_PIXELS on macOS: the back buffer window_lock_pixels hands out (size in pixels, format, stride,
 * kept between frames) and what window_present_pixels puts on the window, read back two ways that do not go through the
 * backend's own code: the view drawn into a bitmap by AppKit (cacheDisplayInRect), and the window as the window server
 * composites it (CGWindowListCreateImage; it needs the session to let a process read its own windows, and the test says so
 * when it does not). Skips (77) without a graphical session, unless ZP_REQUIRE_DISPLAY is set.
 */
#include "cocoa_test_util.h"

static PlatformWindow *make(int w, int h)
{
    WindowConfig cfg = {.title = "test_pixels_cocoa", .width = w, .height = h, .x = 60, .y = 60, .render = RENDER_PIXELS, .resizable = true};
    PlatformWindow *win = window_create(&cfg);
    if (win)
        pump_for(win, 250);
    return win;
}

/* The buffer, as an Image that shares its memory. */
static Image as_image(const Framebuffer *fb)
{
    return (Image){.w = fb->width, .h = fb->height, .px = fb->pixels};
}

/* Four quadrants of known colours with a one-pixel white frame and a diagonal, so that a flip, a swap of the colour channels,
   a shift or a scaling all show. */
static void paint(Framebuffer *fb)
{
    uint32_t colours[4] = {0xFFFF0000u, 0xFF00FF00u, 0xFF0000FFu, 0xFFFFFF00u}; /* top left, top right, bottom left, bottom right */
    for (int y = 0; y < fb->height; y++)
        for (int x = 0; x < fb->width; x++)
        {
            uint32_t c = colours[(y >= fb->height / 2) * 2 + (x >= fb->width / 2)];
            if (x == 0 || y == 0 || x == fb->width - 1 || y == fb->height - 1 || x == y)
                c = 0xFFFFFFFFu;
            fb->pixels[(size_t)y * (size_t)fb->stride + (size_t)x] = c;
        }
}

/* What was presented must be what the window shows: the image read back, against the buffer, up to `tolerance`. The window
   server composites a moment after the present, so the read is tried again for a little while. */
static bool shows(PlatformWindow *w, const Framebuffer *fb, bool by_window_server, int tolerance, long *worst)
{
    Image want = as_image(fb);
    double t0 = time_seconds();
    long diff = -1;
    do
    {
        Image got;
        bool ok = by_window_server ? capture_window(w, &got) : capture_view(w, &got);
        if (ok)
        {
            diff = image_diff(&want, &got, tolerance);
            if (diff == 0 || time_seconds() - t0 > 3.0)
            {
                image_free(&got);
                break;
            }
            image_free(&got);
        }
        else if (time_seconds() - t0 > 1.0)
            break;
        pump_for(w, 50);
    } while (time_seconds() - t0 < 3.0);
    if (worst)
        *worst = diff;
    return diff == 0;
}

static void test_surface(PlatformWindow *w)
{
    int ww, wh, fw, fh;
    window_get_size(w, &ww, &wh);
    window_get_framebuffer_size(w, &fw, &fh);
    float scale = window_content_scale(w);
    Framebuffer fb;
    CHECK(window_lock_pixels(w, &fb));
    printf("  size %dx%d points, framebuffer %dx%d pixels (scale %.2f); buffer %dx%d, stride %d\n", ww, wh, fw, fh, (double)scale,
           fb.width, fb.height, fb.stride);
    CHECK(fb.pixels != NULL);
    CHECK(fb.width == fw && fb.height == fh); /* the buffer is in pixels: a scale 2 window has four times the points' worth */
    CHECK(fb.stride >= fb.width);
    CHECK(fb.width == (int)lround((double)ww * (double)scale));
    CHECK(fb.height == (int)lround((double)wh * (double)scale));
    /* it starts black (zeroed), and what is written stays between frames */
    bool zero = true;
    for (int i = 0; i < fb.width && zero; i++)
        zero = fb.pixels[i] == 0;
    CHECK(zero);
    fb.pixels[5] = 0xFF123456u;
    Framebuffer again;
    CHECK(window_lock_pixels(w, &again));
    CHECK(again.pixels == fb.pixels && again.width == fb.width && again.height == fb.height);
    CHECK(again.pixels[5] == 0xFF123456u);

    /* the library's own drawing primitives work on it, with the format it has: 0xAARRGGBB */
    draw_clear(&again, 0xFF102030u);
    CHECK(draw_get_pixel(&again, 3, 3) == 0xFF102030u);
    window_present_pixels(w);
    window_present_pixels(w); /* twice in a row is fine */
}

static void test_content(PlatformWindow *w)
{
    Framebuffer fb;
    CHECK(window_lock_pixels(w, &fb));
    paint(&fb);
    window_present_pixels(w);
    long worst;
    bool by_view = shows(w, &fb, false, 1, &worst);
    printf("  the view drawn into a bitmap: %ld pixels differ from the buffer\n", worst);
    CHECK(by_view);

    Image probe;
    if (!capture_window(w, &probe))
    {
        NOTE("the window server's image of the window is not available here (CGWindowListCreateImage gave nothing), "
             "what the window server shows was not compared");
        return;
    }
    image_free(&probe);
    bool by_server = shows(w, &fb, true, 2, &worst);
    printf("  the window as the window server shows it: %ld pixels differ from the buffer (tolerance 2)\n", worst);
    CHECK(by_server);

    /* the colour of each quadrant, in the words of the channels: no red and blue swapped, no flip */
    Image got;
    if (capture_window(w, &got))
    {
        uint32_t tl = got.px[(size_t)(got.h / 4) * got.w + got.w / 4], tr = got.px[(size_t)(got.h / 4) * got.w + 3 * got.w / 4];
        uint32_t bl = got.px[(size_t)(3 * got.h / 4) * got.w + got.w / 4], br = got.px[(size_t)(3 * got.h / 4) * got.w + 3 * got.w / 4];
        printf("  quadrants as the window server shows them: %08X %08X %08X %08X (red, green, blue, yellow expected)\n", tl, tr, bl, br);
        CHECK(color_distance(tl, 0xFFFF0000u) <= 2 && color_distance(tr, 0xFF00FF00u) <= 2);
        CHECK(color_distance(bl, 0xFF0000FFu) <= 2 && color_distance(br, 0xFFFFFF00u) <= 2);
        image_free(&got);
    }
}

/* The alpha of a pixel is not used: the window is opaque, as an XImage and a DIB are. */
static void test_alpha_ignored(PlatformWindow *w)
{
    Framebuffer fb;
    CHECK(window_lock_pixels(w, &fb));
    draw_clear(&fb, 0x00FF0000u); /* alpha 0, red */
    window_present_pixels(w);
    Image got;
    if (capture_view(w, &got))
    {
        CHECK(color_distance(got.px[got.w * 10 + 10], 0xFFFF0000u) <= 1);
        image_free(&got);
    }
}

/* A window that is drawn again by the system (uncovered, moved to another screen) is painted from the buffer as it is, not
   from the last frame: what was written and not yet presented shows. */
static void test_repaint_from_buffer(PlatformWindow *w)
{
    Framebuffer fb;
    CHECK(window_lock_pixels(w, &fb));
    draw_clear(&fb, 0xFF202020u);
    window_present_pixels(w);
    draw_clear(&fb, 0xFF804020u); /* written, not presented */
    msg_void(ns_view(w), "display"); /* AppKit's own request for a repaint */
    Image got;
    if (capture_view(w, &got))
    {
        CHECK(color_distance(got.px[got.w * 10 + 10], 0xFF804020u) <= 1);
        image_free(&got);
    }
}

static void test_resize(PlatformWindow *w)
{
    static const int sizes[][2] = {{300, 220}, {180, 140}, {260, 260}};
    for (int i = 0; i < 3; i++)
    {
        window_set_size(w, sizes[i][0], sizes[i][1]);
        int fw = 0, fh = 0;
        double t0 = time_seconds();
        do
        {
            pump_for(w, 20);
            window_get_framebuffer_size(w, &fw, &fh);
        } while (time_seconds() - t0 < 2.0 && ((int)lround((double)window_content_scale(w) * sizes[i][0]) != fw));
        Framebuffer fb;
        CHECK(window_lock_pixels(w, &fb));
        CHECK(fb.width == fw && fb.height == fh);
        bool black = true;
        for (size_t k = 0; k < (size_t)fb.width * (size_t)fb.height && black; k++)
            black = fb.pixels[k] == 0;
        CHECK(black); /* a buffer of a new size starts black */
        paint(&fb);
        window_present_pixels(w);
        long worst;
        bool ok = shows(w, &fb, false, 1, &worst);
        printf("  resized to %dx%d points: buffer %dx%d, %ld pixels differ\n", sizes[i][0], sizes[i][1], fb.width, fb.height, worst);
        CHECK(ok);
        Image probe;
        if (capture_window(w, &probe))
        {
            image_free(&probe);
            CHECK(shows(w, &fb, true, 2, &worst));
        }
    }
}

static void test_not_pixels(void)
{
    WindowConfig gl = {.title = "gl", .width = 64, .height = 64, .render = RENDER_GL};
    PlatformWindow *g = window_create(&gl);
    if (g)
    {
        Framebuffer fb;
        CHECK(!window_lock_pixels(g, &fb)); /* a GL window has no buffer of pixels */
        window_present_pixels(g);           /* and presenting one does nothing */
        window_destroy(g);
    }
    WindowConfig bare = {.title = "bare", .width = 64, .height = 64, .render = RENDER_VULKAN};
    PlatformWindow *b = window_create(&bare);
    CHECK(b != NULL);
    if (b)
    {
        Framebuffer fb;
        CHECK(!window_lock_pixels(b, &fb));
        window_present_pixels(b);
        window_destroy(b);
    }
    /* presenting before the first lock has nothing to present */
    PlatformWindow *p = make(64, 64);
    CHECK(p != NULL);
    if (p)
    {
        window_present_pixels(p);
        window_destroy(p);
    }
}

static void test_hidden_and_minimized(PlatformWindow *w)
{
    Framebuffer fb;
    CHECK(window_lock_pixels(w, &fb));
    window_hide(w);
    paint(&fb);
    window_present_pixels(w); /* nothing is drawn for a window that is not on screen, and nothing breaks */
    window_show(w);
    pump_for(w, 200);
    window_present_pixels(w);
    CHECK(window_lock_pixels(w, &fb));
}

static void test_cycles(PlatformWindow *w)
{
    enum { WARM = 20, ROUNDS = 100 };
    id app = ns_app();
    unsigned long windows_before = msg_ulong(msg_id(app, "windows"), "count");
    for (int i = 0; i < WARM + ROUNDS; i++)
    {
        WindowConfig cfg = {.title = "cycle", .width = 120 + i % 5, .height = 90, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                            .render = RENDER_PIXELS, .resizable = true};
        PlatformWindow *c = window_create(&cfg);
        CHECK(c != NULL);
        if (!c)
            continue;
        Framebuffer fb;
        if (window_lock_pixels(c, &fb))
        {
            draw_clear(&fb, 0xFF000000u | (uint32_t)i * 2654435u);
            window_present_pixels(c);
        }
        if (i % 10 == 0)
            pump_for(c, 10);
        window_destroy(c);
    }
    pump_for(w, 100);
    unsigned long windows_after = msg_ulong(msg_id(app, "windows"), "count");
    printf("  %d create/lock/present/destroy rounds: NSApp windows %lu -> %lu\n", WARM + ROUNDS, windows_before, windows_after);
    CHECK(windows_after == windows_before);
}

int main(void)
{
    alarm(110);
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    PlatformWindow *w = make(160, 120);
    if (!w)
    {
        printf("%s: %s\n", ZP_REQUIRE ? "FAIL" : "skip", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    printf("surface\n");
    test_surface(w);
    printf("content\n");
    test_content(w);
    printf("alpha is ignored\n");
    test_alpha_ignored(w);
    printf("repaint from the buffer\n");
    test_repaint_from_buffer(w);
    printf("resize\n");
    test_resize(w);
    printf("hidden\n");
    test_hidden_and_minimized(w);
    printf("not a pixel window\n");
    test_not_pixels();
    printf("create and destroy\n");
    test_cycles(w);

    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
