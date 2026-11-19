/*
 * test_wait_x11.c - platform_wait_events and platform_wake on the X11 backend, on the private Xvfb of tools/xvfb_run.sh:
 * the wait with nothing pending sleeps for its timeout using no CPU; an event injected by another process wakes it within
 * a few ms and is then delivered by the normal frame (the wait takes nothing); events that Xlib already read off the socket
 * (a round trip of the program's own does that) do not make it sleep; a drop of a window that belongs to another window's
 * pump counts; a key held is looked at; the server dying while it sleeps ends it. The checks that do not depend on the
 * window system are those of tests/wait_common.h. Every section runs in a child of its own. Skips (77) without a display.
 */
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/XKBlib.h>
#ifdef HAVE_XTEST
#include <X11/extensions/XTest.h>
#endif

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>

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

static PlatformWindow *make(int w, int h)
{
    WindowConfig cfg = {.title = "wait", .width = w, .height = h, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED,
                        .render = RENDER_PIXELS, .resizable = true};
    return window_create(&cfg);
}

static Display *dpy_of(PlatformWindow *w)
{
    return window_native_handle(w, NATIVE_DISPLAY);
}

static void settle(PlatformWindow *w)
{
    for (int i = 0; i < 6; i++)
    {
        window_begin_frame(w);
        wc_msleep(3);
    }
}

static void quiet(PlatformWindow *w)
{
    settle(w);
    wc_drain(w);
}

/* ---- the checks that do not depend on the window system, on a real window ---- */

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
    printf("== signals\n");
    wc_eintr(w);
    printf("== app_run_events\n");
    wc_run_events(w);
    window_destroy(w);
}

static void test_descriptors(void)
{
    printf("== descriptors, without the platform\n");
    /* a server without -noreset resets when its last client goes, and refuses connections meanwhile: another client stays */
    Display *probe = XOpenDisplay(NULL);
    CHECK(probe != NULL);
    wc_fd_leak(1000);
    wc_without_platform();
    if (probe)
        XCloseDisplay(probe);
}

#ifdef HAVE_XTEST

/* ---- events from another process ---- */

/* The child: opens its own connection, waits delay_ms, notes the time, moves the pointer (XTest) between the points a and
   b (index picks one), and tells the parent when it did it. */
static pid_t inject_motion(int delay_ms, int x, int y, int pipe_fd)
{
    pid_t pid = fork();
    if (pid != 0)
        return pid;
    Display *d = XOpenDisplay(NULL);
    if (!d)
        _exit(2);
    wc_msleep(delay_ms);
    double t = wc_now();
    XTestFakeMotionEvent(d, -1, x, y, 0);
    XFlush(d);
    ssize_t n = write(pipe_fd, &t, sizeof t);
    (void)n;
    XCloseDisplay(d);
    _exit(0);
}

static void origin(PlatformWindow *w, int *x, int *y)
{
    Display *d = dpy_of(w);
    Window child;
    XTranslateCoordinates(d, (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW), DefaultRootWindow(d), 0, 0, x, y, &child);
}

static int count_moves(PlatformWindow *w)
{
    int n = 0;
    Event e;
    while (poll_event(w, &e))
        n += e.type == EVENT_MOUSE_MOVE;
    return n;
}

/* An event from another process: the wait comes back true within a few ms, does not take the event (it says true again),
   and the frame that follows delivers it. */
static void test_event_from_process(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    int ox, oy;
    quiet(w);
    origin(w, &ox, &oy);
    int fds[2];
    CHECK(pipe(fds) == 0);
    enum { N = 60 };
    double lat[N];
    int delivered = 0, wrong = 0;
    for (int i = 0; i < N; i++)
    {
        pid_t pid = inject_motion(15, ox + 40 + (i % 2) * 120, oy + 40 + (i % 2) * 60, fds[1]);
        bool r = platform_wait_events(5.0);
        double back = wc_now();
        double stamp = 0;
        CHECK(read(fds[0], &stamp, sizeof stamp) == (ssize_t)sizeof stamp);
        waitpid(pid, NULL, 0);
        lat[i] = back - stamp;
        wrong += !r;
        CHECK(platform_wait_events(0.0)); /* not consumed */
        window_begin_frame(w);
        delivered += count_moves(w) > 0;
        CHECK(!platform_wait_events(0.005)); /* and now nothing is left */
    }
    qsort(lat, N, sizeof lat[0], wc_cmp);
    printf("  an event from another process: wake latency median %.3f ms, 99th %.3f ms, worst %.3f ms (%d)\n",
           lat[N / 2] * 1e3, lat[N * 99 / 100] * 1e3, lat[N - 1] * 1e3, N);
    CHECK(wrong == 0);
    CHECK(delivered == N);
    CHECK(lat[N / 2] < 0.005);
    close(fds[0]);
    close(fds[1]);
    window_destroy(w);
}

/* The lost wake-up: a round trip of the program's own (XSync, XQueryKeymap, any reply wait) reads the events that came
   before the reply into Xlib's queue, and the socket is quiet again. A wait that polls the socket alone sleeps through
   them. */
static void test_events_already_in_xlib(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    int ox, oy;
    quiet(w);
    origin(w, &ox, &oy);
    int late = 0;
    for (int i = 0; i < 30; i++)
    {
        XTestFakeMotionEvent(d, -1, ox + 30 + (i % 2) * 100, oy + 30, 0);
        XSync(d, False); /* the motion event is queued in Xlib now, ahead of the reply */
        wc_msleep(2);
        double t0 = wc_now();
        bool r = platform_wait_events(2.0);
        double took = wc_now() - t0;
        late += !r || took > 0.1;
        window_begin_frame(w);
        CHECK(count_moves(w) > 0);
    }
    CHECK(late == 0);
    /* the same with events that belong to a window whose own pump has not run: another window's pump read them */
    PlatformWindow *w2 = make(200, 150);
    CHECK(w2 != NULL);
    if (w2)
    {
        quiet(w2);
        quiet(w);
        int bx, by;
        origin(w2, &bx, &by);
        XTestFakeMotionEvent(d, -1, bx + 50, by + 50, 0);
        XSync(d, False);
        wc_msleep(5);
        window_begin_frame(w); /* reads the event off the queue and keeps it for w2 */
        double t0 = wc_now();
        bool r = platform_wait_events(2.0);
        CHECK(r);
        CHECK(wc_now() - t0 < 0.1);
        window_begin_frame(w2);
        CHECK(count_moves(w2) > 0);
        CHECK(!platform_wait_events(0.01));
        window_destroy(w2);
    }
    window_destroy(w);
}

/* A key held in a focused window: the wait returns (false) within 0.1 s so that the check for a release the server never
   sent can run, and the library stays quiet otherwise. Auto-repeat is switched off for the test so that the ticks are
   what is measured. */
static void test_held_key_tick(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    Display *d = dpy_of(w);
    int ox, oy;
    quiet(w);
    origin(w, &ox, &oy);
    XSetInputFocus(d, (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW), RevertToParent, CurrentTime);
    XAutoRepeatOff(d);
    XTestFakeMotionEvent(d, -1, ox + 50, oy + 50, 0);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, 'a'), True, 0);
    XSync(d, False);
    wc_msleep(20);
    window_begin_frame(w);
    CHECK(key_down(w, KEY_A));
    wc_drain(w);
    int ticks = 0;
    double t0 = wc_now(), worst = 0;
    while (wc_now() - t0 < 1.0)
    {
        double a = wc_now();
        bool r = platform_wait_events(-1);
        double took = wc_now() - a;
        worst = took > worst ? took : worst;
        ticks += !r;
        window_begin_frame(w);
        CHECK(key_down(w, KEY_A)); /* the server does hold it: the check changes nothing */
    }
    printf("  a key held: %d ticks in 1 s, the longest wait %.1f ms\n", ticks, worst * 1e3);
    CHECK(ticks >= 6 && ticks <= 14);
    CHECK(worst < 0.15);
    XTestFakeKeyEvent(d, XKeysymToKeycode(d, 'a'), False, 0);
    XSync(d, False);
    wc_msleep(20);
    window_begin_frame(w);
    wc_drain(w);
    double c0 = wc_cpu();
    CHECK(!platform_wait_events(0.5)); /* released: no tick any more */
    CHECK(wc_cpu() - c0 < 0.003);
    XAutoRepeatOn(d);
    window_destroy(w);
}

/* app_run_events with input from another process: a frame for each tap, none between, and little CPU. */
static void tap_frame(PlatformWindow *w, void *user)
{
    int *frames = user;
    Event e;
    while (poll_event(w, &e))
        if (e.type == EVENT_MOUSE_MOVE)
            (*frames) += 1000; /* a motion */
    (*frames)++;
    if (*frames >= 8000)
        window_set_should_close(w, true);
}

static void test_run_events_with_input(void)
{
    PlatformWindow *w = make(300, 200);
    CHECK(w != NULL);
    if (!w)
        return;
    int ox, oy;
    quiet(w);
    origin(w, &ox, &oy);
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t kids[8];
    int frames = 0;
    double c0 = wc_cpu(), t0 = wc_now();
    for (int i = 0; i < 8; i++)
        kids[i] = inject_motion(40 * (i + 1), ox + 30 + (i % 2) * 100, oy + 30, fds[1]);
    app_run_events(w, tap_frame, &frames);
    double wall = wc_now() - t0, cpu = wc_cpu() - c0;
    for (int i = 0; i < 8; i++)
        waitpid(kids[i], NULL, 0);
    printf("  app_run_events: %d motions, %d frames in %.0f ms, CPU %.2f ms\n", frames / 1000, frames % 1000, wall * 1e3, cpu * 1e3);
    CHECK(frames / 1000 == 8);
    CHECK(frames % 1000 <= 12); /* the first frame, one per motion, perhaps a service tick: not a frame per turn of a spin */
    CHECK(wall >= 0.3);
    CHECK(cpu < 0.05);
    close(fds[0]);
    close(fds[1]);
    window_destroy(w);
}

/* The window destroyed: by the program (the frame of app_run_events destroys it: the loop ends), then a wait with no
   window left; and by another client while the program sleeps, which tells the loop through the connection. */
static void destroy_frame(PlatformWindow *w, void *user)
{
    (*(int *)user)++;
    window_destroy(w);
}

static void test_window_destroyed(void)
{
    PlatformWindow *w = make(200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    int frames = 0;
    app_run_events(w, destroy_frame, &frames);
    CHECK(frames == 1);
    double t0 = wc_now();
    CHECK(!platform_wait_events(0.05)); /* no window: nothing to wait for but the time */
    CHECK(wc_now() - t0 >= 0.05);
    platform_wake();
    CHECK(platform_wait_events(5.0));

    PlatformWindow *a = make(200, 150), *b = make(200, 150);
    CHECK(a && b);
    if (!a || !b)
        return;
    quiet(a);
    quiet(b);
    window_destroy(a);
    window_begin_frame(b); /* what a destroyed window left in the queue is read and dropped here */
    for (int i = 0; i < 100 && platform_wait_events(0.02); i++) /* the server's answers to it arrive a moment later */
        window_begin_frame(b);
    CHECK(!platform_wait_events(0.03));
    /* the last one goes: its events have no pump, and must not end every wait at once */
    window_destroy(b);
    double t0b = wc_now();
    CHECK(!platform_wait_events(0.2));
    CHECK(wc_now() - t0b >= 0.2);
    t0b = wc_now();
    CHECK(!platform_wait_events(0.2));
    CHECK(wc_now() - t0b >= 0.2);
}

/* The server dies while the program sleeps in a wait with no limit: the wait ends (true: the pump will tell the windows),
   and it does not spin. Runs against a server of its own. */
static pid_t g_own_xvfb;

static void pre_own_server(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return;
    pid_t pid = fork();
    if (pid == 0)
    {
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        close(fds[0]);
        char fd[16];
        snprintf(fd, sizeof fd, "%d", fds[1]);
        FILE *null = freopen("/dev/null", "w", stderr);
        (void)null;
        execlp("Xvfb", "Xvfb", "-displayfd", fd, "-screen", "0", "640x480x24", "-nolisten", "tcp", "-noreset", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char buf[32] = {0};
    ssize_t got = pid > 0 ? read(fds[0], buf, sizeof buf - 1) : -1;
    close(fds[0]);
    if (got > 0 && buf[0] >= '0' && buf[0] <= '9')
    {
        char name[40];
        snprintf(name, sizeof name, ":%d", atoi(buf));
        setenv("DISPLAY", name, 1);
        g_own_xvfb = pid;
    }
    else if (pid > 0)
    {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
    }
}

static void test_server_goes_away(void)
{
    if (g_own_xvfb <= 0)
    {
        printf("  skipped: could not start an Xvfb of its own\n");
        return;
    }
    PlatformWindow *w = make(200, 150);
    CHECK(w != NULL);
    if (!w)
        return;
    quiet(w);
    pid_t killer = fork();
    if (killer == 0)
    {
        wc_msleep(150);
        kill(g_own_xvfb, SIGKILL);
        _exit(0);
    }
    double c0 = wc_cpu(), t0 = wc_now();
    bool r = platform_wait_events(-1);
    double took = wc_now() - t0, cpu = wc_cpu() - c0;
    waitpid(killer, NULL, 0);
    waitpid(g_own_xvfb, NULL, 0);
    printf("  the server killed after 150 ms: the wait ended after %.1f ms, CPU %.2f ms\n", took * 1e3, cpu * 1e3);
    CHECK(r);
    CHECK(took >= 0.14 && took < 2.0);
    CHECK(cpu < 0.02);
    bool closed = false;
    for (int i = 0; i < 5 && !closed; i++)
    {
        window_begin_frame(w);
        Event e;
        while (poll_event(w, &e))
            closed |= e.type == EVENT_WINDOW_CLOSE;
    }
    CHECK(closed);
    CHECK(window_should_close(w));
    /* the loop that sleeps does not hang on a dead server: it ends (should_close) and a wait returns */
    CHECK(platform_wait_events(0.05));
    window_destroy(w);
}

#endif /* HAVE_XTEST */

typedef struct
{
    const char *name;
    void (*fn)(void);
    void (*pre)(void);
} Section;

int main(void)
{
    static const Section sections[] = {
        {"common checks on a window", test_common, NULL},
        {"descriptors", test_descriptors, NULL},
#ifdef HAVE_XTEST
        {"an event from another process", test_event_from_process, NULL},
        {"events already in Xlib's queue", test_events_already_in_xlib, NULL},
        {"a key held", test_held_key_tick, NULL},
        {"app_run_events with input", test_run_events_with_input, NULL},
        {"windows destroyed", test_window_destroyed, NULL},
        {"the server goes away while waiting", test_server_goes_away, pre_own_server},
#endif
    };
    int failed = 0, skipped = 0;
    const int count = (int)(sizeof sections / sizeof sections[0]);
    for (int i = 0; i < count; i++)
    {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(120);
            if (sections[i].pre)
                sections[i].pre();
            if (!platform_init())
                _exit(77);
            sections[i].fn();
            platform_shutdown();
            printf("%s: %d passed, %d failed\n", sections[i].name, g_pass, g_fail);
            fflush(stdout);
            _exit(g_fail ? 1 : 0);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 77)
            skipped++;
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        {
            failed++;
            if (WIFSIGNALED(st))
                printf("FAIL %s: killed by signal %d\n", sections[i].name, WTERMSIG(st));
            else
                printf("FAIL %s (status %d)\n", sections[i].name, WEXITSTATUS(st));
        }
    }
    if (skipped == count)
    {
        printf("skip: no display\n");
        return 77;
    }
    printf("%d sections failed\n", failed);
    return failed ? 1 : 0;
}
