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
#include <stdlib.h>
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
    copy.xkey.serial = 1;
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
    CHECK(extra == 0);
}

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
    fake_key(g_dpy, up, false);
    XUngrabKeyboard(g_dpy, CurrentTime);
    XSync(g_dpy, False);
    CHECK(wait_released(w, KEY_UP));
    CHECK(!key_down(w, KEY_UP));
}

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
    drain(w);

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

static void test_key_sweep(PlatformWindow *w)
{
    static const struct
    {
        KeySym sym;
        int key;
    } keys[] = {
        {XK_space, KEY_SPACE},
        {XK_apostrophe, KEY_APOSTROPHE},
        {XK_comma, KEY_COMMA},
        {XK_minus, KEY_MINUS},
        {XK_period, KEY_PERIOD},
        {XK_slash, KEY_SLASH},
        {XK_semicolon, KEY_SEMICOLON},
        {XK_equal, KEY_EQUAL},
        {XK_bracketleft, KEY_LEFT_BRACKET},
        {XK_backslash, KEY_BACKSLASH},
        {XK_bracketright, KEY_RIGHT_BRACKET},
        {XK_grave, KEY_GRAVE},
        {XK_Escape, KEY_ESCAPE},
        {XK_Return, KEY_ENTER},
        {XK_Tab, KEY_TAB},
        {XK_BackSpace, KEY_BACKSPACE},
        {XK_Insert, KEY_INSERT},
        {XK_Delete, KEY_DELETE},
        {XK_Right, KEY_RIGHT},
        {XK_Left, KEY_LEFT},
        {XK_Down, KEY_DOWN},
        {XK_Up, KEY_UP},
        {XK_Prior, KEY_PAGE_UP},
        {XK_Next, KEY_PAGE_DOWN},
        {XK_Home, KEY_HOME},
        {XK_End, KEY_END},
        {XK_Print, KEY_PRINT_SCREEN},
        {XK_Pause, KEY_PAUSE},
        {XK_KP_Decimal, KEY_KP_DECIMAL},
        {XK_KP_Divide, KEY_KP_DIVIDE},
        {XK_KP_Multiply, KEY_KP_MULTIPLY},
        {XK_KP_Subtract, KEY_KP_SUBTRACT},
        {XK_KP_Add, KEY_KP_ADD},
        {XK_KP_Enter, KEY_KP_ENTER},
        {XK_Shift_L, KEY_LEFT_SHIFT},
        {XK_Control_L, KEY_LEFT_CONTROL},
        {XK_Alt_L, KEY_LEFT_ALT},
        {XK_Super_L, KEY_LEFT_SUPER},
        {XK_Shift_R, KEY_RIGHT_SHIFT},
        {XK_Control_R, KEY_RIGHT_CONTROL},
        {XK_Super_R, KEY_RIGHT_SUPER},
        {XK_Menu, KEY_MENU},
    };
    KeySym all[128];
    int want[128];
    int n = 0;
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++, n++)
    {
        all[n] = keys[i].sym;
        want[n] = keys[i].key;
    }
    for (int i = 0; i < 26; i++, n++)
    {
        all[n] = XK_a + i;
        want[n] = KEY_A + i;
    }
    for (int i = 0; i < 10; i++, n++)
    {
        all[n] = XK_0 + i;
        want[n] = KEY_ZERO + i;
        all[++n] = XK_KP_0 + i;
        want[n] = KEY_KP_0 + i;
    }
    for (int i = 0; i < 12; i++, n++)
    {
        all[n] = XK_F1 + i;
        want[n] = KEY_F1 + i;
    }
    drain(w);
    int wrong = 0;
    for (int i = 0; i < n; i++)
    {
        KeyCode code = XKeysymToKeycode(g_dpy, all[i]);
        if (!code)
            continue;
        fake_key(g_dpy, code, true);
        window_begin_frame(w);
        int others = 0;
        for (int k = 0; k < KEY_MAX; k++)
            others += k != want[i] && key_down(w, k);
        bool ok = key_pressed(w, want[i]) && others == 0;
        fake_key(g_dpy, code, false);
        window_begin_frame(w);
        ok = ok && key_released(w, want[i]);
        if (!ok)
        {
            printf("  %s (keycode %d) is not key %d alone in its frame:", XKeysymToString(all[i]), code, want[i]);
            for (int k = 0; k < KEY_MAX; k++)
                if (key_down(w, k))
                    printf(" %d down", k);
            printf("\n");
            wrong++;
        }
    }
    CHECK(wrong == 0);
}

static void test_keys_together(PlatformWindow *w)
{
    static const KeySym held[] = {XK_w, XK_a, XK_space, XK_Shift_L, XK_Up};
    static const int key[] = {KEY_W, KEY_A, KEY_SPACE, KEY_LEFT_SHIFT, KEY_UP};
    const int n = 5;
    drain(w);
    for (int i = 0; i < n; i++)
        fake_key(g_dpy, XKeysymToKeycode(g_dpy, held[i]), true);
    window_begin_frame(w);
    int down = 0;
    for (int i = 0; i < n; i++)
        down += key_down(w, key[i]);
    CHECK(down == n);
    for (int f = 0; f < 10; f++)
    {
        time_sleep(10);
        window_begin_frame(w);
        for (int i = 0; i < n; i++)
            CHECK(key_down(w, key[i]));
    }
    static const int order[] = {2, 0, 4, 1, 3};
    for (int i = 0; i < n; i++)
    {
        fake_key(g_dpy, XKeysymToKeycode(g_dpy, held[order[i]]), false);
        window_begin_frame(w);
        CHECK(key_released(w, key[order[i]]));
        for (int j = i + 1; j < n; j++)
            CHECK(key_down(w, key[order[j]]));
    }
}

static Window g_other;

static void focus_to(PlatformWindow *w, Window to)
{
    XSetInputFocus(g_dpy, to, RevertToParent, CurrentTime);
    XSync(g_dpy, False);
    for (int i = 0; i < 20; i++)
    {
        window_begin_frame(w);
        if (window_is_focused(w) == (to == g_win))
            break;
        time_sleep(5);
    }
}

static int down_frames(PlatformWindow *w, int key, int frames)
{
    int n = 0;
    for (int i = 0; i < frames; i++)
    {
        time_sleep(5);
        window_begin_frame(w);
        n += key_down(w, key);
    }
    return n;
}

static void test_focus_away_held(PlatformWindow *w)
{
    KeyCode kw = XKeysymToKeycode(g_dpy, XK_w), ks = XKeysymToKeycode(g_dpy, XK_s);
    drain(w);
    fake_key(g_dpy, kw, true);
    window_begin_frame(w);
    CHECK(key_down(w, KEY_W));
    focus_to(w, g_other);
    CHECK(!window_is_focused(w));
    CHECK(!key_down(w, KEY_W));
    focus_to(w, g_win);
    CHECK(window_is_focused(w));
    CHECK(key_down(w, KEY_W));
    CHECK(down_frames(w, KEY_W, 10) == 10);

    focus_to(w, g_other);
    fake_key(g_dpy, kw, false);
    fake_key(g_dpy, ks, true);
    focus_to(w, g_win);
    CHECK(down_frames(w, KEY_W, 30) == 0);
    CHECK(down_frames(w, KEY_S, 10) == 10);
    fake_key(g_dpy, ks, false);
    window_begin_frame(w);
    CHECK(key_released(w, KEY_S));
    CHECK(down_frames(w, KEY_S, 30) == 0);
}

static void test_held_through_changes(PlatformWindow *w)
{
    KeyCode up = XKeysymToKeycode(g_dpy, XK_Up);
    drain(w);
    fake_key(g_dpy, up, true);
    window_begin_frame(w);
    CHECK(key_down(w, KEY_UP));
    int lost = 0;
    for (int step = 0; step < 9; step++)
    {
        switch (step)
        {
        case 0:
            window_set_title(w, "held");
            break;
        case 1:
            window_set_size(w, 96, 80);
            break;
        case 2:
            window_set_size(w, 64, 64);
            break;
        case 3:
            mouse_set_mode(w, MOUSE_MODE_CAPTURED);
            break;
        case 4:
            mouse_set_mode(w, MOUSE_MODE_NORMAL);
            break;
        case 5:
            window_text_input_stop(w);
            break;
        case 6:
            window_text_input_start(w);
            break;
        case 7:
            mouse_set_mode(w, MOUSE_MODE_HIDDEN);
            break;
        case 8:
            mouse_set_mode(w, MOUSE_MODE_NORMAL);
            break;
        }
        lost += 5 - down_frames(w, KEY_UP, 5);
    }
    if (lost)
        printf("  a held key was up in %d frames while the window changed\n", lost);
    CHECK(lost == 0);
    fake_key(g_dpy, up, false);
    window_begin_frame(w);
    CHECK(key_released(w, KEY_UP));
}

static void test_many_and_long(PlatformWindow *w)
{
    static const KeySym syms[] = {XK_q, XK_w, XK_e, XK_r, XK_t, XK_a, XK_s, XK_d, XK_f, XK_g};
    static const int keys[] = {KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_A, KEY_S, KEY_D, KEY_F, KEY_G};
    drain(w);
    while (key_get_pressed(w))
    {
    }
    for (int i = 0; i < 10; i++)
        fake_key(g_dpy, XKeysymToKeycode(g_dpy, syms[i]), true);
    window_begin_frame(w);
    int down = 0;
    for (int i = 0; i < 10; i++)
        down += key_down(w, keys[i]);
    CHECK(down == 10);
    int order = 0;
    for (int i = 0; i < 10; i++)
        order += key_get_pressed(w) == keys[i];
    CHECK(order == 10);
    for (int i = 0; i < 10; i++)
        fake_key(g_dpy, XKeysymToKeycode(g_dpy, syms[i]), false);
    window_begin_frame(w);
    down = 0;
    for (int i = 0; i < 10; i++)
        down += key_down(w, keys[i]);
    CHECK(down == 0);

    KeyCode kw = XKeysymToKeycode(g_dpy, XK_w);
    fake_key(g_dpy, kw, true);
    int frames = 0, held = 0;
    double end = time_seconds() + 2.0;
    while (time_seconds() < end)
    {
        time_sleep(16);
        window_begin_frame(w);
        frames++;
        held += key_down(w, KEY_W);
    }
    if (held != frames)
        printf("  W held 2 s: down in %d of %d frames\n", held, frames);
    CHECK(held == frames);
    fake_key(g_dpy, kw, false);
    window_begin_frame(w);
    CHECK(key_released(w, KEY_W));
}

static void pointer_to(int x, int y)
{
    int rx, ry;
    Window child;
    XTranslateCoordinates(g_dpy, g_win, DefaultRootWindow(g_dpy), x, y, &rx, &ry, &child);
    XTestFakeMotionEvent(g_dpy, -1, rx, ry, 0);
    XSync(g_dpy, False);
}

static void test_mouse_buttons(PlatformWindow *w)
{
    static const struct
    {
        unsigned xbutton;
        int button;
    } map[] = {{1, MOUSE_LEFT}, {2, MOUSE_MIDDLE}, {3, MOUSE_RIGHT}, {8, MOUSE_X1}, {9, MOUSE_X2}};
    pointer_to(10, 20);
    drain(w);
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
    {
        XTestFakeButtonEvent(g_dpy, map[i].xbutton, True, 0);
        XSync(g_dpy, False);
        window_begin_frame(w);
        bool pressed = mouse_button_pressed(w, map[i].button);
        if (!pressed)
            printf("  X button %u did not arrive as button %d in its frame\n", map[i].xbutton, map[i].button);
        CHECK(pressed);
        Event ev;
        int at = 0;
        while (poll_event(w, &ev))
            at += ev.type == EVENT_MOUSE_BUTTON && ev.data.mouse.x == 10 && ev.data.mouse.y == 20;
        CHECK(at == 1);
        XTestFakeButtonEvent(g_dpy, map[i].xbutton, False, 0);
        XSync(g_dpy, False);
        window_begin_frame(w);
        CHECK(mouse_button_released(w, map[i].button));
    }
}

static void test_mouse_wheel(PlatformWindow *w)
{
    static const struct
    {
        unsigned xbutton;
        float x, y;
    } notch[] = {{4, 0, 1}, {5, 0, -1}, {6, -1, 0}, {7, 1, 0}};
    drain(w);
    for (size_t i = 0; i < sizeof notch / sizeof notch[0]; i++)
    {
        XTestFakeButtonEvent(g_dpy, notch[i].xbutton, True, 0);
        XTestFakeButtonEvent(g_dpy, notch[i].xbutton, False, 0);
        XSync(g_dpy, False);
        window_begin_frame(w);
        float x, y;
        mouse_wheel_v(w, &x, &y);
        if (x != notch[i].x || y != notch[i].y)
            printf("  X button %u gave the wheel %g,%g\n", notch[i].xbutton, x, y);
        CHECK(x == notch[i].x && y == notch[i].y);
        for (int b = 0; b < MOUSE_BUTTON_MAX; b++)
            CHECK(!mouse_button_down(w, b));
        window_begin_frame(w);
        mouse_wheel_v(w, &x, &y);
        CHECK(x == 0 && y == 0);
    }
}

static void test_mouse_motion(PlatformWindow *w)
{
    pointer_to(5, 6);
    drain(w);
    CHECK(mouse_x(w) == 5 && mouse_y(w) == 6);
    pointer_to(40, 30);
    window_begin_frame(w);
    int dx, dy;
    mouse_delta(w, &dx, &dy);
    if (mouse_x(w) != 40 || mouse_y(w) != 30 || dx != 35 || dy != 24)
        printf("  pointer at %d,%d delta %d,%d, expected 40,30 and 35,24\n", mouse_x(w), mouse_y(w), dx, dy);
    CHECK(mouse_x(w) == 40 && mouse_y(w) == 30);
    CHECK(dx == 35 && dy == 24);
    window_begin_frame(w);
    mouse_delta(w, &dx, &dy);
    CHECK(dx == 0 && dy == 0);

    Event ev;
    int left = 0, entered = 0;
    pointer_to(200, 200);
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
            entered += ev.type == EVENT_WINDOW_ENTER && ev.data.enter.entered;
            left += ev.type == EVENT_WINDOW_ENTER && !ev.data.enter.entered;
        }
        time_sleep(2);
    }
    pointer_to(30, 30);
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        while (poll_event(w, &ev))
        {
            entered += ev.type == EVENT_WINDOW_ENTER && ev.data.enter.entered;
            left += ev.type == EVENT_WINDOW_ENTER && !ev.data.enter.entered;
        }
        time_sleep(2);
    }
    CHECK(left == 1 && entered == 1);
    CHECK(mouse_x(w) == 30 && mouse_y(w) == 30);

    mouse_set_position(w, 12, 50);
    drain(w);
    Window root, child;
    int rx, ry, wx = -1, wy = -1;
    unsigned mask;
    XQueryPointer(g_dpy, g_win, &root, &child, &rx, &ry, &wx, &wy, &mask);
    CHECK(wx == 12 && wy == 50);
    CHECK(mouse_x(w) == 12 && mouse_y(w) == 50);
}

static void test_mouse_drag_and_bursts(PlatformWindow *w)
{
    window_set_position(w, 100, 100);
    drain(w);
    pointer_to(10, 10);
    drain(w);
    XTestFakeButtonEvent(g_dpy, 1, True, 0);
    XSync(g_dpy, False);
    window_begin_frame(w);
    CHECK(mouse_button_pressed(w, MOUSE_LEFT));
    pointer_to(200, 150);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 200 && mouse_y(w) == 150);
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    pointer_to(-30, -20);
    window_begin_frame(w);
    CHECK(mouse_x(w) == -30 && mouse_y(w) == -20);
    XTestFakeButtonEvent(g_dpy, 1, False, 0);
    XSync(g_dpy, False);
    window_begin_frame(w);
    CHECK(mouse_button_released(w, MOUSE_LEFT));
    pointer_to(30, 30);
    drain(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));

    for (int i = 0; i < 5; i++)
    {
        XTestFakeButtonEvent(g_dpy, 5, True, 0);
        XTestFakeButtonEvent(g_dpy, 5, False, 0);
    }
    XSync(g_dpy, False);
    window_begin_frame(w);
    CHECK(mouse_wheel(w) == -5);

    for (int i = 0; i < 50; i++)
    {
        int rx, ry;
        Window child;
        XTranslateCoordinates(g_dpy, g_win, DefaultRootWindow(g_dpy), i % 60, (i * 7) % 60, &rx, &ry, &child);
        XTestFakeMotionEvent(g_dpy, -1, rx, ry, 0);
    }
    XSync(g_dpy, False);
    window_begin_frame(w);
    int dx, dy;
    mouse_delta(w, &dx, &dy);
    CHECK(mouse_x(w) == 49 && mouse_y(w) == (49 * 7) % 60);
    CHECK(dx == 49 - 30 && dy == (49 * 7) % 60 - 30);
}

static void test_mouse_captured_focus(PlatformWindow *w)
{
    pointer_to(20, 20);
    drain(w);
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    drain(w);
    int sum = 0, worst = 0;
    for (int phase = 0; phase < 3; phase++)
    {
        if (phase == 1)
            focus_to(w, g_other);
        if (phase == 2)
            focus_to(w, g_win);
        for (int i = 0; i < 20; i++)
        {
            XTestFakeRelativeMotionEvent(g_dpy, 3, 0, 0);
            XSync(g_dpy, False);
            time_sleep(2);
            window_begin_frame(w);
            int dx, dy;
            mouse_delta(w, &dx, &dy);
            sum += dx;
            if (abs(dx) > worst)
                worst = abs(dx);
            if (abs(dy) > worst)
                worst = abs(dy);
        }
    }
    if (worst > 3 || sum != 120)
        printf("  captured across a focus change: largest step %d, summed %d (expected 3 and 120)\n", worst, sum);
    CHECK(worst <= 3);
    CHECK(sum == 120);

    for (int i = 0; i < 20; i++)
    {
        mouse_set_mode(w, i % 2 ? MOUSE_MODE_NORMAL : MOUSE_MODE_CAPTURED);
        XTestFakeRelativeMotionEvent(g_dpy, 1, 1, 0);
        XSync(g_dpy, False);
        window_begin_frame(w);
    }
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    drain(w);
    int r = XGrabPointer(g_dpy, g_other, False, ButtonPressMask, GrabModeAsync, GrabModeAsync, None, None, CurrentTime);
    CHECK(r == GrabSuccess);
    XUngrabPointer(g_dpy, CurrentTime);
    XSync(g_dpy, False);

    XTestFakeButtonEvent(g_dpy, 3, True, 0);
    XSync(g_dpy, False);
    window_begin_frame(w);
    CHECK(mouse_button_down(w, MOUSE_RIGHT));
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    drain(w);
    CHECK(mouse_button_down(w, MOUSE_RIGHT));
    XTestFakeButtonEvent(g_dpy, 3, False, 0);
    XSync(g_dpy, False);
    window_begin_frame(w);
    CHECK(mouse_button_released(w, MOUSE_RIGHT));
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    drain(w);
}

static void test_mouse_captured(PlatformWindow *w)
{
    pointer_to(20, 20);
    drain(w);
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    drain(w);
    int sum_dx = 0, sum_dy = 0, ev_dx = 0, ev_dy = 0;
    Event ev;
    for (int i = 0; i < 60; i++)
    {
        if (i < 40)
        {
            XTestFakeRelativeMotionEvent(g_dpy, 7, -3, 0);
            XSync(g_dpy, False);
        }
        window_begin_frame(w);
        int dx, dy;
        mouse_delta(w, &dx, &dy);
        sum_dx += dx;
        sum_dy += dy;
        while (poll_event(w, &ev))
        {
            if (ev.type == EVENT_MOUSE_MOVE)
            {
                ev_dx += ev.data.mouse.dx;
                ev_dy += ev.data.mouse.dy;
            }
        }
        time_sleep(2);
    }
    if (sum_dx != 280 || sum_dy != -120 || ev_dx != 280 || ev_dy != -120)
        printf("  captured: moved 280,-120; mouse_delta summed %d,%d, the events %d,%d\n", sum_dx, sum_dy, ev_dx, ev_dy);
    CHECK(sum_dx == 280 && sum_dy == -120);
    CHECK(ev_dx == 280 && ev_dy == -120);

    sum_dx = sum_dy = 0;
    mouse_set_position(w, 5, 5);
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        int dx, dy;
        mouse_delta(w, &dx, &dy);
        sum_dx += dx;
        sum_dy += dy;
        time_sleep(2);
    }
    CHECK(sum_dx == 0 && sum_dy == 0);
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    drain(w);
}

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
            g_other = XCreateSimpleWindow(g_dpy, DefaultRootWindow(g_dpy), 300, 300, 32, 32, 0, 0, 0);
            XMapWindow(g_dpy, g_other);
            XSync(g_dpy, False);
            test_real_keys(w);
            test_same_frame(w);
            test_text_once(w);
            test_dead_key(w);
            test_grab_keeps_keys(w);
            test_key_sweep(w);
            test_keys_together(w);
            test_mouse_buttons(w);
            test_mouse_wheel(w);
            test_mouse_motion(w);
            test_mouse_captured(w);
            test_focus_away_held(w);
            test_held_through_changes(w);
            test_many_and_long(w);
            test_mouse_drag_and_bursts(w);
            test_mouse_captured_focus(w);
            XDestroyWindow(g_dpy, g_other);
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
