/*
 * test_wayland_window.c - window state on a real Wayland compositor: maximize,
 * fullscreen, hide and show, and the swap of a window the compositor stopped
 * painting. Skips (77) without a compositor.
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>

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

typedef struct
{
    int maximize, restore, focus_gained, focus_lost, fb_resize;
    int fb_w, fb_h;
} Seen;

/* Frames as an application runs them, for about ms milliseconds. */
static void frames(PlatformWindow *w, Seen *s, int ms)
{
    for (int t = 0; t < ms; t += 10)
    {
        window_begin_frame(w);
        Event e;
        while (poll_event(w, &e))
        {
            switch (e.type)
            {
            case EVENT_WINDOW_MAXIMIZE:
                s->maximize++;
                break;
            case EVENT_WINDOW_RESTORE:
                s->restore++;
                break;
            case EVENT_WINDOW_FOCUS:
                if (e.data.focus.gained)
                    s->focus_gained++;
                else
                    s->focus_lost++;
                break;
            case EVENT_WINDOW_FB_RESIZE:
                s->fb_resize++;
                s->fb_w = e.data.resize.w;
                s->fb_h = e.data.resize.h;
                break;
            default:
                break;
            }
        }
        window_swap(w);
        time_sleep(10);
    }
}

static bool wait_maximized(PlatformWindow *w, Seen *s, bool want)
{
    for (int i = 0; i < 300; i++)
    {
        frames(w, s, 10);
        if (window_is_maximized(w) == want)
            return true;
    }
    return false;
}

static bool wait_size(PlatformWindow *w, Seen *s, bool bigger, int w0, int h0)
{
    for (int i = 0; i < 300; i++)
    {
        frames(w, s, 10);
        int cw, ch;
        window_get_size(w, &cw, &ch);
        if (bigger ? (cw > w0 && ch > h0) : (cw == w0 && ch == h0))
            return true;
    }
    return false;
}

static PlatformWindow *make(bool resizable, bool vsync)
{
    WindowConfig cfg = {.title = "test_wayland_window", .width = 300, .height = 200,
                        .resizable = resizable, .vsync = vsync};
    return window_create(&cfg);
}

static void test_maximize(void)
{
    PlatformWindow *w = make(true, false);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    frames(w, &s, 100);
    CHECK(!window_is_maximized(w));

    window_maximize(w);
    CHECK(wait_maximized(w, &s, true));
    CHECK(s.maximize == 1);
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw > 300 && ch > 200);
    window_get_framebuffer_size(w, &cw, &ch);
    CHECK(s.fb_w == cw && s.fb_h == ch);

    window_restore(w);
    CHECK(wait_maximized(w, &s, false));
    CHECK(wait_size(w, &s, false, 300, 200));
    CHECK(s.restore == 1);
    window_destroy(w);
}

static void test_fullscreen(void)
{
    PlatformWindow *w = make(true, false);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    frames(w, &s, 100);
    CHECK(window_get_mode(w) == WINDOW_WINDOWED);

    window_set_mode(w, WINDOW_FULLSCREEN, MONITOR_CURRENT);
    CHECK(window_get_mode(w) == WINDOW_FULLSCREEN);
    CHECK(wait_size(w, &s, true, 300, 200));

    window_set_mode(w, WINDOW_WINDOWED, MONITOR_CURRENT);
    CHECK(window_get_mode(w) == WINDOW_WINDOWED);
    CHECK(wait_size(w, &s, false, 300, 200));
    window_destroy(w);
}

/* Created fullscreen, the window leaves it for the size it was asked for. */
static void test_created_fullscreen(void)
{
    WindowConfig cfg = {.title = "test_wayland_window", .width = 300, .height = 200,
                        .resizable = true, .mode = WINDOW_FULLSCREEN};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    frames(w, &s, 100);
    CHECK(window_get_mode(w) == WINDOW_FULLSCREEN);
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw > 300 && ch > 200);

    window_set_mode(w, WINDOW_WINDOWED, MONITOR_CURRENT);
    CHECK(wait_size(w, &s, false, 300, 200));
    window_destroy(w);
}

static void test_hide_show(void)
{
    PlatformWindow *w = make(true, true);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    frames(w, &s, 100);
    CHECK(window_is_visible(w));
    bool had_focus = window_is_focused(w);

    window_hide(w);
    CHECK(!window_is_visible(w));
    CHECK(!window_is_focused(w));
    frames(w, &s, 100); /* swaps of a hidden window must come back at once */
    if (had_focus)
        CHECK(s.focus_lost >= 1);

    window_show(w);
    CHECK(window_is_visible(w));
    frames(w, &s, 100);
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 300 && ch == 200);
    window_destroy(w);
}

/* A window shown again right after it was hidden meets the compositor still
   playing its closing. GNOME Shell 46 died of that (SIGSEGV, and the whole session
   with it) while show reused the surface of the window that went away, so the
   waits run from none to past the animation. */
static void test_hide_show_cycles(void)
{
    static const int wait_ms[] = {0, 10, 40, 100, 170, 300};
    PlatformWindow *w = make(true, true);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    frames(w, &s, 50);
    for (int i = 0; i < 30; i++)
    {
        window_hide(w);
        frames(w, &s, wait_ms[i % 6]);
        window_show(w);
        CHECK(window_is_visible(w));
        frames(w, &s, wait_ms[(i * 5 + 2) % 6]);
    }
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 300 && ch == 200);
    window_destroy(w);
}

/* The compositor sends no frame callbacks to a minimized window; a swap that
   waited for one without a limit would stop the loop here. */
static void test_minimized_vsync(void)
{
    PlatformWindow *w = make(true, true);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    frames(w, &s, 100);
    window_minimize(w);
    frames(w, &s, 600);
    CHECK(window_get_mode(w) == WINDOW_WINDOWED); /* still alive and answering */
    window_destroy(w);
}

static void test_limits_and_title(void)
{
    PlatformWindow *w = make(true, false);
    CHECK(w != NULL);
    if (!w)
        return;
    Seen s = {0};
    window_set_title(w, "another title");
    window_set_size_limits(w, 200, 150, 800, 600);
    frames(w, &s, 100);
    window_set_size_limits(w, 0, 0, 0, 0);
    frames(w, &s, 100);
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 300 && ch == 200);
    window_destroy(w);

    /* not resizable: the size it was created with is the only one */
    w = make(false, false);
    CHECK(w != NULL);
    if (w)
    {
        frames(w, &s, 100);
        window_get_size(w, &cw, &ch);
        CHECK(cw == 300 && ch == 200);
        window_destroy(w);
    }
}

int main(void)
{
    if (!platform_init())
    {
        printf("SKIP %s\n", platform_get_error());
        return 77;
    }

    test_maximize();
    test_fullscreen();
    test_created_fullscreen();
    test_hide_show();
    test_hide_show_cycles();
    test_minimized_vsync();
    test_limits_and_title();

    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
