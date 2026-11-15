/*
 * bench_input_x11.c - how input reaches a program on the X11 backend: the time from the X event to the frame that sees it,
 * what one window_begin_frame costs when many events are waiting, and how many X requests each frame sends.
 *
 * A second connection drives the server's own pointer and keyboard with XTest (as tests/test_mouse_x11.c does), at a
 * given rate and each event stamped when it is sent; the window's loop runs at a fixed frame period, stamps the end of
 * each window_begin_frame, and reads every event with poll_event. Run it on a private Xvfb (tools/xvfb_run.sh): the
 * server then has no real device, no compositor and no vsync, so what it shows is the library's share (the pump, the
 * event list, the extra requests) and not what a mouse at 8000 Hz or a screen at 144 Hz adds on a real desktop.
 *
 *   bench_input_x11 [scenario]     scenarios: idle held motion keys wheel mixed captured stall (default: all)
 *
 * Every number is printed as "name  value unit", one per line, so two builds compare with a script. Per scenario:
 *   latency       time from the event being sent to the end of the frame that returned it: median, 99th percentile, worst
 *   pump          time of one window_begin_frame: median, 99th percentile, worst
 *   requests      X requests sent by one window_begin_frame, average (a round trip is one that waits for an answer)
 *   delivered     events injected, events returned by poll_event (a move can be folded into the next one when a frame
 *                 holds more than the event list does), the last position
 *   late          events the server had made, for 2 ms or more, when a frame started and that frame did not return
 */
#define _GNU_SOURCE
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define WIN_X 200
#define WIN_Y 150
#define WIN_W 600
#define WIN_H 400
#define POS_COLS 500 /* a move to (x, y) in the window says which one it is: index = (y - 20) * POS_COLS + (x - 20) */
#define POS_ROWS 300
#define MAX_EVENTS (POS_COLS * POS_ROWS)
#define LATE_MS 2.0

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e3 + (double)ts.tv_nsec / 1e6;
}

static void sleep_until_ms(double t)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(t / 1e3);
    ts.tv_nsec = (long)((t - (double)ts.tv_sec * 1e3) * 1e6);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) != 0)
    {
    }
}

typedef enum
{
    INJ_MOTION,   /* absolute moves inside the window */
    INJ_REL,      /* relative moves (captured mode) */
    INJ_KEYS,     /* press and release of letters */
    INJ_HELD,     /* one key held down, nothing else */
    INJ_WHEEL,    /* wheel steps */
    INJ_MIXED,    /* moves, and a key press and release every 10 ms */
} InjKind;

typedef struct
{
    InjKind kind;
    double rate;  /* events per second */
    int count;    /* events to send */
    double *sent; /* sent[i]: when event i left (ms) */
    int rel_dx, rel_dy; /* INJ_REL: the step */
    int keys_sent;
    volatile int done;
    double started, finished;
} Injector;

static Display *g_inj;
static Window g_win;
static KeyCode g_key_a, g_key_s;

static void *inject_main(void *arg)
{
    Injector *in = arg;
    Display *d = g_inj;
    double t0 = now_ms();
    in->started = t0;
    if (in->kind == INJ_HELD)
    {
        XTestFakeKeyEvent(d, g_key_a, True, 0);
        XFlush(d);
        in->sent[0] = now_ms();
        in->keys_sent = 1;
        in->done = 1;
        return NULL;
    }
    double next_key = t0;
    for (int i = 0; i < in->count; i++)
    {
        sleep_until_ms(t0 + (double)i * 1e3 / in->rate - 0.2); /* the last 0.2 ms are a spin: finer than the timer */
        while (now_ms() < t0 + (double)i * 1e3 / in->rate)
        {
        }
        in->sent[i] = now_ms();
        switch (in->kind)
        {
        case INJ_MOTION:
        case INJ_MIXED:
            XTestFakeMotionEvent(d, -1, WIN_X + 20 + i % POS_COLS, WIN_Y + 20 + (i / POS_COLS) % POS_ROWS, 0);
            if (in->kind == INJ_MIXED && in->sent[i] >= next_key)
            {
                KeyCode k = (in->keys_sent & 1) ? g_key_s : g_key_a;
                XTestFakeKeyEvent(d, k, True, 0);
                XTestFakeKeyEvent(d, k, False, 0);
                in->keys_sent++;
                next_key += 10.0;
            }
            break;
        case INJ_REL:
            XTestFakeRelativeMotionEvent(d, in->rel_dx, in->rel_dy, 0);
            break;
        case INJ_KEYS:
        {
            KeyCode k = (i & 2) ? g_key_s : g_key_a;
            XTestFakeKeyEvent(d, k, (i & 1) ? False : True, 0);
            break;
        }
        case INJ_WHEEL:
            XTestFakeButtonEvent(d, 4, True, 0);
            XTestFakeButtonEvent(d, 4, False, 0);
            break;
        default:
            break;
        }
        XFlush(d);
    }
    in->finished = now_ms();
    in->done = 1;
    return NULL;
}

/* ---- statistics ---- */

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

typedef struct
{
    double median, p99, worst;
} Stat;

static Stat stat_of(double *v, int n)
{
    Stat s = {0, 0, 0};
    if (n <= 0)
        return s;
    qsort(v, (size_t)n, sizeof *v, cmp_double);
    s.median = v[n / 2];
    s.p99 = v[(int)((double)(n - 1) * 0.99)];
    s.worst = v[n - 1];
    return s;
}

static void print_stat(const char *name, const char *what, Stat s, const char *unit, double scale)
{
    printf("%-34s %-8s median %9.1f %s\n", name, what, s.median * scale, unit);
    printf("%-34s %-8s p99    %9.1f %s\n", name, what, s.p99 * scale, unit);
    printf("%-34s %-8s worst  %9.1f %s\n", name, what, s.worst * scale, unit);
}

/* ---- one scenario ---- */

typedef struct
{
    const char *name;
    InjKind kind;
    double rate;
    int count;
    double frame_ms;
    bool captured;
    int rel_dx, rel_dy;
    double stall_ms;   /* the frame loop sleeps this long once, after the first third of the events */
    double min_run_ms; /* the frame loop runs at least this long (a key held sends nothing more) */
} Scenario;

#define MAX_FRAMES 4000

static void run(PlatformWindow *w, const Scenario *sc)
{
    static double sent[MAX_EVENTS];
    static double seen[MAX_EVENTS];
    static int seen_frame[MAX_EVENTS];
    static double pump_ms[MAX_FRAMES], pump_start[MAX_FRAMES];
    static double requests[MAX_FRAMES];
    static double latency[MAX_EVENTS];
    Display *wd = window_native_handle(w, NATIVE_DISPLAY);

    memset(seen, 0, sizeof seen);
    for (int i = 0; i < MAX_EVENTS; i++)
        seen_frame[i] = -1;
    Injector in = {.kind = sc->kind, .rate = sc->rate, .count = sc->count, .sent = sent, .rel_dx = sc->rel_dx, .rel_dy = sc->rel_dy};
    if (sc->captured)
        mouse_set_mode(w, MOUSE_MODE_CAPTURED);

    /* settle: the window reads whatever is left from the last scenario */
    for (int i = 0; i < 5; i++)
    {
        window_begin_frame(w);
        time_sleep(5);
    }
    XWarpPointer(g_inj, None, DefaultRootWindow(g_inj), 0, 0, 0, 0, WIN_X + WIN_W / 2, WIN_Y + WIN_H / 2);
    XSync(g_inj, False);
    for (int i = 0; i < 3; i++)
        window_begin_frame(w);

    int frames = 0, delivered = 0, last_x = -1, last_y = -1, keys = 0, wheels = 0, nlat = 0, late = 0;
    long sum_dx = 0, sum_dy = 0;
    int events_in_frame_max = 0;
    pthread_t th;
    double t_start = now_ms();
    double next = t_start + sc->frame_ms;
    bool stalled = false;
    pthread_create(&th, NULL, inject_main, &in);
    int tail = 0;
    while (frames < MAX_FRAMES)
    {
        sleep_until_ms(next);
        next += sc->frame_ms;
        if (sc->stall_ms > 0 && !stalled && now_ms() - t_start > (double)sc->count * 1e3 / sc->rate / 3)
        {
            stalled = true;
            sleep_until_ms(now_ms() + sc->stall_ms);
            next = now_ms();
        }
        unsigned long r0 = XNextRequest(wd);
        double t0 = now_ms();
        window_begin_frame(w);
        double t1 = now_ms();
        requests[frames] = (double)(XNextRequest(wd) - r0);
        pump_start[frames] = t0;
        pump_ms[frames] = t1 - t0;
        Event e;
        int n = 0;
        while (poll_event(w, &e))
        {
            n++;
            if (e.type == EVENT_MOUSE_MOVE)
            {
                sum_dx += e.data.mouse.dx;
                sum_dy += e.data.mouse.dy;
                if (!sc->captured)
                {
                    int idx = (e.data.mouse.y - 20) * POS_COLS + (e.data.mouse.x - 20);
                    last_x = e.data.mouse.x;
                    last_y = e.data.mouse.y;
                    if (idx >= 0 && idx < sc->count && seen_frame[idx] < 0)
                    {
                        seen_frame[idx] = frames;
                        seen[idx] = t1;
                        delivered++;
                    }
                }
                else
                    delivered++;
            }
            else if (e.type == EVENT_KEY)
            {
                if (sc->kind == INJ_KEYS && keys < sc->count)
                    latency[nlat++] = t1 - sent[keys]; /* the events come back in the order they were sent */
                keys++;
            }
            else if (e.type == EVENT_MOUSE_WHEEL)
            {
                if (sc->kind == INJ_WHEEL && wheels < sc->count)
                    latency[nlat++] = t1 - sent[wheels];
                wheels++;
            }
        }
        if (n > events_in_frame_max)
            events_in_frame_max = n;
        frames++;
        if (in.done && now_ms() - t_start >= sc->min_run_ms && ++tail > 4)
            break;
    }
    pthread_join(th, NULL);

    if (sc->kind == INJ_MOTION || sc->kind == INJ_MIXED)
    {
        for (int i = 0; i < sc->count; i++)
        {
            if (seen_frame[i] < 0)
                continue;
            latency[nlat++] = seen[i] - sent[i];
            /* late: some earlier frame started at least LATE_MS after it was sent, and this event was not in it */
            int f = seen_frame[i];
            if (f > 0 && pump_start[f - 1] - sent[i] >= LATE_MS)
                late++;
        }
    }
    Stat lat = stat_of(latency, nlat);
    char label[64];
    snprintf(label, sizeof label, "%s", sc->name);
    if (nlat)
        print_stat(label, "latency", lat, "us", 1e3);
    Stat pump = stat_of(pump_ms, frames);
    print_stat(label, "pump", pump, "us", 1e3);
    double rq = 0, rq_max = 0;
    for (int i = 0; i < frames; i++)
    {
        rq += requests[i];
        if (requests[i] > rq_max)
            rq_max = requests[i];
    }
    printf("%-34s %-8s average %8.2f requests per frame\n", label, "requests", frames ? rq / frames : 0.0);
    printf("%-34s %-8s worst   %8.0f requests per frame\n", label, "requests", rq_max);
    if (sc->kind == INJ_MOTION || sc->kind == INJ_MIXED)
        printf("%-34s %-8s %d of %d moves returned, the last at %d,%d, late %d, most events in a frame %d\n", label,
               "delivered", delivered, sc->count, last_x, last_y, late, events_in_frame_max);
    else if (sc->kind == INJ_REL)
        printf("%-34s %-8s %d moves returned, summed dx %ld dy %ld, sent dx %ld dy %ld, most events in a frame %d\n", label,
               "delivered", delivered, sum_dx, sum_dy, (long)sc->rel_dx * sc->count, (long)sc->rel_dy * sc->count,
               events_in_frame_max);
    else
        printf("%-34s %-8s %d key events, %d wheel steps (sent %d), %d frames\n", label, "delivered", keys, wheels,
               sc->count, frames);
    printf("%-34s %-8s %d frames of %.1f ms, injected %d at %.0f per second\n", label, "run", frames, sc->frame_ms,
           sc->count, in.finished > in.started ? (double)sc->count * 1e3 / (in.finished - in.started) : 0.0);
    fflush(stdout);

    if (sc->kind == INJ_HELD)
    {
        XTestFakeKeyEvent(g_inj, g_key_a, False, 0);
        XSync(g_inj, False);
    }
    if (sc->captured)
        mouse_set_mode(w, MOUSE_MODE_NORMAL);
    for (int i = 0; i < 5; i++)
    {
        window_begin_frame(w);
        time_sleep(5);
    }
}

/* frames with nothing to read: what an idle window_begin_frame costs */
static void run_idle(PlatformWindow *w)
{
    Display *wd = window_native_handle(w, NATIVE_DISPLAY);
    static double pump[600], req[600];
    for (int f = 0; f < 600; f++)
    {
        sleep_until_ms(now_ms() + 2.0);
        unsigned long r0 = XNextRequest(wd);
        double t0 = now_ms();
        window_begin_frame(w);
        pump[f] = now_ms() - t0;
        req[f] = (double)(XNextRequest(wd) - r0);
    }
    Stat s = stat_of(pump, 600);
    print_stat("idle (no events)", "pump", s, "us", 1e3);
    double r = 0;
    for (int f = 0; f < 600; f++)
        r += req[f];
    printf("%-34s %-8s average %8.2f requests per frame\n", "idle (no events)", "requests", r / 600);
}

int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;
    if (!platform_init())
    {
        printf("bench_input_x11: no platform: %s\n", platform_get_error());
        return 77;
    }
    WindowConfig cfg = {.title = "bench_input_x11", .width = WIN_W, .height = WIN_H, .x = WIN_X, .y = WIN_Y, .render = RENDER_PIXELS};
    PlatformWindow *w = window_create(&cfg);
    g_inj = XOpenDisplay(NULL);
    int ev, er, ma, mi;
    if (!w || !g_inj || !XTestQueryExtension(g_inj, &ev, &er, &ma, &mi))
    {
        printf("bench_input_x11: needs a window and the XTest extension\n");
        return 77;
    }
    g_win = (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW);
    g_key_a = XKeysymToKeycode(g_inj, XK_a);
    g_key_s = XKeysymToKeycode(g_inj, XK_s);
    for (int i = 0; i < 100 && !window_is_focused(w); i++)
    {
        if (i % 25 == 0)
        {
            XSetInputFocus(g_inj, g_win, RevertToParent, CurrentTime);
            XSync(g_inj, False);
        }
        window_begin_frame(w);
        time_sleep(5);
    }
    printf("window focused: %s\n", window_is_focused(w) ? "yes" : "NO (key scenarios will show nothing)");

    static const Scenario all[] = {
        {"key held, nothing else", INJ_HELD, 1, 1, 16.7, false, 0, 0, 0, 3000},
        {"motion 125 Hz, 60 fps", INJ_MOTION, 125, 250, 16.7, false, 0, 0, 0, 0},
        {"motion 500 Hz, 60 fps", INJ_MOTION, 500, 1000, 16.7, false, 0, 0, 0, 0},
        {"motion 1000 Hz, 60 fps", INJ_MOTION, 1000, 2000, 16.7, false, 0, 0, 0, 0},
        {"motion 8000 Hz, 60 fps", INJ_MOTION, 8000, 16000, 16.7, false, 0, 0, 0, 0},
        {"motion 8000 Hz, 30 fps", INJ_MOTION, 8000, 16000, 33.3, false, 0, 0, 0, 0},
        {"motion 8000 Hz, 10 fps", INJ_MOTION, 8000, 16000, 100.0, false, 0, 0, 0, 0},
        {"motion 8000 Hz, 1 s stall", INJ_MOTION, 8000, 24000, 16.7, false, 0, 0, 1000.0, 0},
        {"keys 200 per second", INJ_KEYS, 200, 400, 16.7, false, 0, 0, 0, 0},
        {"wheel 100 per second", INJ_WHEEL, 100, 200, 16.7, false, 0, 0, 0, 0},
        {"motion 1000 Hz and keys", INJ_MIXED, 1000, 2000, 16.7, false, 0, 0, 0, 0},
        {"captured 125 Hz", INJ_REL, 125, 250, 16.7, true, 3, 2, 0, 0},
        {"captured 1000 Hz", INJ_REL, 1000, 2000, 16.7, true, 3, 2, 0, 0},
        {"captured 8000 Hz", INJ_REL, 8000, 16000, 16.7, true, 1, 1, 0, 0},
        {"captured flick 8 px at 8000 Hz", INJ_REL, 8000, 8000, 16.7, true, 8, 0, 0, 0},
        {"captured flick 2 px at 1000 Hz, 30 fps", INJ_REL, 1000, 2000, 33.3, true, 2, 0, 0, 0},
    };
    if (!only || strstr("idle (no events)", only))
        run_idle(w);
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++)
    {
        const Scenario *sc = &all[i];
        if (only && !strstr(sc->name, only))
            continue;
        run(w, sc);
    }
    window_destroy(w);
    XCloseDisplay(g_inj);
    platform_shutdown();
    return 0;
}
