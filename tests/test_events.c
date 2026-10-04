/*
 * test_events.c - the event hook, text as an event, lock-key state and mouse
 * capture, over the fake backend.
 */
#include "platform.h"
#include "backend_fake.h"

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

int main(void)
{
    if (!platform_init())
        return 1;
    PlatformWindow *w = make();
    CHECK(w != NULL);
    if (!w)
        return 1;
    test_hook(w);
    test_text_event(w);
    test_lock_mods(w);
    test_capture(w);
    window_destroy(w);
    test_window_features();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
