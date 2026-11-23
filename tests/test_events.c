/*
 * test_events.c - the event hook, text as an event, lock-key state and mouse
 * capture, over the fake backend.
 */
#include "platform.h"
#include "backend_fake.h"
#include "core_internal.h"
#include "gamepad_internal.h"

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

static PlatformWindow *make(void)
{
    WindowConfig cfg = {.title = "t", .width = 320, .height = 200, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    return window_create(&cfg);
}

typedef struct
{
    EventType types[32];
    int n;
    PlatformWindow *seen_window;
    void *seen_user;
    bool key_down_inside_hook; /* what key_down said while the hook ran for a KEY down */
} Seen;

static void hook(PlatformWindow *w, const Event *e, void *user)
{
    Seen *s = user;
    if (s->n < 32)
        s->types[s->n++] = e->type;
    s->seen_window = w;
    s->seen_user = user;
    if (e->type == EVENT_KEY && e->data.key.down)
        s->key_down_inside_hook = key_down(w, e->data.key.key);
}

static void test_hook(PlatformWindow *w)
{
    Seen seen = {0};
    window_set_event_hook(w, hook, &seen);

    fake_key(w, KEY_A, true, false);
    fake_mouse_move(w, 10, 20);
    fake_inject_char(w, 'a');
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_wheel(w, 0, 1);
    fake_touch(w, 3, 5, 6, TOUCH_DOWN);
    fake_resize(w, 400, 300);
    window_begin_frame(w);

    CHECK(seen.n == 7);
    EventType want[] = {EVENT_KEY, EVENT_MOUSE_MOVE, EVENT_CHAR, EVENT_MOUSE_BUTTON, EVENT_MOUSE_WHEEL, EVENT_TOUCH, EVENT_WINDOW_RESIZE};
    for (int i = 0; i < 7 && i < seen.n; i++)
        CHECK(seen.types[i] == want[i]);
    CHECK(seen.seen_window == w);
    CHECK(seen.seen_user == &seen);
    CHECK(!seen.key_down_inside_hook); /* the hook runs before the polled state moves */
    CHECK(key_down(w, KEY_A));         /* ... which has moved by the time begin_frame returns */

    /* the hook did not take the events away from poll_event */
    int polled = 0;
    Event e;
    while (poll_event(w, &e))
        polled++;
    CHECK(polled == 7);

    window_set_event_hook(w, NULL, NULL);
    int before = seen.n;
    fake_key(w, KEY_B, true, false);
    window_begin_frame(w);
    CHECK(seen.n == before);

    fake_touch(w, 3, 5, 6, TOUCH_UP); /* the finger this test put down */
    fake_key(w, KEY_A, false, false);
    fake_key(w, KEY_B, false, false);
    fake_mouse_button(w, MOUSE_LEFT, false);
    window_begin_frame(w);
}

/* More events in one frame than the frame list holds (a slow frame, a fast mouse, a window being dragged): what
   only says "it is now here" is merged, and a key or a button is never lost for the sake of pointer motion. */
/* A storm of captured-mouse moves: the moves that are merged or dropped keep their relative motion, so the
   dx/dy read from the events add up to everything that was pushed. */
static void test_event_storm_deltas(PlatformWindow *w)
{
    window_begin_frame(w);
    while (poll_event(w, &(Event){0}))
    {
    }
    const int moves = 4 * FRAME_EVENT_MAX;
    for (int i = 0; i < moves; i++)
    {
        Event m = {.type = EVENT_MOUSE_MOVE};
        m.data.mouse.x = i;
        m.data.mouse.dx = 1;
        m.data.mouse.dy = -2;
        core_push_event(&w->core, &m);
        if (i % 8 == 0)
        {
            Event k = {.type = EVENT_KEY};
            k.data.key.key = KEY_A;
            k.data.key.down = (i % 16) == 0;
            core_push_event(&w->core, &k);
        }
    }
    int dx = 0, dy = 0;
    Event e;
    while (poll_event(w, &e))
        if (e.type == EVENT_MOUSE_MOVE)
        {
            dx += e.data.mouse.dx;
            dy += e.data.mouse.dy;
        }
    CHECK(dx == moves);
    CHECK(dy == -2 * moves);
}

static void push_move(PlatformWindow *w, int x, int y)
{
    Event m = {.type = EVENT_MOUSE_MOVE};
    m.data.mouse.x = x;
    m.data.mouse.y = y;
    core_push_event(&w->core, &m);
}

static void push_button(PlatformWindow *w, int button, bool down)
{
    Event b = {.type = EVENT_MOUSE_BUTTON};
    b.data.mouse.button = button;
    b.data.mouse.down = down;
    core_push_event(&w->core, &b);
}

/* A press, a drag, a release and the pointer moving on, all in one frame that holds more than the list does: the
   events still come in the order they happened, so the release is never ahead of a move that came before it, nor
   behind one that came after, and none of the buttons is lost. */
static void test_button_order_in_full_list(PlatformWindow *w)
{
    window_begin_frame(w);
    while (poll_event(w, &(Event){0}))
    {
    }
    push_move(w, 100, 80);
    push_button(w, MOUSE_LEFT, true);
    for (int i = 0; i < 3 * FRAME_EVENT_MAX; i++)
        push_move(w, 20 + (i * 7) % 250, 20 + (i * 3) % 150);
    push_move(w, 111, 77);
    push_button(w, MOUSE_LEFT, false);
    push_move(w, 222, 133);
    push_move(w, 233, 144);

    Event e;
    int n = 0, down = -1, up = -1, last_before = -1, first_after = -1, last_move = -1;
    Event list[FRAME_EVENT_MAX];
    while (poll_event(w, &e) && n < FRAME_EVENT_MAX)
        list[n++] = e;
    for (int i = 0; i < n; i++)
    {
        if (list[i].type == EVENT_MOUSE_BUTTON)
        {
            if (list[i].data.mouse.down && down < 0)
                down = i;
            if (!list[i].data.mouse.down && up < 0)
                up = i;
        }
    }
    for (int i = 0; i < n; i++)
        if (list[i].type == EVENT_MOUSE_MOVE)
        {
            last_move = i;
            if (up >= 0 && i < up)
                last_before = i;
            else if (up >= 0 && first_after < 0)
                first_after = i;
        }
    CHECK(down >= 0); /* the press is not pushed out by the moves */
    CHECK(up > down);
    CHECK(last_before > down && list[last_before].data.mouse.x == 111 && list[last_before].data.mouse.y == 77);
    CHECK(first_after > up);
    CHECK(last_move == n - 1 && list[last_move].data.mouse.x == 233 && list[last_move].data.mouse.y == 144);
    CHECK(mouse_x(w) == 233 && mouse_y(w) == 144);
}

/* The focus going away lets go of what is held, in the stream too (not only in the polled state), once; the
   release the system sends later for the same press is not a second one. */
static void test_focus_loss_releases(PlatformWindow *w)
{
    window_begin_frame(w);
    while (poll_event(w, &(Event){0}))
    {
    }
    touch_set_mouse_emulation(w, true);
    fake_mouse_move(w, 40, 50);
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_button(w, MOUSE_RIGHT, true);
    window_begin_frame(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT) && mouse_button_down(w, MOUSE_RIGHT));
    while (poll_event(w, &(Event){0}))
    {
    }

    Event lost = {.type = EVENT_WINDOW_FOCUS};
    lost.data.focus.gained = false;
    fake_inject_event(w, &lost);
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && !mouse_button_down(w, MOUSE_RIGHT));
    Event e;
    int releases = 0, focus_at = -1, last_release = -1, touch_up = 0, i = 0;
    while (poll_event(w, &e))
    {
        if (e.type == EVENT_MOUSE_BUTTON && !e.data.mouse.down)
        {
            releases++;
            last_release = i;
            CHECK(e.data.mouse.x == 40 && e.data.mouse.y == 50);
        }
        if (e.type == EVENT_WINDOW_FOCUS)
            focus_at = i;
        if (e.type == EVENT_TOUCH && e.data.touch.phase == TOUCH_UP)
            touch_up++;
        i++;
    }
    CHECK(releases == 2);
    CHECK(focus_at > last_release && last_release >= 0); /* the releases come before the loss */
    CHECK(touch_up == 1); /* the finger the left button was standing for lifted too */

    /* the system's own release, late, for a press that was already let go of */
    fake_mouse_button(w, MOUSE_LEFT, false);
    window_begin_frame(w);
    CHECK(!poll_event(w, &e));

    /* the next click is a whole click again, a finger included */
    Event back = {.type = EVENT_WINDOW_FOCUS};
    back.data.focus.gained = true;
    fake_inject_event(w, &back);
    fake_mouse_button(w, MOUSE_LEFT, true);
    window_begin_frame(w);
    int touch_down = 0;
    while (poll_event(w, &e))
        if (e.type == EVENT_TOUCH && e.data.touch.phase == TOUCH_DOWN)
            touch_down++;
    CHECK(touch_down == 1);
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    fake_mouse_button(w, MOUSE_LEFT, false);
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    touch_set_mouse_emulation(w, false);
}

/* A click whose press and release both arrive before the frame begins is still a click for a program that reads
   the state once a frame: down (and pressed) in this frame, released in the next. */
static void test_click_in_one_frame(PlatformWindow *w)
{
    window_begin_frame(w);
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_button(w, MOUSE_LEFT, false);
    window_begin_frame(w);
    CHECK(mouse_button_pressed(w, MOUSE_LEFT) && mouse_button_down(w, MOUSE_LEFT) && !mouse_button_released(w, MOUSE_LEFT));
    int presses = 0, releases = 0;
    Event e;
    while (poll_event(w, &e))
        if (e.type == EVENT_MOUSE_BUTTON)
        {
            if (e.data.mouse.down)
                presses++;
            else
                releases++;
        }
    CHECK(presses == 1 && releases == 1); /* the events keep their own order and number */
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && mouse_button_released(w, MOUSE_LEFT) && !mouse_button_pressed(w, MOUSE_LEFT));
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && !mouse_button_released(w, MOUSE_LEFT));

    /* a click made of frames of its own is not delayed */
    fake_mouse_button(w, MOUSE_RIGHT, true);
    window_begin_frame(w);
    CHECK(mouse_button_pressed(w, MOUSE_RIGHT));
    fake_mouse_button(w, MOUSE_RIGHT, false);
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_RIGHT) && mouse_button_released(w, MOUSE_RIGHT));

    /* press, release, press again in one frame: it is one click, still held, and the release that follows lets go */
    fake_mouse_button(w, MOUSE_MIDDLE, true);
    fake_mouse_button(w, MOUSE_MIDDLE, false);
    fake_mouse_button(w, MOUSE_MIDDLE, true);
    window_begin_frame(w);
    CHECK(mouse_button_down(w, MOUSE_MIDDLE));
    fake_mouse_button(w, MOUSE_MIDDLE, false);
    window_begin_frame(w);
    CHECK(mouse_button_released(w, MOUSE_MIDDLE) && !mouse_button_down(w, MOUSE_MIDDLE));
}

static void test_event_storm(PlatformWindow *w)
{
    window_begin_frame(w);
    while (poll_event(w, &(Event){0}))
    {
    }
    for (int i = 0; i < 3 * FRAME_EVENT_MAX; i++)
    {
        Event m = {.type = EVENT_MOUSE_MOVE};
        m.data.mouse.x = i;
        m.data.mouse.y = i + 1;
        core_push_event(&w->core, &m);
        if (i % 3 == 0)
        {
            Event r = {.type = EVENT_WINDOW_RESIZE};
            r.data.resize.w = 300 + i;
            r.data.resize.h = 200;
            core_push_event(&w->core, &r);
            Event f = {.type = EVENT_WINDOW_FB_RESIZE};
            f.data.resize.w = 300 + i;
            f.data.resize.h = 200;
            core_push_event(&w->core, &f);
        }
    }
    Event k = {.type = EVENT_KEY};
    k.data.key.key = KEY_Q;
    k.data.key.down = true;
    core_push_event(&w->core, &k);
    Event c = {.type = EVENT_CHAR};
    c.data.codepoint = 'q';
    core_push_event(&w->core, &c);
    Event b = {.type = EVENT_MOUSE_BUTTON};
    b.data.mouse.button = MOUSE_LEFT;
    b.data.mouse.down = true;
    core_push_event(&w->core, &b);
    Event last_resize = {.type = EVENT_WINDOW_RESIZE};
    last_resize.data.resize.w = 1999;
    last_resize.data.resize.h = 777;
    core_push_event(&w->core, &last_resize);
    Event last_fb = {.type = EVENT_WINDOW_FB_RESIZE};
    last_fb.data.resize.w = 1999;
    last_fb.data.resize.h = 777;
    core_push_event(&w->core, &last_fb);
    Event last_move = {.type = EVENT_MOUSE_MOVE};
    last_move.data.mouse.x = 4242;
    last_move.data.mouse.y = 4243;
    core_push_event(&w->core, &last_move);

    int keys = 0, chars = 0, buttons = 0, total = 0;
    int resize_w = -1, resize_h = -1, fb_w = -1, move_x = -1, move_y = -1;
    EventType order[4];
    int ordered = 0;
    Event e;
    while (poll_event(w, &e))
    {
        total++;
        if (e.type == EVENT_KEY && e.data.key.key == KEY_Q && e.data.key.down)
        {
            keys++;
            if (ordered < 4)
                order[ordered++] = e.type;
        }
        else if (e.type == EVENT_CHAR && e.data.codepoint == 'q')
        {
            chars++;
            if (ordered < 4)
                order[ordered++] = e.type;
        }
        else if (e.type == EVENT_MOUSE_BUTTON && e.data.mouse.down)
        {
            buttons++;
            if (ordered < 4)
                order[ordered++] = e.type;
        }
        else if (e.type == EVENT_WINDOW_RESIZE)
        {
            resize_w = e.data.resize.w;
            resize_h = e.data.resize.h;
        }
        else if (e.type == EVENT_WINDOW_FB_RESIZE)
            fb_w = e.data.resize.w;
        else if (e.type == EVENT_MOUSE_MOVE)
        {
            move_x = e.data.mouse.x;
            move_y = e.data.mouse.y;
        }
    }
    CHECK(total <= FRAME_EVENT_MAX);
    CHECK(keys == 1);
    CHECK(chars == 1);
    CHECK(buttons == 1);
    CHECK(ordered == 3 && order[0] == EVENT_KEY && order[1] == EVENT_CHAR && order[2] == EVENT_MOUSE_BUTTON);
    CHECK(resize_w == 1999 && resize_h == 777);
    CHECK(fb_w == 1999);
    CHECK(move_x == 4242 && move_y == 4243);
    /* the polled state never depended on the list */
    int mx = 0, my = 0;
    mouse_position(w, &mx, &my);
    CHECK(mx == 4242 && my == 4243);
    CHECK(key_down(w, KEY_Q));
    CHECK(mouse_button_down(w, MOUSE_LEFT));

    Event up = {.type = EVENT_KEY};
    up.data.key.key = KEY_Q;
    core_push_event(&w->core, &up);
    Event bup = {.type = EVENT_MOUSE_BUTTON};
    bup.data.mouse.button = MOUSE_LEFT;
    core_push_event(&w->core, &bup);
    window_begin_frame(w);
    while (char_get_pressed(w))
    {
    }
}

static void test_text_event(PlatformWindow *w)
{
    window_begin_frame(w);
    while (char_get_pressed(w))
    {
    }

    fake_inject_char(w, 0xE9); /* é */
    fake_inject_char(w, 0x4E2D);
    window_begin_frame(w);

    int chars = 0;
    uint32_t cps[4] = {0};
    Event e;
    while (poll_event(w, &e))
        if (e.type == EVENT_CHAR && chars < 4)
            cps[chars++] = e.data.codepoint;
    CHECK(chars == 2);
    CHECK(cps[0] == 0xE9 && cps[1] == 0x4E2D);

    /* ... and the queue still hands each one out exactly once */
    CHECK(char_get_pressed(w) == 0xE9);
    CHECK(char_get_pressed(w) == 0x4E2D);
    CHECK(char_get_pressed(w) == 0);
}

/* The text rule: a key pressed while Ctrl or Super is held is a shortcut and types nothing; Shift and AltGr (Ctrl and Alt
   together on Windows and the web) still type; the clipboard shortcuts of old arrive as the key and its modifier. */
static void drain_events(PlatformWindow *w, int *chars, uint32_t *first, int *key_mods)
{
    Event e;
    *chars = 0;
    *key_mods = -1;
    window_begin_frame(w);
    while (poll_event(w, &e))
    {
        if (e.type == EVENT_CHAR && (*chars)++ == 0)
            *first = e.data.codepoint;
        else if (e.type == EVENT_KEY && e.data.key.down && e.data.key.key != KEY_LEFT_CONTROL &&
                 e.data.key.key != KEY_LEFT_SHIFT && e.data.key.key != KEY_LEFT_ALT && e.data.key.key != KEY_LEFT_SUPER)
            *key_mods = e.data.key.mods;
    }
}

static void test_text_rule(PlatformWindow *w)
{
    static const struct
    {
        const char *name;
        int held[2];
        int key;
        uint32_t cp;
        int want_chars, want_mods;
    } cases[] = {
        {"a", {0, 0}, KEY_A, 'a', 1, 0},
        {"Shift+a", {KEY_LEFT_SHIFT, 0}, KEY_A, 'A', 1, KEYMOD_SHIFT},
        {"Ctrl+a", {KEY_LEFT_CONTROL, 0}, KEY_A, 'a', 0, KEYMOD_CTRL},
        {"Ctrl+1", {KEY_LEFT_CONTROL, 0}, KEY_ZERO + 1, '1', 0, KEYMOD_CTRL},
        {"Ctrl+space", {KEY_LEFT_CONTROL, 0}, KEY_SPACE, ' ', 0, KEYMOD_CTRL},
        {"Ctrl+Shift+v", {KEY_LEFT_CONTROL, KEY_LEFT_SHIFT}, KEY_V, 'V', 0, KEYMOD_CTRL | KEYMOD_SHIFT},
        {"Super+a", {KEY_LEFT_SUPER, 0}, KEY_A, 'a', 0, KEYMOD_SUPER},
        {"Alt+a", {KEY_LEFT_ALT, 0}, KEY_A, 'a', 1, KEYMOD_ALT},
        {"AltGr+q (Ctrl+Alt)", {KEY_LEFT_CONTROL, KEY_LEFT_ALT}, KEY_Q, '@', 1, KEYMOD_CTRL | KEYMOD_ALT},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        window_begin_frame(w);
        for (int h = 0; h < 2; h++)
            if (cases[i].held[h])
                fake_key(w, cases[i].held[h], true, false);
        fake_key_text(w, cases[i].key, cases[i].cp);
        int chars, mods;
        uint32_t first = 0;
        drain_events(w, &chars, &first, &mods);
        if (chars != cases[i].want_chars)
            printf("  %s typed %d characters, wanted %d\n", cases[i].name, chars, cases[i].want_chars);
        CHECK(chars == cases[i].want_chars);
        CHECK(chars == 0 || first == cases[i].cp);
        CHECK(mods == cases[i].want_mods);
        fake_key(w, cases[i].key, false, false);
        for (int h = 1; h >= 0; h--)
            if (cases[i].held[h])
                fake_key(w, cases[i].held[h], false, false);
        drain_events(w, &chars, &first, &mods);
    }
}

static void test_clipboard_shortcut_keys(PlatformWindow *w)
{
    static const struct
    {
        int held, key, mods;
    } k[] = {{KEY_LEFT_SHIFT, KEY_INSERT, KEYMOD_SHIFT}, {KEY_LEFT_CONTROL, KEY_INSERT, KEYMOD_CTRL},
             {KEY_LEFT_SHIFT, KEY_DELETE, KEYMOD_SHIFT}};
    for (size_t i = 0; i < 3; i++)
    {
        window_begin_frame(w);
        fake_key(w, k[i].held, true, false);
        fake_key(w, k[i].key, true, false);
        int chars, mods;
        uint32_t first;
        drain_events(w, &chars, &first, &mods);
        CHECK(mods == k[i].mods);
        CHECK(chars == 0);
        fake_key(w, k[i].key, false, false);
        fake_key(w, k[i].held, false, false);
        drain_events(w, &chars, &first, &mods);
    }
}

static void test_lock_mods(PlatformWindow *w)
{
    window_begin_frame(w);
    CHECK((key_mods(w) & (KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK)) == 0);

    fake_set_lock_state(KEYMOD_CAPS_LOCK);
    CHECK(key_mods(w) == KEYMOD_CAPS_LOCK);

    fake_key(w, KEY_LEFT_SHIFT, true, false);
    window_begin_frame(w);
    fake_set_lock_state(KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK);
    CHECK(key_mods(w) == (KEYMOD_SHIFT | KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK));

    fake_set_lock_state(0);
    fake_key(w, KEY_LEFT_SHIFT, false, false);
    window_begin_frame(w);
    CHECK(key_mods(w) == 0);
}

static void test_capture(PlatformWindow *w)
{
    CHECK(mouse_capture(w, true));
    CHECK(!mouse_capture(w, false));
}


/* ---- gamepad and joystick connect / disconnect events, per window ---- */

static int device_events(PlatformWindow *w, EventType type, int index)
{
    int n = 0;
    Event e;
    while (poll_event(w, &e))
        n += e.type == type && e.data.device.index == index;
    return n;
}

static void test_device_events(PlatformWindow *w)
{
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_GAMEPAD_CONNECTED, 0) == 0);

    int pad = gamepad_internal_connect("Pad");
    CHECK(pad == 0);
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_GAMEPAD_CONNECTED, 0) == 1);
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_GAMEPAD_CONNECTED, 0) == 0); /* told once */

    gamepad_internal_disconnect(pad);
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_GAMEPAD_DISCONNECTED, 0) == 1);

    /* gone and back between two frames: a different device, so both events */
    pad = gamepad_internal_connect("Pad");
    window_begin_frame(w);
    poll_event(w, &(Event){0}); /* (consume) */
    gamepad_internal_disconnect(pad);
    pad = gamepad_internal_connect("Other pad");
    window_begin_frame(w);
    int disc = 0, conn = 0;
    Event e;
    EventType order[4];
    int n = 0;
    while (poll_event(w, &e))
    {
        if (e.type == EVENT_GAMEPAD_DISCONNECTED && e.data.device.index == 0)
            disc++, order[n++] = e.type;
        if (e.type == EVENT_GAMEPAD_CONNECTED && e.data.device.index == 0)
            conn++, order[n++] = e.type;
    }
    CHECK(disc == 1 && conn == 1);
    CHECK(n == 2 && order[0] == EVENT_GAMEPAD_DISCONNECTED && order[1] == EVENT_GAMEPAD_CONNECTED);

    /* a second window starts from nothing: it is told about the pad already there */
    WindowConfig cfg = {.title = "w2", .width = 100, .height = 80, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w2 = window_create(&cfg);
    CHECK(w2 != NULL);
    if (w2)
    {
        window_begin_frame(w2);
        CHECK(device_events(w2, EVENT_GAMEPAD_CONNECTED, 0) == 1);
        window_destroy(w2);
    }

    /* joysticks the same way */
    int joy = joystick_internal_connect("Stick", 2, 4, 1);
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_JOYSTICK_CONNECTED, joy) == 1);
    joystick_internal_disconnect(joy);
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_JOYSTICK_DISCONNECTED, joy) == 1);

    gamepad_internal_disconnect(pad);
    window_begin_frame(w);
    CHECK(device_events(w, EVENT_GAMEPAD_DISCONNECTED, 0) == 1);
}


/* ---- a touch screen on a desktop: the first finger is the mouse ---- */

static int count_type(PlatformWindow *w, EventType type)
{
    int n = 0;
    Event e;
    while (poll_event(w, &e))
        n += e.type == type;
    return n;
}

static void test_real_touch(PlatformWindow *w)
{
    window_begin_frame(w);
    while (poll_event(w, &(Event){0}))
    {
    }

    /* finger down: a touch, and the mouse moves to it and presses */
    fake_real_touch(w, 5, 40, 60, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(touch_count(w) == 1 && touch_id(w, 0) == 5);
    CHECK(mouse_x(w) == 40 && mouse_y(w) == 60);
    CHECK(mouse_button_down(w, MOUSE_LEFT) && mouse_button_pressed(w, MOUSE_LEFT));
    CHECK(count_type(w, EVENT_MOUSE_BUTTON) == 1);

    /* dragging it drags the mouse */
    fake_real_touch(w, 5, 90, 120, TOUCH_MOVE);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 90 && mouse_y(w) == 120 && mouse_button_down(w, MOUSE_LEFT));

    /* the system's own echo of the touch (a mouse event) is dropped while a finger is down */
    fake_mouse_move(w, 7, 7);
    fake_mouse_button(w, MOUSE_RIGHT, true);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 90 && mouse_y(w) == 120);
    CHECK(!mouse_button_down(w, MOUSE_RIGHT));
    CHECK(count_type(w, EVENT_MOUSE_MOVE) == 0);

    /* a second finger is a touch only: the mouse stays with the first */
    fake_real_touch(w, 6, 200, 10, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(touch_count(w) == 2);
    CHECK(mouse_x(w) == 90 && mouse_button_down(w, MOUSE_LEFT));
    CHECK(count_type(w, EVENT_MOUSE_BUTTON) == 0);
    fake_real_touch(w, 6, 210, 20, TOUCH_MOVE);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 90 && mouse_y(w) == 120);

    /* the first finger lifts: the mouse releases, and the second does not take over */
    fake_real_touch(w, 5, 90, 120, TOUCH_UP);
    window_begin_frame(w);
    CHECK(touch_count(w) == 1);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && mouse_button_released(w, MOUSE_LEFT));
    fake_real_touch(w, 6, 230, 40, TOUCH_MOVE);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 90);

    /* all up: real mouse events count again */
    fake_real_touch(w, 6, 230, 40, TOUCH_UP);
    window_begin_frame(w);
    CHECK(touch_count(w) == 0);
    fake_mouse_move(w, 11, 22);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 11 && mouse_y(w) == 22);

    /* a cancelled touch also lets go of the mouse */
    fake_real_touch(w, 9, 5, 5, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT));
    fake_real_touch(w, 9, 5, 5, TOUCH_CANCEL);
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT) && touch_count(w) == 0);

    /* gestures see a real touch like any other */
    fake_real_touch(w, 1, 100, 100, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(gesture_is_detected(w, GESTURE_TAP));
    fake_real_touch(w, 1, 100, 100, TOUCH_UP);
    window_begin_frame(w);

    /* with mouse->touch emulation on, a real finger is not doubled by its own mouse echo */
    touch_set_mouse_emulation(w, true);
    fake_real_touch(w, 2, 30, 30, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(touch_count(w) == 1);
    fake_real_touch(w, 2, 30, 30, TOUCH_UP);
    window_begin_frame(w);
    CHECK(touch_count(w) == 0);
    touch_set_mouse_emulation(w, false);
}


/* ---- text input and the input method ---- */

static void test_text_input(PlatformWindow *w)
{
    window_begin_frame(w);
    while (char_get_pressed(w))
    {
    }
    CHECK(window_text_input_active(w) && fake_text_input_on(w));
    CHECK(strcmp(window_text_composition(w), "") == 0);

    /* an input method composing: edit events, and the composition is readable */
    fake_text_edit(w, "ni", 2);
    window_begin_frame(w);
    Event e;
    bool saw = false;
    while (poll_event(w, &e))
        if (e.type == EVENT_TEXT_EDIT)
            saw = strcmp(e.data.edit.text, "ni") == 0 && e.data.edit.cursor == 2;
    CHECK(saw);
    CHECK(strcmp(window_text_composition(w), "ni") == 0);

    /* committing: an empty edit, then the characters */
    fake_text_edit(w, "", 0);
    fake_inject_char(w, 0x4F60);
    window_begin_frame(w);
    CHECK(strcmp(window_text_composition(w), "") == 0);
    CHECK(char_get_pressed(w) == 0x4F60);

    /* a long composition is cut to what the event holds, never overflowed */
    char longtext[200];
    memset(longtext, 'z', sizeof longtext - 1);
    longtext[sizeof longtext - 1] = '\0';
    fake_text_edit(w, longtext, 10);
    window_begin_frame(w);
    CHECK(strlen(window_text_composition(w)) == 63);
    fake_text_edit(w, "", 0);
    window_begin_frame(w);

    /* the caret rectangle reaches the platform */
    window_set_text_input_rect(w, 10, 20, 3, 16);
    int r[4];
    fake_text_input_rect(w, r);
    CHECK(r[0] == 10 && r[1] == 20 && r[2] == 3 && r[3] == 16);

    /* stopping: no input method, no characters, no composition */
    fake_text_edit(w, "ab", 2);
    window_begin_frame(w);
    CHECK(strcmp(window_text_composition(w), "ab") == 0);
    window_text_input_stop(w);
    CHECK(!window_text_input_active(w) && !fake_text_input_on(w));
    CHECK(strcmp(window_text_composition(w), "") == 0);
    fake_inject_char(w, 'q');
    fake_key(w, KEY_Q, true, false);
    window_begin_frame(w);
    CHECK(char_get_pressed(w) == 0);       /* no text */
    CHECK(key_down(w, KEY_Q));             /* keys still work */
    int chars = 0;
    while (poll_event(w, &e))
        chars += e.type == EVENT_CHAR;
    CHECK(chars == 0);
    fake_key(w, KEY_Q, false, false);

    window_text_input_start(w);
    CHECK(window_text_input_active(w) && fake_text_input_on(w));
    fake_inject_char(w, 'q');
    window_begin_frame(w);
    CHECK(char_get_pressed(w) == 'q');
}

static HitTestResult hit_fn(PlatformWindow *w, int x, int y, void *user)
{
    (void)w;
    int *calls = user;
    (*calls)++;
    if (y < 30)
        return x > 280 ? HIT_NORMAL : HIT_DRAG;
    if (x >= 316)
        return HIT_RESIZE_RIGHT;
    return HIT_NORMAL;
}

static void test_window_features(void)
{
    WindowConfig cfg = {.title = "f", .width = 320, .height = 200, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return;

    /* decorations: on by default, off for undecorated, popups and tooltips, switchable */
    CHECK(fake_is_decorated(w));
    window_set_decorated(w, false);
    CHECK(!fake_is_decorated(w));
    window_set_decorated(w, true);
    CHECK(fake_is_decorated(w));

    WindowConfig pc = cfg;
    pc.kind = WINDOW_KIND_POPUP;
    pc.parent = w;
    PlatformWindow *popup = window_create(&pc);
    CHECK(popup && !fake_is_decorated(popup));
    pc.kind = WINDOW_KIND_NORMAL;
    pc.undecorated = true;
    PlatformWindow *bare = window_create(&pc);
    CHECK(bare && !fake_is_decorated(bare));
    if (popup)
        window_destroy(popup);
    if (bare)
        window_destroy(bare);

    /* hit test */
    int calls = 0;
    CHECK(fake_hit_test(w, 10, 10) == HIT_NORMAL); /* none set yet */
    window_set_hit_test(w, hit_fn, &calls);
    CHECK(fake_hit_test(w, 100, 10) == HIT_DRAG);
    CHECK(fake_hit_test(w, 318, 100) == HIT_RESIZE_RIGHT);
    CHECK(fake_hit_test(w, 100, 100) == HIT_NORMAL);
    CHECK(calls == 3);
    window_set_hit_test(w, NULL, NULL);
    CHECK(fake_hit_test(w, 100, 10) == HIT_NORMAL);

    /* cursor images */
    uint32_t px[4 * 4];
    for (int i = 0; i < 16; i++)
        px[i] = 0xFF000000u | (uint32_t)i;
    CHECK(cursor_create(NULL, 4, 4, 0, 0) == NULL);
    CHECK(cursor_create(px, 0, 4, 0, 0) == NULL);
    CHECK(cursor_create(px, 4, 4, 4, 0) == NULL);  /* the hot spot is outside */
    CHECK(cursor_create(px, 4, 4, 0, -1) == NULL);
    CHECK(cursor_create(px, 300, 4, 0, 0) == NULL); /* too large */
    PlatformCursor *c = cursor_create(px, 4, 4, 1, 2);
    CHECK(c != NULL);
    mouse_set_cursor_image(w, c);
    CHECK(fake_cursor_image(w) == c);
    mouse_set_cursor_image(w, NULL);
    CHECK(fake_cursor_image(w) == NULL);
    cursor_destroy(c);
    cursor_destroy(NULL); /* harmless */

    window_destroy(w);
}

/* ---- what a script's events carry: the same fields the X11 backend fills from the server's state ---- */

static int frame_events(PlatformWindow *w, Event *out, int cap)
{
    window_begin_frame(w);
    int n = 0;
    Event e;
    while (poll_event(w, &e))
        if (n < cap)
            out[n++] = e;
    return n;
}

static void test_fake_fidelity(PlatformWindow *w)
{
    Event ev[16];
    frame_events(w, ev, 16);
    int px, py;

    /* a click after a move is at the pointer (it was at 0, 0 whatever the pointer was) */
    fake_mouse_move(w, 10, 20);
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_button(w, MOUSE_LEFT, false);
    int n = frame_events(w, ev, 16);
    CHECK(n == 3 && ev[1].type == EVENT_MOUSE_BUTTON && ev[2].type == EVENT_MOUSE_BUTTON);
    CHECK(ev[1].data.mouse.x == 10 && ev[1].data.mouse.y == 20 && ev[2].data.mouse.x == 10 && ev[2].data.mouse.y == 20);
    fake_mouse_move(w, 30, 40); /* and it follows the pointer */
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_move(w, 31, 41);
    fake_mouse_button(w, MOUSE_LEFT, false);
    n = frame_events(w, ev, 16);
    CHECK(n == 4 && ev[1].data.mouse.x == 30 && ev[1].data.mouse.y == 40 && ev[3].data.mouse.x == 31 && ev[3].data.mouse.y == 41);

    /* a click at a place: the pointer goes there (one move), and a second click there adds none */
    fake_mouse_button_at(w, 50, 60, MOUSE_RIGHT, true);
    fake_mouse_button_at(w, 50, 60, MOUSE_RIGHT, false);
    n = frame_events(w, ev, 16);
    CHECK(n == 3);
    CHECK(ev[0].type == EVENT_MOUSE_MOVE && ev[0].data.mouse.x == 50 && ev[0].data.mouse.y == 60);
    CHECK(ev[1].type == EVENT_MOUSE_BUTTON && ev[1].data.mouse.down && ev[1].data.mouse.button == MOUSE_RIGHT);
    CHECK(ev[1].data.mouse.x == 50 && ev[1].data.mouse.y == 60);
    CHECK(ev[2].type == EVENT_MOUSE_BUTTON && !ev[2].data.mouse.down && ev[2].data.mouse.x == 50 && ev[2].data.mouse.y == 60);
    mouse_position(w, &px, &py);
    CHECK(px == 50 && py == 60);

    /* keys: the modifiers held before the key, a scancode */
    fake_key(w, KEY_LEFT_CONTROL, true, false);
    fake_key(w, KEY_LEFT_SHIFT, true, false);
    fake_key(w, KEY_C, true, false);
    fake_key(w, KEY_C, false, false);
    fake_key(w, KEY_LEFT_SHIFT, false, false);
    fake_key(w, KEY_LEFT_CONTROL, false, false);
    n = frame_events(w, ev, 16);
    CHECK(n == 6);
    const int want[6] = {0, KEYMOD_CTRL, KEYMOD_CTRL | KEYMOD_SHIFT, KEYMOD_CTRL | KEYMOD_SHIFT, KEYMOD_CTRL | KEYMOD_SHIFT,
                         KEYMOD_CTRL};
    for (int i = 0; i < n && i < 6; i++)
    {
        CHECK(ev[i].type == EVENT_KEY && ev[i].data.key.mods == want[i]);
        CHECK(ev[i].data.key.scancode != 0);
    }
    CHECK(key_mods(w) == 0);

    /* a captured mouse: the step of each move; a free one carries none, as in X11 */
    fake_mouse_move(w, 100, 100);
    frame_events(w, ev, 16);
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    fake_mouse_move(w, 110, 95);
    fake_mouse_move(w, 112, 95);
    n = frame_events(w, ev, 16);
    CHECK(n == 2 && ev[0].data.mouse.dx == 10 && ev[0].data.mouse.dy == -5 && ev[1].data.mouse.dx == 2 && ev[1].data.mouse.dy == 0);
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    fake_mouse_move(w, 120, 95);
    n = frame_events(w, ev, 16);
    CHECK(n == 1 && ev[0].data.mouse.dx == 0 && ev[0].data.mouse.dy == 0);

    /* a finger has pressure while it is down, as the backends report it */
    fake_touch(w, 5, 1, 2, TOUCH_DOWN);
    fake_touch(w, 5, 3, 4, TOUCH_MOVE);
    fake_touch(w, 5, 3, 4, TOUCH_UP);
    n = frame_events(w, ev, 16);
    CHECK(n == 3 && ev[0].data.touch.pressure == 1.0f && ev[1].data.touch.pressure == 1.0f && ev[2].data.touch.pressure == 0.0f);

    /* a resize is the window's size once it is delivered */
    int ww = 0, hh = 0, fw = 0, fh = 0;
    fake_resize(w, 640, 480);
    fake_fb_resize(w, 1280, 960);
    frame_events(w, ev, 16);
    window_get_size(w, &ww, &hh);
    window_get_framebuffer_size(w, &fw, &fh);
    CHECK(ww == 640 && hh == 480 && fw == 1280 && fh == 960);
}

int main(void)
{
    if (!platform_init())
        return 1;
    PlatformWindow *w = make();
    CHECK(w != NULL);
    if (!w)
        return 1;
    test_hook(w);
    test_event_storm(w);
    test_event_storm_deltas(w);
    test_button_order_in_full_list(w);
    test_focus_loss_releases(w);
    test_click_in_one_frame(w);
    test_text_event(w);
    test_text_rule(w);
    test_clipboard_shortcut_keys(w);
    test_lock_mods(w);
    test_capture(w);
    test_device_events(w);
    test_real_touch(w);
    test_text_input(w);
    test_fake_fidelity(w);
    window_destroy(w);
    test_window_features();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
