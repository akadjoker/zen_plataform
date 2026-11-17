/*
 * test_cocoa_window.c - the macOS backend's window, events and monitors on a real window server. Plain C through the public
 * API, as test_x11_window.c is; built only where the native backend is. Skips (77) when there is no graphical session,
 * unless ZP_REQUIRE_DISPLAY is set, which makes that a failure (the CI job sets it: a runner without a window server
 * must not turn into a green run that tested nothing).
 *
 * Window servers are asynchronous (a minimize animates, a zoom takes a moment), so what a call changes is waited for
 * with a bounded pump, never asserted at once. What the platform cannot be made to do from here (the runner's session
 * may refuse to minimize, or to give the focus) prints a note and does not fail; what did happen must agree with itself
 * (the state says minimized, so the event must have come).
 *
 * Two oracles that do not go through the backend's own getters: CoreGraphics' window list (where the window server says
 * the window is, in the same top-left convention) and, read-only, AppKit through the Objective-C runtime's C interface.
 */
/* The Mach headers some Apple headers include declare a thread_create of their own, which platform.h's conflicts with when
   both are in one file. This test uses the library's threads nowhere, so they are kept apart here. */
#define thread_create zen_platform_thread_create
#include "platform.h"
#undef thread_create

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <malloc/malloc.h>
#include <objc/message.h>
#include <objc/runtime.h>

#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

#define NOTE(...)           \
    do                      \
    {                       \
        printf("note: ");   \
        printf(__VA_ARGS__); \
        printf("\n");       \
    } while (0)

/* ========================================================================== */
/*  events seen                                                               */
/* ========================================================================== */

typedef struct
{
    int close, resize, fb_resize, move, focus_gained, focus_lost, minimize, maximize, restore;
    int enter, leave, scale_changed;
    int resize_w, resize_h, fb_w, fb_h, move_x, move_y;
    float scale;
    int order_resize_then_fb; /* counts a RESIZE straight followed by an FB_RESIZE */
    EventType last;
} Seen;

static Seen g_seen;

static void seen_reset(void)
{
    memset(&g_seen, 0, sizeof g_seen);
}

/* Drain the frame's event list into the counters. */
static void absorb(PlatformWindow *w)
{
    Event e;
    while (poll_event(w, &e))
    {
        switch (e.type)
        {
        case EVENT_WINDOW_CLOSE:
            g_seen.close++;
            break;
        case EVENT_WINDOW_RESIZE:
            g_seen.resize++;
            g_seen.resize_w = e.data.resize.w;
            g_seen.resize_h = e.data.resize.h;
            break;
        case EVENT_WINDOW_FB_RESIZE:
            g_seen.fb_resize++;
            g_seen.fb_w = e.data.resize.w;
            g_seen.fb_h = e.data.resize.h;
            if (g_seen.last == EVENT_WINDOW_RESIZE)
                g_seen.order_resize_then_fb++;
            break;
        case EVENT_WINDOW_MOVE:
            g_seen.move++;
            g_seen.move_x = e.data.move.x;
            g_seen.move_y = e.data.move.y;
            break;
        case EVENT_WINDOW_FOCUS:
            if (e.data.focus.gained)
                g_seen.focus_gained++;
            else
                g_seen.focus_lost++;
            break;
        case EVENT_WINDOW_MINIMIZE:
            g_seen.minimize++;
            break;
        case EVENT_WINDOW_MAXIMIZE:
            g_seen.maximize++;
            break;
        case EVENT_WINDOW_RESTORE:
            g_seen.restore++;
            break;
        case EVENT_WINDOW_ENTER:
            if (e.data.enter.entered)
                g_seen.enter++;
            else
                g_seen.leave++;
            break;
        case EVENT_WINDOW_SCALE_CHANGED:
            g_seen.scale_changed++;
            g_seen.scale = e.data.scale.scale;
            break;
        default:
            break;
        }
        g_seen.last = e.type;
    }
}

static void pump_for(PlatformWindow *w, int ms)
{
    double t0 = time_seconds();
    do
    {
        window_begin_frame(w);
        absorb(w);
        time_sleep(4);
    } while (time_seconds() - t0 < ms / 1000.0);
}

/* Pump until `cond` holds or `ms` have passed; `out` says which. */
#define WAIT_UNTIL(w, cond, ms, out)                  \
    do                                                \
    {                                                 \
        double t0_ = time_seconds();                  \
        out = false;                                  \
        for (;;)                                      \
        {                                             \
            window_begin_frame(w);                    \
            absorb(w);                                \
            if (cond)                                 \
            {                                         \
                out = true;                           \
                break;                                \
            }                                         \
            if (time_seconds() - t0_ > (ms) / 1000.0) \
                break;                                \
            time_sleep(4);                            \
        }                                             \
    } while (0)

static void settle(PlatformWindow *w)
{
    pump_for(w, 150);
}

/* ========================================================================== */
/*  oracles                                                                   */
/* ========================================================================== */

/* AppKit, read only, through the Objective-C runtime: what the NSWindow says for itself. objc_msgSend has no prototype of its own
   (it is called through the type of the method), so each call casts it to that type. */
#define MSG(ret, ...) ((ret(*)(__VA_ARGS__))(void *)objc_msgSend)

static id ns_class(const char *name)
{
    return (id)objc_getClass(name);
}
static id msg_id(id o, const char *sel)
{
    return MSG(id, id, SEL)(o, sel_registerName(sel));
}
static id msg_id_ulong(id o, const char *sel, unsigned long arg)
{
    return MSG(id, id, SEL, unsigned long)(o, sel_registerName(sel), arg);
}
static void msg_void_id(id o, const char *sel, id arg)
{
    MSG(void, id, SEL, id)(o, sel_registerName(sel), arg);
}
static bool msg_bool(id o, const char *sel)
{
    return MSG(unsigned char, id, SEL)(o, sel_registerName(sel)) != 0;
}
static unsigned long msg_ulong(id o, const char *sel)
{
    return MSG(unsigned long, id, SEL)(o, sel_registerName(sel));
}
static long msg_long(id o, const char *sel)
{
    return MSG(long, id, SEL)(o, sel_registerName(sel));
}
static const char *msg_cstr(id o, const char *sel)
{
    return MSG(const char *, id, SEL)(o, sel_registerName(sel));
}
static SEL msg_sel(id o, const char *sel)
{
    return MSG(SEL, id, SEL)(o, sel_registerName(sel));
}

static id ns_app(void)
{
    return msg_id(ns_class("NSApplication"), "sharedApplication");
}
static id ns_window(PlatformWindow *w)
{
    return (id)window_native_handle(w, NATIVE_WINDOW);
}

/* The window server's view of this process's windows (layer 0: ordinary windows), through CoreGraphics. Frame bounds,
   in the library's convention: origin at the top left of the primary display, y down, the title bar included. */
typedef struct
{
    int count;
    CGRect bounds[16];
} ServerWindows;

static ServerWindows server_windows(void)
{
    ServerWindows sw;
    memset(&sw, 0, sizeof sw);
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list)
        return sw;
    pid_t me = getpid();
    for (CFIndex i = 0; i < CFArrayGetCount(list) && sw.count < 16; i++)
    {
        CFDictionaryRef d = CFArrayGetValueAtIndex(list, i);
        int pid = 0, layer = -1;
        CFNumberRef n = CFDictionaryGetValue(d, kCGWindowOwnerPID);
        if (n)
            CFNumberGetValue(n, kCFNumberIntType, &pid);
        n = CFDictionaryGetValue(d, kCGWindowLayer);
        if (n)
            CFNumberGetValue(n, kCFNumberIntType, &layer);
        if (pid != me || layer != 0)
            continue;
        CGRect r;
        CFDictionaryRef b = CFDictionaryGetValue(d, kCGWindowBounds);
        if (b && CGRectMakeWithDictionaryRepresentation(b, &r))
            sw.bounds[sw.count++] = r;
    }
    CFRelease(list);
    return sw;
}

/* ========================================================================== */
/*  geometry, monitors, scale                                                 */
/* ========================================================================== */

static void test_monitors(void)
{
    int n = monitor_count();
    CHECK(n >= 1);
    int primaries = 0;
    for (int i = 0; i < n; i++)
    {
        MonitorInfo m;
        CHECK(monitor_get_info(i, &m));
        printf("  monitor %d: '%s' %d,%d %dx%d work %d,%d %dx%d  %d x %d mm  %d Hz  scale %.1f%s\n", i, m.name ? m.name : "(null)", m.x, m.y, m.width, m.height,
               m.work_x, m.work_y, m.work_w, m.work_h, m.phys_width_mm, m.phys_height_mm, m.refresh_hz, (double)m.content_scale, m.primary ? "  primary" : "");
        CHECK(m.index == i);
        CHECK(m.name && m.name[0]);
        CHECK(m.width > 0 && m.height > 0);
        CHECK(m.refresh_hz > 0 && m.refresh_hz <= 1000);
        CHECK(m.content_scale >= 1.0f && m.content_scale <= 4.0f);
        CHECK(m.phys_width_mm >= 0 && m.phys_height_mm >= 0);
        /* the usable area lies inside the monitor */
        CHECK(m.work_w > 0 && m.work_h > 0);
        CHECK(m.work_x >= m.x && m.work_y >= m.y);
        CHECK(m.work_x + m.work_w <= m.x + m.width && m.work_y + m.work_h <= m.y + m.height);
        if (m.primary)
        {
            primaries++;
            CHECK(m.x == 0 && m.y == 0); /* the primary one is the origin of the shared space */
        }
        CHECK(monitor_from_point(m.x + m.width / 2, m.y + m.height / 2) == i);
    }
    CHECK(primaries == 1);
    MonitorInfo none;
    CHECK(!monitor_get_info(n, &none));
    CHECK(!monitor_get_info(-1, &none));
    CHECK(monitor_from_point(-100000, -100000) == -1);
}

static void test_initial(PlatformWindow *w)
{
    int ww, wh, fw, fh;
    window_get_size(w, &ww, &wh);
    window_get_framebuffer_size(w, &fw, &fh);
    float scale = window_content_scale(w);
    printf("  size %dx%d points, framebuffer %dx%d pixels, scale %.2f\n", ww, wh, fw, fh, (double)scale);
    CHECK(ww == 240 && wh == 180);
    CHECK(scale >= 1.0f && scale <= 4.0f);
    /* the framebuffer is the size times the scale, to the pixel */
    CHECK(fw == (int)lround((double)ww * (double)scale));
    CHECK(fh == (int)lround((double)wh * (double)scale));
    MonitorInfo m;
    CHECK(monitor_get_info(monitor_from_window(w), &m));
    CHECK(m.content_scale == scale); /* the window sits on a monitor of that scale */
    CHECK(window_get_mode(w) == WINDOW_WINDOWED);
}

static void test_size(PlatformWindow *w)
{
    bool ok;
    seen_reset();
    window_set_size(w, 300, 200);
    int ww, wh;
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 300 && wh == 200), 2000, ok);
    CHECK(ok);
    settle(w);
    float scale = window_content_scale(w);
    CHECK(g_seen.resize >= 1 && g_seen.resize_w == 300 && g_seen.resize_h == 200);
    CHECK(g_seen.fb_resize >= 1 && g_seen.fb_w == (int)lround(300.0 * (double)scale) && g_seen.fb_h == (int)lround(200.0 * (double)scale));
    CHECK(g_seen.order_resize_then_fb >= 1); /* the size, then the pixels */
    int fw, fh;
    window_get_framebuffer_size(w, &fw, &fh);
    CHECK(fw == g_seen.fb_w && fh == g_seen.fb_h);
    printf("  set_size 300x200: events resize=%d fb_resize=%d, framebuffer %dx%d\n", g_seen.resize, g_seen.fb_resize, fw, fh);

    /* the top left corner does not move when the size changes (as on X11 and Win32) */
    int x0, y0, x1, y1;
    window_get_position(w, &x0, &y0);
    window_set_size(w, 260, 170);
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 260 && wh == 170), 2000, ok);
    CHECK(ok);
    window_get_position(w, &x1, &y1);
    CHECK(x0 == x1 && y0 == y1);
}

static void test_position(PlatformWindow *w)
{
    bool ok;
    int x, y;
    seen_reset();
    window_set_position(w, 120, 140);
    WAIT_UNTIL(w, (window_get_position(w, &x, &y), x == 120 && y == 140), 2000, ok);
    CHECK(ok);
    printf("  set_position 120,140: reads back %d,%d\n", x, y);
    settle(w);
    CHECK(g_seen.move >= 1 && g_seen.move_x == 120 && g_seen.move_y == 140);

    /* the window server agrees, in the same convention: the frame starts at the same x, and the title bar is above the
       content area, so the frame's top is above the content's top by a title bar's height, not by a screen */
    int cw, ch;
    window_get_size(w, &cw, &ch);
    ServerWindows sw = {0};
    bool found = false;
    for (int i = 0; i < 100 && !found; i++)
    {
        sw = server_windows();
        found = sw.count >= 1;
        if (!found)
            pump_for(w, 20);
    }
    if (found)
    {
        /* the first (front) window of the process that has our size */
        CGRect r = sw.bounds[0];
        for (int i = 0; i < sw.count; i++)
            if ((int)lround(r.size.width) != cw && (int)lround(sw.bounds[i].size.width) == cw)
                r = sw.bounds[i];
        printf("  window server: frame %.0f,%.0f %.0fx%.0f; content %d,%d %dx%d\n", r.origin.x, r.origin.y, r.size.width, r.size.height, x, y, cw, ch);
        CHECK((int)lround(r.origin.x) == x);
        CHECK((int)lround(r.size.width) == cw);
        CHECK((int)lround(r.origin.y) <= y && y - (int)lround(r.origin.y) < 80);
        CHECK((int)lround(r.size.height) >= ch && (int)lround(r.size.height) - ch < 80);
    }
    else
        NOTE("the window server's list had no window of this process, the cross-check was skipped");

    /* centred on the primary monitor by the core: the middle of the window is the middle of the monitor */
    MonitorInfo m;
    CHECK(monitor_get_info(0, &m));
    window_center_on_monitor(w, 0);
    int wx = 0, wy = 0;
    WAIT_UNTIL(w, (window_get_position(w, &wx, &wy), wx == m.x + (m.width - cw) / 2 && wy == m.y + (m.height - ch) / 2), 2000, ok);
    CHECK(ok);
}

static void test_limits(PlatformWindow *w)
{
    bool ok;
    int ww, wh;
    window_set_size(w, 250, 200);
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 250 && wh == 200), 2000, ok);
    CHECK(ok);

    window_set_size_limits(w, 200, 150, 400, 300);
    window_set_size(w, 100, 100); /* below the minimum */
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 200 && wh == 150), 2000, ok);
    CHECK(ok);
    window_set_size(w, 900, 900); /* above the maximum */
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 400 && wh == 300), 2000, ok);
    CHECK(ok);
    /* one side limited, the other free: zero means no limit */
    window_set_size_limits(w, 0, 150, 0, 0);
    window_set_size(w, 700, 100);
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 700 && wh == 150), 2000, ok);
    CHECK(ok);
    /* and none at all */
    window_set_size_limits(w, 0, 0, 0, 0);
    window_set_size(w, 320, 240);
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 320 && wh == 240), 2000, ok);
    CHECK(ok);
    window_set_size(w, 240, 180);
    WAIT_UNTIL(w, (window_get_size(w, &ww, &wh), ww == 240 && wh == 180), 2000, ok);
    CHECK(ok);
}

/* ========================================================================== */
/*  title, show / hide, minimize, maximize, focus                             */
/* ========================================================================== */

static void test_title(PlatformWindow *w)
{
    const char *titles[] = {"cocoa window test", "t\xC3\xADtulo \xE2\x82\xAC \xE6\x97\xA5\xE6\x9C\xAC", ""};
    for (int i = 0; i < 3; i++)
    {
        window_set_title(w, titles[i]);
        const char *got = msg_cstr(msg_id(ns_window(w), "title"), "UTF8String");
        CHECK(got && strcmp(got, titles[i]) == 0);
    }
    window_set_title(w, NULL); /* no title is an empty one, not a crash */
    CHECK(strcmp(msg_cstr(msg_id(ns_window(w), "title"), "UTF8String"), "") == 0);
    window_set_title(w, "cocoa window test");
}

static void test_show_hide(PlatformWindow *w)
{
    bool ok;
    CHECK(window_is_visible(w));
    window_hide(w);
    WAIT_UNTIL(w, !window_is_visible(w), 2000, ok);
    CHECK(ok);
    CHECK(!msg_bool(ns_window(w), "isVisible"));
    ServerWindows sw = server_windows();
    for (int i = 0; i < 100 && sw.count != 0; i++)
    {
        pump_for(w, 20);
        sw = server_windows();
    }
    CHECK(sw.count == 0); /* the window server no longer has it on screen */

    window_show(w);
    WAIT_UNTIL(w, window_is_visible(w), 2000, ok);
    CHECK(ok);
    CHECK(msg_bool(ns_window(w), "isVisible"));

    /* a window made hidden stays hidden until it is shown */
    WindowConfig cfg = {.title = "hidden", .width = 100, .height = 80, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_VULKAN, .hidden = true};
    PlatformWindow *h = window_create(&cfg);
    CHECK(h != NULL);
    if (h)
    {
        settle(h);
        CHECK(!window_is_visible(h));
        window_show(h);
        WAIT_UNTIL(h, window_is_visible(h), 2000, ok);
        CHECK(ok);
        window_destroy(h);
    }
    settle(w);
}

static void test_minimize(PlatformWindow *w)
{
    bool ok;
    seen_reset();
    window_minimize(w);
    WAIT_UNTIL(w, window_is_minimized(w), 6000, ok);
    if (!ok)
    {
        NOTE("the window server did not minimize the window in 6 s, minimize checks skipped");
        return;
    }
    settle(w);
    CHECK(msg_bool(ns_window(w), "isMiniaturized"));
    CHECK(g_seen.minimize >= 1);
    CHECK(window_is_visible(w)); /* in the Dock, but not hidden by the program */
    printf("  minimized: minimize events %d\n", g_seen.minimize);

    seen_reset();
    window_restore(w);
    WAIT_UNTIL(w, !window_is_minimized(w), 6000, ok);
    CHECK(ok);
    settle(w);
    CHECK(g_seen.restore >= 1);
    CHECK(!msg_bool(ns_window(w), "isMiniaturized"));
    CHECK(window_is_visible(w));
    printf("  restored: restore events %d\n", g_seen.restore);
}

static void test_maximize(PlatformWindow *w)
{
    bool ok;
    int bw, bh;
    window_get_size(w, &bw, &bh);
    seen_reset();
    window_maximize(w);
    WAIT_UNTIL(w, window_is_maximized(w), 4000, ok);
    if (!ok)
    {
        NOTE("the window was not zoomed in 4 s, maximize checks skipped");
        return;
    }
    int mw, mh;
    WAIT_UNTIL(w, (window_get_size(w, &mw, &mh), mw > bw && mh > bh), 3000, ok);
    CHECK(ok);
    settle(w);
    CHECK(msg_bool(ns_window(w), "isZoomed"));
    CHECK(g_seen.maximize >= 1);
    CHECK(g_seen.resize >= 1 && g_seen.resize_w == mw && g_seen.resize_h == mh);
    printf("  maximized: %dx%d -> %dx%d, maximize events %d\n", bw, bh, mw, mh, g_seen.maximize);
    window_maximize(w); /* already zoomed: asking again changes nothing (zoom itself toggles) */
    settle(w);
    CHECK(window_is_maximized(w));

    seen_reset();
    window_restore(w);
    WAIT_UNTIL(w, !window_is_maximized(w), 4000, ok);
    CHECK(ok);
    int rw, rh;
    WAIT_UNTIL(w, (window_get_size(w, &rw, &rh), rw == bw && rh == bh), 3000, ok);
    CHECK(ok);
    settle(w);
    CHECK(g_seen.restore >= 1);
    printf("  restored: %dx%d, restore events %d\n", rw, rh, g_seen.restore);
}

static void test_focus(PlatformWindow *w)
{
    bool ok;
    seen_reset();
    window_focus(w);
    WAIT_UNTIL(w, window_is_focused(w), 3000, ok);
    if (!ok)
    {
        NOTE("the window did not become the key window in 3 s (the session may not allow it), focus checks skipped");
        return;
    }
    settle(w);
    CHECK(msg_bool(ns_window(w), "isKeyWindow"));
    CHECK(g_seen.focus_gained >= 1 || window_is_focused(w)); /* the gain was seen by now, here or at the first frame */
    printf("  focused: gained events %d\n", g_seen.focus_gained);

    /* a window that is not on screen cannot take the focus: asking is not an error and does nothing */
    PlatformWindow *w2 = NULL;
    WindowConfig cfg = {.title = "second", .width = 120, .height = 90, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_VULKAN, .hidden = true};
    w2 = window_create(&cfg);
    CHECK(w2 != NULL);
    if (w2)
    {
        window_focus(w2);
        settle(w2);
        CHECK(!window_is_visible(w2));
        CHECK(!window_is_focused(w2));
        window_show(w2);
        window_focus(w2);
        WAIT_UNTIL(w2, window_is_focused(w2), 3000, ok);
        if (ok)
        {
            /* the first window lost it, and said so */
            settle(w);
            CHECK(!window_is_focused(w));
            CHECK(g_seen.focus_lost >= 1);
        }
        window_destroy(w2);
        window_focus(w);
        WAIT_UNTIL(w, window_is_focused(w), 3000, ok);
    }
}

/* Move the pointer (CGWarpMouseCursorPosition takes the same global coordinates as the library: top left of the primary display,
   y down) and, if the window does not notice by itself, nudge it with a real move event, which a session may or may not let a
   program post. */
static void move_pointer(int x, int y, bool nudge)
{
    CGWarpMouseCursorPosition(CGPointMake(x, y));
    if (nudge)
    {
        CGEventRef move = CGEventCreateMouseEvent(NULL, kCGEventMouseMoved, CGPointMake(x + 1, y + 1), kCGMouseButtonLeft);
        if (move)
        {
            CGEventPost(kCGHIDEventTap, move);
            CFRelease(move);
        }
    }
}

static void test_pointer(PlatformWindow *w)
{
    bool ok;
    int x, y, cw, ch, px, py;
    window_get_position(w, &x, &y);
    window_get_size(w, &cw, &ch);
    MonitorInfo m;
    CHECK(monitor_get_info(monitor_from_window(w), &m));
    /* a spot of the monitor far from the window, for the pointer to start from and to leave to */
    int far_x = x + cw / 2 < m.x + m.width / 2 ? m.x + m.width - 4 : m.x + 4;
    int far_y = y + ch / 2 < m.y + m.height / 2 ? m.y + m.height - 4 : m.y + 4;
    int in_x = x + cw / 2, in_y = y + ch / 2;

    /* the global position, at once and without any event */
    move_pointer(in_x, in_y, false);
    for (int i = 0; i < 100; i++)
    {
        mouse_global_position(&px, &py);
        if (abs(px - in_x) <= 1 && abs(py - in_y) <= 1)
            break;
        pump_for(w, 10);
    }
    mouse_global_position(&px, &py);
    printf("  warped to %d,%d: mouse_global_position %d,%d\n", in_x, in_y, px, py);
    CHECK(abs(px - in_x) <= 1 && abs(py - in_y) <= 1);
    CHECK(monitor_from_mouse() == monitor_from_window(w));

    /* out of the window: if it was hovered, the leave arrives */
    seen_reset();
    bool was_hovered = window_is_hovered(w);
    move_pointer(far_x, far_y, false);
    WAIT_UNTIL(w, !window_is_hovered(w), 1500, ok);
    if (!ok)
    {
        move_pointer(far_x, far_y, true);
        WAIT_UNTIL(w, !window_is_hovered(w), 1500, ok);
    }
    if (!ok)
    {
        NOTE("the window never saw the pointer leave (the session may not deliver synthetic pointer moves), enter and leave checks skipped");
        return;
    }
    if (was_hovered)
        CHECK(g_seen.leave >= 1); /* it was inside and now it is not: the leave was raised */
    printf("  left: hovered was %d, leave events %d\n", was_hovered, g_seen.leave);

    /* and back in: the enter */
    seen_reset();
    move_pointer(in_x, in_y, false);
    WAIT_UNTIL(w, window_is_hovered(w), 1500, ok);
    if (!ok)
    {
        move_pointer(in_x, in_y, true);
        WAIT_UNTIL(w, window_is_hovered(w), 1500, ok);
    }
    CHECK(ok);
    if (ok)
    {
        settle(w);
        /* AppKit may say leave and enter again while the tracking area settles, so the count of events is not asked to be one:
           what is asked is that the events add up to the state, from a window that was not hovered. */
        printf("  entered: enter events %d, leave events %d\n", g_seen.enter, g_seen.leave);
        CHECK(g_seen.enter >= 1 && g_seen.enter - g_seen.leave == (window_is_hovered(w) ? 1 : 0));
    }
    seen_reset();
    bool hovered_before = window_is_hovered(w);
    move_pointer(far_x, far_y, true);
    WAIT_UNTIL(w, !window_is_hovered(w), 1500, ok);
    CHECK(ok);
    if (ok)
    {
        settle(w);
        printf("  left again: enter events %d, leave events %d\n", g_seen.enter, g_seen.leave);
        CHECK(g_seen.leave - g_seen.enter == (hovered_before ? 1 : 0));
    }
}

/* ========================================================================== */
/*  application: close, Quit, live callback                                   */
/* ========================================================================== */

static void test_close_and_quit(PlatformWindow *w)
{
    seen_reset();
    /* the close button: the program is asked, the window stays */
    msg_void_id(ns_window(w), "performClose:", NULL);
    pump_for(w, 200);
    CHECK(g_seen.close == 1);
    CHECK(window_is_visible(w));
    CHECK(window_should_close(w)); /* the core turned the event into a request */
    window_set_should_close(w, false);

    /* Quit: a menu with Cmd+Q, whose action ends nothing by itself but asks every window to close */
    id app = ns_app();
    CHECK(msg_long(app, "activationPolicy") == 0); /* NSApplicationActivationPolicyRegular */
    id menu = msg_id(app, "mainMenu");
    CHECK(menu != NULL);
    id first = menu ? msg_id_ulong(menu, "itemAtIndex:", 0) : NULL;
    id sub = first ? msg_id(first, "submenu") : NULL;
    id quit = sub ? msg_id_ulong(sub, "itemAtIndex:", 0) : NULL;
    CHECK(quit != NULL);
    if (quit)
    {
        CHECK(strcmp(msg_cstr(msg_id(quit, "keyEquivalent"), "UTF8String"), "q") == 0);
        CHECK(msg_sel(quit, "action") == sel_registerName("terminate:"));
        printf("  menu: '%s' (Cmd+Q)\n", msg_cstr(msg_id(quit, "title"), "UTF8String"));
    }
    seen_reset();
    msg_void_id(app, "terminate:", NULL);
    pump_for(w, 200);
    CHECK(g_seen.close == 1);
    CHECK(window_should_close(w));
    window_set_should_close(w, false);
    CHECK(window_is_visible(w));
}

static int g_live_calls;
static PlatformWindow *g_live_w;
static void live_cb(PlatformWindow *w, void *user)
{
    (void)user;
    g_live_w = w;
    g_live_calls++;
}

static void test_live_callback(PlatformWindow *w)
{
    /* AppKit's drag and resize loops cannot be started from here, but what they set off can: the view is told a live resize
       begins, and from then on the callback is called every ~16 ms, whatever the application's own loop is doing */
    id view = msg_id(ns_window(w), "contentView");
    CHECK(view != NULL);
    window_set_live_callback(w, live_cb, NULL);
    g_live_calls = 0;
    pump_for(w, 100);
    CHECK(g_live_calls == 0); /* never outside a live loop */

    msg_void_id(view, "viewWillStartLiveResize", NULL);
    pump_for(w, 400);
    int during = g_live_calls;
    printf("  live callback: %d calls in 400 ms of a live resize\n", during);
    CHECK(during >= 5 && during <= 40);
    CHECK(g_live_w == w);

    /* a size change in the middle of it is drawn at once */
    int before = g_live_calls;
    window_set_size(w, 250, 190);
    CHECK(g_live_calls > before);

    msg_void_id(view, "viewDidEndLiveResize", NULL);
    int after_end = g_live_calls;
    pump_for(w, 200);
    CHECK(g_live_calls == after_end); /* and it stops with it */

    window_set_live_callback(w, NULL, NULL);
    msg_void_id(view, "viewWillStartLiveResize", NULL);
    pump_for(w, 100);
    msg_void_id(view, "viewDidEndLiveResize", NULL);
    CHECK(g_live_calls == after_end); /* removed: no more calls */
    window_set_size(w, 240, 180);
    settle(w);
}

/* ========================================================================== */
/*  what is not there yet says so                                             */
/* ========================================================================== */

static void test_not_implemented(PlatformWindow *w)
{
    /* Every phase of the backend is in now: the GL and pixel windows, the keyboard, the mouse and the cursors, text input, the
       clipboard and drops, the dialogs, the window modes and looks, and the Vulkan surface are tested in test_gl_cocoa,
       test_pixels_cocoa, test_input_cocoa, test_text_cocoa, test_clipboard_cocoa, test_drop_cocoa, test_dialog_cocoa,
       test_window_cocoa and test_vulkan_cocoa. Here only that a window of each render mode can be made. */
    WindowConfig gl = {.title = "gl", .width = 100, .height = 100, .render = RENDER_GL};
    PlatformWindow *gw = window_create(&gl);
    CHECK(gw != NULL);
    if (gw)
    {
        CHECK(window_native_handle(gw, NATIVE_GL_CONTEXT) != NULL);
        window_destroy(gw);
    }
    WindowConfig px = {.title = "px", .width = 100, .height = 100, .render = RENDER_PIXELS};
    PlatformWindow *pw = window_create(&px);
    CHECK(pw != NULL);
    if (pw)
    {
        Framebuffer fb;
        CHECK(window_lock_pixels(pw, &fb));
        window_destroy(pw);
    }

    /* the test window is RENDER_VULKAN: it has neither a surface of pixels nor a context */
    Framebuffer fb;
    CHECK(!window_lock_pixels(w, &fb));
    CHECK(window_native_handle(w, NATIVE_GL_CONTEXT) == NULL);
    CHECK(gl_proc_address("glClear") != NULL);
    CHECK(gl_worker_create(w) == NULL);
    uint32_t n = 0;
    CHECK(vulkan_instance_extensions(&n) != NULL && n == 2);
    CHECK((key_mods(w) & ~KEYMOD_CAPS_LOCK) == 0);  /* no modifier is held on the runner (the Caps Lock light is not asked about) */
    window_request_attention(w); /* real: the Dock icon bounces, nothing to check but that it does not crash */
}

/* ========================================================================== */
/*  create and destroy                                                        */
/* ========================================================================== */

/* Under AddressSanitizer freed memory waits in a quarantine that malloc_zone_statistics counts as in use, so the heap says
   nothing about leaks there; the windows AppKit still holds are counted either way. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define ZP_ASAN 1
#endif
#endif

static size_t heap_in_use(unsigned *blocks)
{
    malloc_statistics_t st;
    memset(&st, 0, sizeof st);
    malloc_zone_statistics(NULL, &st);
    if (blocks)
        *blocks = st.blocks_in_use;
    return st.size_in_use;
}

static void test_create_destroy_many(PlatformWindow *w)
{
    enum { WARM = 30, ROUNDS = 300 };
    WindowConfig cfg = {.title = "cycle", .width = 120, .height = 90, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_VULKAN, .resizable = true};
    id app = ns_app();
    unsigned long windows_before = msg_ulong(msg_id(app, "windows"), "count");
    ServerWindows sw0 = server_windows();

    for (int i = 0; i < WARM; i++)
    {
        PlatformWindow *c = window_create(&cfg);
        CHECK(c != NULL);
        if (c)
        {
            window_begin_frame(c);
            window_destroy(c);
        }
    }
    unsigned b0, b1;
    size_t h0 = heap_in_use(&b0);
    int failed = 0;
    for (int i = 0; i < ROUNDS; i++)
    {
        PlatformWindow *c = window_create(&cfg);
        if (!c)
        {
            failed++;
            continue;
        }
        if (i % 10 == 0)
            pump_for(c, 10);
        window_set_size(c, 130 + i % 7, 95);
        window_destroy(c);
    }
    CHECK(failed == 0);
    pump_for(w, 100); /* AppKit lets go of what it held on to for a moment */
    size_t h1 = heap_in_use(&b1);
    unsigned long windows_after = msg_ulong(msg_id(app, "windows"), "count");
    ServerWindows sw1 = server_windows();
    long grown = (long)h1 - (long)h0;
    printf("  %d create/destroy rounds after %d to warm up: heap %zu -> %zu bytes (%+ld, %+.1f per round), blocks %u -> %u; "
           "NSApp windows %lu -> %lu; on screen %d -> %d\n",
           ROUNDS, WARM, h0, h1, grown, (double)grown / ROUNDS, b0, b1, windows_before, windows_after, sw0.count, sw1.count);
    CHECK(windows_after == windows_before); /* none of them is still held by AppKit */
    CHECK(sw1.count == sw0.count);
#if defined(ZP_ASAN)
    NOTE("AddressSanitizer is on: the heap check is skipped (its quarantine counts as in use)");
#else
    /* A window that is not let go of leaves its whole object graph behind: the first run without a pool around the AppKit
       calls grew 340 KB and 3000 blocks a round. What stays after the pools is a few hundred bytes a round (AppKit's own tables,
       the numbers above), so the limit is a thousand of the 300 rounds' worth of that. */
    CHECK(grown < 1024 * 1024);
    CHECK((long)b1 - (long)b0 < 6000);
#endif
}

/* A window made at a place goes there: where it was asked, or the middle of the monitor's usable area, or staggered. */
static void test_placement(void)
{
    MonitorInfo m;
    CHECK(monitor_get_info(0, &m));
    WindowConfig cfg = {.title = "placed", .width = 200, .height = 120, .x = 60, .y = 150, .render = RENDER_VULKAN};
    PlatformWindow *a = window_create(&cfg);
    CHECK(a != NULL);
    if (a)
    {
        settle(a);
        int x, y;
        window_get_position(a, &x, &y);
        CHECK(x == 60 && y == 150);
        window_destroy(a);
    }

    cfg.x = cfg.y = WINDOW_POS_CENTERED;
    cfg.monitor = 0;
    PlatformWindow *c = window_create(&cfg);
    CHECK(c != NULL);
    if (c)
    {
        settle(c);
        ServerWindows sw = {0};
        CGRect r = CGRectZero;
        bool found = false;
        for (int i = 0; i < 100 && !found; i++)
        {
            sw = server_windows();
            for (int k = 0; k < sw.count; k++)
                if ((int)lround(sw.bounds[k].size.width) == 200)
                {
                    r = sw.bounds[k];
                    found = true;
                }
            if (!found)
                pump_for(c, 20);
        }
        if (found)
        {
            double cx = r.origin.x + r.size.width / 2, cy = r.origin.y + r.size.height / 2;
            double wx = m.work_x + m.work_w / 2.0, wy = m.work_y + m.work_h / 2.0;
            printf("  centred: frame %.0f,%.0f %.0fx%.0f, middle %.1f,%.1f; usable area middle %.1f,%.1f\n", r.origin.x, r.origin.y,
                   r.size.width, r.size.height, cx, cy, wx, wy);
            CHECK(fabs(cx - wx) <= 1.0 && fabs(cy - wy) <= 1.0);
        }
        else
            NOTE("the window server's list did not show the centred window, the check was skipped");
        window_destroy(c);
    }

    /* no position given: the windows are staggered, not piled on one spot */
    cfg.x = cfg.y = WINDOW_POS_UNDEFINED;
    PlatformWindow *u1 = window_create(&cfg);
    PlatformWindow *u2 = window_create(&cfg);
    CHECK(u1 && u2);
    if (u1 && u2)
    {
        int x1, y1, x2, y2;
        window_get_position(u1, &x1, &y1);
        window_get_position(u2, &x2, &y2);
        printf("  undefined: %d,%d then %d,%d\n", x1, y1, x2, y2);
        CHECK(x1 != x2 || y1 != y2);
    }
    window_destroy(u1);
    window_destroy(u2);
}

/* One queue feeds every window, but an event belongs to the window it is for: pumping one window leaves the other's
   events for its own pump. */
static void test_two_windows(PlatformWindow *w)
{
    WindowConfig cfg = {.title = "second", .width = 160, .height = 120, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_VULKAN, .resizable = true};
    PlatformWindow *b = window_create(&cfg);
    CHECK(b != NULL);
    if (!b)
        return;
    settle(b);
    settle(w);
    window_set_size(b, 190, 110);
    seen_reset();
    pump_for(w, 150); /* only the first window is pumped */
    CHECK(g_seen.resize == 0 && g_seen.fb_resize == 0);
    seen_reset();
    pump_for(b, 150);
    printf("  second window: resize events %d (%dx%d) once it is pumped\n", g_seen.resize, g_seen.resize_w, g_seen.resize_h);
    CHECK(g_seen.resize >= 1 && g_seen.resize_w == 190 && g_seen.resize_h == 110);
    window_destroy(b);
    settle(w);
}

/* ========================================================================== */
/*  scale change: a display mode with a different scale, if the display has one  */
/* ========================================================================== */

static bool set_display_mode(CGDirectDisplayID display, CGDisplayModeRef mode)
{
    CGDisplayConfigRef config;
    if (CGBeginDisplayConfiguration(&config) != kCGErrorSuccess)
        return false;
    CGConfigureDisplayWithDisplayMode(config, display, mode, NULL);
    /* for this program only: the system puts the old mode back when the program ends, whatever happens to it */
    if (CGCompleteDisplayConfiguration(config, kCGConfigureForAppOnly) != kCGErrorSuccess)
    {
        CGCancelDisplayConfiguration(config);
        return false;
    }
    return true;
}

/* The switch itself and what the window does about it. */
static void switch_scale(PlatformWindow *w, CGDirectDisplayID display, CGDisplayModeRef current, CGDisplayModeRef other)
{
    bool ok;
    float before = window_content_scale(w);
    seen_reset();
    printf("  switching to %zux%zu pixels (scale %.0f -> %.0f)\n", CGDisplayModeGetPixelWidth(other), CGDisplayModeGetPixelHeight(other),
           (double)before, (double)CGDisplayModeGetPixelWidth(other) / (double)CGDisplayModeGetWidth(other));
    if (!set_display_mode(display, other))
    {
        NOTE("the display mode could not be changed (a session that does not allow it), scale change checks skipped");
        return;
    }
    WAIT_UNTIL(w, window_content_scale(w) != before, 6000, ok);
    if (!ok)
    {
        NOTE("the window's scale did not change in 6 s after the mode did");
        set_display_mode(display, current);
        return;
    }
    settle(w);
    float after = window_content_scale(w);
    int fw, fh, aw, ah;
    window_get_framebuffer_size(w, &fw, &fh);
    window_get_size(w, &aw, &ah);
    printf("  scale %.1f -> %.1f: scale-changed events %d (last %.1f), framebuffer %dx%d for %dx%d points\n", (double)before, (double)after,
           g_seen.scale_changed, (double)g_seen.scale, fw, fh, aw, ah);
    CHECK(g_seen.scale_changed >= 1);
    CHECK(g_seen.scale == after);
    CHECK(fw == (int)lround((double)aw * (double)after) && fh == (int)lround((double)ah * (double)after));
    CHECK(g_seen.fb_resize >= 1 && g_seen.fb_w == fw && g_seen.fb_h == fh); /* the pixels were announced */
    MonitorInfo m;
    CHECK(monitor_get_info(monitor_from_window(w), &m) && m.content_scale == after);

    /* and back */
    seen_reset();
    if (set_display_mode(display, current))
    {
        WAIT_UNTIL(w, window_content_scale(w) == before, 6000, ok);
        CHECK(ok);
        settle(w);
        CHECK(g_seen.scale_changed >= 1 && g_seen.scale == before);
        window_get_framebuffer_size(w, &fw, &fh);
        window_get_size(w, &aw, &ah);
        CHECK(fw == (int)lround((double)aw * (double)before) && fh == (int)lround((double)ah * (double)before));
    }
}

/* The backing scale factor of a window changes when the display's mode changes (the same event as a move to a screen with
   another scale: windowDidChangeBackingProperties). The runner's display may offer a mode with other pixels for the same
   points; if it does, the window is told, and the framebuffer follows. */
static void test_scale_change(PlatformWindow *w)
{
    /* It changes the display's mode (for this program only, and back), which flashes the screen of whoever runs the test: only
       when asked for, as the CI job does. */
    if (!getenv("ZP_TEST_DISPLAY_MODES"))
    {
        NOTE("ZP_TEST_DISPLAY_MODES is not set, the display mode switch (scale change) was skipped");
        return;
    }
    CGDirectDisplayID display = CGMainDisplayID();
    CGDisplayModeRef current = CGDisplayCopyDisplayMode(display);
    if (!current)
    {
        NOTE("no current display mode, scale change checks skipped");
        return;
    }
    const void *keys[1] = {kCGDisplayShowDuplicateLowResolutionModes};
    const void *vals[1] = {kCFBooleanTrue};
    CFDictionaryRef options = CFDictionaryCreate(NULL, keys, vals, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(display, options);
    size_t cw = CGDisplayModeGetWidth(current), ch = CGDisplayModeGetHeight(current);
    size_t cpw = CGDisplayModeGetPixelWidth(current), cph = CGDisplayModeGetPixelHeight(current);
    printf("  display mode now: %zux%zu points, %zux%zu pixels, %.0f Hz; %ld modes offered\n", cw, ch, cpw, cph,
           CGDisplayModeGetRefreshRate(current), modes ? (long)CFArrayGetCount(modes) : 0L);

    CGDisplayModeRef other = NULL;
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++)
    {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        size_t mw = CGDisplayModeGetWidth(m), mh = CGDisplayModeGetHeight(m);
        size_t mpw = CGDisplayModeGetPixelWidth(m), mph = CGDisplayModeGetPixelHeight(m);
        if (i < 12)
            printf("    mode %ld: %zux%zu points, %zux%zu pixels\n", (long)i, mw, mh, mpw, mph);
        /* the same size in points, at a different scale (the layout does not change, only the pixels do) */
        if (!other && mw == cw && mh == ch && mpw != cpw && CGDisplayModeIsUsableForDesktopGUI(m))
            other = m;
    }
    if (other)
        switch_scale(w, display, current, other);
    else
        NOTE("the display offers no mode with the same size in points and another scale, scale change checks skipped");
    if (modes)
        CFRelease(modes);
    if (options)
        CFRelease(options);
    CGDisplayModeRelease(current);
}

static void test_init_cycle(void)
{
    platform_shutdown();
    CHECK(platform_init());
    WindowConfig cfg = {.title = "again", .width = 100, .height = 80, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_VULKAN};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (w)
    {
        settle(w);
        int ww, wh;
        window_get_size(w, &ww, &wh);
        CHECK(ww == 100 && wh == 80);
        window_destroy(w);
    }
    /* a window made before platform_init is refused, with a reason */
    platform_shutdown();
    platform_clear_error();
    CHECK(window_create(&cfg) == NULL);
    CHECK(platform_get_error()[0] != '\0');
    CHECK(platform_init());
}

int main(void)
{
    alarm(100); /* a hard limit of the test's own, under the one ctest sets */
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return getenv("ZP_REQUIRE_DISPLAY") ? 1 : 77;
    }
    WindowConfig cfg = {.title = "cocoa window test", .width = 240, .height = 180, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_VULKAN, .resizable = true};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("%s: %s\n", getenv("ZP_REQUIRE_DISPLAY") ? "FAIL" : "skip", platform_get_error());
        return getenv("ZP_REQUIRE_DISPLAY") ? 1 : 77;
    }
    settle(w);

    printf("monitors\n");
    test_monitors();
    printf("initial state\n");
    test_initial(w);
    printf("size\n");
    test_size(w);
    printf("position\n");
    test_position(w);
    printf("limits\n");
    test_limits(w);
    printf("title\n");
    test_title(w);
    printf("show and hide\n");
    test_show_hide(w);
    printf("minimize\n");
    test_minimize(w);
    printf("maximize\n");
    test_maximize(w);
    printf("focus\n");
    test_focus(w);
    printf("pointer\n");
    test_pointer(w);
    printf("placement\n");
    test_placement();
    printf("two windows\n");
    test_two_windows(w);
    printf("close and quit\n");
    test_close_and_quit(w);
    printf("live callback\n");
    test_live_callback(w);
    printf("the stubs of the later phases\n");
    test_not_implemented(w);
    printf("scale change\n");
    test_scale_change(w);
    printf("create and destroy\n");
    test_create_destroy_many(w);

    window_destroy(w);
    printf("shutdown and init again\n");
    test_init_cycle();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
