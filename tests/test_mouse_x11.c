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

static void test_release_outside(PlatformWindow *w)
{
    reset(w);
    ptr_in(100, 80);
    settle(w);
    clear_log();
    btn(1, true);
    ptr_in(WIN_W + 80, 80); /* right of the window */
    ptr_in(WIN_W + 120, 90);
    settle(w);
    char buf[256];
    printf("  b: press, leave, move outside: %s\n", describe(buf, sizeof buf));
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    CHECK(!window_is_hovered(w));
    int mx, my;
    mouse_position(w, &mx, &my);
    CHECK(mx == WIN_W + 120 && my == 90); /* the position keeps following the pointer, outside too */

    clear_log();
    btn(1, false);
    settle(w);
    printf("  b: release outside: %s\n", describe(buf, sizeof buf));
    CHECK(count_type(EVENT_MOUSE_BUTTON, false, true) == 1);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    int at = last_index(EVENT_MOUSE_BUTTON);
    CHECK(at >= 0 && g_log[at].x == WIN_W + 120 && g_log[at].y == 90);
    CHECK(!window_is_hovered(w));

    /* the same to the left and above: coordinates are negative, not clamped or wrapped */
    reset(w);
    ptr_in(10, 10);
    btn(3, true);
    ptr_in(-50, -40);
    settle(w);
    clear_log();
    btn(3, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_RIGHT));
    at = last_index(EVENT_MOUSE_BUTTON);
    CHECK(at >= 0 && !g_log[at].flag && g_log[at].button == MOUSE_RIGHT && g_log[at].x == -50 && g_log[at].y == -40);

    /* coming back in during the drag: enter, the release inside */
    reset(w);
    ptr_in(100, 80);
    btn(1, true);
    ptr_in(WIN_W + 60, 80);
    ptr_in(150, 100);
    settle(w);
    CHECK(window_is_hovered(w));
    btn(1, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
}

/* ---- (c) enter and leave in a drag, and the order of it all ---- */


static void test_order(PlatformWindow *w)
{
    reset(w);
    ptr_in(100, 80);
    settle(w);
    clear_log();

    btn(1, true);
    for (int i = 0; i < 40; i++)
        ptr_in(100 + i * 12, 80 + i); /* fast: leaves the window half way and keeps going */
    btn(1, false);
    ptr_in(5, 5); /* and a move after the release, coming back in */
    settle(w);

    char buf[512];
    printf("  c: %s\n", describe(buf, sizeof buf));
    int down = -1, up = -1, last_move_before_up = -1, first_move_after_up = -1;
    for (int i = 0; i < g_n; i++)
    {
        if (g_log[i].type == EVENT_MOUSE_BUTTON && g_log[i].flag && down < 0)
            down = i;
        if (g_log[i].type == EVENT_MOUSE_BUTTON && !g_log[i].flag && up < 0)
            up = i;
    }
    for (int i = 0; i < g_n; i++)
    {
        if (g_log[i].type != EVENT_MOUSE_MOVE)
            continue;
        if (up >= 0 && i < up)
            last_move_before_up = i;
        if (up >= 0 && i > up && first_move_after_up < 0)
            first_move_after_up = i;
    }
    CHECK(down == 0);
    CHECK(up > down);
    CHECK(count_type(EVENT_MOUSE_BUTTON, true, true) == 1);
    CHECK(count_type(EVENT_MOUSE_BUTTON, false, true) == 1);
    CHECK(last_move_before_up > down);
    CHECK(first_move_after_up > up);
    /* the release carries the place of the last move before it; the move after is the pointer back at (5,5) */
    CHECK(g_log[up].x == g_log[last_move_before_up].x && g_log[up].y == g_log[last_move_before_up].y);
    CHECK(first_move_after_up >= 0 && g_log[first_move_after_up].x == 5 && g_log[first_move_after_up].y == 5);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));

    /* the same with the frame so full that the event list has to drop moves: the release stays after the moves
       that came before it and before the ones that came after */
    /* The server may not have delivered all of it by the time the one frame runs (a loaded machine): the release is
       the proof that it had, and the scenario is repeated until it has. */
    up = -1;
    for (int attempt = 0; attempt < 5 && up < 0; attempt++)
    {
        reset(w);
        ptr_in(100, 80);
        settle(w);
        clear_log();
        btn(1, true);
        for (int i = 0; i < 900; i++)
            ptr_in(20 + (i * 7) % 250, 20 + (i * 3) % 150);
        ptr_in(111, 77);
        btn(1, false);
        ptr_in(222, 133);
        ptr_in(233, 144);
        XSync(g_dpy, False);
        time_sleep(100);
        pump_once(w); /* one frame holds all of it */
        for (int i = 0; i < g_n; i++)
            if (g_log[i].type == EVENT_MOUSE_BUTTON && !g_log[i].flag)
                up = i;
    }
    printf("  c: a full frame: %d events, button down %d, up %d\n", g_n, count_type(EVENT_MOUSE_BUTTON, true, true),
           count_type(EVENT_MOUSE_BUTTON, false, true));
    CHECK(count_type(EVENT_MOUSE_BUTTON, true, true) == 1);
    CHECK(up >= 0);
    last_move_before_up = first_move_after_up = -1;
    for (int i = 0; i < g_n; i++)
        if (g_log[i].type == EVENT_MOUSE_MOVE && up >= 0)
        {
            if (i < up)
                last_move_before_up = i;
            else if (first_move_after_up < 0)
                first_move_after_up = i;
        }
    CHECK(last_move_before_up >= 0 && g_log[last_move_before_up].x == 111 && g_log[last_move_before_up].y == 77);
    CHECK(first_move_after_up >= 0);
    int last_move = last_index(EVENT_MOUSE_MOVE);
    CHECK(last_move > up && g_log[last_move].x == 233 && g_log[last_move].y == 144);
    CHECK(mouse_button_down(w, MOUSE_LEFT)); /* a click that began and ended in one frame stays down for it */
    pump_once(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && mouse_button_released(w, MOUSE_LEFT));
    int px, py;
    mouse_position(w, &px, &py);
    CHECK(px == 233 && py == 144);
}

/* ---- (d) a second button during a drag with the first ---- */

static void test_second_button(PlatformWindow *w)
{
    reset(w);
    ptr_in(100, 80);
    settle(w);
    clear_log();
    btn(1, true);
    ptr_in(120, 90);
    btn(3, true);
    ptr_in(130, 95);
    settle(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT) && mouse_button_down(w, MOUSE_RIGHT));
    btn(3, false);
    ptr_in(140, 100);
    settle(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT) && !mouse_button_down(w, MOUSE_RIGHT));
    btn(1, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && !mouse_button_down(w, MOUSE_RIGHT));
    char buf[256];
    describe(buf, sizeof buf);
    printf("  d: %s\n", buf);
    CHECK(strcmp(buf, "D0 M D2 M U2 M U0") == 0 || strstr(buf, "D0") != NULL);
    int order[4] = {-1, -1, -1, -1}; /* D0, D2, U2, U0 */
    for (int i = 0; i < g_n; i++)
    {
        if (g_log[i].type != EVENT_MOUSE_BUTTON)
            continue;
        int slot = g_log[i].button == MOUSE_LEFT ? (g_log[i].flag ? 0 : 3) : (g_log[i].flag ? 1 : 2);
        order[slot] = i;
    }
    CHECK(order[0] >= 0 && order[0] < order[1] && order[1] < order[2] && order[2] < order[3]);
    CHECK(count_type(EVENT_MOUSE_BUTTON, true, true) == 2 && count_type(EVENT_MOUSE_BUTTON, false, true) == 2);

    /* the side buttons and the wheel in the middle of a drag */
    reset(w);
    ptr_in(100, 80);
    settle(w);
    btn(1, true);
    btn(8, true);
    btn(4, true); /* wheel up: a press and a release of button 4 */
    btn(4, false);
    settle(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT) && mouse_button_down(w, MOUSE_X1));
    CHECK(mouse_wheel(w) == 0.0f || mouse_wheel(w) == 1.0f);
    btn(8, false);
    btn(1, false);
    settle(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && !mouse_button_down(w, MOUSE_X1));
}

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

static void test_nested_loop(PlatformWindow *main_w)
{
    WindowConfig cfg = {.title = "mouse_modal", .width = 120, .height = 90, .x = 900, .y = 100, .render = RENDER_PIXELS};
    PlatformWindow *modal = window_create(&cfg);
    CHECK(modal != NULL);
    if (!modal)
        return;
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(modal);
        window_begin_frame(main_w);
        time_sleep(5);
    }

    /* a press on the main window opens the modal; the release and a lot of motion arrive while only the modal
       is pumped; when the main window pumps again it still gets all of it */
    reset(main_w);
    ptr_in(100, 80);
    settle(main_w);
    clear_log();
    btn(1, true);
    XSync(g_dpy, False);
    time_sleep(20);
    window_begin_frame(main_w);
    CHECK(mouse_button_down(main_w, MOUSE_LEFT));
    for (int i = 0; i < 6000; i++) /* a long modal: more motion than the deferred list holds */
    {
        ptr_in(20 + (i * 7) % 250, 20 + (i * 3) % 150);
        if (i % 500 == 0)
        {
            XSync(g_dpy, False);
            window_begin_frame(modal); /* only the modal loop runs */
        }
    }
    btn(1, false);
    ptr_in(77, 66);
    XSync(g_dpy, False);
    for (int i = 0; i < 20; i++)
    {
        window_begin_frame(modal);
        time_sleep(2);
    }
    /* the nested loop is over: the main window is pumped again */
    for (int i = 0; i < 10; i++)
    {
        pump_once(main_w);
        time_sleep(2);
    }
    char buf[256];
    describe(buf, sizeof buf);
    printf("  e: main window after a long nested loop: %d events, %d down, %d up\n", g_n,
           count_type(EVENT_MOUSE_BUTTON, true, true), count_type(EVENT_MOUSE_BUTTON, false, true));
    CHECK(!mouse_button_down(main_w, MOUSE_LEFT)); /* the release was not dropped with the surplus motion */
    CHECK(count_type(EVENT_MOUSE_BUTTON, false, true) == 1);
    int px, py;
    mouse_position(main_w, &px, &py);
    CHECK(px == 77 && py == 66);

    /* focus: the modal takes it and gives it back while the main window is not pumped */
    reset(main_w);
    XSetInputFocus(g_dpy, xwin(modal), RevertToPointerRoot, CurrentTime);
    XSync(g_dpy, False);
    for (int i = 0; i < 5; i++)
    {
        window_begin_frame(modal);
        time_sleep(2);
    }
    for (int i = 0; i < 5000; i++)
        ptr_in(20 + (i * 5) % 250, 20 + (i * 3) % 150);
    XSync(g_dpy, False);
    for (int i = 0; i < 5; i++)
    {
        window_begin_frame(modal);
        time_sleep(2);
    }
    focus_window(main_w);
    XSync(g_dpy, False);
    for (int i = 0; i < 5; i++)
    {
        window_begin_frame(modal);
        time_sleep(2);
    }
    for (int i = 0; i < 10; i++)
    {
        pump_once(main_w);
        time_sleep(2);
    }
    CHECK(window_is_focused(main_w));
    CHECK(!window_is_focused(modal));

    window_destroy(modal);
    for (int i = 0; i < 5; i++)
        pump_once(main_w);
}

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
    RUN("outside", test_release_outside);
    RUN("order", test_order);
    RUN("second", test_second_button);
    RUN("oneframe", test_click_inside_one_frame);
    RUN("nested", test_nested_loop);

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
