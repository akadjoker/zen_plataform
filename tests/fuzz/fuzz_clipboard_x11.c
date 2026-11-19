/*
 * fuzz_clipboard_x11.c - the clipboard when the owner of the selection is hostile: it answers (or does not) with a
 * property of any type, format and size, with a different property than the one asked for, with INCR transfers of chunks
 * of any size (or never ending), with an empty TARGETS. The owner is a thread with a connection of its own; the program
 * asks (clipboard_get, clipboard_get_data of a few MIME types, clipboard_has_data, clipboard_get_image, which also
 * decodes whatever comes as a PNG) and what the owner does comes from the input: the first half of the bytes picks what
 * the program asks, the second half is the owner's script. Needs a server (tools/fuzz.sh starts one).
 */
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Display *g_hd;
static Window g_hw;
static Atom g_clipboard, g_targets, g_incr, g_utf8, g_png;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_script[1 << 16];
static size_t g_len, g_pos;

static uint8_t next8(void)
{
    uint8_t b = g_pos < g_len ? g_script[g_pos] : 0;
    g_pos++;
    return b;
}

static int quiet(Display *d, XErrorEvent *e)
{
    (void)d, (void)e;
    return 0;
}

/* the INCR transfer in progress: the requestor and property, and the number of chunks still to send */
static Window incr_req;
static Atom incr_prop, incr_target;
static int incr_left;

/* XChangeProperty takes format 16 as an array of short and format 32 as an array of long (8 bytes each on this
   machine): the bytes of the input are copied into such an array */
static void set_prop(Display *d, Window w, Atom prop, Atom type, int fmt, const uint8_t *bytes, size_t len)
{
    size_t unit = (size_t)fmt / 8, n = len / unit;
    size_t width = fmt == 32 ? sizeof(long) : unit;
    uint8_t *mem = calloc(n * width + 8, 1);
    for (size_t i = 0; i < n; i++)
        memcpy(mem + i * width, bytes + i * unit, unit);
    XChangeProperty(d, w, prop, type, fmt, PropModeReplace, mem, (int)n);
    free(mem);
}

static void send_chunk(void)
{
    size_t n = incr_left > 0 ? (size_t)next8() * 32 + next8() : 0; /* up to 8 KB; the last one is empty */
    uint8_t *buf = malloc(n + 1);
    for (size_t i = 0; i < n; i++)
        buf[i] = next8();
    XChangeProperty(g_hd, incr_req, incr_prop, incr_target, 8, PropModeReplace, buf, (int)n);
    free(buf);
    if (incr_left > 0)
        incr_left--;
    else
        incr_req = None;
    XFlush(g_hd);
}

static void answer(const XSelectionRequestEvent *r)
{
    XEvent n;
    memset(&n, 0, sizeof n);
    n.xselection.type = SelectionNotify;
    n.xselection.requestor = r->requestor;
    n.xselection.selection = r->selection;
    n.xselection.target = r->target;
    n.xselection.time = r->time;
    n.xselection.property = r->property ? r->property : r->target;
    uint8_t mode = next8() % 7;
    if (mode == 0)
        return; /* never answers: the program waits a second */
    if (mode == 1)
        n.xselection.property = None;
    else if (mode == 2)
        n.xselection.property = (Atom)next8() + 1; /* another property than the one asked for */
    else if (mode == 3)
    {
        /* INCR: the size announced, then the chunks as the program deletes the property */
        long total = (long)next8() << 16 | (long)next8() << 8 | next8();
        XChangeProperty(g_hd, r->requestor, n.xselection.property, g_incr, 32, PropModeReplace, (unsigned char *)&total, 1);
        XSelectInput(g_hd, r->requestor, PropertyChangeMask); /* to see the program delete the property */
        incr_req = r->requestor;
        incr_prop = n.xselection.property;
        incr_target = r->target;
        incr_left = next8() % 12;
    }
    else
    {
        static const int fmts[] = {8, 8, 16, 32};
        int fmt = fmts[next8() % 4];
        size_t len = (size_t)next8() * 64 + next8();
        uint8_t *buf = calloc(len + 8, 1);
        uint8_t kind = next8();
        if (kind % 4 == 0 && len > 24)
        {
            /* a PNG of the right shape at the front, whatever after it */
            static const uint8_t sig[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 4, 0, 0, 0, 4, 8, 6};
            memcpy(buf, sig, len < sizeof sig ? len : sizeof sig);
            for (size_t i = sizeof sig; i < len; i++)
                buf[i] = next8();
        }
        else
            for (size_t i = 0; i < len; i++)
                buf[i] = next8();
        Atom type = kind % 4 == 1 ? g_utf8 : kind % 4 == 2 ? g_png : kind % 4 == 3 ? g_targets : XA_STRING;
        if (kind >= 192)
            type = (Atom)next8() + 1;
        set_prop(g_hd, r->requestor, n.xselection.property, type, fmt, buf, len);
        free(buf);
    }
    XSendEvent(g_hd, r->requestor, False, 0, &n);
    XFlush(g_hd);
}

static void *owner_main(void *arg)
{
    (void)arg;
    for (;;)
    {
        XEvent ev;
        XNextEvent(g_hd, &ev);
        pthread_mutex_lock(&g_lock);
        if (ev.type == SelectionRequest)
            answer(&ev.xselectionrequest);
        else if (ev.type == PropertyNotify && incr_req && ev.xproperty.window == incr_req && ev.xproperty.atom == incr_prop &&
                 ev.xproperty.state == PropertyDelete)
            send_chunk();
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc, (void)argv;
    if (!platform_init())
        return 77;
    g_hd = XOpenDisplay(NULL);
    if (!g_hd)
        return 77;
    XSetErrorHandler(quiet);
    g_hw = XCreateSimpleWindow(g_hd, DefaultRootWindow(g_hd), 0, 0, 10, 10, 0, 0, 0);
    XSelectInput(g_hd, g_hw, PropertyChangeMask);
    g_clipboard = XInternAtom(g_hd, "CLIPBOARD", False);
    g_targets = XInternAtom(g_hd, "TARGETS", False);
    g_incr = XInternAtom(g_hd, "INCR", False);
    g_utf8 = XInternAtom(g_hd, "UTF8_STRING", False);
    g_png = XInternAtom(g_hd, "image/png", False);
    XSetSelectionOwner(g_hd, g_clipboard, g_hw, CurrentTime);
    XSelectInput(g_hd, DefaultRootWindow(g_hd), 0);
    XFlush(g_hd);
    pthread_t t;
    pthread_create(&t, NULL, owner_main, NULL);
    pthread_detach(t);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!g_hd)
        return 0;
    size_t half = size / 2;
    pthread_mutex_lock(&g_lock);
    g_len = size - half > sizeof g_script ? sizeof g_script : size - half;
    memcpy(g_script, data + half, g_len);
    g_pos = 0;
    incr_req = None;
    pthread_mutex_unlock(&g_lock);
    static const char *const mimes[] = {"text/plain", "text/plain;charset=utf-8", "image/png", "text/uri-list", "x/y"};
    for (size_t i = 0; i < half && i < 6; i++)
    {
        switch (data[i] % 5)
        {
        case 0:
            (void)clipboard_get();
            break;
        case 1:
        {
            size_t n = 0;
            void *d = clipboard_get_data(mimes[data[i] / 5 % 5], &n);
            if (d)
            {
                if (n)
                    ((volatile uint8_t *)d)[n - 1]; /* the whole of it is readable */
                fs_free(d);
            }
            break;
        }
        case 2:
            (void)clipboard_has_data(mimes[data[i] / 5 % 5]);
            break;
        case 3:
        {
            Framebuffer fb;
            memset(&fb, 0, sizeof fb);
            if (clipboard_get_image(&fb))
                framebuffer_free(&fb);
            break;
        }
        default:
            clipboard_set("fuzz"); /* takes the selection from the hostile owner, which then asks for it back */
            XSetSelectionOwner(g_hd, g_clipboard, g_hw, CurrentTime);
            XFlush(g_hd);
            break;
        }
    }
    return 0;
}
