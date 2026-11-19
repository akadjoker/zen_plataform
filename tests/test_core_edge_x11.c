/*
 * test_core_edge_x11.c - edge cases of the core on the X11 backend that the other X11 tests do not reach: what a
 * pixel window costs, a thousand windows in a row, sizes the X protocol cannot carry, titles of every kind, window
 * states switched in any order, windows destroyed while their events or keys are still in flight, the clipboard at
 * its extremes and the clocks. Every section runs in a child of its own (a fatal X error ends a process, and the
 * section that does it must be named, not take the others down with it). Skips (77) without a display.
 */
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif

#include <dirent.h>
#include <locale.h>
#include <sys/inotify.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define EDGE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define EDGE_ASAN 1
#endif
#endif

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

static PlatformWindow *make(RenderMode render, int w, int h)
{
    WindowConfig cfg = {.title = "edge", .width = w, .height = h, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = render, .resizable = true};
    return window_create(&cfg);
}

static Display *dpy_of(PlatformWindow *w)
{
    return window_native_handle(w, NATIVE_DISPLAY);
}

static Window xwin(PlatformWindow *w)
{
    return (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW);
}

static void settle(PlatformWindow *w)
{
    for (int i = 0; i < 6; i++)
    {
        window_begin_frame(w);
        time_sleep(3);
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

static long rss_kb(void)
{
    long pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (f)
    {
        if (fscanf(f, "%ld %ld", &pages, &resident) != 2)
            resident = 0;
        fclose(f);
    }
    return resident * (sysconf(_SC_PAGESIZE) / 1024);
}

/* A process that opens no GL window must not have the GL vendor's driver (Mesa and its LLVM, the NVIDIA stack)
   mapped: the first GLX call loads it, and the backend used to make that call in platform_init. */
static bool vendor_gl_mapped(void)
{
    char line[1024];
    bool found = false;
    FILE *f = fopen("/proc/self/maps", "r");
    while (f && fgets(line, sizeof line, f))
        found |= strstr(line, "libGLX_") != NULL || strstr(line, "libgallium") != NULL || strstr(line, "libnvidia-glcore") != NULL;
    if (f)
        fclose(f);
    return found;
}

static void test_pixel_window_loads_no_gl_driver(void)
{
    CHECK(!vendor_gl_mapped()); /* after platform_init alone */
    PlatformWindow *w = make(RENDER_PIXELS, 200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    settle(w);
    Framebuffer fb;
    CHECK(window_lock_pixels(w, &fb));
    window_present_pixels(w);
    window_set_vsync(w, false); /* not a GL window: nothing to set, and no GL needed to find that out */
    CHECK(!vendor_gl_mapped());
    window_destroy(w);
}

/* A thousand windows made and destroyed in a row: no descriptor, no window on the server, no memory left behind. */
static void test_thousand_windows(void)
{
    Display *probe = XOpenDisplay(NULL);
    CHECK(probe != NULL);
    if (!probe)
        return;
    Window root = DefaultRootWindow(probe), r, parent, *kids = NULL;
    unsigned nbefore = 0, nafter = 0;
    XQueryTree(probe, root, &r, &parent, &kids, &nbefore);
    if (kids)
        XFree(kids);
    for (int i = 0; i < 100; i++) /* warm up: the libraries load, the allocator settles */
    {
        PlatformWindow *w = make(RENDER_PIXELS, 64, 48);
        if (w)
            window_destroy(w);
    }
    const int fds = open_fds();
    const long rss = rss_kb();
    int made = 0;
    for (int i = 0; i < 1000; i++)
    {
        PlatformWindow *w = make(RENDER_PIXELS, 64 + i % 5, 48);
        if (!w)
            continue;
        made++;
        if (i % 50 == 0)
            window_begin_frame(w);
        window_destroy(w);
    }
    CHECK(made == 1000);
    CHECK(open_fds() == fds);
    printf("  1000 windows: rss %ld kB -> %ld kB\n", rss, rss_kb());
#ifndef EDGE_ASAN /* ASan keeps what is freed in a quarantine of its own, which grows the RSS by design */
    CHECK(rss_kb() - rss < 2048);
#endif
    XSync(probe, False);
    kids = NULL;
    XQueryTree(probe, root, &r, &parent, &kids, &nafter);
    if (kids)
        XFree(kids);
    CHECK(nafter == nbefore);
    XCloseDisplay(probe);
}

/* The X protocol carries a window size in 16 bits and refuses 0: a size outside that is not a reason to end the
   program (Xlib's own answer to the refusal). */
static void test_sizes(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 0, 0);
    CHECK(w != NULL);
    if (w)
    {
        int cw = 0, ch = 0;
        window_get_size(w, &cw, &ch);
        CHECK(cw > 0 && ch > 0);
        window_destroy(w);
    }
    w = make(RENDER_PIXELS, -40, -40);
    CHECK(w != NULL);
    if (w)
        window_destroy(w);
    w = make(RENDER_PIXELS, 100000, 100000);
    CHECK(w != NULL);
    if (w)
    {
        XSync(dpy_of(w), False);
        int cw = 0, ch = 0;
        window_get_size(w, &cw, &ch);
        CHECK(cw > 0 && cw <= 65535 && ch > 0 && ch <= 65535);
        window_destroy(w);
    }
    w = make(RENDER_PIXELS, 200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    static const int sizes[][2] = {{0, 0}, {-5, -5}, {1, 1}, {70000, 5}, {5, 70000}, {INT_MAX, INT_MAX}, {INT_MIN, INT_MIN}, {300, 200}};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++)
    {
        window_set_size(w, sizes[i][0], sizes[i][1]);
        XSync(dpy_of(w), False);
        settle(w);
        int cw = 0, ch = 0;
        window_get_size(w, &cw, &ch);
        CHECK(cw >= 1 && cw <= 65535 && ch >= 1 && ch <= 65535);
    }
    int cw = 0, ch = 0;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 300 && ch == 200); /* and the window answers normally after all that */
    /* limits that contradict each other, zero, negative and huge ones: taken or ignored, never fatal */
    window_set_size_limits(w, 500, 500, 100, 100);
    window_set_size_limits(w, 0, 0, 0, 0);
    window_set_size_limits(w, -1, -1, -1, -1);
    window_set_size_limits(w, INT_MAX, INT_MAX, INT_MAX, INT_MAX);
    window_set_size_limits(w, 100, 80, 400, 300);
    window_set_position(w, -100000, 100000);
    window_set_position(w, INT_MAX, INT_MIN);
    XSync(dpy_of(w), False);
    settle(w);
    window_destroy(w);
}

static char *read_name(Display *d, Window win)
{
    Atom utf8 = XInternAtom(d, "UTF8_STRING", False), net = XInternAtom(d, "_NET_WM_NAME", False), type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(d, win, net, 0, 1 << 20, False, utf8, &type, &fmt, &n, &after, &data) != Success || !data)
        return NULL;
    char *copy = malloc(n + 1);
    memcpy(copy, data, n);
    copy[n] = 0;
    XFree(data);
    return copy;
}

static void test_titles(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    window_set_title(w, "");
    window_set_title(w, NULL);
    window_set_title(w, "t\xc3\xadtulo \xe2\x82\xac");
    XSync(d, False);
    char *got = read_name(d, xwin(w));
    CHECK(got && strcmp(got, "t\xc3\xadtulo \xe2\x82\xac") == 0);
    free(got);

    char *big = malloc(65537);
    memset(big, 'x', 65536);
    big[65536] = 0;
    window_set_title(w, big);
    XSync(d, False);
    got = read_name(d, xwin(w));
    CHECK(got && strlen(got) >= 4096); /* a very long title may be cut, it must not be lost or end the program */
    free(got);
    free(big);

    window_set_title(w, "\xff\xfe\xc0\xaf\x80 broken \xed\xa0\x80 utf-8 \xf8\x88\x80\x80\x80");
    window_set_title(w, "\xe2\x82"); /* cut in the middle of a character */
    XSync(d, False);

    char *huge = malloc(8u << 20);
    memset(huge, 'h', (8u << 20) - 1);
    huge[(8u << 20) - 1] = 0;
    window_set_title(w, huge); /* more than a server without BIG-REQUESTS takes in one request */
    XSync(d, False);
    free(huge);
    window_set_title(w, "after");
    XSync(d, False);
    got = read_name(d, xwin(w));
    CHECK(got && strcmp(got, "after") == 0);
    free(got);
    window_destroy(w);
}

/* Show, hide, minimise, maximise, restore, fullscreen and the rest switched fast in an order nobody planned. */
static void test_state_storm(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 320, 240);
    CHECK(w != NULL);
    if (!w)
        return;
    unsigned seed = 12345;
    for (int i = 0; i < 600; i++)
    {
        seed = seed * 1103515245u + 12345u;
        switch ((seed >> 16) % 16)
        {
        case 0: window_show(w); break;
        case 1: window_hide(w); break;
        case 2: window_minimize(w); break;
        case 3: window_maximize(w); break;
        case 4: window_restore(w); break;
        case 5: window_set_mode(w, WINDOW_FULLSCREEN, 0); break;
        case 6: window_set_mode(w, WINDOW_FULLSCREEN_BORDERLESS, 0); break;
        case 7: window_set_mode(w, WINDOW_WINDOWED, 0); break;
        case 8: window_set_decorated(w, (seed >> 20) & 1); break;
        case 9: window_set_always_on_top(w, (seed >> 20) & 1); break;
        case 10: window_set_opacity(w, (float)((seed >> 20) % 150) / 100.0f - 0.2f); break;
        case 11: window_request_attention(w); break;
        case 12: window_focus(w); break;
        case 13: window_set_position(w, (int)((seed >> 8) % 400) - 100, (int)((seed >> 12) % 300) - 100); break;
        case 14: window_set_size(w, 100 + (int)((seed >> 8) % 300), 100 + (int)((seed >> 12) % 200)); break;
        default: window_center_on_monitor(w, 0); break;
        }
        if (i % 7 == 0)
            window_begin_frame(w);
    }
    window_set_mode(w, WINDOW_WINDOWED, 0);
    window_show(w);
    XSync(dpy_of(w), False);
    settle(w);
    (void)window_is_visible(w), (void)window_is_minimized(w), (void)window_is_maximized(w);
    /* the window is still ours: a resize reaches the application */
    window_set_size(w, 333, 222);
    XSync(dpy_of(w), False);
    time_sleep(30);
    window_begin_frame(w);
    int cw = 0, ch = 0;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 333 && ch == 222);
    window_destroy(w);
}

/* A window destroyed while another one pumps the same connection, and in the middle of its own event loop. */
static void test_destroy_during_events(void)
{
    PlatformWindow *a = make(RENDER_PIXELS, 200, 150), *b = make(RENDER_PIXELS, 200, 150);
    CHECK(a && b);
    if (!a || !b)
        return;
    Display *d = dpy_of(a);
    settle(a);
    settle(b);
    for (int i = 0; i < 200; i++) /* events for b waiting while a is the one that pumps */
    {
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        ev.xmotion.type = MotionNotify;
        ev.xmotion.window = xwin(b);
        ev.xmotion.x = ev.xmotion.y = i % 100;
        XSendEvent(d, xwin(b), False, NoEventMask, &ev);
    }
    XSync(d, False);
    window_begin_frame(a); /* b's events are taken off the connection and kept for b */
    window_destroy(b);     /* b goes with its events unread */
    XSync(d, False);
    window_begin_frame(a);

    Event e;
    int seen = 0;
    window_set_size(a, 210, 160);
    XSync(d, False);
    time_sleep(20);
    window_begin_frame(a);
    while (poll_event(a, &e))
    {
        seen++;
        window_destroy(a); /* destroyed from inside its own event loop, which then stops */
        a = NULL;
        break;
    }
    CHECK(seen == 1);
    PlatformWindow *c = make(RENDER_PIXELS, 100, 100);
    CHECK(c != NULL);
    if (c)
    {
        settle(c);
        window_destroy(c);
    }
}

#ifdef HAVE_XTEST
/* Keys and a button held when their window is destroyed, released while no window exists: the next window starts with
   nothing down and is told of nothing. */
static void test_held_across_destroy(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    int rx = 0, ry = 0;
    window_get_position(w, &rx, &ry);
    settle(w);
    KeyCode a = XKeysymToKeycode(d, 'a');
    XTestFakeMotionEvent(d, -1, rx + 100, ry + 100, 0);
    XTestFakeKeyEvent(d, a, True, 0);
    XTestFakeButtonEvent(d, 1, True, 0);
    XSync(d, False);
    time_sleep(40);
    window_begin_frame(w);
    CHECK(key_down(w, KEY_A));
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    window_destroy(w);
    XTestFakeKeyEvent(d, a, False, 0); /* released while no window exists; the platform's connection outlives the window */
    XTestFakeButtonEvent(d, 1, False, 0);
    XSync(d, False);
    time_sleep(30);
    PlatformWindow *w2 = make(RENDER_PIXELS, 300, 200);
    CHECK(w2 != NULL);
    if (!w2)
        return;
    int told = 0;
    for (int i = 0; i < 8; i++)
    {
        window_begin_frame(w2);
        Event e;
        while (poll_event(w2, &e))
            told += e.type == EVENT_KEY || e.type == EVENT_MOUSE_BUTTON || e.type == EVENT_CHAR;
        time_sleep(5);
    }
    CHECK(!key_down(w2, KEY_A));
    CHECK(!mouse_button_down(w2, MOUSE_LEFT));
    CHECK(told == 0);
    window_destroy(w2);
}
#endif

#ifdef HAVE_XTEST
/* Hundreds of wheel notches reach the application in one frame, more than the event list holds: the sum is exact
   (vertical and horizontal), and a notch is not a button press. */
static void test_wheel_burst(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    int rx = 0, ry = 0;
    window_get_position(w, &rx, &ry);
    settle(w);
    XTestFakeMotionEvent(d, -1, rx + 100, ry + 100, 0);
    XSync(d, False);
    time_sleep(20);
    window_begin_frame(w);
    const int up = 700, down = 200, left = 90, right = 30;
    for (int i = 0; i < up; i++)
    {
        XTestFakeButtonEvent(d, 4, True, 0);
        XTestFakeButtonEvent(d, 4, False, 0);
    }
    for (int i = 0; i < down; i++)
    {
        XTestFakeButtonEvent(d, 5, True, 0);
        XTestFakeButtonEvent(d, 5, False, 0);
    }
    for (int i = 0; i < left; i++)
    {
        XTestFakeButtonEvent(d, 6, True, 0);
        XTestFakeButtonEvent(d, 6, False, 0);
    }
    for (int i = 0; i < right; i++)
    {
        XTestFakeButtonEvent(d, 7, True, 0);
        XTestFakeButtonEvent(d, 7, False, 0);
    }
    XSync(d, False);
    time_sleep(100);
    window_begin_frame(w);
    float wx = 0, wy = 0;
    mouse_wheel_v(w, &wx, &wy);
    CHECK(wy == (float)(up - down));
    CHECK(wx == (float)(right - left));
    for (int b = 0; b < MOUSE_BUTTON_MAX; b++)
        CHECK(!mouse_button_down(w, b));
    window_destroy(w);
}
#endif

/* The pointer mode, the shape and the grab switched in any order, a shape changed ten thousand times, and the window
   destroyed in the middle of a capture: the server's pointer is free afterwards (a second connection can grab it). */
static void test_cursors_and_grabs(void)
{
    PlatformWindow *w = make(RENDER_PIXELS, 300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    settle(w);
    for (int i = 0; i < 10000; i++)
        mouse_set_cursor(w, i % (CURSOR_COUNT + 2) - 1); /* -1 and CURSOR_COUNT are not shapes */
    unsigned seed = 7;
    for (int i = 0; i < 400; i++)
    {
        seed = seed * 1103515245u + 12345u;
        switch ((seed >> 16) % 5)
        {
        case 0: mouse_set_mode(w, MOUSE_MODE_NORMAL); break;
        case 1: mouse_set_mode(w, MOUSE_MODE_HIDDEN); break;
        case 2: mouse_set_mode(w, MOUSE_MODE_CAPTURED); break;
        case 3: mouse_capture(w, (seed >> 20) & 1); break;
        default: mouse_set_cursor(w, (int)((seed >> 20) % CURSOR_COUNT)); break;
        }
        if (i % 5 == 0)
            window_begin_frame(w);
    }
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    window_begin_frame(w);
    window_destroy(w); /* captured */
    Display *probe = XOpenDisplay(NULL);
    CHECK(probe != NULL);
    if (probe)
    {
        XSync(probe, False);
        CHECK(XGrabPointer(probe, DefaultRootWindow(probe), False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None,
                           CurrentTime) == GrabSuccess);
        XUngrabPointer(probe, CurrentTime);
        XCloseDisplay(probe);
    }
    (void)d;
}

/* Many windows at once on one connection, each with its own state, destroyed in an order that is not the order they
   were made in. */
static void test_many_windows(void)
{
    enum { N = 96 };
    PlatformWindow *w[N];
    int made = 0;
    for (int i = 0; i < N; i++)
    {
        w[i] = make(RENDER_PIXELS, 80 + i, 60 + i);
        made += w[i] != NULL;
        if (w[i])
            window_set_user_ptr(w[i], &w[i]);
    }
    CHECK(made == N);
    const int fds = open_fds();
    for (int round = 0; round < 3; round++)
        for (int i = 0; i < N; i++)
            if (w[i])
            {
                window_begin_frame(w[i]);
                char title[32];
                snprintf(title, sizeof title, "w%d", i);
                window_set_title(w[i], title);
                CHECK(window_get_user_ptr(w[i]) == &w[i]);
            }
    for (int i = 0; i < N; i += 2) /* evens first, then the odds from the end */
        if (w[i])
            window_destroy(w[i]), w[i] = NULL;
    for (int i = N - 1; i >= 0; i--)
        if (w[i])
        {
            window_begin_frame(w[i]); /* the others went while this one had events waiting */
            window_destroy(w[i]);
            w[i] = NULL;
        }
    CHECK(open_fds() == fds);
}

static void test_clipboard_extremes(void)
{
    static const struct
    {
        const char *mime;
        size_t size;
    } cases[] = {{"text/plain;charset=utf-8", 0}, {"text/plain;charset=utf-8", 1}, {"application/octet-stream", 3},
                 {"text/plain;charset=utf-8", 17}, {"application/octet-stream", 8u << 20}};
    PlatformWindow *w = make(RENDER_PIXELS, 100, 80);
    CHECK(w != NULL);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        size_t n = cases[i].size;
        unsigned char *src = malloc(n ? n : 1);
        for (size_t k = 0; k < n; k++)
            src[k] = (unsigned char)(k * 7 + 3);
        if (n == 3)
            src[1] = 0; /* an embedded NUL */
        if (n == 17)
            memcpy(src, "\xff\xfe\xc0\xaf\x80\xed\xa0\x80 bad", 12); /* not UTF-8 */
        bool set = clipboard_set_data(cases[i].mime, src, n);
        size_t got = 0;
        void *back = set ? clipboard_get_data(cases[i].mime, &got) : NULL;
        if (n == 0)
            CHECK(!set || (got == 0));
        else
        {
            CHECK(set);
            CHECK(back != NULL && got == n && memcmp(back, src, n) == 0);
        }
        if (back)
            fs_free(back);
        free(src);
    }
    clipboard_set("");
    clipboard_set("x");
    const char *t = clipboard_get();
    CHECK(t && strcmp(t, "x") == 0);
    if (w)
        window_destroy(w);
}

/* The key a physical key reports follows the keyboard mapping while the program runs: the backend keeps a keycode ->
   key table and must drop it on MappingNotify and on the XKB events. Changed from the test with
   XChangeKeyboardMapping (a core request: MappingNotify) and with setxkbmap (a keymap loaded: the XKB events);
   it changes the mapping of the whole server, so it runs only on the private one (tools/xvfb_run.sh sets
   ZP_PRIVATE_X). */
static int key_after_press(PlatformWindow *w, KeyCode kc)
{
    Display *d = dpy_of(w);
    XTestFakeKeyEvent(d, kc, True, 0);
    XTestFakeKeyEvent(d, kc, False, 0);
    XSync(d, False);
    time_sleep(30);
    int got = -1;
    for (int i = 0; i < 4; i++)
    {
        window_begin_frame(w);
        Event e;
        while (poll_event(w, &e))
            if (e.type == EVENT_KEY && e.data.key.down && got < 0)
                got = e.data.key.key;
        time_sleep(5);
    }
    return got;
}

static void test_keymap_follows(void)
{
    if (!getenv("ZP_PRIVATE_X"))
    {
        printf("  skipped: changes the keyboard mapping of the server, which is not a private one\n");
        return;
    }
    PlatformWindow *w = make(RENDER_PIXELS, 300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    int rx = 0, ry = 0;
    window_get_position(w, &rx, &ry);
    settle(w);
    XTestFakeMotionEvent(d, -1, rx + 100, ry + 100, 0);
    XSync(d, False);
    KeyCode a = XKeysymToKeycode(d, 'a');
    CHECK(key_after_press(w, a) == KEY_A);
    CHECK(key_after_press(w, a) == KEY_A); /* the second one comes from the table */

    int per = 0;
    KeySym *orig = XGetKeyboardMapping(d, a, 1, &per);
    CHECK(orig != NULL && per > 0);
    if (!orig)
        return;
    KeySym *mine = calloc((size_t)per, sizeof *mine);
    mine[0] = XK_z;
    XChangeKeyboardMapping(d, a, per, mine, 1);
    XSync(d, False);
    CHECK(key_after_press(w, a) == KEY_Z);
    CHECK(key_after_press(w, a) == KEY_Z);
    mine[0] = orig[0];
    XChangeKeyboardMapping(d, a, per, orig, 1);
    XSync(d, False);
    CHECK(key_after_press(w, a) == KEY_A);
    free(mine);
    XFree(orig);

    /* a whole layout: on a German keyboard the key that says y on a US one says z */
    KeyCode y = XKeysymToKeycode(d, 'y');
    CHECK(key_after_press(w, y) == KEY_Y);
    if (system("setxkbmap de 2> /dev/null") == 0)
    {
        CHECK(key_after_press(w, y) == KEY_Z);
        CHECK(system("setxkbmap us 2> /dev/null") == 0);
        CHECK(key_after_press(w, y) == KEY_Y);
    }
    else
        printf("  setxkbmap de did not work: the layout half is skipped\n");
    window_destroy(w);
}

/* XMODIFIERS names an input method server that is not running (ibus, down). The built-in method of Xlib is the
   fallback; opening it parses the compose table, which backend_init used to do at once. It is now opened by the first
   key, before that key is filtered, so the first dead key still composes. The compose table here is a file of the
   test (XCOMPOSEFILE), watched with inotify: it must not be opened by platform_init and window_create, and must be
   by the first key. */
static char g_compose_path[256];

static void pre_dead_server(void)
{
    snprintf(g_compose_path, sizeof g_compose_path, "/tmp/zp-compose-%d", (int)getpid());
    FILE *f = fopen(g_compose_path, "w");
    if (f)
    {
        fputs("<dead_acute> <a> : \"\xce\xa9\" Greek_OMEGA\n", f); /* a table of one line: dead acute, a -> omega */
        fclose(f);
    }
    setenv("XCOMPOSEFILE", g_compose_path, 1);
    setenv("XMODIFIERS", "@im=ibus", 1);
    static const char *const locales[] = {"pt_PT.UTF-8", "en_US.UTF-8", "C.UTF-8"};
    for (unsigned i = 0; i < sizeof locales / sizeof locales[0]; i++)
        if (setlocale(LC_CTYPE, locales[i]))
        {
            setenv("LC_ALL", locales[i], 1);
            break;
        }
}

/* The key goes to the window under the pointer, which is whichever of the windows is on top: every window of the
   process is pumped (one pump reads the shared queue and hands the others their events to take at their own pump). */
static void drain_chars(PlatformWindow *const *ws, int nw, uint32_t *out, int *n, int cap)
{
    for (int i = 0; i < 4; i++)
    {
        for (int k = 0; k < nw; k++)
        {
            window_begin_frame(ws[k]);
            Event e;
            while (poll_event(ws[k], &e))
                if (e.type == EVENT_CHAR && *n < cap)
                    out[(*n)++] = e.data.codepoint;
        }
        time_sleep(5);
    }
}

static void test_first_dead_key_late_method(void)
{
    if (!getenv("ZP_PRIVATE_X"))
    {
        printf("  skipped: changes the keyboard mapping of the server, which is not a private one\n");
        return;
    }
    int ino = inotify_init1(IN_NONBLOCK);
    CHECK(ino >= 0);
    if (ino < 0)
        return;
    /* the watch comes after platform_init (main ran it), so what it sees is what came after: nothing yet */
    CHECK(inotify_add_watch(ino, g_compose_path, IN_OPEN | IN_ACCESS) >= 0);
    PlatformWindow *w = make(RENDER_PIXELS, 300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    PlatformWindow *w2 = make(RENDER_PIXELS, 200, 100); /* a second one, made before the method opens */
    CHECK(w2 != NULL);
    Display *d = dpy_of(w);
    int rx = 0, ry = 0;
    window_get_position(w, &rx, &ry);
    settle(w);
    char ev[512];
    CHECK(read(ino, ev, sizeof ev) < 0); /* nothing opened the compose table: no key yet */

    KeyCode dead = XKeysymToKeycode(d, 'q'), a = XKeysymToKeycode(d, 'a');
    int per = 0;
    KeySym *orig = XGetKeyboardMapping(d, dead, 1, &per);
    KeySym *mine = calloc((size_t)per, sizeof *mine);
    mine[0] = XK_dead_acute;
    XChangeKeyboardMapping(d, dead, per, mine, 1);
    XTestFakeMotionEvent(d, -1, rx + 100, ry + 100, 0);
    XSync(d, False);

    /* the first key this process ever gets is the dead key */
    XTestFakeKeyEvent(d, dead, True, 0);
    XTestFakeKeyEvent(d, dead, False, 0);
    XTestFakeKeyEvent(d, a, True, 0);
    XTestFakeKeyEvent(d, a, False, 0);
    XSync(d, False);
    time_sleep(40);
    uint32_t chars[16];
    int n = 0;
    PlatformWindow *two[2] = {w, w2};
    drain_chars(two, 2, chars, &n, 16);
    CHECK(n == 1 && chars[0] == 0x3A9); /* omega, from the test's table: composed, with no plain 'a' beside it */
    CHECK(read(ino, ev, sizeof ev) > 0); /* and the table was read by now */

    /* a window made after the method opened composes too */
    PlatformWindow *w3 = make(RENDER_PIXELS, 300, 200);
    CHECK(w3 != NULL);
    if (w3)
    {
        int x3 = 0, y3 = 0;
        window_get_position(w3, &x3, &y3);
        settle(w3);
        XTestFakeMotionEvent(d, -1, x3 + 100, y3 + 100, 0);
        XTestFakeKeyEvent(d, dead, True, 0);
        XTestFakeKeyEvent(d, dead, False, 0);
        XTestFakeKeyEvent(d, a, True, 0);
        XTestFakeKeyEvent(d, a, False, 0);
        XSync(d, False);
        time_sleep(40);
        n = 0;
        PlatformWindow *three[3] = {w, w2, w3};
        drain_chars(three, 3, chars, &n, 16);
        CHECK(n == 1 && chars[0] == 0x3A9);
        window_destroy(w3);
    }
    XChangeKeyboardMapping(d, dead, per, orig, 1);
    XSync(d, False);
    free(mine);
    XFree(orig);
    window_destroy(w2);
    window_destroy(w);
    close(ino);
    unlink(g_compose_path);
}

static void test_clocks(void)
{
    uint64_t prev = time_nanos();
    int backwards = 0;
    for (int i = 0; i < 200000; i++)
    {
        uint64_t now = time_nanos();
        backwards += now < prev;
        prev = now;
    }
    CHECK(backwards == 0);
    double s0 = time_seconds();
    CHECK(s0 >= 0);
    static const unsigned waits[] = {0, 1, 16};
    for (unsigned k = 0; k < 3; k++)
    {
        int early = 0;
        uint64_t worst = 0, sum = 0;
        for (int i = 0; i < 20; i++)
        {
            uint64_t a = time_nanos();
            time_sleep(waits[k]);
            uint64_t took = time_nanos() - a;
            early += took + 50000 < (uint64_t)waits[k] * 1000000u; /* shorter than asked, 50 us of slack */
            sum += took;
            if (took > worst)
                worst = took;
        }
        printf("  time_sleep(%u): mean %.3f ms, worst %.3f ms\n", waits[k], (double)sum / 20e6, (double)worst / 1e6);
        CHECK(early == 0);
    }
    CHECK(time_seconds() >= s0);
}

typedef struct
{
    const char *name;
    void (*fn)(void);
    void (*pre)(void); /* runs in the child before platform_init: the environment the section needs */
} Section;

int main(void)
{
    static const Section sections[] = {
        {"pixel window loads no GL driver", test_pixel_window_loads_no_gl_driver},
        {"a thousand windows", test_thousand_windows},
        {"sizes", test_sizes},
        {"titles", test_titles},
        {"state storm", test_state_storm},
        {"destroy during events", test_destroy_during_events},
        {"many windows", test_many_windows},
#ifdef HAVE_XTEST
        {"held across destroy", test_held_across_destroy},
        {"wheel burst", test_wheel_burst},
        {"keymap follows", test_keymap_follows},
        {"first dead key, input method opened late", test_first_dead_key_late_method, pre_dead_server},
#endif
        {"cursors and grabs", test_cursors_and_grabs},
        {"clipboard extremes", test_clipboard_extremes},
        {"clocks", test_clocks},
    };
    int failed = 0, skipped = 0;
    for (unsigned i = 0; i < sizeof sections / sizeof sections[0]; i++)
    {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(90);
            if (sections[i].pre)
                sections[i].pre();
            if (!platform_init())
                _exit(77);
            sections[i].fn();
            platform_shutdown();
            printf("%s: %d passed, %d failed\n", sections[i].name, g_pass, g_fail);
            fflush(stdout);
            _exit(g_fail ? 1 : 0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 77)
            skipped++;
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        {
            failed++;
            if (WIFSIGNALED(st))
                printf("FAIL %s: killed by signal %d\n", sections[i].name, WTERMSIG(st));
            else if (WIFEXITED(st) && WEXITSTATUS(st) == 1)
                printf("FAIL %s\n", sections[i].name);
            else
                printf("FAIL %s: the process ended with status %d (a fatal X error?)\n", sections[i].name, WEXITSTATUS(st));
        }
    }
    if (skipped == (int)(sizeof sections / sizeof sections[0]))
    {
        printf("skip: no display\n");
        return 77;
    }
    printf("%d sections failed\n", failed);
    return failed ? 1 : 0;
}
