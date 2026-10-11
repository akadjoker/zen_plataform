/*
 * test_gl_wayland.c - the GL context and the window size on a real Wayland
 * compositor. Skips (77) without one.
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>

#define GL_VERSION 0x1F02
#define GL_SAMPLE_BUFFERS 0x80A8
#define GL_SAMPLES 0x80A9
#define GL_CONTEXT_FLAGS 0x821E
#define GL_CONTEXT_PROFILE_MASK 0x9126
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_CONTEXT_CORE_PROFILE_BIT 0x1
#define GL_CONTEXT_COMPATIBILITY_PROFILE_BIT 0x2
#define GL_CONTEXT_FLAG_DEBUG_BIT 0x2
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401

typedef void (*GetIntegerv)(unsigned name, int *out);
typedef const unsigned char *(*GetString)(unsigned name);
typedef void (*ClearColor)(float r, float g, float b, float a);
typedef void (*Clear)(unsigned mask);
typedef void (*ReadPixels)(int x, int y, int w, int h, unsigned format, unsigned type, void *out);

static GetIntegerv gl_get_integerv;
static GetString gl_get_string;
static int g_pass, g_fail;

#define CHECK(cond)                                                \
    do                                                             \
    {                                                              \
        if (cond)                                                  \
        {                                                          \
            g_pass++;                                              \
        }                                                          \
        else                                                       \
        {                                                          \
            g_fail++;                                              \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

static int geti(unsigned name)
{
    int v = 0;
    gl_get_integerv(name, &v);
    return v;
}

static int version(void)
{
    return geti(GL_MAJOR_VERSION) * 10 + geti(GL_MINOR_VERSION);
}

static PlatformWindow *make(GLConfig gl)
{
    WindowConfig cfg = {.title = "test_gl_wayland", .width = 64, .height = 64, .gl = gl};
    PlatformWindow *w = window_create(&cfg);
    if (w)
    {
        *(void **)&gl_get_integerv = gl_proc_address("glGetIntegerv");
        *(void **)&gl_get_string = gl_proc_address("glGetString");
    }
    return w;
}

static void test_default(void)
{
    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (!w)
        return;
    CHECK(version() >= 33);
    CHECK(geti(GL_CONTEXT_PROFILE_MASK) & GL_CONTEXT_CORE_PROFILE_BIT);
    CHECK(!(geti(GL_CONTEXT_FLAGS) & GL_CONTEXT_FLAG_DEBUG_BIT));
    window_set_vsync(w, true);
    window_set_vsync(w, false);
    window_swap(w);
    window_destroy(w);
}

static void test_profiles(void)
{
    PlatformWindow *w = make((GLConfig){.profile = GL_PROFILE_CORE, .major = 3, .minor = 3});
    CHECK(w != NULL);
    if (w)
    {
        CHECK(version() >= 33);
        CHECK(geti(GL_CONTEXT_PROFILE_MASK) & GL_CONTEXT_CORE_PROFILE_BIT);
        window_destroy(w);
    }

    w = make((GLConfig){.profile = GL_PROFILE_COMPAT, .major = 3, .minor = 2});
    CHECK(w != NULL);
    if (w)
    {
        CHECK(version() >= 32);
        CHECK(geti(GL_CONTEXT_PROFILE_MASK) & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT);
        window_destroy(w);
    }

    w = make((GLConfig){.profile = GL_PROFILE_ES, .major = 3, .minor = 0});
    CHECK(w != NULL);
    if (w)
    {
        const char *v = (const char *)gl_get_string(GL_VERSION);
        CHECK(v && strncmp(v, "OpenGL ES 3", 11) == 0);
        window_destroy(w);
    }

    w = make((GLConfig){.profile = GL_PROFILE_ES});
    CHECK(w != NULL);
    if (w)
    {
        const char *v = (const char *)gl_get_string(GL_VERSION);
        CHECK(v && strncmp(v, "OpenGL ES 3", 11) == 0);
        window_destroy(w);
    }
}

static void test_debug(void)
{
    PlatformWindow *w = make((GLConfig){.debug = true});
    CHECK(w != NULL);
    if (!w)
        return;
    CHECK(geti(GL_CONTEXT_FLAGS) & GL_CONTEXT_FLAG_DEBUG_BIT);
    window_destroy(w);
}

static void test_msaa(void)
{
    platform_clear_error();
    PlatformWindow *w = make((GLConfig){.msaa = 4});
    if (!w)
    {
        CHECK(strstr(platform_get_error(), "4 samples") != NULL);
        return;
    }
    CHECK(geti(GL_SAMPLE_BUFFERS) == 1);
    CHECK(geti(GL_SAMPLES) >= 4);
    window_destroy(w);
}

static void test_failures(void)
{
    for (int i = 0; i < 20; i++)
    {
        platform_clear_error();
        PlatformWindow *w = make((GLConfig){.profile = GL_PROFILE_CORE, .major = 9, .minor = 9});
        CHECK(w == NULL);
        CHECK(strstr(platform_get_error(), "core 9.9") != NULL);
    }

    platform_clear_error();
    CHECK(make((GLConfig){.profile = (GLProfile)99}) == NULL);
    CHECK(strstr(platform_get_error(), "invalid OpenGL configuration") != NULL);

    platform_clear_error();
    CHECK(make((GLConfig){.msaa = 1024}) == NULL);
    CHECK(strstr(platform_get_error(), "1024 samples") != NULL);

    /* a window kind of rendering the backend does not have yet fails the same way */
    WindowConfig px = {.title = "px", .width = 64, .height = 64, .render = RENDER_PIXELS};
    platform_clear_error();
    CHECK(window_create(&px) == NULL);
    CHECK(platform_get_error()[0] != 0);

    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (w)
        window_destroy(w);
}

static bool next_resize(PlatformWindow *w, EventType type, int *rw, int *rh)
{
    Event e;
    while (poll_event(w, &e))
    {
        if (e.type == type)
        {
            *rw = e.data.resize.w;
            *rh = e.data.resize.h;
            return true;
        }
    }
    return false;
}

/* The client chooses the size of a floating window: the new buffer has to be
   there, and the application hears about the size in both spaces. The driver
   takes the new size at the frame boundary, so the frames run as an application
   runs them: the events are read right after the swap, before any drawing.
   The window settles first: the compositor sends a configure with the old size
   when it maps and focuses the window, and a size asked for before that arrives
   is only a request that configure can undo. */
static void test_resize(void)
{
    PlatformWindow *w = make((GLConfig){0});
    CHECK(w != NULL);
    if (!w)
        return;
    for (int i = 0; i < 30; i++)
    {
        window_begin_frame(w);
        window_swap(w);
        time_sleep(10);
    }

    int cw = 0, ch = 0;
    window_get_size(w, &cw, &ch);
    CHECK(cw > 0 && ch > 0);
    window_get_framebuffer_size(w, &cw, &ch);
    CHECK(cw > 0 && ch > 0);

    window_set_size(w, 160, 120);
    window_begin_frame(w);

    int rw = 0, rh = 0;
    CHECK(next_resize(w, EVENT_WINDOW_RESIZE, &rw, &rh));
    CHECK(rw == 160 && rh == 120);
    window_get_size(w, &rw, &rh);
    CHECK(rw == 160 && rh == 120);
    window_get_framebuffer_size(w, &rw, &rh);
    CHECK(rw == 160 && rh == 120);

    ClearColor clear_color = (ClearColor)gl_proc_address("glClearColor");
    Clear clear = (Clear)gl_proc_address("glClear");
    ReadPixels read_pixels = (ReadPixels)gl_proc_address("glReadPixels");
    CHECK(clear_color && clear && read_pixels);
    if (clear_color && clear && read_pixels)
    {
        /* the far corner only exists if the buffer followed the window */
        unsigned char pixel[4] = {0, 0, 0, 0};
        clear_color(1.0f, 0.0f, 0.0f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        read_pixels(159, 119, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(pixel[0] == 255 && pixel[1] == 0 && pixel[2] == 0);
    }
    window_swap(w);
    window_destroy(w);
}

/* One context draws to two windows: each keeps what was drawn to it. */
static void test_one_context_two_windows(void)
{
    WindowConfig cfg = {.title = "test_gl_wayland", .width = 64, .height = 64};
    PlatformWindow *first = window_create(&cfg);
    PlatformWindow *second = window_create(&cfg);
    CHECK(first != NULL);
    CHECK(second != NULL);
    if (!first || !second)
    {
        if (first)
            window_destroy(first);
        if (second)
            window_destroy(second);
        return;
    }
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(first);
        window_begin_frame(second);
    }

    window_make_current(first);
    ClearColor clear_color = (ClearColor)gl_proc_address("glClearColor");
    Clear clear = (Clear)gl_proc_address("glClear");
    ReadPixels read_pixels = (ReadPixels)gl_proc_address("glReadPixels");
    CHECK(clear_color && clear && read_pixels);
    if (clear_color && clear && read_pixels)
    {
        unsigned char pixel[4] = {0, 0, 0, 0};
        clear_color(1.0f, 0.0f, 0.0f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);

        window_make_current_on(second, first);
        clear_color(0.0f, 0.0f, 1.0f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        read_pixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(pixel[0] == 0 && pixel[2] == 255);

        window_make_current_on(first, first);
        clear_color(1.0f, 0.0f, 0.0f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        read_pixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(pixel[0] == 255 && pixel[2] == 0);

        window_swap(second);
        window_swap(first);

        /* Destroying the other window leaves the first context current. */
        window_destroy(second);
        second = NULL;
        pixel[0] = 0;
        clear_color(0.0f, 1.0f, 0.0f, 1.0f);
        clear(GL_COLOR_BUFFER_BIT);
        read_pixels(8, 8, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel);
        CHECK(pixel[1] == 255 && pixel[0] == 0);
    }
    if (second)
        window_destroy(second);
    window_destroy(first);
}

int main(void)
{
    if (!platform_init())
    {
        printf("SKIP %s\n", platform_get_error());
        return 77;
    }

    test_default();
    test_profiles();
    test_debug();
    test_msaa();
    test_failures();
    test_resize();
    test_one_context_two_windows();

    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
