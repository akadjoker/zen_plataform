/* bench_main.c - the modes of the comparison, written once and linked with one library file (bench_zp.c,
   bench_glfw.c, bench_sdl2.c). Each mode prints one line of key=value on stdout.

     bench_<lib> startup [gl]          ms from the process start (BENCH_T0, CLOCK_MONOTONIC ns, set by bench_run) to the
                                       window viewable and one event loop turn done, and its phases
     bench_<lib> idle N [gl]           ns per event loop turn with no event, N turns
     bench_<lib> cycle N [gl]          ns per window create + destroy, N cycles
     bench_<lib> mem [gl]              RSS and PSS after init, window, 1000 idle turns; shared libraries mapped
     bench_<lib> events motion|key N batch [gl]   waits for bench_run to inject N events, then drains them (see bench_run.c);
                                       batch 0: all N are waiting when the first turn starts (a frame that took long);
                                       batch B: B events wait for each turn (a frame in which B events arrived) */
#define _GNU_SOURCE
#include "bench.h"
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <poll.h>
#include <unistd.h>
#include <X11/Xutil.h>

static long long now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static long long cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void nap_ms(int ms)
{
    struct timespec ts = {0, ms * 1000000L};
    nanosleep(&ts, NULL);
}

static int viewable(void *win)
{
    XWindowAttributes a;
    return XGetWindowAttributes(lib_display(win), lib_xwin(win), &a) && a.map_state == IsViewable;
}

/* poll until the window is mapped on the server (bounded: a library that never maps fails the run) */
static int wait_viewable(void *win, Counts *c)
{
    for (int i = 0; i < 2000; i++)
    {
        lib_poll(win, c);
        if (viewable(win))
            return 1;
        nap_ms(1);
    }
    return 0;
}

static long kb_of(const char *buf, const char *key)
{
    const char *p = strstr(buf, key);
    return p ? atol(p + strlen(key)) : -1;
}

static void memory(long *rss, long *pss, long *pss_anon, long *pss_file, int *libs)
{
    char buf[2048] = {0};
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (f)
    {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = 0;
        fclose(f);
    }
    *rss = kb_of(buf, "Rss:");
    *pss = kb_of(buf, "Pss:");
    *pss_anon = kb_of(buf, "Pss_Anon:");
    *pss_file = kb_of(buf, "Pss_File:");
    /* distinct .so files mapped (the executable and the vdso are not libraries) */
    char line[1024], seen[256][256];
    int n = 0;
    f = fopen("/proc/self/maps", "r");
    while (f && fgets(line, sizeof line, f))
    {
        char *p = strchr(line, '/');
        if (!p || !strstr(p, ".so"))
            continue;
        p[strcspn(p, "\n")] = 0;
        int dup = 0;
        for (int i = 0; i < n; i++)
            dup |= strcmp(seen[i], p) == 0;
        if (!dup && n < 256)
            strncpy(seen[n++], p, 255);
    }
    if (f)
        fclose(f);
    *libs = n;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return 2;
    const char *mode = argv[1];
    int gl = strcmp(argv[argc - 1], "gl") == 0;
    long long n = argc > 2 ? atoll(argv[2]) : 0;
    long long t0 = getenv("BENCH_T0") ? atoll(getenv("BENCH_T0")) : now_ns();
    Counts c = {0};

    /* The input method of a window needs the locale; zen_platform sets it itself, GLFW and SDL leave it to the
       application. BENCH_SETLOCALE=1 makes the application do it for all three, so they do the same work. */
    if (getenv("BENCH_SETLOCALE"))
        setlocale(LC_CTYPE, "");

    if (strcmp(mode, "startup") == 0)
    {
        long long t1 = now_ns();
        if (!lib_init())
            return 1;
        long long t2 = now_ns();
        void *win = lib_window(gl, -1, -1, 640, 480);
        if (!win)
            return 1;
        long long t3 = now_ns();
        if (!wait_viewable(win, &c))
            return 1;
        long long t4 = now_ns();
        printf("lib=%s gl=%d startup_ms=%.3f main_ms=%.3f init_ms=%.3f window_ms=%.3f loop_ms=%.3f\n", lib_name(), gl,
               (t4 - t0) / 1e6, (t1 - t0) / 1e6, (t2 - t1) / 1e6, (t3 - t2) / 1e6, (t4 - t3) / 1e6);
        lib_window_free(win);
        lib_exit();
        return 0;
    }
    if (strcmp(mode, "idle") == 0)
    {
        if (!lib_init())
            return 1;
        void *win = lib_window(gl, -1, -1, 640, 480);
        if (!win || !wait_viewable(win, &c))
            return 1;
        for (int i = 0; i < 200; i++)
            lib_poll(win, &c);
        long long a = now_ns(), ca = cpu_ns();
        for (long long i = 0; i < n; i++)
            lib_poll(win, &c);
        long long b = now_ns(), cb = cpu_ns();
        printf("lib=%s gl=%d idle_n=%lld ns_per_poll=%.2f cpu_ns_per_poll=%.2f\n", lib_name(), gl, n,
               (double)(b - a) / (double)(n ? n : 1), (double)(cb - ca) / (double)(n ? n : 1));
        lib_window_free(win);
        lib_exit();
        return 0;
    }
    if (strcmp(mode, "cycle") == 0)
    {
        if (!lib_init())
            return 1;
        long long a = now_ns(), ca = cpu_ns();
        for (long long i = 0; i < n; i++)
        {
            void *win = lib_window(gl, -1, -1, 640, 480);
            if (!win)
                return 1;
            lib_window_free(win);
        }
        long long b = now_ns(), cb = cpu_ns();
        printf("lib=%s gl=%d cycle_n=%lld us_per_cycle=%.2f cpu_us_per_cycle=%.2f\n", lib_name(), gl, n,
               (double)(b - a) / 1e3 / (double)(n ? n : 1), (double)(cb - ca) / 1e3 / (double)(n ? n : 1));
        lib_exit();
        return 0;
    }
    if (strcmp(mode, "mem") == 0)
    {
        if (!lib_init())
            return 1;
        void *win = lib_window(gl, -1, -1, 640, 480);
        if (!win || !wait_viewable(win, &c))
            return 1;
        for (int i = 0; i < 1000; i++)
            lib_poll(win, &c);
        long rss, pss, pa, pf;
        int libs;
        memory(&rss, &pss, &pa, &pf, &libs);
        printf("lib=%s gl=%d rss_kb=%ld pss_kb=%ld pss_anon_kb=%ld pss_file_kb=%ld libs=%d\n", lib_name(), gl, rss, pss,
               pa, pf, libs);
        lib_window_free(win);
        lib_exit();
        return 0;
    }
    if (strcmp(mode, "events") == 0)
    {
        n = argc > 3 ? atoll(argv[3]) : 0;
        int batch = argc > 4 ? atoi(argv[4]) : 0;
        if (!lib_init())
            return 1;
        void *win = lib_window(gl, 100, 100, 400, 300);
        if (!win || !wait_viewable(win, &c))
            return 1;
        /* What a window manager does when it maps a window: gives it the keyboard focus. Without a manager (this Xvfb)
           nobody does, and a library that waits for FocusIn before it takes keys (SDL) or before it lets its input
           context compose (GLFW) would run a different path from one that does not wait. All three get the focus. */
        XSetInputFocus(lib_display(win), lib_xwin(win), RevertToParent, CurrentTime);
        XSync(lib_display(win), False);
        /* bench_run moves the pointer into the window, then asks for the first line; the second line is the go */
        char line[64];
        printf("READY1\n");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            return 1;
        for (int i = 0; i < 50; i++)
        {
            lib_poll(win, &c);
            nap_ms(1);
        }
        memset(&c, 0, sizeof c);
        printf("READY2\n");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin))
            return 1;
        if (batch > 0)
        {
            /* One turn per batch, on the harness's word; only the turns are timed. The events of a batch are all in
               the socket when its turn starts (bench_run syncs before it says go). */
            long long wall = 0, cpu = 0, turns2 = 0;
            while (fgets(line, sizeof line, stdin) && strncmp(line, "go", 2) == 0)
            {
                long long a = now_ns(), ca = cpu_ns();
                lib_poll(win, &c);
                wall += now_ns() - a;
                cpu += cpu_ns() - ca;
                turns2++;
                printf("ok\n");
                fflush(stdout);
            }
            printf("lib=%s gl=%d kind=%s injected=%lld batch=%d motion=%ld key_down=%ld key_up=%ld text=%ld other=%ld "
                   "turns=%lld wall_ms=%.3f cpu_ms=%.3f\n",
                   lib_name(), gl, argv[2], n, batch, c.motion, c.key_down, c.key_up, c.text, c.other, turns2,
                   wall / 1e6, cpu / 1e6);
            lib_window_free(win);
            lib_exit();
            return 0;
        }
        /* The events are in the socket (or on their way: the server keeps writing while this reads). A library does
           not have to deliver all of them (a queue that holds so many, zen_platform compacting moves), so the end
           is not an event: it is the connection quiet and nothing delivered for 30 ms. The time and the CPU time
           are those up to the last turn that did anything. */
        int fd = ConnectionNumber(lib_display(win));
        long long a = now_ns(), ca = cpu_ns(), last = a, clast = ca;
        long long turns = 0;
        for (;;)
        {
            long before = c.motion + c.key_down + c.key_up + c.text + c.other;
            lib_poll(win, &c);
            turns++;
            struct pollfd p = {fd, POLLIN, 0};
            int busy = c.motion + c.key_down + c.key_up + c.text + c.other != before || poll(&p, 1, 0) > 0 ||
                       XEventsQueued(lib_display(win), QueuedAlready) > 0;
            long long t = now_ns();
            if (busy)
            {
                last = t;
                clast = cpu_ns();
            }
            else if (t - last > 30000000LL)
                break;
            else
                nap_ms(1);
        }
        printf("lib=%s gl=%d kind=%s injected=%lld batch=0 motion=%ld key_down=%ld key_up=%ld text=%ld other=%ld turns=%lld "
               "wall_ms=%.3f cpu_ms=%.3f\n",
               lib_name(), gl, argv[2], n, c.motion, c.key_down, c.key_up, c.text, c.other, turns, (last - a) / 1e6,
               (clast - ca) / 1e6);
        lib_window_free(win);
        lib_exit();
        return 0;
    }
    return 2;
}
