/*
 * test_clipboard_x11.c - the real X11 CLIPBOARD selection, against other X clients.
 * The "other applications" are forked children with their own Display connection:
 * one owns the selection and serves it (including INCR for the large PNG), one
 * reads what this process owns. Needs a display; skips (77) without one.
 */
#include "platform.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define BIG_SIZE (2 * 1024 * 1024 + 123) /* not a multiple of any chunk size */
#define TEXT "ol\xC3\xA1 zen \xE2\x80\x94 clipboard"
#define CUSTOM_MIME "application/x-zen-test"
#define CUSTOM_DATA "custom!"

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

static uint8_t *make_big(void)
{
    uint8_t *p = malloc(BIG_SIZE);
    for (size_t i = 0; i < BIG_SIZE; i++)
        p[i] = (uint8_t)(i * 31 + 7);
    return p;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* ---- child 1: a foreign clipboard owner ---- */

typedef struct
{
    Display *d;
    Window w;
    Atom clipboard, targets, utf8, incr, png, custom, prop_data;
    /* one INCR transfer */
    bool incr_active;
    Window incr_requestor;
    Atom incr_property, incr_target;
    size_t incr_offset;
} Owner;

#define OWNER_CHUNK 65536

static void owner_answer(Owner *o, const uint8_t *big, XSelectionRequestEvent *rq)
{
    XSelectionEvent r = {.type = SelectionNotify, .display = rq->display, .requestor = rq->requestor,
                         .selection = rq->selection, .target = rq->target, .time = rq->time, .property = None};
    Atom prop = rq->property ? rq->property : rq->target;
    if (rq->target == o->targets)
    {
        Atom t[] = {o->targets, o->utf8, o->png, o->custom};
        XChangeProperty(o->d, rq->requestor, prop, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 4);
        r.property = prop;
    }
    else if (rq->target == o->utf8)
    {
        XChangeProperty(o->d, rq->requestor, prop, o->utf8, 8, PropModeReplace, (unsigned char *)TEXT, (int)strlen(TEXT));
        r.property = prop;
    }
    else if (rq->target == o->custom)
    {
        XChangeProperty(o->d, rq->requestor, prop, o->custom, 8, PropModeReplace, (unsigned char *)CUSTOM_DATA, (int)strlen(CUSTOM_DATA));
        r.property = prop;
    }
    else if (rq->target == o->png && !o->incr_active)
    {
        o->incr_active = true;
        o->incr_requestor = rq->requestor;
        o->incr_property = prop;
        o->incr_target = rq->target;
        o->incr_offset = 0;
        XSelectInput(o->d, rq->requestor, PropertyChangeMask);
        long total = (long)BIG_SIZE;
        XChangeProperty(o->d, rq->requestor, prop, o->incr, 32, PropModeReplace, (unsigned char *)&total, 1);
        r.property = prop;
    }
    (void)big;
    XSendEvent(o->d, rq->requestor, True, NoEventMask, (XEvent *)&r);
    XFlush(o->d);
}

static void owner_incr_step(Owner *o, const uint8_t *big, XPropertyEvent *pe)
{
    if (!o->incr_active || pe->window != o->incr_requestor || pe->atom != o->incr_property || pe->state != PropertyDelete)
        return;
    size_t left = BIG_SIZE - o->incr_offset;
    size_t n = left > OWNER_CHUNK ? OWNER_CHUNK : left;
    XChangeProperty(o->d, o->incr_requestor, o->incr_property, o->incr_target, 8, PropModeReplace,
                    big + o->incr_offset, (int)n);
    o->incr_offset += n;
    if (n == 0)
        o->incr_active = false;
    XFlush(o->d);
}

static void run_owner(int ready_fd)
{
    Owner o = {0};
    o.d = XOpenDisplay(NULL);
    if (!o.d)
        _exit(2);
    o.w = XCreateSimpleWindow(o.d, DefaultRootWindow(o.d), 0, 0, 1, 1, 0, 0, 0);
    o.clipboard = XInternAtom(o.d, "CLIPBOARD", False);
    o.targets = XInternAtom(o.d, "TARGETS", False);
    o.utf8 = XInternAtom(o.d, "UTF8_STRING", False);
    o.incr = XInternAtom(o.d, "INCR", False);
    o.png = XInternAtom(o.d, "image/png", False);
    o.custom = XInternAtom(o.d, CUSTOM_MIME, False);
    uint8_t *big = make_big();
    XSetSelectionOwner(o.d, o.clipboard, o.w, CurrentTime);
    XSync(o.d, False);
    (void)!write(ready_fd, "1", 1);

    double end = now_s() + 20.0;
    while (now_s() < end)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(o.d), &fds);
        struct timeval tv = {0, 20000};
        select(ConnectionNumber(o.d) + 1, &fds, NULL, NULL, &tv);
        while (XPending(o.d))
        {
            XEvent ev;
            XNextEvent(o.d, &ev);
            if (ev.type == SelectionRequest)
                owner_answer(&o, big, &ev.xselectionrequest);
            else if (ev.type == PropertyNotify)
                owner_incr_step(&o, big, &ev.xproperty);
        }
    }
    _exit(0);
}

/* ---- child 2: a foreign reader of what the parent owns ---- */

typedef struct
{
    Display *d;
    Window w;
    Atom clipboard, prop;
} Reader;

static bool reader_wait(Reader *r, int type, XEvent *ev)
{
    double end = now_s() + 5.0;
    while (now_s() < end)
    {
        if (XCheckTypedWindowEvent(r->d, r->w, type, ev))
            return true;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(r->d), &fds);
        struct timeval tv = {0, 10000};
        select(ConnectionNumber(r->d) + 1, &fds, NULL, NULL, &tv);
        XPending(r->d);
    }
    return false;
}

/* Fetch one target; returns a malloc'd copy. INCR is followed. */
static uint8_t *reader_fetch(Reader *r, Atom target, size_t *out_n, int *out_format)
{
    XEvent ev;
    XDeleteProperty(r->d, r->w, r->prop);
    XConvertSelection(r->d, r->clipboard, target, r->prop, r->w, CurrentTime);
    XFlush(r->d);
    if (!reader_wait(r, SelectionNotify, &ev) || ev.xselection.property == None)
        return NULL;

    Atom type, incr = XInternAtom(r->d, "INCR", False);
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    XGetWindowProperty(r->d, r->w, r->prop, 0, 1 << 28, True, AnyPropertyType, &type, &fmt, &n, &after, &data);
    if (type != incr)
    {
        size_t unit = fmt == 32 ? sizeof(long) : (size_t)fmt / 8;
        uint8_t *copy = malloc(n * unit + 1);
        memcpy(copy, data, n * unit);
        copy[n * unit] = 0;
        *out_n = n * unit;
        *out_format = fmt;
        XFree(data);
        return copy;
    }
    if (data)
        XFree(data);
    uint8_t *all = NULL;
    size_t have = 0;
    for (;;)
    {
        do
        {
            if (!reader_wait(r, PropertyNotify, &ev))
                return NULL;
        } while (ev.xproperty.atom != r->prop || ev.xproperty.state != PropertyNewValue);
        XGetWindowProperty(r->d, r->w, r->prop, 0, 1 << 28, True, AnyPropertyType, &type, &fmt, &n, &after, &data);
        if (type == None) /* the stale NewValue from the INCR announcement */
            continue;
        if (n == 0)
        {
            if (data)
                XFree(data);
            break;
        }
        all = realloc(all, have + n);
        memcpy(all + have, data, n);
        have += n;
        XFree(data);
    }
    *out_n = have;
    *out_format = 8;
    return all;
}

/* exit code 0 = everything matched */
static void run_reader(void)
{
    Reader r = {0};
    r.d = XOpenDisplay(NULL);
    if (!r.d)
        _exit(2);
    r.w = XCreateSimpleWindow(r.d, DefaultRootWindow(r.d), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(r.d, r.w, PropertyChangeMask);
    r.clipboard = XInternAtom(r.d, "CLIPBOARD", False);
    r.prop = XInternAtom(r.d, "READER_PROP", False);
    Atom utf8 = XInternAtom(r.d, "UTF8_STRING", False);
    Atom png = XInternAtom(r.d, "image/png", False);
    Atom targets = XInternAtom(r.d, "TARGETS", False);
    Atom custom = XInternAtom(r.d, CUSTOM_MIME, False);

    size_t n;
    int fmt;
    uint8_t *t = reader_fetch(&r, targets, &n, &fmt);
    if (!t || fmt != 32)
        _exit(10);
    bool has_utf8 = false, has_png = false, has_custom = false;
    for (size_t i = 0; i + sizeof(Atom) <= n; i += sizeof(Atom))
    {
        Atom a;
        memcpy(&a, t + i, sizeof a);
        has_utf8 |= a == utf8;
        has_png |= a == png;
        has_custom |= a == custom;
    }
    if (!has_utf8 || !has_png || !has_custom)
        _exit(11);

    uint8_t *text = reader_fetch(&r, utf8, &n, &fmt);
    if (!text || n != strlen(TEXT) || memcmp(text, TEXT, n) != 0)
        _exit(12);

    uint8_t *cu = reader_fetch(&r, custom, &n, &fmt);
    if (!cu || n != strlen(CUSTOM_DATA) || memcmp(cu, CUSTOM_DATA, n) != 0)
        _exit(13);

    uint8_t *img = reader_fetch(&r, png, &n, &fmt);
    uint8_t *big = make_big();
    if (!img || n != BIG_SIZE || memcmp(img, big, BIG_SIZE) != 0)
        _exit(14);

    /* a target we never offered is refused */
    uint8_t *none = reader_fetch(&r, XInternAtom(r.d, "image/x-never", False), &n, &fmt);
    if (none)
        _exit(15);
    _exit(0);
}

/* ---- the tests ---- */

static pid_t spawn_owner(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        run_owner(fds[1]);
    }
    close(fds[1]);
    char c;
    ssize_t got = read(fds[0], &c, 1);
    close(fds[0]);
    return got == 1 ? pid : -1;
}

static void test_foreign_owner(void)
{
    pid_t pid = spawn_owner();
    CHECK(pid > 0);
    if (pid <= 0)
        return;

    CHECK(clipboard_has_data(CLIPBOARD_TEXT));
    CHECK(clipboard_has_data(CLIPBOARD_PNG));
    CHECK(clipboard_has_data(CUSTOM_MIME));
    CHECK(!clipboard_has_data("image/x-never"));
    CHECK(!clipboard_has_data("text/uri-list"));

    CHECK(strcmp(clipboard_get(), TEXT) == 0);

    size_t n = 0;
    char *c = clipboard_get_data(CUSTOM_MIME, &n);
    CHECK(c && n == strlen(CUSTOM_DATA) && memcmp(c, CUSTOM_DATA, n) == 0);
    fs_free(c);

    /* 2 MB: arrives by INCR, in 64 KB chunks */
    double t0 = now_s();
    uint8_t *img = clipboard_get_data(CLIPBOARD_PNG, &n);
    uint8_t *big = make_big();
    CHECK(img != NULL && n == BIG_SIZE);
    CHECK(img && memcmp(img, big, BIG_SIZE) == 0);
    CHECK(now_s() - t0 < 5.0);
    free(big);
    fs_free(img);

    CHECK(clipboard_get_data("image/x-never", &n) == NULL);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

/* An owner that never answers (hung, or gone without letting go of the selection)
   costs one timeout: the calls after it fail at once, so a paste or a refresh does
   not freeze the loop for a second per call. */
static pid_t spawn_dead_owner(void)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        Display *d = XOpenDisplay(NULL);
        if (!d)
            _exit(1);
        Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
        XSetSelectionOwner(d, XInternAtom(d, "CLIPBOARD", False), w, CurrentTime);
        XSync(d, False);
        (void)!write(fds[1], "r", 1);
        for (;;)
            pause(); /* never reads its events, so never answers */
    }
    close(fds[1]);
    char c;
    ssize_t got = read(fds[0], &c, 1);
    close(fds[0]);
    return got == 1 ? pid : -1;
}

static void test_dead_owner(void)
{
    pid_t pid = spawn_dead_owner();
    CHECK(pid > 0);
    if (pid <= 0)
        return;

    double t0 = now_s();
    CHECK(!clipboard_has_data(CLIPBOARD_TEXT));
    double first = now_s() - t0;
    CHECK(first < 3.0);

    t0 = now_s();
    CHECK(!clipboard_has_data(CLIPBOARD_PNG));
    CHECK(!clipboard_has_data(CLIPBOARD_URIS));
    CHECK(clipboard_get()[0] == '\0');
    size_t n = 0;
    CHECK(clipboard_get_data(CLIPBOARD_PNG, &n) == NULL);
    double rest = now_s() - t0;
    if (rest >= 0.2)
        printf("  the calls after the first took %.2fs\n", rest);
    CHECK(rest < 0.2);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

/* A desktop clipboard manager may take the selection from us as soon as we set it
   (so the content outlives the app). We then free our data, as we should, and the
   paste is served by the manager, which keeps only some types. That is not a
   failure of ours: when the reader fails and we find we no longer own the content,
   set it again and retry. A bare X server, as in CI, passes on the first attempt. */
/* An owner that starts an INCR transfer and then sends one byte every 200 ms for ever: no single step times out, so the
   program used to wait as long as the owner liked. It gives up after about ten seconds (CLIP_INCR_BASE_MS). */
static void run_dripping_owner(int ready_fd)
{
    Display *d = XOpenDisplay(NULL);
    if (!d)
        _exit(1);
    /* the first window id of a new client is the one the client before it had, if that one is gone: the program remembers
       the dead owner of the test before this one by window id, so this one's window is the second */
    XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 10, 10, 0, 0, 0);
    Atom clip = XInternAtom(d, "CLIPBOARD", False), incr = XInternAtom(d, "INCR", False);
    XSetSelectionOwner(d, clip, w, CurrentTime);
    XFlush(d);
    char c = 'r';
    if (write(ready_fd, &c, 1) != 1)
        _exit(1);
    close(ready_fd);
    Window req = None;
    Atom prop = None;
    for (;;)
    {
        XEvent ev;
        XNextEvent(d, &ev);
        if (ev.type == SelectionRequest)
        {
            XSelectionRequestEvent *r = &ev.xselectionrequest;
            long total = 1L << 20;
            XEvent n;
            memset(&n, 0, sizeof n);
            XSelectInput(d, r->requestor, PropertyChangeMask);
            XChangeProperty(d, r->requestor, r->property, incr, 32, PropModeReplace, (unsigned char *)&total, 1);
            n.xselection.type = SelectionNotify;
            n.xselection.requestor = r->requestor;
            n.xselection.selection = r->selection;
            n.xselection.target = r->target;
            n.xselection.property = r->property;
            n.xselection.time = r->time;
            XSendEvent(d, r->requestor, False, 0, &n);
            req = r->requestor;
            prop = r->property;
            XFlush(d);
        }
        else if (ev.type == PropertyNotify && req && ev.xproperty.window == req && ev.xproperty.atom == prop &&
                 ev.xproperty.state == PropertyDelete)
        {
            usleep(200000);
            XChangeProperty(d, req, prop, XA_STRING, 8, PropModeReplace, (const unsigned char *)"x", 1);
            XFlush(d);
        }
    }
}

static void test_dripping_incr_owner(void)
{
    int fds[2];
    CHECK(pipe(fds) == 0);
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        run_dripping_owner(fds[1]);
    }
    close(fds[1]);
    char c;
    CHECK(read(fds[0], &c, 1) == 1);
    close(fds[0]);
    double t0 = now_s();
    size_t n = 7;
    void *d = clipboard_get_data(CLIPBOARD_TEXT, &n);
    double took = now_s() - t0;
    printf("  a paste from an owner that drips for ever: gave up after %.1f s\n", took);
    CHECK(d == NULL && n == 0);
    CHECK(took > 5.0 && took < 20.0);
    fs_free(d);
    double t1 = now_s();
    CHECK(clipboard_get_data(CLIPBOARD_TEXT, &n) == NULL); /* and is not asked again at once */
    CHECK(now_s() - t1 < 2.0);
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

static bool read_back_with_reader(PlatformWindow *w, const ClipboardItem *items, int count, int *attempts)
{
    for (*attempts = 1; *attempts <= 5; (*attempts)++)
    {
        if (!clipboard_set_items(items, count))
            return false;
        for (int i = 0; i < 10 && w; i++) /* let the selection settle */
        {
            window_begin_frame(w);
            struct timespec ts = {0, 5000000};
            nanosleep(&ts, NULL);
        }
        pid_t pid = fork();
        if (pid == 0)
            run_reader();
        int status = -1;
        double end = now_s() + 20.0;
        while (now_s() < end)
        {
            if (w)
                window_begin_frame(w); /* answers the selection requests */
            if (waitpid(pid, &status, WNOHANG) == pid)
                break;
            struct timespec ts = {0, 2000000};
            nanosleep(&ts, NULL);
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
            return true;
        printf("reader child exited with %d (attempt %d)\n", WIFEXITED(status) ? WEXITSTATUS(status) : -1, *attempts);
        if (clipboard_has_data(CUSTOM_MIME))
            return false; /* still ours, so the failure is real */
    }
    return false;
}

static void test_we_own(void)
{
    uint8_t *big = make_big();
    ClipboardItem items[] = {
        {CLIPBOARD_TEXT, TEXT, strlen(TEXT)},
        {CLIPBOARD_PNG, big, BIG_SIZE},
        {CUSTOM_MIME, CUSTOM_DATA, strlen(CUSTOM_DATA)},
    };
    CHECK(clipboard_set_items(items, 3));

    PlatformWindow *w = window_create(&(WindowConfig){.title = "owner", .width = 64, .height = 64});
    CHECK(w != NULL);

    /* a different client pastes it, while this one keeps its event loop going */
    int attempts;
    CHECK(read_back_with_reader(w, items, 3, &attempts));

    /* clearing releases our claim: what we offered is gone (CUSTOM_MIME is a type no
       clipboard manager would have kept) */
    CHECK(clipboard_set_items(NULL, 0));
    CHECK(!clipboard_has_data(CUSTOM_MIME));
    if (w)
        window_destroy(w);
    free(big);
}


/* ---- the primary selection (PRIMARY): the same machinery, a second selection ---- */

#define BIG_TEXT_SIZE (1024 * 1024 + 77)

static char *make_big_text(void)
{
    char *p = malloc(BIG_TEXT_SIZE + 1);
    for (size_t i = 0; i < BIG_TEXT_SIZE; i++)
        p[i] = (char)('a' + (i * 7 + i / 26) % 26);
    p[BIG_TEXT_SIZE] = 0;
    return p;
}

enum
{
    OWN_SMALL, /* answers TEXT */
    OWN_BIG,   /* answers the 1 MB text by INCR */
    OWN_DIES   /* starts the INCR transfer, sends two chunks and exits */
};

/* a foreign program that owns the selection `which` ("PRIMARY" or "CLIPBOARD") and serves text, in one of three ways */
static void run_text_owner(int ready_fd, const char *which, int mode, const char *small)
{
    Display *d = XOpenDisplay(NULL);
    if (!d)
        _exit(2);
    Window w = XCreateSimpleWindow(d, DefaultRootWindow(d), 0, 0, 1, 1, 0, 0, 0);
    Atom sel = XInternAtom(d, which, False), targets = XInternAtom(d, "TARGETS", False),
         utf8 = XInternAtom(d, "UTF8_STRING", False), incr = XInternAtom(d, "INCR", False);
    char *big = mode != OWN_SMALL ? make_big_text() : NULL;
    XSetSelectionOwner(d, sel, w, CurrentTime);
    XSync(d, False);
    (void)!write(ready_fd, "1", 1);
    Window req = None;
    Atom prop = None;
    size_t offset = 0;
    int chunks = 0;
    double end = now_s() + 25.0;
    while (now_s() < end)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ConnectionNumber(d), &fds);
        struct timeval tv = {0, 20000};
        select(ConnectionNumber(d) + 1, &fds, NULL, NULL, &tv);
        while (XPending(d))
        {
            XEvent ev;
            XNextEvent(d, &ev);
            if (ev.type == SelectionRequest)
            {
                XSelectionRequestEvent *rq = &ev.xselectionrequest;
                XSelectionEvent r = {.type = SelectionNotify, .display = rq->display, .requestor = rq->requestor,
                                     .selection = rq->selection, .target = rq->target, .time = rq->time, .property = None};
                Atom pr = rq->property ? rq->property : rq->target;
                if (rq->target == targets)
                {
                    Atom t[] = {targets, utf8};
                    XChangeProperty(d, rq->requestor, pr, XA_ATOM, 32, PropModeReplace, (unsigned char *)t, 2);
                    r.property = pr;
                }
                else if (rq->target == utf8 && mode == OWN_SMALL)
                {
                    XChangeProperty(d, rq->requestor, pr, utf8, 8, PropModeReplace, (const unsigned char *)small, (int)strlen(small));
                    r.property = pr;
                }
                else if (rq->target == utf8 && !req)
                {
                    req = rq->requestor;
                    prop = pr;
                    offset = 0;
                    chunks = 0;
                    XSelectInput(d, req, PropertyChangeMask);
                    long total = (long)BIG_TEXT_SIZE;
                    XChangeProperty(d, req, pr, incr, 32, PropModeReplace, (unsigned char *)&total, 1);
                    r.property = pr;
                }
                XSendEvent(d, rq->requestor, True, NoEventMask, (XEvent *)&r);
                XFlush(d);
            }
            else if (ev.type == PropertyNotify && req && ev.xproperty.window == req && ev.xproperty.atom == prop &&
                     ev.xproperty.state == PropertyDelete)
            {
                if (mode == OWN_DIES && chunks == 2)
                    _exit(0); /* gone in the middle of the transfer, without the last chunk */
                size_t n = BIG_TEXT_SIZE - offset > 65536 ? 65536 : BIG_TEXT_SIZE - offset;
                XChangeProperty(d, req, prop, utf8, 8, PropModeReplace, (unsigned char *)big + offset, (int)n);
                offset += n;
                chunks++;
                if (n == 0)
                    req = None;
                XFlush(d);
            }
        }
    }
    _exit(0);
}

static pid_t spawn_text_owner(const char *which, int mode, const char *small)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid == 0)
    {
        close(fds[0]);
        run_text_owner(fds[1], which, mode, small);
    }
    close(fds[1]);
    char c;
    ssize_t got = read(fds[0], &c, 1);
    close(fds[0]);
    return got == 1 ? pid : -1;
}

/* A foreign reader of what the parent owns: `which` is fetched as UTF8_STRING and must be `expect` (the first `len` bytes of
   what it reads, when len is not 0 and the text is the big one); TARGETS must list UTF8_STRING and no image. Exit 0 = all matched. */
static void run_text_reader(const char *which, const char *expect, bool big)
{
    Reader r = {0};
    r.d = XOpenDisplay(NULL);
    if (!r.d)
        _exit(2);
    r.w = XCreateSimpleWindow(r.d, DefaultRootWindow(r.d), 0, 0, 1, 1, 0, 0, 0);
    XSelectInput(r.d, r.w, PropertyChangeMask);
    r.clipboard = XInternAtom(r.d, which, False);
    r.prop = XInternAtom(r.d, "READER_PROP", False);
    Atom utf8 = XInternAtom(r.d, "UTF8_STRING", False);
    Atom png = XInternAtom(r.d, "image/png", False);
    size_t n;
    int fmt;
    uint8_t *t = reader_fetch(&r, XInternAtom(r.d, "TARGETS", False), &n, &fmt);
    if (!t || fmt != 32)
        _exit(10);
    bool has_utf8 = false, has_png = false;
    for (size_t i = 0; i + sizeof(Atom) <= n; i += sizeof(Atom))
    {
        Atom a;
        memcpy(&a, t + i, sizeof a);
        has_utf8 |= a == utf8;
        has_png |= a == png;
    }
    if (!has_utf8 || has_png)
        _exit(11);
    uint8_t *text = reader_fetch(&r, utf8, &n, &fmt);
    size_t want = strlen(expect);
    if (!text || n != want || memcmp(text, expect, want) != 0)
        _exit(big ? 13 : 12);
    _exit(0);
}

static int wait_child(pid_t pid, PlatformWindow *w, double seconds)
{
    int status = -1;
    double end = now_s() + seconds;
    while (now_s() < end)
    {
        if (w)
            window_begin_frame(w); /* answers the selection requests */
        if (waitpid(pid, &status, WNOHANG) == pid)
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        struct timespec ts = {0, 2000000};
        nanosleep(&ts, NULL);
    }
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    return -2;
}

static int read_back_text(PlatformWindow *w, const char *which, const char *expect, bool big)
{
    pid_t pid = fork();
    if (pid == 0)
        run_text_reader(which, expect, big);
    return wait_child(pid, w, 20.0);
}

static void settle_frames(PlatformWindow *w)
{
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        struct timespec ts = {0, 5000000};
        nanosleep(&ts, NULL);
    }
}

static void test_primary_foreign_owner(void)
{
    CHECK(clipboard_has_primary_support());
    clipboard_set(NULL);
    clipboard_set_primary(NULL);

    /* another program owns PRIMARY and we read it; the clipboard is another place and is empty */
    pid_t pid = spawn_text_owner("PRIMARY", OWN_SMALL, TEXT);
    CHECK(pid > 0);
    if (pid > 0)
    {
        CHECK(strcmp(clipboard_get_primary(), TEXT) == 0);
        CHECK(clipboard_get()[0] == '\0');
        CHECK(!clipboard_has_data(CLIPBOARD_TEXT));
        /* giving up a selection that another program owns leaves it alone */
        CHECK(clipboard_set_primary(NULL));
        CHECK(strcmp(clipboard_get_primary(), TEXT) == 0);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }
    CHECK(clipboard_get_primary()[0] == '\0'); /* the owner is gone */

    /* the same text by INCR, 1 MB in 64 KB chunks */
    pid = spawn_text_owner("PRIMARY", OWN_BIG, NULL);
    CHECK(pid > 0);
    if (pid > 0)
    {
        double t0 = now_s();
        char *big = make_big_text();
        const char *got = clipboard_get_primary();
        CHECK(strlen(got) == BIG_TEXT_SIZE && memcmp(got, big, BIG_TEXT_SIZE) == 0);
        CHECK(now_s() - t0 < 5.0);
        free(big);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
    }

    /* the owner of the clipboard and the owner of PRIMARY are two programs with two texts */
    pid_t a = spawn_text_owner("CLIPBOARD", OWN_SMALL, "from the clipboard owner");
    pid_t b = spawn_text_owner("PRIMARY", OWN_SMALL, TEXT);
    CHECK(a > 0 && b > 0);
    CHECK(strcmp(clipboard_get(), "from the clipboard owner") == 0);
    CHECK(strcmp(clipboard_get_primary(), TEXT) == 0);
    for (pid_t k = 0; k < 2; k++)
    {
        kill(k ? b : a, SIGTERM);
        waitpid(k ? b : a, NULL, 0);
    }
}

/* The owner goes away in the middle of an INCR transfer: the read fails in the time of one step (not the deadline of the
   whole transfer, the owner is not there to drip), gives nothing (not the half it had sent), and the selection is free. */
static void test_primary_owner_dies(void)
{
    pid_t pid = spawn_text_owner("PRIMARY", OWN_DIES, NULL);
    CHECK(pid > 0);
    if (pid <= 0)
        return;
    double t0 = now_s();
    size_t n = 7;
    CHECK(clipboard_get_primary()[0] == '\0');
    double took = now_s() - t0;
    printf("  a primary selection whose owner died in the middle of the transfer: gave up after %.2f s\n", took);
    CHECK(took < 5.0);
    waitpid(pid, NULL, 0);
    t0 = now_s();
    CHECK(clipboard_get_primary()[0] == '\0');
    CHECK(clipboard_get_data(CLIPBOARD_TEXT, &n) == NULL);
    CHECK(now_s() - t0 < 1.0);
    CHECK(clipboard_set_primary("after the owner died")); /* and the selection is ours to take */
    CHECK(strcmp(clipboard_get_primary(), "after the owner died") == 0);
    clipboard_set_primary(NULL);
}

static void test_primary_we_own(void)
{
    PlatformWindow *w = window_create(&(WindowConfig){.title = "primary owner", .width = 64, .height = 64});
    CHECK(w != NULL);
    clipboard_set(NULL);
    clipboard_set_primary(NULL);

    /* we own PRIMARY; another program reads it. Small first, then a text too large for one property (INCR). */
    CHECK(clipboard_set_primary(TEXT));
    settle_frames(w);
    int first = read_back_text(w, "PRIMARY", TEXT, false);
    if (first)
        printf("  the reader of the small primary selection exited with %d\n", first);
    CHECK(first == 0);
    char *big = make_big_text();
    CHECK(clipboard_set_primary(big));
    settle_frames(w);
    int rc = read_back_text(w, "PRIMARY", big, true);
    if (rc)
        printf("  the reader of the large primary selection exited with %d\n", rc);
    CHECK(rc == 0);
    CHECK(strlen(clipboard_get_primary()) == BIG_TEXT_SIZE); /* ours: read from memory */

    /* the clipboard is not the primary selection: two texts, each where it was put, neither disturbs the other */
    clipboard_set("clip text");
    CHECK(clipboard_set_primary("primary text"));
    settle_frames(w);
    CHECK(strcmp(clipboard_get(), "clip text") == 0);
    CHECK(strcmp(clipboard_get_primary(), "primary text") == 0);
    CHECK(read_back_text(w, "CLIPBOARD", "clip text", false) == 0);
    CHECK(read_back_text(w, "PRIMARY", "primary text", false) == 0);

    /* another program takes PRIMARY from us (the user selects something in it): we are told by SelectionClear and what we
       read is theirs; the clipboard is still ours */
    pid_t pid = spawn_text_owner("PRIMARY", OWN_SMALL, TEXT);
    CHECK(pid > 0);
    settle_frames(w);
    CHECK(strcmp(clipboard_get_primary(), TEXT) == 0);
    CHECK(strcmp(clipboard_get(), "clip text") == 0);
    CHECK(clipboard_set_primary(NULL)); /* we do not own it: theirs stays */
    CHECK(strcmp(clipboard_get_primary(), TEXT) == 0);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);

    /* giving the primary selection up releases it; so does "" */
    CHECK(clipboard_set_primary("temporary"));
    CHECK(clipboard_set_primary(""));
    CHECK(clipboard_get_primary()[0] == '\0');
    CHECK(strcmp(clipboard_get(), "clip text") == 0);

    /* our selection is served from the entries of ONE selection: while a large primary transfer is under way, giving the
       primary up must not free the data of a transfer of the clipboard (and the other way around) */
    CHECK(clipboard_set_primary(big));
    clipboard_set("clip again");
    settle_frames(w);
    CHECK(read_back_text(w, "CLIPBOARD", "clip again", false) == 0);
    CHECK(clipboard_set_primary(NULL));
    CHECK(strcmp(clipboard_get(), "clip again") == 0);
    clipboard_set(NULL);
    free(big);
    if (w)
        window_destroy(w);
}

/* Giving a selection up makes the server tell us (SelectionClear) that we lost it, and the message waits in the queue until
   the next pump. Taking the selection again before that pump must not lose what was just set: the old message is about the
   old ownership. */
static void test_stale_selection_clear(void)
{
    PlatformWindow *w = window_create(&(WindowConfig){.title = "stale clear", .width = 64, .height = 64});
    CHECK(w != NULL);
    for (int round = 0; round < 2; round++)
    {
        const bool primary = round == 1;
        void (*set_none)(void) = NULL;
        (void)set_none;
        if (primary)
        {
            clipboard_set_primary("first");
            clipboard_set_primary(NULL);
            CHECK(clipboard_set_primary("second"));
        }
        else
        {
            clipboard_set("first");
            clipboard_set(NULL);
            clipboard_set("second");
        }
        settle_frames(w);
        const char *got = primary ? clipboard_get_primary() : clipboard_get();
        if (strcmp(got, "second") != 0)
            printf("  %s: after giving it up and taking it again it holds \"%s\"\n", primary ? "PRIMARY" : "CLIPBOARD", got);
        CHECK(strcmp(got, "second") == 0);
        CHECK(read_back_text(w, primary ? "PRIMARY" : "CLIPBOARD", "second", false) == 0);
    }
    clipboard_set(NULL);
    clipboard_set_primary(NULL);
    if (w)
        window_destroy(w);
}

int main(void)
{
    if (!platform_init())
    {
        printf("skip: no display\n");
        return 77;
    }
    test_foreign_owner();
    test_dead_owner();
    test_dripping_incr_owner();
    test_we_own();
    test_primary_foreign_owner();
    test_primary_owner_dies();
    test_primary_we_own();
    test_stale_selection_clear();
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
