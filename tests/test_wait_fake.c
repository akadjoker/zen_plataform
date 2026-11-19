/*
 * test_wait_fake.c - platform_wait_events, platform_wake and app_run_events on the fake backend: no display, so it runs
 * anywhere and under ThreadSanitizer (tools/check.sh runs it there). The checks are those of tests/wait_common.h; what the
 * fake adds is the part of the contract a window system does not: a wait returns at once for an event that was injected,
 * and does not take it (the frame that follows delivers it), and a click released in the frame of its press, or a gesture
 * that turns from a tap into a hold, is a frame that is due.
 */
#include "platform.h"
#include "backend_fake.h"

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

#include "wait_common.h"

static PlatformWindow *make_window(void)
{
    WindowConfig cfg = {.title = "wait", .width = 320, .height = 240, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    return window_create(&cfg);
}

/* An injected event ends a wait at once, and the wait does not take it. */
static void test_events_are_not_consumed(PlatformWindow *w)
{
    wc_drain(w);
    fake_key(w, KEY_A, true, false);
    fake_mouse_move(w, 10, 20);
    for (int i = 0; i < 3; i++) /* the same answer every time: nothing was taken */
    {
        double t0 = wc_now();
        CHECK(platform_wait_events(5.0));
        CHECK(wc_now() - t0 < 0.05);
    }
    window_begin_frame(w);
    int keys = 0, moves = 0;
    Event e;
    while (poll_event(w, &e))
    {
        keys += e.type == EVENT_KEY;
        moves += e.type == EVENT_MOUSE_MOVE;
    }
    CHECK(keys == 1 && moves == 1);
    CHECK(key_down(w, KEY_A));
    CHECK(!platform_wait_events(0.02)); /* now they are delivered and nothing is waiting */
    fake_key(w, KEY_A, false, false);
    window_begin_frame(w);
}

/* A click whose release came in the frame of the press: mouse_down stays true until the next window_begin_frame, so a loop
   that sleeps has to run that frame (true at once), once. */
static void test_click_release_is_a_frame_due(PlatformWindow *w)
{
    wc_drain(w);
    fake_mouse_button(w, MOUSE_LEFT, true);
    fake_mouse_button(w, MOUSE_LEFT, false);
    window_begin_frame(w);
    CHECK(mouse_button_down(w, MOUSE_LEFT)); /* still down: seen by a program that samples once per frame */
    double t0 = wc_now();
    CHECK(platform_wait_events(5.0));       /* the release is the next frame's */
    CHECK(wc_now() - t0 < 0.05);
    window_begin_frame(w);
    CHECK(!mouse_button_down(w, MOUSE_LEFT));
    CHECK(mouse_button_released(w, MOUSE_LEFT));
    CHECK(!platform_wait_events(0.02));
}

/* A touch that goes down is a TAP for the frame it came in and a HOLD from the next: that frame is due, once. A swipe
   (a quick flick) is reported for one frame and then ends: the frame that ends it is due too. */
static void test_gesture_is_a_frame_due(PlatformWindow *w)
{
    wc_drain(w);
    fake_touch(w, 1, 100, 100, TOUCH_DOWN);
    window_begin_frame(w);
    CHECK(gesture_detected(w) == GESTURE_TAP);
    double t0 = wc_now();
    CHECK(platform_wait_events(5.0));
    CHECK(wc_now() - t0 < 0.05);
    window_begin_frame(w);
    CHECK(gesture_detected(w) == GESTURE_HOLD);
    CHECK(!platform_wait_events(0.02)); /* a hold goes on by itself: nothing is due */
    fake_touch(w, 1, 100, 100, TOUCH_UP);
    window_begin_frame(w);
    CHECK(gesture_detected(w) == GESTURE_NONE);
    CHECK(!platform_wait_events(0.02));

    fake_touch(w, 2, 50, 100, TOUCH_DOWN);
    window_begin_frame(w);
    fake_touch(w, 2, 400, 100, TOUCH_UP);
    window_begin_frame(w);
    if (gesture_detected(w) == GESTURE_SWIPE_RIGHT)
    {
        CHECK(platform_wait_events(5.0));
        window_begin_frame(w);
        CHECK(gesture_detected(w) == GESTURE_NONE);
    }
    wc_drain(w);
    CHECK(!platform_wait_events(0.02));
}

/* An event queued by a hook-free injection reaches app_run_events' frame, and the loop sleeps otherwise. */
static void ev_frame(PlatformWindow *w, void *user)
{
    int *keys = user;
    Event e;
    while (poll_event(w, &e))
        *keys += e.type == EVENT_KEY && e.data.key.down;
    if (*keys >= 1)
        window_set_should_close(w, true);
}

static void test_run_events_sees_injected_input(PlatformWindow *w)
{
    wc_drain(w);
    int keys = 0;
    fake_key(w, KEY_B, true, false);
    app_run_events(w, ev_frame, &keys);
    CHECK(keys == 1);
    window_set_should_close(w, false);
    fake_key(w, KEY_B, false, false);
    window_begin_frame(w);
}

int main(void)
{
    if (!platform_init())
    {
        printf("platform_init failed\n");
        return 1;
    }
    PlatformWindow *w = make_window();
    CHECK(w != NULL);
    if (!w)
        return 1;
    printf("== timeouts\n");
    wc_timeouts(w);
    printf("== idle CPU\n");
    wc_idle_cpu(w, 2.0);
    printf("== wake\n");
    wc_wake_semantics(w);
    wc_wake_latency(w);
    printf("== a storm of wakes\n");
    wc_wake_storm(w);
    printf("== signals\n");
    wc_eintr(w);
    printf("== injected events\n");
    test_events_are_not_consumed(w);
    test_click_release_is_a_frame_due(w);
    test_gesture_is_a_frame_due(w);
    printf("== app_run_events\n");
    test_run_events_sees_injected_input(w);
    wc_run_events(w);
    printf("== descriptors\n");
    window_destroy(w);
    wc_fd_leak(1000);
    wc_without_platform();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
