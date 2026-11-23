/*
 * test_core_edge.c - the core past its limits, over the fake backend: a frame with far more events than the list
 * holds (keys, text, wheel, buttons, pointer), where what is kept and what is lost is part of the contract:
 *   - the list holds FRAME_EVENT_MAX events; once it is full a pointer, size or position event replaces the last of
 *     its kind (a mouse move keeps the sum of both deltas) and anything else that cannot push one of those out is
 *     not put in the list, the oldest events are the ones kept;
 *   - the state (key_down, mouse buttons, wheel, position) and the key and char queues are filled whatever happens
 *     to the list, so a polled program loses nothing; the queues hold one entry less than their length and drop
 *     the newest.
 */
#include "platform.h"
#include "backend_fake.h"
#include "core_internal.h"

#include <stdio.h>
#include <stdlib.h>
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

/* straight into the core, as a backend does while it pumps: the fake backend would hold them until the next frame */
static void push_key(PlatformWindow *w, int key, bool down)
{
    Event e = {.type = EVENT_KEY};
    e.data.key.key = key;
    e.data.key.down = down;
    core_push_event(&w->core, &e);
}

static void push_move(PlatformWindow *w, int x, int y)
{
    Event e = {.type = EVENT_MOUSE_MOVE};
    e.data.mouse.dx = x - w->core.in.mouse_x;
    e.data.mouse.dy = y - w->core.in.mouse_y;
    e.data.mouse.x = x;
    e.data.mouse.y = y;
    core_push_event(&w->core, &e);
}

static void push_wheel(PlatformWindow *w, float y)
{
    Event e = {.type = EVENT_MOUSE_WHEEL};
    e.data.wheel.y = y;
    core_push_event(&w->core, &e);
}

static void push_button(PlatformWindow *w, int button, bool down)
{
    Event e = {.type = EVENT_MOUSE_BUTTON};
    e.data.mouse.button = button;
    e.data.mouse.down = down;
    core_push_event(&w->core, &e);
}

static void push_char(PlatformWindow *w, uint32_t cp)
{
    Event e = {.type = EVENT_CHAR};
    e.data.codepoint = cp;
    core_push_event(&w->core, &e);
}

static int drain(PlatformWindow *w, Event *out, int cap)
{
    int n = 0;
    Event e;
    while (poll_event(w, &e))
    {
        if (n < cap)
            out[n] = e;
        n++;
    }
    return n;
}

/* 5000 key events in one frame: the first FRAME_EVENT_MAX are listed in order, the rest are not, the state is the
   last one, and the key queue has the first presses. */
static void test_key_flood(PlatformWindow *w)
{
    window_begin_frame(w);
    while (key_get_pressed(w))
    {
    }
    const int total = 5000;
    for (int i = 0; i < total; i++)
        push_key(w, KEY_A + (i / 2) % 5, i % 2 == 0);
    /* the last event is a release of KEY_A + (2499 % 5) */
    Event *list = calloc((size_t)total, sizeof *list);
    int n = drain(w, list, total);
    CHECK(n == FRAME_EVENT_MAX);
    int in_order = 1;
    for (int i = 0; i < n && i < FRAME_EVENT_MAX; i++)
        in_order &= list[i].type == EVENT_KEY && list[i].data.key.key == KEY_A + (i / 2) % 5 &&
                    list[i].data.key.down == (i % 2 == 0);
    CHECK(in_order);
    for (int k = KEY_A; k <= KEY_A + 4; k++)
        CHECK(!key_down(w, k)); /* every key ended up released */
    int popped = 0, first = key_get_pressed(w);
    CHECK(first == KEY_A);
    for (int k = first; k; k = key_get_pressed(w))
        popped++;
    CHECK(popped == KEYCODE_QUEUE_LEN - 1);
    free(list);
}

/* 1000 wheel notches, 1000 text events, a button click and 1000 pointer moves among them: the sum of the wheel and the
   last position are exact, the click is seen, the char queue holds its first CHAR_QUEUE_LEN - 1. */
static void test_mixed_flood(PlatformWindow *w)
{
    window_begin_frame(w);
    while (char_get_pressed(w))
    {
    }
    for (int i = 0; i < 1000; i++)
    {
        push_wheel(w, (i % 5) == 0 ? -1.0f : 1.0f);
        push_char(w, 'a' + i % 26);
        push_move(w, i, 2 * i);
        if (i == 400)
        {
            push_button(w, MOUSE_LEFT, true);
            push_button(w, MOUSE_LEFT, false);
        }
    }
    Event list[FRAME_EVENT_MAX + 8];
    int n = drain(w, list, FRAME_EVENT_MAX + 8);
    CHECK(n <= FRAME_EVENT_MAX);
    float wx = 0, wy = 0;
    mouse_wheel_v(w, &wx, &wy);
    CHECK(wy == 600.0f && wx == 0.0f); /* 800 notches of +1 and 200 of -1 */
    CHECK(mouse_x(w) == 999 && mouse_y(w) == 1998);
    CHECK(mouse_button_down(w, MOUSE_LEFT)); /* press and release in one frame: down until the next one */
    /* the pointer events are all pushed out by the keys, text and wheel that follow: the list may hold none of them,
       the position is the state's */
    int popped = 0;
    uint32_t c = char_get_pressed(w);
    CHECK(c == 'a');
    for (; c; c = char_get_pressed(w))
        popped++;
    CHECK(popped == CHAR_QUEUE_LEN - 1);
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    CHECK(mouse_button_released(w, MOUSE_LEFT));
}

/* A full list of keys, then pointer moves: a move cannot push a key out, it is the keys that stay; a move after a
   mover made room folds into it. */
static void test_movers_in_full_list(PlatformWindow *w)
{
    window_begin_frame(w);
    for (int i = 0; i < FRAME_EVENT_MAX - 1; i++)
        push_key(w, KEY_B, i % 2 == 0);
    push_move(w, 1, 1); /* the list is now full, with one mover at the end */
    for (int i = 0; i < 100; i++)
        push_move(w, 2 + i, 3);
    push_key(w, KEY_C, true); /* pushes the mover out: it was the only one that could go */
    push_key(w, KEY_C, false); /* nothing left to push out: not listed */
    Event list[FRAME_EVENT_MAX];
    int n = drain(w, list, FRAME_EVENT_MAX);
    CHECK(n == FRAME_EVENT_MAX);
    CHECK(list[FRAME_EVENT_MAX - 1].type == EVENT_KEY && list[FRAME_EVENT_MAX - 1].data.key.key == KEY_C &&
          list[FRAME_EVENT_MAX - 1].data.key.down);
    CHECK(mouse_x(w) == 101 && mouse_y(w) == 3);
    CHECK(!key_down(w, KEY_C));
}

/* A backend whose frames the system calls (the browser: emscripten_set_main_loop never returns to app_run) puts each frame in
   a scope of its own, core_scope_enter / core_scope_leave, instead of the one app_run holds around a loop: what the frame
   destroys is freed when the frame's scope ends, not before (the frame goes on using the window) and not never (app_run never
   comes back to free it). The sequence of backend_web.c's main_tick, on the fake backend. */
static void test_frame_scopes_defer_destroy(void)
{
    int before = core_window_count();
    for (int tick = 0; tick < 3; tick++)
    {
        PlatformWindow *w = make();
        CHECK(w != NULL);
        if (!w)
            return;
        CHECK(core_window_count() == before + 1);
        core_scope_enter();
        window_begin_frame(w);
        window_destroy(w); /* in the frame callback */
        CHECK(window_should_close(w));           /* closed from now on... */
        CHECK(core_window_count() == before + 1); /* ...and still there for the rest of the tick (window_swap) */
        window_swap(w);
        bool done = window_should_close(w);
        core_scope_leave();
        CHECK(done);
        CHECK(core_window_count() == before); /* freed when the tick left its scope */
    }
    /* scopes nest: the outermost frees */
    PlatformWindow *w = make();
    CHECK(w != NULL);
    if (!w)
        return;
    core_scope_enter();
    core_scope_enter();
    window_destroy(w);
    core_scope_leave();
    CHECK(core_window_count() == before + 1);
    core_scope_leave();
    CHECK(core_window_count() == before);
}

/* A finger at a coordinate that is not a number, or too large for an int (a driver or a bug: the system sends finite values):
   the mouse it drives and touch_x / touch_y get a clamped pixel, not the undefined result of converting the float (the fuzzer
   found it: inf is outside the range of representable values of type int). */
static void test_touch_coordinates_out_of_range(void)
{
    PlatformWindow *w = make();
    CHECK(w != NULL);
    if (!w)
        return;
    const float inf = __builtin_inff(), nan = __builtin_nanf("");
    fake_real_touch(w, 1, inf, -inf, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 1000000000 && mouse_y(w) == -1000000000);
    CHECK(touch_count(w) == 1);
    CHECK(touch_x(w, 0) == 1000000000 && touch_y(w, 0) == -1000000000);
    fake_real_touch(w, 1, nan, 1e38f, TOUCH_MOVE);
    window_begin_frame(w);
    CHECK(mouse_x(w) == 0 && mouse_y(w) == 1000000000);
    fake_real_touch(w, 1, 1e20f, nan, TOUCH_UP);
    window_begin_frame(w);
    CHECK(touch_count(w) == 0);
    window_destroy(w);
}

int main(void)
{
    if (!platform_init())
        return 1;
    test_frame_scopes_defer_destroy();
    test_touch_coordinates_out_of_range();
    PlatformWindow *w = make();
    CHECK(w != NULL);
    if (!w)
        return 1;
    test_key_flood(w);
    test_mixed_flood(w);
    test_movers_in_full_list(w);
    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
