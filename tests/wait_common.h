/*
 * wait_common.h - the checks of platform_wait_events and platform_wake that do not depend on the window system, written
 * once and run by test_wait_fake (the fake backend: no display, so TSan and any machine), test_wait_x11 and
 * test_wait_wayland on the real backends. The includer defines CHECK and g_pass / g_fail, and calls these with a window
 * on which nothing is pending. POSIX only: it uses threads, signals and /proc.
 */
#ifndef WAIT_COMMON_H
#define WAIT_COMMON_H

#include "platform.h"

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

static double wc_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static double wc_cpu(void) /* CPU time of the whole process: user and system, every thread */
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void wc_msleep(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0)
    {
    }
}

static int wc_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static int wc_open_fds(void)
{
    int n = 0;
    DIR *d = opendir("/proc/self/fd");
    if (!d)
        d = opendir("/dev/fd"); /* macOS and the BSDs */
    if (!d)
        return -1;
    while (readdir(d))
        n++;
    closedir(d);
    return n;
}

/* Nothing waiting: the wake of an earlier check, and anything the window system has queued, is taken. */
static void wc_drain(PlatformWindow *w)
{
    for (int i = 0; i < 50 && platform_wait_events(0); i++)
        if (w)
            window_begin_frame(w);
}

/* A timeout of 0, 1 ms, 16 ms and 1 s: the wait never ends early (the part of a millisecond is rounded up), takes the
   time and little more, and says false. The median is held to a few ms (the machine may be loaded: the worst is printed). */
static void wc_timeouts(PlatformWindow *w)
{
    static const struct
    {
        double t;
        int n;
    } cases[] = {{0.0, 400}, {0.001, 60}, {0.016, 30}, {1.0, 2}};
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++)
    {
        wc_drain(w);
        double v[400], worst = 0;
        bool early = false, said_true = false;
        for (int i = 0; i < cases[c].n; i++)
        {
            double t0 = wc_now();
            bool r = platform_wait_events(cases[c].t);
            v[i] = wc_now() - t0;
            said_true |= r;
            early |= v[i] < cases[c].t;
            worst = v[i] > worst ? v[i] : worst;
        }
        qsort(v, (size_t)cases[c].n, sizeof v[0], wc_cmp);
        double med = v[cases[c].n / 2];
        printf("  timeout %5.3f s: median %.3f ms, worst %.3f ms over %d\n", cases[c].t, med * 1e3, worst * 1e3, cases[c].n);
        CHECK(!said_true);
        CHECK(!early);
        CHECK(med < cases[c].t + 0.003 + cases[c].t * 0.01);
    }
}

/* The process spends nothing while it waits: 2 s asleep cost less than 5 ms of CPU. */
static void wc_idle_cpu(PlatformWindow *w, double seconds)
{
    wc_drain(w);
    double c0 = wc_cpu(), t0 = wc_now();
    bool r = platform_wait_events(seconds);
    double wall = wc_now() - t0, cpu = wc_cpu() - c0;
    printf("  idle %.1f s: wall %.3f s, CPU %.3f ms\n", seconds, wall, cpu * 1e3);
    CHECK(!r);
    CHECK(wall >= seconds && wall < seconds + 0.1);
    CHECK(cpu < 0.005);
}

/* A wake before the wait ends the next wait at once, once; wakes that come before it count as one. */
static void wc_wake_semantics(PlatformWindow *w)
{
    wc_drain(w);
    platform_wake();
    double t0 = wc_now();
    CHECK(platform_wait_events(5.0));
    CHECK(wc_now() - t0 < 0.05);
    CHECK(!platform_wait_events(0.03)); /* exactly once */
    for (int i = 0; i < 5; i++)
        platform_wake();
    t0 = wc_now();
    CHECK(platform_wait_events(5.0));
    CHECK(wc_now() - t0 < 0.05);
    CHECK(!platform_wait_events(0.03)); /* the five were one */
    CHECK(!platform_wait_events(0.0));
    /* a timeout of 0 with a wake pending takes it too */
    platform_wake();
    CHECK(platform_wait_events(0.0));
    CHECK(!platform_wait_events(0.0));
    /* NaN is a poll, huge and negative values are no limit (a wake ends them) */
    CHECK(!platform_wait_events(__builtin_nan("")));
    platform_wake();
    CHECK(platform_wait_events(1e300));
    platform_wake();
    CHECK(platform_wait_events(-5.0));
}

/* ---- another thread ---- */

typedef struct
{
    int delay_ms;
    double *stamp; /* when the wake was posted */
} Waker;

static void *wc_waker_main(void *arg)
{
    Waker *k = arg;
    wc_msleep(k->delay_ms);
    *k->stamp = wc_now(); /* read by the main thread after pthread_join */
    platform_wake();
    return NULL;
}

/* A wake from another thread ends a wait that has no limit: how long after the post, 200 times. */
static void wc_wake_latency(PlatformWindow *w)
{
    enum { N = 200 };
    double lat[N];
    int late = 0;
    for (int i = 0; i < N; i++)
    {
        wc_drain(w);
        double stamp = 0;
        Waker k = {2 + i % 5, &stamp};
        pthread_t t;
        CHECK(pthread_create(&t, NULL, wc_waker_main, &k) == 0);
        bool r = platform_wait_events(-1);
        double back = wc_now();
        pthread_join(t, NULL);
        CHECK(r);
        lat[i] = back - stamp;
        late += lat[i] > 0.05;
    }
    qsort(lat, N, sizeof lat[0], wc_cmp);
    printf("  wake from a thread, %d times: median %.3f ms, 99th %.3f ms, worst %.3f ms\n", N, lat[N / 2] * 1e3,
           lat[N * 99 / 100] * 1e3, lat[N - 1] * 1e3);
    CHECK(lat[N / 2] < 0.003);
    CHECK(late <= 2);
}

enum { STORM_THREADS = 4, STORM_EACH = 2500 };
static int g_storm_posted;

static void *wc_storm_main(void *arg)
{
    (void)arg;
    for (int i = 0; i < STORM_EACH; i++)
    {
        __atomic_add_fetch(&g_storm_posted, 1, __ATOMIC_SEQ_CST); /* the thing that happened... */
        platform_wake();                                          /* ...and the call that says so */
        if (i % 97 == 0)
            sched_yield();
    }
    return NULL;
}

/* 10 000 wakes from 4 threads against a main thread that waits (and works a little between waits, so some wakes come
   while it is not asleep): after every wake the main thread comes back and finds what was posted, and when the posters are
   done it gets everything. A lost wake would leave it waiting for the 5 s guard with posts it never saw. */
static void wc_wake_storm(PlatformWindow *w)
{
    wc_drain(w);
    g_storm_posted = 0;
    pthread_t t[STORM_THREADS];
    for (int i = 0; i < STORM_THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, wc_storm_main, NULL) == 0);
    const int total = STORM_THREADS * STORM_EACH;
    int seen = 0, returns = 0, lost = 0;
    double t0 = wc_now();
    while (seen < total)
    {
        if (!platform_wait_events(5.0))
        {
            lost = 1; /* 5 s with posts that no wake brought us to */
            break;
        }
        returns++;
        seen = __atomic_load_n(&g_storm_posted, __ATOMIC_SEQ_CST);
        if (returns % 5 == 0)
            for (volatile int spin = 0; spin < 2000; spin++)
            {
            }
    }
    for (int i = 0; i < STORM_THREADS; i++)
        pthread_join(t[i], NULL);
    printf("  %d wakes from %d threads in %.1f ms: the main thread came back %d times\n", total, STORM_THREADS,
           (wc_now() - t0) * 1e3, returns);
    CHECK(!lost);
    CHECK(seen == total);
    wc_drain(w);
}

/* ---- signals ---- */

static volatile sig_atomic_t g_sig_count;

static void wc_on_alarm(int sig)
{
    (void)sig;
    g_sig_count++;
}

static void wc_on_alarm_wake(int sig)
{
    (void)sig;
    g_sig_count++;
    platform_wake(); /* a write on a descriptor that exists: all a handler needs to get a loop to look at its flag */
}

static void wc_set_alarm(void (*fn)(int), int ms)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = fn;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; /* no SA_RESTART: poll(2) is never restarted anyway, but the point is the call comes back */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 0}, {ms / 1000, (ms % 1000) * 1000}};
    setitimer(ITIMER_REAL, &it, NULL);
}

/* A signal makes the wait come back (false, early) so a program can look at its flag; a handler that wakes makes the next
   wait return at once. */
static void wc_eintr(PlatformWindow *w)
{
    wc_drain(w);
    g_sig_count = 0;
    wc_set_alarm(wc_on_alarm, 30);
    double t0 = wc_now();
    bool r = platform_wait_events(5.0);
    double took = wc_now() - t0;
    CHECK(g_sig_count == 1);
    CHECK(!r);
    CHECK(took < 1.0);
    printf("  a signal ended a 5 s wait after %.1f ms\n", took * 1e3);
    /* the wait that follows is a normal one */
    CHECK(!platform_wait_events(0.01));

    g_sig_count = 0;
    wc_set_alarm(wc_on_alarm_wake, 30);
    t0 = wc_now();
    r = platform_wait_events(5.0);
    took = wc_now() - t0;
    if (!r) /* the signal came first: the wake it posted waits for the next wait */
    {
        t0 = wc_now();
        r = platform_wait_events(5.0);
        took = wc_now() - t0;
        CHECK(took < 0.05);
    }
    CHECK(r);
    CHECK(g_sig_count == 1);
    CHECK(!platform_wait_events(0.02));
    signal(SIGALRM, SIG_DFL);
}

/* ---- the loop that sleeps ---- */

typedef struct
{
    int frames;
    int limit;
    double at[16];
    int mode; /* 0 nothing, 1 app_request_frame until limit, 2 app_request_frame_at once */
    double delay;
    PlatformWindow *w;
} LoopState;

static void wc_loop_frame(PlatformWindow *w, void *user)
{
    LoopState *s = user;
    if (s->frames < 16)
        s->at[s->frames] = wc_now();
    s->frames++;
    if (s->mode == 1 && s->frames < s->limit)
        app_request_frame();
    else if (s->mode == 2 && s->frames == 1)
        app_request_frame_at(time_seconds() + s->delay);
    if (s->frames >= s->limit)
        window_set_should_close(w, true);
}

typedef struct
{
    int wakes, gap_ms;
} Poster;

static void *wc_poster_main(void *arg)
{
    Poster *p = arg;
    for (int i = 0; i < p->wakes; i++)
    {
        wc_msleep(p->gap_ms);
        platform_wake();
    }
    return NULL;
}

/* app_run_events: the first frame runs; with nothing happening no other does and the process sleeps; each wake (spaced
   far enough not to merge) runs one; app_request_frame runs frames back to back; app_request_frame_at runs one at the time. */
static void wc_run_events(PlatformWindow *w)
{
    wc_drain(w);
    /* idle between wakes: 6 wakes 60 ms apart give 1 + 6 frames and a little CPU */
    {
        LoopState s = {.limit = 7, .mode = 0, .w = w};
        Poster p = {6, 60};
        pthread_t t;
        pthread_create(&t, NULL, wc_poster_main, &p);
        double c0 = wc_cpu(), t0 = wc_now();
        app_run_events(w, wc_loop_frame, &s);
        double wall = wc_now() - t0, cpu = wc_cpu() - c0;
        pthread_join(t, NULL);
        printf("  app_run_events: %d frames in %.0f ms, CPU %.2f ms\n", s.frames, wall * 1e3, cpu * 1e3);
        CHECK(s.frames == 7);
        CHECK(wall >= 0.35);
        CHECK(cpu < 0.02);
        window_set_should_close(w, false);
        wc_drain(w);
    }
    /* an animation: each frame asks for the next, ten of them, with no waiting in between */
    {
        LoopState s = {.limit = 10, .mode = 1, .w = w};
        double t0 = wc_now();
        app_run_events(w, wc_loop_frame, &s);
        CHECK(s.frames == 10);
        CHECK(wc_now() - t0 < 0.5);
        window_set_should_close(w, false);
        wc_drain(w);
    }
    /* a deadline: the second frame comes 80 ms after the first, not before, and nothing runs in between */
    {
        LoopState s = {.limit = 2, .mode = 2, .delay = 0.08, .w = w};
        double c0 = wc_cpu();
        app_run_events(w, wc_loop_frame, &s);
        double cpu = wc_cpu() - c0;
        CHECK(s.frames == 2);
        double gap = s.at[1] - s.at[0];
        printf("  app_request_frame_at(+80 ms): the frame came %.2f ms later, CPU %.2f ms\n", gap * 1e3, cpu * 1e3);
        CHECK(gap >= 0.08 && gap < 0.12);
        CHECK(cpu < 0.01);
        window_set_should_close(w, false);
        wc_drain(w);
    }
    /* an earlier deadline wins over a later one, and the request is cleared once it has run */
    {
        app_request_frame_at(time_seconds() + 5.0);
        app_request_frame_at(time_seconds() + 0.05);
        LoopState s = {.limit = 3, .mode = 0, .w = w};
        Poster p = {2, 150};
        pthread_t t;
        pthread_create(&t, NULL, wc_poster_main, &p);
        double t0 = wc_now();
        app_run_events(w, wc_loop_frame, &s);
        pthread_join(t, NULL);
        /* frame 1 (the first), frame 2 (the 50 ms deadline), frame 3 (the first wake at 150 ms) */
        CHECK(s.frames == 3);
        CHECK(wc_now() - t0 < 0.5);
        window_set_should_close(w, false);
        wc_drain(w);
    }
}

/* ---- the platform comes and goes ---- */

/* A thousand init/shutdown cycles, with a wait and a wake in each: no descriptor is left behind (the wake's own is made once
   and kept for the life of the process, so the count is the same after the first cycle). */
static void wc_fd_leak(int cycles)
{
    platform_shutdown();
    for (int i = 0; i < 5; i++)
    {
        CHECK(platform_init());
        platform_wake();
        platform_wait_events(0);
        platform_shutdown();
    }
    int before = wc_open_fds();
    for (int i = 0; i < cycles; i++)
    {
        if (!platform_init())
        {
            CHECK(false);
            return;
        }
        platform_wake();
        platform_wait_events(0);
        platform_wait_events(0);
        platform_shutdown();
    }
    int after = wc_open_fds();
    printf("  descriptors before %d, after %d cycles %d\n", before, cycles, after);
    CHECK(before > 0 && after <= before);
}

/* Without the platform the wait sleeps on the wake alone: a wake before platform_init is not lost, and a wait with the
   platform down times out. */
static void wc_without_platform(void)
{
    platform_shutdown();
    CHECK(!platform_wait_events(0.02));
    platform_wake();
    double t0 = wc_now();
    CHECK(platform_wait_events(5.0));
    CHECK(wc_now() - t0 < 0.05);
    CHECK(!platform_wait_events(0.02));
    CHECK(platform_init());
}

#endif /* WAIT_COMMON_H */
