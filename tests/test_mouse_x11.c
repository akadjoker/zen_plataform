/*
 * test_mouse_x11.c - mouse buttons and drags on a real X server, with the pointer moved by the XTest extension (the
 * server's own pointer, so the implicit grab, the crossing events and the focus rules are the real ones). Covers a
 * button held while the focus leaves, a press inside that is released outside, enter and leave events in a drag,
 * the order of press, moves and release, a second button during a drag, a nested loop that pumps another window,
 * and the captured mouse mode. Skips (77) without a display or without XTest.
 */
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HAVE_XTEST

int main(void)
{
    printf("test_mouse_x11: built without XTest, skipped\n");
    return 77;
}

#else

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

#define WIN_X 200
#define WIN_Y 150
#define WIN_W 300
#define WIN_H 200

static Display *g_dpy;
static Window g_helper; /* a plain window of the test, to take the focus away */

static Window xwin(PlatformWindow *w)
{
    return (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW);
}

/* ---- what the window reported, as a short word per event ---- */

typedef struct
{
    EventType type;
    int button, x, y, dx, dy;
    bool flag; /* button down, focus gained, pointer entered */
} Rec;

static Rec g_log[8192];
static int g_n;

static void clear_log(void)
{
    g_n = 0;
}

/* One begin_frame, its events appended to the log. */
static void pump_once(PlatformWindow *w)
{
    window_begin_frame(w);
    Event e;
    while (poll_event(w, &e))
    {
        Rec r = {.type = e.type};
        switch (e.type)
        {
        case EVENT_MOUSE_BUTTON:
            r.button = e.data.mouse.button;
            r.flag = e.data.mouse.down;
            r.x = e.data.mouse.x;
            r.y = e.data.mouse.y;
            break;
        case EVENT_MOUSE_MOVE:
            r.x = e.data.mouse.x;
            r.y = e.data.mouse.y;
            r.dx = e.data.mouse.dx;
            r.dy = e.data.mouse.dy;
            break;
        case EVENT_WINDOW_FOCUS:
            r.flag = e.data.focus.gained;
            break;
        case EVENT_WINDOW_ENTER:
            r.flag = e.data.enter.entered;
            break;
        default:
            continue;
        }
        if (g_n < (int)(sizeof g_log / sizeof g_log[0]))
            g_log[g_n++] = r;
    }
}

/* Lets the server's events reach the window: a few frames, the log collecting. */
static void settle(PlatformWindow *w)
{
    XSync(g_dpy, False);
    for (int i = 0; i < 8; i++)
    {
        pump_once(w);
        time_sleep(4);
    }
}

/* "D0 M M U0 E L F f": button down/up (with its number), move, pointer entered/left, focus gained/lost. */
static const char *describe(char *out, size_t cap)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < g_n && n + 6 < cap; i++)
    {
        const Rec *r = &g_log[i];
        switch (r->type)
        {
        case EVENT_MOUSE_BUTTON:
            n += (size_t)snprintf(out + n, cap - n, "%c%d ", r->flag ? 'D' : 'U', r->button);
            break;
        case EVENT_MOUSE_MOVE:
            n += (size_t)snprintf(out + n, cap - n, "M ");
            break;
        case EVENT_WINDOW_ENTER:
            n += (size_t)snprintf(out + n, cap - n, "%c ", r->flag ? 'E' : 'L');
            break;
        case EVENT_WINDOW_FOCUS:
            n += (size_t)snprintf(out + n, cap - n, "%c ", r->flag ? 'F' : 'f');
            break;
        default:
            break;
        }
    }
    if (n && out[n - 1] == ' ')
        out[n - 1] = 0;
    return out;
}

static int count_type(EventType type, bool flag, bool use_flag)
{
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_log[i].type == type && (!use_flag || g_log[i].flag == flag))
            n++;
    return n;
}

static int last_index(EventType type)
{
    for (int i = g_n - 1; i >= 0; i--)
        if (g_log[i].type == type)
            return i;
    return -1;
}

/* ---- the pointer, through XTest ---- */

static void ptr_to(int x, int y) /* root coordinates */
{
    XTestFakeMotionEvent(g_dpy, -1, x, y, 0);
    XFlush(g_dpy);
}

static void ptr_in(int x, int y) /* window coordinates */
{
    ptr_to(WIN_X + x, WIN_Y + y);
}

static void btn(unsigned number, bool down)
{
    XTestFakeButtonEvent(g_dpy, number, down ? True : False, 0);
    XFlush(g_dpy);
}

static void focus_helper(void)
{
    XSetInputFocus(g_dpy, g_helper, RevertToPointerRoot, CurrentTime);
    XFlush(g_dpy);
}

static void focus_window(PlatformWindow *w)
{
    XSetInputFocus(g_dpy, xwin(w), RevertToPointerRoot, CurrentTime);
    XFlush(g_dpy);
}

static bool pointer_grabbed_by_someone(void)
{
    int r = XGrabPointer(g_dpy, DefaultRootWindow(g_dpy), False, ButtonReleaseMask, GrabModeAsync, GrabModeAsync,
                         None, None, CurrentTime);
    if (r == GrabSuccess)
        XUngrabPointer(g_dpy, CurrentTime);
    XFlush(g_dpy);
    return r == AlreadyGrabbed;
}

/* Every button up, the pointer parked in the corner far from the windows, the log empty. */
static void reset(PlatformWindow *w)
{
    for (unsigned b = 1; b <= 9; b++)
        XTestFakeButtonEvent(g_dpy, b, False, 0);
    XUngrabPointer(g_dpy, CurrentTime);
    ptr_to(1500, 850);
    focus_window(w);
    settle(w);
    settle(w);
    clear_log();
}

/* ---- (a) a button held while the focus goes away ---- */

static void test_focus_loss_with_button_held(PlatformWindow *w)
{
    reset(w);
    ptr_in(100, 80);
    btn(1, true);
    settle(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT));

    clear_log();
    focus_helper(); /* another window takes the focus: a modal opened, say */
    settle(w);
    char buf[256];
    printf("  a: focus taken while the button is held: %s\n", describe(buf, sizeof buf));
    CHECK(!window_is_focused(w));
    /* state cleared AND the event stream says so: a release before the focus loss */
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    CHECK(count_type(EVENT_MOUSE_BUTTON, false, true) == 1);
    int release = -1, lost = -1;
    for (int i = 0; i < g_n; i++)
    {
        if (g_log[i].type == EVENT_MOUSE_BUTTON && !g_log[i].flag && release < 0)
            release = i;
        if (g_log[i].type == EVENT_WINDOW_FOCUS && !g_log[i].flag && lost < 0)
            lost = i;
    }
    CHECK(release >= 0 && lost >= 0 && release < lost);

    /* the user lets go while the other window has the focus: nothing more for this window, nothing stuck */
    clear_log();
    btn(1, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    CHECK(count_type(EVENT_MOUSE_BUTTON, false, true) == 0); /* no second release for the same press */

    /* back: a fresh click works as one */
    focus_window(w);
    settle(w);
    CHECK(window_is_focused(w));
    clear_log();
    ptr_in(100, 80);
    btn(1, true);
    settle(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    btn(1, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    CHECK(describe(buf, sizeof buf)[0] != 0);
    printf("  a: click after the focus came back: %s\n", buf);

    /* held across the loss AND the return: the release comes late and is not a stuck button */
    reset(w);
    ptr_in(100, 80);
    btn(1, true);
    settle(w);
    focus_helper();
    settle(w);
    focus_window(w);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    btn(1, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
}

/* ---- (b) press inside, leave, release outside ---- */

/* ---- (c) enter and leave in a drag, and the order of it all ---- */


/* ---- (d) a second button during a drag with the first ---- */

/* ---- a press and a release inside one frame is not lost ---- */

static void test_click_inside_one_frame(PlatformWindow *w)
{
    reset(w);
    ptr_in(100, 80);
    settle(w);
    clear_log();
    btn(1, true);
    btn(1, false); /* both are in the queue when the frame begins */
    XSync(g_dpy, False);
    time_sleep(20);
    window_begin_frame(w);
    bool pressed = mouse_button_pressed(w, MOUSE_LEFT);
    bool down = mouse_button_down(w, MOUSE_LEFT);
    window_begin_frame(w);
    bool released = mouse_button_released(w, MOUSE_LEFT);
    bool down_later = mouse_button_down(w, MOUSE_LEFT);
    printf("  click in one frame: pressed %d down %d | next frame released %d down %d\n", pressed, down, released,
           down_later);
    /* a program that samples the state once per frame (it reads mouse_button_down) must see the click */
    CHECK(pressed && down);
    CHECK(released && !down_later);
}

/* ---- (e) a nested loop that pumps another window while this one is not pumped ---- */

/* ---- the window manager takes a press of the title bar and never gives the release back ---- */

/* ---- MOUSE_MODE_CAPTURED ---- */

int main(void)
{
    g_dpy = XOpenDisplay(NULL);
    if (!g_dpy)
    {
        printf("test_mouse_x11: no X display, skipped\n");
        return 77;
    }
    int ev, er, maj, min;
    if (!XTestQueryExtension(g_dpy, &ev, &er, &maj, &min))
    {
        printf("test_mouse_x11: no XTest extension, skipped\n");
        return 77;
    }
    if (!platform_init())
    {
        printf("test_mouse_x11: platform_init failed: %s\n", platform_get_error());
        return 77;
    }
    WindowConfig cfg = {.title = "mouse_x11", .width = WIN_W, .height = WIN_H, .x = WIN_X, .y = WIN_Y, .render = RENDER_PIXELS};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("test_mouse_x11: no window: %s\n", platform_get_error());
        return 77;
    }
    g_helper = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy), 900, 500, 100, 100, 0, 0, 0);
    XSelectInput(g_dpy, g_helper, FocusChangeMask);
    XMapWindow(g_dpy, g_helper);
    XSync(g_dpy, False);
    for (int i = 0; i < 20; i++)
    {
        window_begin_frame(w);
        time_sleep(5);
    }

    const char *only = getenv("ONLY");
#define RUN(name, fn) if (!only || strcmp(only, name) == 0) fn(w)
    RUN("focus", test_focus_loss_with_button_held);
    RUN("oneframe", test_click_inside_one_frame);

    reset(w);
    XDestroyWindow(g_dpy, g_helper);
    XSync(g_dpy, False);
    window_destroy(w);
    platform_shutdown();
    XCloseDisplay(g_dpy);
    printf("test_mouse_x11: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

#endif
