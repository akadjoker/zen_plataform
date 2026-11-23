/*
 * test_wait_cocoa.c - platform_wait_events, platform_wake and app_run_events on the macOS backend, with a real window. The
 * checks are those of tests/wait_common.h (timeouts, an idle wait that uses no CPU, the wake from other threads and its latency,
 * a storm of wakes, the loop that sleeps, descriptors, the wait without the platform) and what AppKit adds: an event posted to
 * the application's queue, from this thread and from another, ends the wait and is not taken by it; and an event the backend
 * makes outside the pump (a notification with no NSEvent behind it) ends it too.
 *
 * Signals: the run loop is not interrupted by a signal as poll(2) is, so a signal whose handler does not wake is not seen by
 * the wait (wc_eintr's first half does not hold here and is not run); a handler that calls platform_wake ends it.
 */
#include "cocoa_test_util.h"

#include <pthread.h>

#include "wait_common.h"

static PlatformWindow *make(int w, int h)
{
    WindowConfig cfg = {.title = "cocoa wait test", .width = w, .height = h, .x = 80, .y = 80, .render = RENDER_PIXELS, .resizable = true};
    return window_create(&cfg);
}

static void quiet(PlatformWindow *w)
{
    pump_for(w, 300);
    wc_drain(w);
}

static int count_type(PlatformWindow *w, EventType type)
{
    int n = 0;
    Event e;
    while (poll_event(w, &e))
        n += e.type == type;
    return n;
}

static void test_common(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    quiet(w);
    printf("== timeouts\n");
    wc_timeouts(w);
    printf("== idle CPU\n");
    wc_idle_cpu(w, 2.0);
    printf("== wake\n");
    wc_wake_semantics(w);
    wc_wake_latency(w);
    printf("== a storm of wakes\n");
    wc_wake_storm(w);
    printf("== app_run_events\n");
    wc_run_events(w);
    window_destroy(w);
}

/* A handler that wakes ends the wait: the wake is a write to a descriptor the run loop watches. */
static void test_signal_that_wakes(void)
{
    PlatformWindow *w = make(200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    quiet(w);
    g_sig_count = 0;
    wc_set_alarm(wc_on_alarm_wake, 30);
    double t0 = wc_now();
    bool r = platform_wait_events(5.0);
    double took = wc_now() - t0;
    printf("  a signal whose handler wakes ended a 5 s wait after %.1f ms\n", took * 1e3);
    CHECK(r);
    CHECK(took < 1.0);
    CHECK(g_sig_count == 1);
    CHECK(!platform_wait_events(0.02));
    signal(SIGALRM, SIG_DFL);
    window_destroy(w);
}

/* An event in the application's queue: the wait says true at once, every time, and leaves it for the frame. */
static void test_event_in_queue(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    quiet(w);
    int late = 0, delivered = 0;
    for (int i = 0; i < 40; i++)
    {
        post(event_mouse(w, 5 /* NSEventTypeMouseMoved */, 30 + (i % 2) * 100, 40, 0, 0));
        double t0 = wc_now();
        bool r = platform_wait_events(5.0);
        late += !r || wc_now() - t0 > 0.05;
        CHECK(platform_wait_events(0.0)); /* not consumed */
        window_begin_frame(w);
        delivered += count_type(w, EVENT_MOUSE_MOVE) > 0;
        CHECK(!platform_wait_events(0.005)); /* and now nothing is left */
    }
    printf("  an event already in the queue: %d of 40 waits late, %d of 40 delivered by the frame\n", late, delivered);
    CHECK(late == 0);
    CHECK(delivered == 40);
    window_destroy(w);
}

/* The same from another thread (postEvent:atStart: may be called from any): the wait is blocked when the event comes. */
typedef struct
{
    id event;
    int delay_ms;
    double at;
} EventPoster;

static void *poster_main(void *arg)
{
    EventPoster *p = arg;
    wc_msleep(p->delay_ms);
    p->at = wc_now();
    post(p->event);
    return NULL;
}

static void test_event_from_thread(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    quiet(w);
    enum { N = 40 };
    double lat[N];
    int wrong = 0, delivered = 0;
    for (int i = 0; i < N; i++)
    {
        EventPoster p = {.event = event_mouse(w, 5, 30 + (i % 2) * 100, 60, 0, 0), .delay_ms = 15};
        msg_id(p.event, "retain");
        pthread_t t;
        pthread_create(&t, NULL, poster_main, &p);
        bool r = platform_wait_events(5.0);
        double back = wc_now();
        pthread_join(t, NULL);
        msg_void(p.event, "release");
        lat[i] = back - p.at;
        wrong += !r;
        window_begin_frame(w);
        delivered += count_type(w, EVENT_MOUSE_MOVE) > 0;
        CHECK(!platform_wait_events(0.005));
    }
    qsort(lat, N, sizeof lat[0], wc_cmp);
    printf("  an event posted from another thread: wake latency median %.3f ms, 99th %.3f ms, worst %.3f ms (%d)\n", lat[N / 2] * 1e3,
           lat[N * 99 / 100] * 1e3, lat[N - 1] * 1e3, N);
    CHECK(wrong == 0);
    CHECK(delivered == N);
    CHECK(lat[N / 2] < 0.005);
    window_destroy(w);
}

/* An event with no NSEvent behind it: a resize asked from a timer of the run loop, which fires inside the sleep. The window's
   delegate raises EVENT_WINDOW_RESIZE outside the pump, and the wait ends for it. */
static PlatformWindow *g_timer_window;

static void resize_timer(CFRunLoopTimerRef timer, void *info)
{
    (void)timer, (void)info;
    window_set_size(g_timer_window, 340, 230);
}

static void test_event_made_in_the_sleep(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    quiet(w);
    g_timer_window = w;
    CFRunLoopTimerRef timer = CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + 0.05, 0, 0, 0, resize_timer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    double t0 = wc_now();
    bool r = platform_wait_events(5.0);
    double took = wc_now() - t0;
    printf("  a resize made by a run loop timer inside the sleep ended a 5 s wait after %.1f ms\n", took * 1e3);
    CHECK(r);
    CHECK(took < 1.0);
    window_begin_frame(w);
    CHECK(count_type(w, EVENT_WINDOW_RESIZE) >= 1);
    int sw, sh;
    window_get_size(w, &sw, &sh);
    CHECK(sw == 340 && sh == 230);
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    quiet(w);
    CHECK(!platform_wait_events(0.02));
    window_destroy(w);
}

static void test_descriptors(void)
{
    printf("== descriptors\n");
    wc_fd_leak(200);
    printf("== without the platform\n");
    wc_without_platform();
}

int main(void)
{
    alarm(240);
    (void)wc_eintr; /* not run here: see the top of the file */
    if (!platform_init())
    {
        printf("skip: %s\n", platform_get_error());
        return ZP_REQUIRE ? 1 : 77;
    }
    test_common();
    printf("== a signal that wakes\n");
    test_signal_that_wakes();
    printf("== events of the application's queue\n");
    test_event_in_queue();
    test_event_from_thread();
    printf("== an event made inside the sleep\n");
    test_event_made_in_the_sleep();
    test_descriptors();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
