#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#include <X11/keysym.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif

#include <langinfo.h>
#include <stdio.h>
#include <string.h>

#define TITLE "test_input_x11"

static int g_pass, g_fail;
static Display *g_dpy;
static Window g_win;

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

static Window find_window(const char *title)
{
    Window root = DefaultRootWindow(g_dpy);
    Window parent;
    Window *kids = NULL;
    unsigned count = 0;
    Window found = None;
    if (!XQueryTree(g_dpy, root, &root, &parent, &kids, &count))
        return None;
    for (unsigned i = 0; i < count && found == None; i++)
    {
        char *name = NULL;
        if (XFetchName(g_dpy, kids[i], &name) && name)
        {
            if (strcmp(name, title) == 0)
                found = kids[i];
            XFree(name);
        }
    }
    if (kids)
        XFree(kids);
    return found;
}

static void send_key(KeyCode code, bool press, unsigned state)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xkey.type = press ? KeyPress : KeyRelease;
    ev.xkey.display = g_dpy;
    ev.xkey.window = g_win;
    ev.xkey.root = DefaultRootWindow(g_dpy);
    ev.xkey.subwindow = None;
    ev.xkey.time = CurrentTime;
    ev.xkey.same_screen = True;
    ev.xkey.keycode = code;
    ev.xkey.state = state;
    XSendEvent(g_dpy, g_win, False, press ? KeyPressMask : KeyReleaseMask, &ev);
    XSync(g_dpy, False);
}

static void send_focus(bool in)
{
    XEvent ev;
    memset(&ev, 0, sizeof ev);
    ev.xfocus.type = in ? FocusIn : FocusOut;
    ev.xfocus.display = g_dpy;
    ev.xfocus.window = g_win;
    ev.xfocus.mode = NotifyNormal;
    ev.xfocus.detail = NotifyNonlinear;
    XSendEvent(g_dpy, g_win, False, FocusChangeMask, &ev);
    XSync(g_dpy, False);
}

static bool wait_pressed(PlatformWindow *w, int key)
{
    for (int i = 0; i < 500; i++)
    {
        window_begin_frame(w);
        if (key_pressed(w, key))
            return true;
        time_sleep(2);
    }
    return false;
}

static bool wait_released(PlatformWindow *w, int key)
{
    for (int i = 0; i < 500; i++)
    {
        window_begin_frame(w);
        if (key_released(w, key))
            return true;
        time_sleep(2);
    }
    return false;
}

/* The modifier keys held, without the lock states the machine happens to be in. */
static int held_mods(PlatformWindow *w)
{
    return key_mods(w) & ~(KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK);
}

static void test_translation(PlatformWindow *w)
{
    static const struct
    {
        KeySym sym;
        int key;
        const char *name;
    } table[] = {
        {XK_KP_0, KEY_KP_0, "KP_0"},
        {XK_KP_1, KEY_KP_1, "KP_1"},
        {XK_KP_2, KEY_KP_2, "KP_2"},
        {XK_KP_3, KEY_KP_3, "KP_3"},
        {XK_KP_4, KEY_KP_4, "KP_4"},
        {XK_KP_5, KEY_KP_5, "KP_5"},
        {XK_KP_6, KEY_KP_6, "KP_6"},
        {XK_KP_7, KEY_KP_7, "KP_7"},
        {XK_KP_8, KEY_KP_8, "KP_8"},
        {XK_KP_9, KEY_KP_9, "KP_9"},
        {XK_KP_Decimal, KEY_KP_DECIMAL, "KP_Decimal"},
        {XK_KP_Divide, KEY_KP_DIVIDE, "KP_Divide"},
        {XK_KP_Multiply, KEY_KP_MULTIPLY, "KP_Multiply"},
        {XK_KP_Subtract, KEY_KP_SUBTRACT, "KP_Subtract"},
        {XK_KP_Add, KEY_KP_ADD, "KP_Add"},
        {XK_KP_Enter, KEY_KP_ENTER, "KP_Enter"},
        {XK_KP_Equal, KEY_KP_EQUAL, "KP_Equal"},
        {XK_Menu, KEY_MENU, "Menu"},
        {XK_Scroll_Lock, KEY_SCROLL_LOCK, "Scroll_Lock"},
        {XK_a, KEY_A, "a"},
        {XK_Return, KEY_ENTER, "Return"},
        {XK_F5, KEY_F5, "F5"},
    };
    for (size_t i = 0; i < sizeof table / sizeof table[0]; i++)
    {
        KeyCode code = XKeysymToKeycode(g_dpy, table[i].sym);
        if (code == 0)
        {
            printf("FAIL no keycode for %s in this keymap\n", table[i].name);
            g_fail++;
            continue;
        }
        send_key(code, true, 0);
        bool pressed = wait_pressed(w, table[i].key);
        if (!pressed)
            printf("  %s did not arrive as key %d\n", table[i].name, table[i].key);
        CHECK(pressed);
        CHECK(key_down(w, table[i].key));
        send_key(code, false, 0);
        CHECK(wait_released(w, table[i].key));
    }
}

static void test_modifiers(PlatformWindow *w)
{
    KeyCode shift = XKeysymToKeycode(g_dpy, XK_Shift_L);
    KeyCode ctrl = XKeysymToKeycode(g_dpy, XK_Control_R);
    KeyCode a = XKeysymToKeycode(g_dpy, XK_a);

    CHECK(held_mods(w) == 0);
    send_key(shift, true, 0);
    CHECK(wait_pressed(w, KEY_LEFT_SHIFT));
    CHECK(held_mods(w) == KEYMOD_SHIFT);
    send_key(ctrl, true, ShiftMask);
    CHECK(wait_pressed(w, KEY_RIGHT_CONTROL));
    CHECK(held_mods(w) == (KEYMOD_SHIFT | KEYMOD_CTRL));

    send_key(a, true, ShiftMask | ControlMask);
    window_begin_frame(w);
    Event ev;
    int mods = -1;
    for (int i = 0; i < 500 && mods < 0; i++)
    {
        while (poll_event(w, &ev))
        {
            if (ev.type == EVENT_KEY && ev.data.key.key == KEY_A && ev.data.key.down)
                mods = ev.data.key.mods;
        }
        if (mods < 0)
        {
            time_sleep(2);
            window_begin_frame(w);
        }
    }
    CHECK(mods == (KEYMOD_SHIFT | KEYMOD_CTRL));

    send_key(a, false, ShiftMask | ControlMask);
    CHECK(wait_released(w, KEY_A));
    send_key(shift, false, ShiftMask | ControlMask);
    CHECK(wait_released(w, KEY_LEFT_SHIFT));
    CHECK(held_mods(w) == KEYMOD_CTRL);
    send_key(ctrl, false, ControlMask);
    CHECK(wait_released(w, KEY_RIGHT_CONTROL));
    CHECK(held_mods(w) == 0);
}

/* A press of a key that is already down is an auto-repeat: the key stays down, it
   is not a new press, and the event says so. */
static void test_repeat(PlatformWindow *w)
{
    KeyCode a = XKeysymToKeycode(g_dpy, XK_a);

    send_key(a, true, 0);
    CHECK(wait_pressed(w, KEY_A));
    Event ev;
    while (poll_event(w, &ev))
    {
    }

    send_key(a, true, 0);
    int repeats = 0;
    int presses = 0;
    for (int i = 0; i < 500 && repeats == 0 && presses == 0; i++)
    {
        window_begin_frame(w);
        CHECK(!key_pressed(w, KEY_A));
        while (poll_event(w, &ev))
        {
            if (ev.type != EVENT_KEY || ev.data.key.key != KEY_A || !ev.data.key.down)
                continue;
            if (ev.data.key.repeat)
                repeats++;
            else
                presses++;
        }
        if (repeats == 0 && presses == 0)
            time_sleep(2);
    }
    CHECK(repeats == 1);
    CHECK(presses == 0);
    CHECK(key_down(w, KEY_A));

    send_key(a, false, 0);
    CHECK(wait_released(w, KEY_A));
    CHECK(!key_down(w, KEY_A));
}

static void test_focus_loss(PlatformWindow *w)
{
    KeyCode alt = XKeysymToKeycode(g_dpy, XK_Alt_L);
    KeyCode a = XKeysymToKeycode(g_dpy, XK_a);

    send_key(alt, true, 0);
    CHECK(wait_pressed(w, KEY_LEFT_ALT));
    send_key(a, true, Mod1Mask);
    CHECK(wait_pressed(w, KEY_A));
    CHECK(held_mods(w) == KEYMOD_ALT);

    send_focus(false);
    CHECK(wait_released(w, KEY_LEFT_ALT));
    CHECK(!key_down(w, KEY_A));
    CHECK(!key_down(w, KEY_LEFT_ALT));
    CHECK(held_mods(w) == 0);
    CHECK(!window_is_focused(w));

    send_focus(true); /* the real focus never left */
    for (int i = 0; i < 50 && !window_is_focused(w); i++)
    {
        window_begin_frame(w);
        time_sleep(2);
    }
    CHECK(window_is_focused(w));
}

/* A key the server never held (sent with XSendEvent) looks to the sync against the
   server's keymap like a release that went missing: it must let go by itself. */
static void test_lost_release_heals(PlatformWindow *w)
{
    KeyCode z = XKeysymToKeycode(g_dpy, XK_z);
    send_key(z, true, 0);
    CHECK(wait_pressed(w, KEY_Z));
    CHECK(key_down(w, KEY_Z));
    CHECK(wait_released(w, KEY_Z)); /* no release was ever sent */
    CHECK(!key_down(w, KEY_Z));
}

/* An input method that fell behind hands its copy of a press back after the release
   (Xlib puts it back in the queue): it is older than the release, so it is no new
   press and the key does not stick. Text input is off so no method takes it again. */
static void test_late_copy(PlatformWindow *w)
{
    KeyCode q = XKeysymToKeycode(g_dpy, XK_q);
    Display *wd = window_native_handle(w, NATIVE_DISPLAY);
    window_text_input_stop(w);
    send_key(q, true, 0);
    CHECK(wait_pressed(w, KEY_Q));
    send_key(q, false, 0);
    CHECK(wait_released(w, KEY_Q));

    XEvent copy;
    memset(&copy, 0, sizeof copy);
    copy.xkey.type = KeyPress;
    copy.xkey.serial = 1; /* older than anything on the connection */
    copy.xkey.display = wd;
    copy.xkey.window = g_win;
    copy.xkey.root = DefaultRootWindow(wd);
    copy.xkey.same_screen = True;
    copy.xkey.keycode = q;
    XPutBackEvent(wd, &copy);
    int presses = 0;
    Event ev;
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        while (poll_event(w, &ev))
            presses += ev.type == EVENT_KEY && ev.data.key.down;
        time_sleep(2);
    }
    CHECK(presses == 0);
    CHECK(!key_down(w, KEY_Q));
    window_text_input_start(w);
}

#ifdef HAVE_XTEST
static void fake_key(Display *d, KeyCode code, bool press)
{
    XTestFakeKeyEvent(d, code, press, 0);
    XSync(d, False);
}

/* Whatever has the focus gets the keys. With no window manager (Xvfb, a bare
   Xwayland) nobody gives it, so the test takes it; window_is_focused alone does not
   say, the synthetic focus events of the tests above set it too. */
static bool take_focus(PlatformWindow *w)
{
    for (int i = 0; i < 100; i++)
    {
        Window focus;
        int revert;
        XGetInputFocus(g_dpy, &focus, &revert);
        if (focus == g_win && window_is_focused(w))
            return true;
        if (i % 25 == 0)
        {
            XSetInputFocus(g_dpy, g_win, RevertToParent, CurrentTime);
            XSync(g_dpy, False);
        }
        window_begin_frame(w);
        time_sleep(5);
    }
    return false;
}

static void drain(PlatformWindow *w)
{
    Event ev;
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
        }
        time_sleep(5);
    }
}

/* A key the server has sent is in the very next frame. An input method (ibus) takes
   every key event and hands a copy back a moment later: waiting for that copy put
   each key one frame late. And its copy is not a second key event. */
static void test_same_frame(PlatformWindow *w)
{
    static const KeySym keys[] = {XK_a, XK_s, XK_d, XK_Up, XK_Left, XK_space, XK_Shift_L, XK_Return};
    const int n = (int)(sizeof keys / sizeof keys[0]);
    const int map[] = {KEY_A, KEY_S, KEY_D, KEY_UP, KEY_LEFT, KEY_SPACE, KEY_LEFT_SHIFT, KEY_ENTER};
    drain(w);
    int late = 0;
    for (int i = 0; i < n; i++)
    {
        KeyCode code = XKeysymToKeycode(g_dpy, keys[i]);
        fake_key(g_dpy, code, true);
        window_begin_frame(w);
        late += !key_pressed(w, map[i]);
        fake_key(g_dpy, code, false);
        window_begin_frame(w);
        late += !key_released(w, map[i]);
    }
    if (late)
        printf("  %d of %d key events came a frame late\n", late, 2 * n);
    CHECK(late == 0);

    int extra = 0;
    Event ev;
    for (int i = 0; i < 20; i++)
    {
        time_sleep(5);
        window_begin_frame(w);
        while (poll_event(w, &ev))
            extra += ev.type == EVENT_KEY;
    }
    CHECK(extra == 0); /* the input method's copies were not reported again */
}

/* Typed through the input method: each key once, the text once. */
static void test_text_once(PlatformWindow *w)
{
    static const KeySym word[] = {XK_h, XK_i, XK_space, XK_o, XK_o};
    if (!XSupportsLocale())
    {
        printf("SKIP text: Xlib does not support the locale\n");
        return;
    }
    drain(w);
    for (size_t i = 0; i < sizeof word / sizeof word[0]; i++)
    {
        KeyCode code = XKeysymToKeycode(g_dpy, word[i]);
        XTestFakeKeyEvent(g_dpy, code, True, 0);
        XTestFakeKeyEvent(g_dpy, code, False, 0);
    }
    XSync(g_dpy, False);
    char text[32] = "";
    int len = 0, presses = 0, releases = 0;
    Event ev;
    for (int i = 0; i < 40; i++)
    {
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
            if (ev.type == EVENT_CHAR && len < 31)
                text[len++] = (char)ev.data.codepoint;
            if (ev.type == EVENT_KEY && ev.data.key.down)
                presses++;
            else if (ev.type == EVENT_KEY)
                releases++;
        }
        time_sleep(5);
    }
    if (strcmp(text, "hi oo") != 0)
        printf("  typed \"hi oo\", got \"%s\"\n", text);
    CHECK(strcmp(text, "hi oo") == 0);
    CHECK(presses == 5);
    CHECK(releases == 5);
}

/* Another client grabbing the keyboard for a moment (a global shortcut, the input
   method's switcher) is no focus change: a held key stays down in every frame, and
   no focus event reaches the application. A release that went to the grab instead
   is found by the sync with the server's keymap. */
static void test_grab_keeps_keys(PlatformWindow *w)
{
    KeyCode up = XKeysymToKeycode(g_dpy, XK_Up);
    Window root = DefaultRootWindow(g_dpy);
    drain(w);
    fake_key(g_dpy, up, true);
    CHECK(wait_pressed(w, KEY_UP));
    int dropped = 0, focus_events = 0;
    Event ev;
    for (int round = 0; round < 3; round++)
    {
        XGrabKeyboard(g_dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
        XSync(g_dpy, False);
        for (int i = 0; i < 5; i++)
        {
            time_sleep(5);
            window_begin_frame(w);
            dropped += !key_down(w, KEY_UP);
            while (poll_event(w, &ev))
                focus_events += ev.type == EVENT_WINDOW_FOCUS;
        }
        XUngrabKeyboard(g_dpy, CurrentTime);
        XSync(g_dpy, False);
        for (int i = 0; i < 5; i++)
        {
            time_sleep(5);
            window_begin_frame(w);
            dropped += !key_down(w, KEY_UP);
            while (poll_event(w, &ev))
                focus_events += ev.type == EVENT_WINDOW_FOCUS;
        }
    }
    CHECK(dropped == 0);
    CHECK(focus_events == 0);
    CHECK(window_is_focused(w));

    XGrabKeyboard(g_dpy, root, False, GrabModeAsync, GrabModeAsync, CurrentTime);
    fake_key(g_dpy, up, false); /* goes to the grab, not to the window */
    XUngrabKeyboard(g_dpy, CurrentTime);
    XSync(g_dpy, False);
    CHECK(wait_released(w, KEY_UP));
    CHECK(!key_down(w, KEY_UP));
}

/* A dead key and a letter compose one character, with Xlib's own input method or
   ibus: the keys are reported as they come, the method's commit (a KeyPress with
   keycode 0) is text and no key. The dead key goes on a spare keycode for the test. */
static void test_dead_key(PlatformWindow *w)
{
    const char *codeset = nl_langinfo(CODESET);
    if (!XSupportsLocale() || strcmp(codeset, "UTF-8") != 0)
    {
        printf("SKIP dead key: the locale is not UTF-8 (%s)\n", codeset);
        return;
    }
    int min, max;
    XDisplayKeycodes(g_dpy, &min, &max);
    KeyCode spare = 0;
    for (int kc = max; kc >= min && !spare; kc--)
    {
        if (XkbKeycodeToKeysym(g_dpy, (KeyCode)kc, 0, 0) == NoSymbol)
            spare = (KeyCode)kc;
    }
    if (!spare)
    {
        printf("SKIP dead key: no spare keycode\n");
        return;
    }
    KeySym sym = XK_dead_acute;
    XChangeKeyboardMapping(g_dpy, spare, 1, &sym, 1);
    XSync(g_dpy, False);
    drain(w); /* the new mapping reaches the window and the input method */

    KeyCode e = XKeysymToKeycode(g_dpy, XK_e);
    fake_key(g_dpy, spare, true);
    fake_key(g_dpy, spare, false);
    fake_key(g_dpy, e, true);
    fake_key(g_dpy, e, false);
    uint32_t text[8];
    int len = 0, commits = 0;
    Event ev;
    for (int i = 0; i < 40; i++)
    {
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
            if (ev.type == EVENT_CHAR && len < 8)
                text[len++] = ev.data.codepoint;
            if (ev.type == EVENT_KEY && ev.data.key.scancode == 0)
                commits++;
        }
        time_sleep(5);
    }
    if (len != 1 || text[0] != 0xE9)
        printf("  dead acute + e gave %d characters, the first U+%04X\n", len, len ? (unsigned)text[0] : 0u);
    CHECK(len == 1 && text[0] == 0xE9);
    CHECK(commits == 0);
    CHECK(!key_down(w, KEY_E));

    sym = NoSymbol;
    XChangeKeyboardMapping(g_dpy, spare, 1, &sym, 1);
    XSync(g_dpy, False);
    drain(w);
}

/* Keys the server really holds, delivered the way a keyboard does it. */
static void test_real_keys(PlatformWindow *w)
{
    KeyCode shift = XKeysymToKeycode(g_dpy, XK_Shift_L);

    fake_key(g_dpy, shift, true);
    CHECK(wait_pressed(w, KEY_LEFT_SHIFT));
    CHECK(key_mods(w) == KEYMOD_SHIFT);
    time_sleep(400); /* held longer than the grace of the lost-release check: it must stay down */
    window_begin_frame(w);
    window_begin_frame(w);
    CHECK(key_down(w, KEY_LEFT_SHIFT));
    fake_key(g_dpy, shift, false);
    CHECK(wait_released(w, KEY_LEFT_SHIFT));
    CHECK(key_mods(w) == 0);

    /* Lose the release: the window stops listening for it while the key goes up. */
    Display *wd = window_native_handle(w, NATIVE_DISPLAY);
    XWindowAttributes attr;
    XGetWindowAttributes(wd, g_win, &attr);
    fake_key(g_dpy, shift, true);
    CHECK(wait_pressed(w, KEY_LEFT_SHIFT));
    XSelectInput(wd, g_win, attr.your_event_mask & ~KeyReleaseMask);
    XSync(wd, False);
    fake_key(g_dpy, shift, false);
    XSync(wd, False);
    XSelectInput(wd, g_win, attr.your_event_mask);
    XSync(wd, False);
    CHECK(wait_released(w, KEY_LEFT_SHIFT)); /* found stuck by the sync, not by an event */
    CHECK(!key_down(w, KEY_LEFT_SHIFT));
    CHECK(key_mods(w) == 0);
}
#endif

static void test_cursors(PlatformWindow *w)
{
    for (int round = 0; round < 3; round++)
    {
        for (int c = -2; c < CURSOR_COUNT + 3; c++)
            mouse_set_cursor(w, c);
    }
    mouse_set_mode(w, MOUSE_MODE_HIDDEN);
    mouse_set_cursor(w, CURSOR_RESIZE_NWSE);
    mouse_set_mode(w, MOUSE_MODE_NORMAL);

    WindowConfig cfg = {.title = "test_input_x11_second", .width = 32, .height = 32, .render = RENDER_PIXELS};
    PlatformWindow *w2 = window_create(&cfg);
    CHECK(w2 != NULL);
    if (w2)
    {
        mouse_set_cursor(w2, CURSOR_RESIZE_ALL);
        window_destroy(w2);
    }
    mouse_set_cursor(w, CURSOR_RESIZE_ALL);
    mouse_set_cursor(w, CURSOR_RESIZE_NESW);
    window_begin_frame(w);
    CHECK(true);
}

int main(void)
{
    if (!platform_init())
    {
        printf("SKIP %s\n", platform_get_error());
        return 77;
    }
    CHECK(platform_get_error()[0] == '\0');
    g_dpy = XOpenDisplay(NULL);
    WindowConfig cfg = {.title = TITLE, .width = 64, .height = 64, .render = RENDER_PIXELS};
    PlatformWindow *w = window_create(&cfg);
    if (!g_dpy || !w)
    {
        printf("FAIL setup\n");
        return 1;
    }
    for (int i = 0; i < 50 && !g_win; i++)
    {
        window_begin_frame(w);
        g_win = find_window(TITLE);
        time_sleep(2);
    }
    CHECK(g_win != None);

    if (g_win)
    {
        test_translation(w);
        test_modifiers(w);
        test_repeat(w);
        test_focus_loss(w);
        test_lost_release_heals(w);
        test_late_copy(w);
#ifdef HAVE_XTEST
        int xt_event, xt_error, xt_major, xt_minor;
        if (!XTestQueryExtension(g_dpy, &xt_event, &xt_error, &xt_major, &xt_minor))
            printf("SKIP real keys: no XTEST\n");
        else if (!take_focus(w))
            printf("SKIP real keys: the window never got the focus\n");
        else
        {
            test_real_keys(w);
            test_same_frame(w);
            test_text_once(w);
            test_dead_key(w);
            test_grab_keeps_keys(w);
        }
#endif
        test_cursors(w);
    }

    window_destroy(w);
    XCloseDisplay(g_dpy);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
