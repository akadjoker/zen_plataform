/*
 * test_drop_x11.c - files dragged from another X client into a window (XDND).
 * The source is a forked child with its own Display: it announces the drag, owns
 * XdndSelection with a text/uri-list, and drops. Needs a display; skips (77) without one.
 */
#include "platform.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define XDND_VERSION 5

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

/* What the source does: a file manager, a text editor, or one that stops answering. */
enum { SRC_FILES, SRC_NO_URI, SRC_NEVER_ANSWERS };

/* One line of comment, two local files (one escaped), a web link and a file on
   another machine: only the two local ones are paths. */
static const char URIS[] = "# a comment\r\n"
                           "file:///tmp/zen%20drop/a%C3%A1.txt\r\n"
                           "http://example.com/x\r\n"
                           "file://localhost/tmp/b.png\r\n"
                           "file://otherhost/tmp/c\r\n";

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ---- the source: a child process with its own connection ---- */

static Atom atom(Display *d, const char *name)
{
    return XInternAtom(d, name, False);
}

static void send(Display *d, Window to, Atom type, long l0, long l1, long l2, long l3, long l4)
{
    XEvent m;
    memset(&m, 0, sizeof m);
    m.xclient.type = ClientMessage;
    m.xclient.window = to;
    m.xclient.message_type = type;
    m.xclient.format = 32;
    m.xclient.data.l[0] = l0;
    m.xclient.data.l[1] = l1;
    m.xclient.data.l[2] = l2;
    m.xclient.data.l[3] = l3;
    m.xclient.data.l[4] = l4;
    XSendEvent(d, to, False, NoEventMask, &m);
    XFlush(d);
}

static bool next_event(Display *d, XEvent *ev, double until)
{
    for (;;)
    {
        if (XPending(d))
        {
            XNextEvent(d, ev);
            return true;
        }
        if (now_s() > until)
            return false;
        usleep(2000);
    }
}

static void answer_request(Display *d, XSelectionRequestEvent *rq, Atom uri)
{
    XSelectionEvent r = {.type = SelectionNotify, .display = rq->display, .requestor = rq->requestor,
                         .selection = rq->selection, .target = rq->target, .time = rq->time, .property = None};
    Atom prop = rq->property ? rq->property : rq->target;
    if (rq->target == uri)
    {
        XChangeProperty(d, rq->requestor, prop, uri, 8, PropModeReplace, (const unsigned char *)URIS,
                        (int)strlen(URIS));
        r.property = prop;
    }
    XSendEvent(d, rq->requestor, False, NoEventMask, (XEvent *)&r);
    XFlush(d);
}

/* Exit status: 0 when the source got what it expected, 1 when not, 2+ for timeouts. */
static void run_source(Window target, int scenario)
{
    Display *d = XOpenDisplay(NULL);
    if (!d)
        _exit(2);
    Window src = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    Atom sel = atom(d, "XdndSelection");
    Atom uri = atom(d, "text/uri-list");
    Atom plain = atom(d, "text/plain");
    Atom png = atom(d, "image/png");
    Atom utf8 = atom(d, "UTF8_STRING");
    Atom custom = atom(d, "application/x-zen-drop");

    /* four types, so the list goes in XdndTypeList and not in the message */
    Atom types[4] = {plain, png, scenario == SRC_NO_URI ? custom : uri, scenario == SRC_NO_URI ? utf8 : custom};
    XSetSelectionOwner(d, sel, src, CurrentTime);
    XChangeProperty(d, src, atom(d, "XdndTypeList"), XA_ATOM, 32, PropModeReplace, (unsigned char *)types, 4);

    send(d, target, atom(d, "XdndEnter"), (long)src, (long)(XDND_VERSION << 24) | 1, (long)types[0],
         (long)types[1], (long)types[2]);
    send(d, target, atom(d, "XdndPosition"), (long)src, 0, (10 << 16) | 10, CurrentTime,
         (long)atom(d, "XdndActionCopy"));

    XEvent ev;
    long status_flags = -1;
    double until = now_s() + 3;
    while (status_flags < 0 && next_event(d, &ev, until))
        if (ev.type == ClientMessage && ev.xclient.message_type == atom(d, "XdndStatus"))
            status_flags = ev.xclient.data.l[1];
    if (status_flags < 0)
        _exit(2);
    bool accepted = (status_flags & 1) != 0;
    if (scenario == SRC_NO_URI)
        _exit(accepted ? 1 : 0);
    if (!accepted)
        _exit(1);

    send(d, target, atom(d, "XdndDrop"), (long)src, 0, CurrentTime, 0, 0);

    bool finished = false;
    long finished_flags = -1;
    until = now_s() + (scenario == SRC_NEVER_ANSWERS ? 8 : 4);
    while (!finished && next_event(d, &ev, until))
    {
        if (ev.type == SelectionRequest && ev.xselectionrequest.selection == sel)
        {
            if (scenario != SRC_NEVER_ANSWERS)
                answer_request(d, &ev.xselectionrequest, uri);
        }
        else if (ev.type == ClientMessage && ev.xclient.message_type == atom(d, "XdndFinished"))
        {
            finished = true;
            finished_flags = ev.xclient.data.l[1];
        }
    }
    if (scenario == SRC_NEVER_ANSWERS)
        _exit(3);
    _exit(finished && (finished_flags & 1) ? 0 : 1);
}

/* ---- the window: this process, pumped by the platform ---- */

static char *g_paths[8];
static int g_n;

static void clear_drop(void)
{
    for (int i = 0; i < g_n && i < 8; i++)
        free(g_paths[i]);
    g_n = 0;
}

/* Pumps the window until a drop arrives or secs pass; a drop's paths are copied out. */
static bool pump_for_drop(PlatformWindow *w, double secs, int *frames)
{
    double until = now_s() + secs;
    bool got = false;
    *frames = 0;
    while (!got && now_s() < until)
    {
        window_begin_frame(w);
        Event e;
        while (poll_event(w, &e))
            if (e.type == EVENT_WINDOW_DROP && !got)
            {
                got = true;
                g_n = e.data.drop.count;
                for (int i = 0; i < g_n && i < 8; i++)
                    g_paths[i] = strdup(e.data.drop.paths[i]);
            }
        (*frames)++;
        usleep(2000);
    }
    return got;
}

static int wait_child(pid_t pid)
{
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

static void test_files(PlatformWindow *w, Window target)
{
    int frames;
    pid_t pid = fork();
    if (pid == 0)
        run_source(target, SRC_FILES);
    bool got = pump_for_drop(w, 5, &frames);
    CHECK(got);
    CHECK(g_n == 2);
    CHECK(g_n == 2 && strcmp(g_paths[0], "/tmp/zen drop/a\xC3\xA1.txt") == 0);
    CHECK(g_n == 2 && strcmp(g_paths[1], "/tmp/b.png") == 0);
    CHECK(wait_child(pid) == 0); /* the source was told it was accepted */
    clear_drop();
}

static void test_no_uri(PlatformWindow *w, Window target)
{
    int frames;
    pid_t pid = fork();
    if (pid == 0)
        run_source(target, SRC_NO_URI);
    bool got = pump_for_drop(w, 2, &frames);
    CHECK(!got); /* refused in XdndStatus, so no drop */
    CHECK(wait_child(pid) == 0);
    clear_drop();
}

static void test_never_answers(PlatformWindow *w, Window target)
{
    int frames;
    pid_t pid = fork();
    if (pid == 0)
        run_source(target, SRC_NEVER_ANSWERS);
    /* Pumping goes on the whole time: a source that never answers cannot stop the loop. */
    bool got = pump_for_drop(w, 1.5, &frames);
    CHECK(!got);
    CHECK(frames > 100);
    kill(pid, SIGKILL);
    wait_child(pid);
    /* The window gives up on that drop after its timeout, so the next one works. */
    pump_for_drop(w, 6, &frames);
    clear_drop();
}

int main(void)
{
    if (!platform_init())
    {
        printf("skip: no display\n");
        return 77;
    }
    PlatformWindow *w = window_create(&(WindowConfig){.title = "drop", .width = 200, .height = 120});
    if (!w)
    {
        printf("skip: no window\n");
        platform_shutdown();
        return 77;
    }
    Window target = (Window)(uintptr_t)window_native_handle(w, NATIVE_WINDOW);

    test_files(w, target);
    test_no_uri(w, target);
    test_never_answers(w, target);
    test_files(w, target); /* after the timeout: the window takes drags again */

    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
