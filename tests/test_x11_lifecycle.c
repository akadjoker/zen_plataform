/*
 * test_x11_lifecycle.c - the X11 backend under stress: a storm of resizes in one frame, a close request that
 * arrives twice, and init/shutdown cycles that must not leak file descriptors or memory. Skips (77) without a
 * display.
 */
#include "platform.h"

#include <X11/Xlib.h>

#include <dirent.h>
#include <stdint.h>
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

static Window xwin(PlatformWindow *w)
{
    return (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW);
}

static void settle(PlatformWindow *w)
{
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        time_sleep(5);
    }
}

static int open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        return -1;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

/* A window manager sends one ConfigureNotify per step while a window is dragged. A frame that took long finds
   hundreds queued: the last size must still reach the application. */
static void test_resize_storm(void)
{
    WindowConfig cfg = {.title = "storm", .width = 200, .height = 150, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *dpy = window_native_handle(w, NATIVE_DISPLAY);
    settle(w);

    const int steps = 600;
    for (int i = 0; i < steps; i++)
        XResizeWindow(dpy, xwin(w), (unsigned)(300 + i), (unsigned)(200 + i % 7));
    XSync(dpy, False);
    time_sleep(150);

    window_begin_frame(w);
    int last_w = -1, last_h = -1, last_fb_w = -1, resizes = 0;
    Event e;
    while (poll_event(w, &e))
    {
        if (e.type == EVENT_WINDOW_RESIZE)
        {
            last_w = e.data.resize.w;
            last_h = e.data.resize.h;
            resizes++;
        }
        else if (e.type == EVENT_WINDOW_FB_RESIZE)
            last_fb_w = e.data.resize.w;
    }
    int cw = 0, ch = 0;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 300 + steps - 1);
    CHECK(ch == 200 + (steps - 1) % 7);
    CHECK(resizes > 0);
    CHECK(last_w == cw);
    CHECK(last_h == ch);
    CHECK(last_fb_w == cw);
    window_destroy(w);
}

static void test_close_twice(void)
{
    WindowConfig cfg = {.title = "close", .width = 120, .height = 90, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *dpy = window_native_handle(w, NATIVE_DISPLAY);
    settle(w);
    CHECK(!window_should_close(w));

    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xclient.type = ClientMessage;
    ev.xclient.window = xwin(w);
    ev.xclient.message_type = XInternAtom(dpy, "WM_PROTOCOLS", False);
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = (long)XInternAtom(dpy, "WM_DELETE_WINDOW", False);
    ev.xclient.data.l[1] = CurrentTime;
    XSendEvent(dpy, xwin(w), False, NoEventMask, &ev);
    XSendEvent(dpy, xwin(w), False, NoEventMask, &ev);
    XSync(dpy, False);
    time_sleep(30);

    window_begin_frame(w);
    int closes = 0;
    Event e;
    while (poll_event(w, &e))
        closes += e.type == EVENT_WINDOW_CLOSE;
    CHECK(closes >= 1);
    CHECK(window_should_close(w));

    /* The application ignores the first request and asks again, then still closes only once. */
    window_set_should_close(w, false);
    XSendEvent(dpy, xwin(w), False, NoEventMask, &ev);
    XSync(dpy, False);
    time_sleep(30);
    window_begin_frame(w);
    CHECK(window_should_close(w));
    window_destroy(w);
    window_destroy(NULL);
}

static void test_cycles(void)
{
    /* One cycle first, so the lazily loaded libraries and the first connection are already counted. */
    for (int round = 0; round < 2; round++)
    {
        WindowConfig cfg = {.title = "cycle", .width = 100, .height = 80, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
        PlatformWindow *w = window_create(&cfg);
        if (w)
        {
            settle(w);
            window_destroy(w);
        }
        platform_shutdown();
        if (!platform_init())
        {
            CHECK(false);
            return;
        }
    }
    const int before = open_fds();
    for (int round = 0; round < 25; round++)
    {
        WindowConfig cfg = {.title = "cycle", .width = 100, .height = 80, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
        PlatformWindow *w = window_create(&cfg);
        CHECK(w != NULL);
        if (!w)
            break;
        PlatformWindow *w2 = window_create(&cfg);
        CHECK(w2 != NULL);
        window_begin_frame(w);
        clipboard_set("cycle");
        CHECK(clipboard_get() != NULL);
        if (w2)
            window_destroy(w2);
        window_destroy(w);
        platform_shutdown();
        if (!platform_init())
        {
            CHECK(false);
            return;
        }
    }
    CHECK(open_fds() == before);
}

int main(void)
{
    if (!platform_init())
    {
        printf("skip: no display\n");
        return 77;
    }
    test_resize_storm();
    test_close_twice();
    test_cycles();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
