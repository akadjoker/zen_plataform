/*
 * test_wait_wayland.c - platform_wait_events and platform_wake on the Wayland backend, against the small compositor of
 * test_input_wayland.c (included below, its main renamed): the wait with nothing pending sleeps for its timeout using no
 * CPU; an event the compositor sends wakes it within a few ms and is then delivered by the normal frame (the wait reads the
 * socket and dispatches the listeners, which only fill the windows' queues, so nothing is taken from the application); events
 * that libwayland already read (a roundtrip of the program's own does that) do not make it sleep; a held key's repeats,
 * which the library makes itself, end the sleep in time; the compositor going away while the program sleeps ends it. The checks
 * that do not depend on the window system are those of tests/wait_common.h. Skips (77) when a piece is missing.
 */
#define main wayland_input_main
#include "test_input_wayland.c"
#undef main

#include "wait_common.h"

int wl_display_roundtrip(struct wl_display *display); /* libwayland-client, which the library links; its header clashes with the server's */

static PlatformWindow *make_window(void)
{
    WindowConfig cfg = {.title = "wait", .width = 320, .height = 240, .resizable = true};
    return window_create(&cfg);
}

static void quiet(PlatformWindow *w)
{
    for (int i = 0; i < 20; i++)
    {
        frame(w);
        usleep(2000);
    }
    CHECK(settle(w));
    wc_drain(w);
}

/* ---- events from the compositor ---- */

typedef struct
{
    int delay_ms, x, y;
    double stamp;
} Sender;

static void *sender_main(void *arg)
{
    Sender *s = arg;
    wc_msleep(s->delay_ms);
    s->stamp = wc_now();
    cmd(CMD_PTR_MOVE, s->x, s->y, 0, 0);
    cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
    return NULL;
}

/* An event from the compositor: the wait comes back true within a few ms, says true again (nothing was taken), and the
   frame that follows delivers it. */
static void test_event_from_compositor(PlatformWindow *w)
{
    cmd(CMD_PTR_ENTER, 50, 50, 0, 0);
    CHECK(settle(w));
    wc_drain(w);
    enum { N = 60 };
    double lat[N];
    int delivered = 0, wrong = 0;
    for (int i = 0; i < N; i++)
    {
        reset();
        Sender s = {.delay_ms = 10, .x = 60 + (i % 2) * 50, .y = 70};
        pthread_t t;
        pthread_create(&t, NULL, sender_main, &s);
        bool r = platform_wait_events(5.0);
        double back = wc_now();
        pthread_join(t, NULL);
        lat[i] = back - s.stamp;
        wrong += !r;
        CHECK(platform_wait_events(0.0)); /* not consumed */
        frame(w);
        delivered += count(EVENT_MOUSE_MOVE) > 0;
        CHECK(!platform_wait_events(0.005));
    }
    qsort(lat, N, sizeof lat[0], wc_cmp);
    printf("  an event from the compositor: wake latency median %.3f ms, 99th %.3f ms, worst %.3f ms (%d)\n",
           lat[N / 2] * 1e3, lat[N * 99 / 100] * 1e3, lat[N - 1] * 1e3, N);
    CHECK(wrong == 0);
    CHECK(delivered == N);
    CHECK(lat[N / 2] < 0.005);
}

/* The lost wake-up: libwayland read the event off the socket (the program's own roundtrip does) and dispatched it, and the
   socket is quiet again. */
static void test_events_already_read(PlatformWindow *w)
{
    struct wl_display *d = window_native_handle(w, NATIVE_DISPLAY);
    CHECK(d != NULL);
    if (!d)
        return;
    int late = 0;
    for (int i = 0; i < 20; i++)
    {
        reset();
        cmd(CMD_PTR_MOVE, 60 + (i % 2) * 50, 70, 0, 0);
        cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
        wc_msleep(5); /* the compositor has sent it by now */
        wl_display_roundtrip(d);
        double t0 = wc_now();
        bool r = platform_wait_events(2.0);
        late += !r || wc_now() - t0 > 0.1;
        frame(w);
        CHECK(count(EVENT_MOUSE_MOVE) > 0);
    }
    CHECK(late == 0);
    wc_drain(w);
}

/* A key held: the library makes the repeats, so the sleep ends in time for each (false: nothing is waiting yet, the pump
   makes them), and the repeats arrive at the rate the compositor asked for without a frame loop spinning. */
static void test_key_repeat_wakes(PlatformWindow *w)
{
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    CHECK(settle(w));
    wc_drain(w);
    reset();
    cmd(CMD_KEY, EV_A, 1, 0, 0);
    double c0 = wc_cpu(), t0 = wc_now();
    int returns = 0;
    while (wc_now() - t0 < 1.0)
    {
        platform_wait_events(0.5);
        frame(w);
        returns++;
    }
    double cpu = wc_cpu() - c0;
    int repeats = count_key(KEY_A, true, true);
    printf("  a key held for 1 s: %d repeats, the loop turned %d times, CPU %.2f ms\n", repeats, returns, cpu * 1e3);
    CHECK(count_key(KEY_A, true, false) == 1);
    CHECK(repeats >= 20); /* 40 per second after 120 ms */
    CHECK(returns < repeats + 10);
    CHECK(cpu < 0.05);
    cmd(CMD_KEY, EV_A, 0, 0, 0);
    CHECK(settle(w));
    wc_drain(w);
    /* released: no wake-up of its own any more */
    double c1 = wc_cpu();
    CHECK(!platform_wait_events(0.3));
    CHECK(wc_cpu() - c1 < 0.003);
}

/* The compositor goes away while the program sleeps with no limit. Last: it ends the compositor. */
static void *killer_main(void *arg)
{
    (void)arg;
    wc_msleep(150);
    cmd(CMD_QUIT, 0, 0, 0, 0);
    pthread_join(S.thread, NULL);
    wl_display_destroy_clients(S.display); /* closes the client's connection (wl_display_destroy alone leaves it open) */
    wl_display_destroy(S.display);
    S.display = NULL;
    return NULL;
}

static void test_compositor_goes_away(PlatformWindow *w)
{
    pthread_t k;
    pthread_create(&k, NULL, killer_main, NULL);
    double c0 = wc_cpu(), t0 = wc_now();
    bool r = platform_wait_events(-1);
    double took = wc_now() - t0, cpu = wc_cpu() - c0;
    pthread_join(k, NULL);
    printf("  the compositor gone after 150 ms: the wait ended after %.1f ms, CPU %.2f ms\n", took * 1e3, cpu * 1e3);
    CHECK(r);
    CHECK(took >= 0.14 && took < 2.0);
    CHECK(cpu < 0.02);
    bool closed = false;
    for (int i = 0; i < 5 && !closed; i++)
    {
        frame(w);
        closed = count(EVENT_WINDOW_CLOSE) > 0;
    }
    CHECK(closed);
    CHECK(window_should_close(w));
    CHECK(platform_wait_events(0.05)); /* a lost connection is an event for ever: the loop ends on should_close */
}

int main(void)
{
    if (!start_compositor())
    {
        printf("SKIP the test compositor did not start\n");
        return 77;
    }
    if (!platform_init())
    {
        printf("SKIP %s\n", platform_get_error());
        return 77;
    }
    PlatformWindow *w = make_window();
    if (!w)
    {
        printf("FAIL window_create: %s\n", platform_get_error());
        return 1;
    }
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
    printf("== signals\n");
    wc_eintr(w);
    printf("== app_run_events\n");
    wc_run_events(w);
    printf("== events from the compositor\n");
    test_event_from_compositor(w);
    test_events_already_read(w);
    test_key_repeat_wakes(w);
    printf("== descriptors, without the platform\n");
    window_destroy(w);
    wc_fd_leak(getenv("ZP_WAIT_CYCLES") ? atoi(getenv("ZP_WAIT_CYCLES")) : 1000);
    wc_without_platform();
    w = make_window();
    CHECK(w != NULL);
    if (w)
    {
        quiet(w);
        printf("== the compositor goes away\n");
        test_compositor_goes_away(w);
        window_destroy(w);
    }
    platform_shutdown();
    if (S.display)
    {
        cmd(CMD_QUIT, 0, 0, 0, 0);
        pthread_join(S.thread, NULL);
        wl_display_destroy(S.display);
    }
    xkb_state_unref(S.state);
    xkb_keymap_unref(S.keymap);
    xkb_context_unref(S.xkb);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
