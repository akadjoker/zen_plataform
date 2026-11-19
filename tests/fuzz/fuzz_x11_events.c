/*
 * fuzz_x11_events.c - what another client of the X server can send to a window of the program: ClientMessages of any
 * type, format and length, a SelectionNotify with a property that is absent, huge, of any type or format, ConfigureNotify
 * with absurd sizes, any other event type with any bytes in it, and a whole XDND drop whose source answers with what the
 * input says (a uri-list of any shape, no property, a property of another name). The hostile client is a second connection
 * to the same server: XSendEvent for the events, a window of its own that owns XdndSelection and answers the library's
 * request. The input is a script of operations (byte 0 of each picks it, the bytes after fill it, a script that runs out
 * reads zeros). Needs a server: tools/fuzz.sh starts the Xvfb. A failure is a sanitizer report, a hang, or an Xlib exit.
 */
#include "platform.h"

#include <X11/Xlib.h>
#include <X11/Xatom.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PlatformWindow *g_win;
static Display *g_hd; /* the hostile client */
static Window g_hw;   /* its window */
static Window g_target;
static Atom A[24];
enum { NA = 24 };
static const char *const names[NA] = {"WM_PROTOCOLS", "WM_DELETE_WINDOW", "_NET_WM_NAME", "UTF8_STRING", "_NET_WM_STATE", "CLIPBOARD",
                                      "TARGETS", "PLATFORM_CLIPBOARD", "INCR", "text/plain", "TEXT", "XdndAware", "XdndEnter",
                                      "XdndPosition", "XdndStatus", "XdndLeave", "XdndDrop", "XdndFinished", "XdndSelection",
                                      "XdndTypeList", "XdndActionCopy", "PLATFORM_XDND", "text/uri-list", "_NET_ACTIVE_WINDOW"};

typedef struct
{
    const uint8_t *p, *end;
} Cur;

static uint8_t u8(Cur *c)
{
    return c->p < c->end ? *c->p++ : 0;
}
static uint32_t u32(Cur *c)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | u8(c);
    return v;
}
static Atom atom(Cur *c)
{
    uint8_t b = u8(c);
    return b < NA ? A[b] : (Atom)u32(c) & 0x1FFFFFFF; /* the atoms of the program, or any number */
}
static Window window_of(Cur *c)
{
    uint8_t b = u8(c);
    return b < 200 ? g_target : b < 230 ? g_hw : (Window)u32(c);
}
static int error_handler(Display *d, XErrorEvent *e)
{
    (void)d, (void)e;
    return 0; /* the hostile client's own mistakes are not what is tested */
}

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc, (void)argv;
    if (!platform_init())
        return 77; /* standalone_main.c: skip */
    WindowConfig cfg = {.title = "fuzz", .width = 200, .height = 150, .render = RENDER_PIXELS, .resizable = true};
    cfg.x = WINDOW_POS_UNDEFINED;
    cfg.y = WINDOW_POS_UNDEFINED;
    g_win = window_create(&cfg);
    if (!g_win)
        return 0;
    g_target = (Window)(uintptr_t)window_native_handle(g_win, NATIVE_WINDOW);
    g_hd = XOpenDisplay(NULL);
    if (!g_hd)
        return 0;
    XSetErrorHandler(error_handler);
    g_hw = XCreateSimpleWindow(g_hd, DefaultRootWindow(g_hd), 0, 0, 10, 10, 0, 0, 0);
    for (int i = 0; i < NA; i++)
        A[i] = XInternAtom(g_hd, names[i], False);
    XSync(g_hd, False);
    return 0;
}

/* the hostile owner of XdndSelection: what it answers a request with comes from the script */
static void serve(Cur *c)
{
    XSync(g_hd, False);
    while (XPending(g_hd))
    {
        XEvent ev;
        XNextEvent(g_hd, &ev);
        if (ev.type != SelectionRequest)
            continue;
        XSelectionRequestEvent *r = &ev.xselectionrequest;
        XEvent n;
        memset(&n, 0, sizeof n);
        n.xselection.type = SelectionNotify;
        n.xselection.requestor = r->requestor;
        n.xselection.selection = r->selection;
        n.xselection.target = r->target;
        n.xselection.time = r->time;
        n.xselection.property = r->property;
        uint8_t mode = u8(c);
        if (mode % 8 == 0)
            n.xselection.property = None; /* refused */
        else if (mode % 8 == 1)
            n.xselection.property = atom(c); /* a property of another name than the one asked for */
        else
        {
            /* the text: any bytes, of any type and format, up to 3 times 4 KB */
            size_t len = (size_t)u8(c) * 16 + u8(c);
            size_t reps = 1 + u8(c) % 3;
            uint8_t *buf = malloc(len * reps + 1);
            for (size_t i = 0; i < len; i++)
                buf[i] = u8(c);
            for (size_t k = 1; k < reps; k++)
                memcpy(buf + k * len, buf, len);
            int fmt = (mode >> 3) % 4 == 0 ? 16 : (mode >> 3) % 4 == 1 ? 32 : 8;
            Atom type = (mode >> 5) % 2 ? A[22] : atom(c);
            size_t units = len * reps / (size_t)(fmt / 8);
            XChangeProperty(g_hd, r->requestor, r->property, type, fmt, PropModeReplace, buf, (int)units);
            free(buf);
        }
        XSendEvent(g_hd, r->requestor, False, 0, &n);
        XFlush(g_hd);
    }
}

static void pump(void)
{
    for (int i = 0; i < 2; i++)
    {
        window_begin_frame(g_win);
        Event e;
        while (poll_event(g_win, &e))
            if (e.type == EVENT_WINDOW_DROP)
                for (int k = 0; k < e.data.drop.count; k++)
                    (void)strlen(e.data.drop.paths[k]); /* every path is a string */
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!g_win || !g_hd)
        return 0;
    Cur cur = {data, data + size}, *c = &cur;
    for (int op = 0; op < 24 && c->p < c->end; op++)
    {
        XEvent ev;
        memset(&ev, 0, sizeof ev);
        switch (u8(c) % 8)
        {
        case 0: /* a ClientMessage: any type, any format, any 20 bytes */
        {
            ev.xclient.type = ClientMessage;
            ev.xclient.window = window_of(c);
            ev.xclient.message_type = atom(c);
            static const int fmts[] = {32, 32, 8, 16, 0, 7, 64};
            ev.xclient.format = fmts[u8(c) % 7];
            for (int i = 0; i < 20; i++)
                ev.xclient.data.b[i] = (char)u8(c);
            if (u8(c) & 1)
                ev.xclient.data.l[0] = (long)g_hw; /* the source of a drag */
            XSendEvent(g_hd, g_target, False, 0, &ev);
            break;
        }
        case 1: /* the start of a drop: enter, position, drop, from the hostile window, offering uri-list */
        {
            XSetSelectionOwner(g_hd, A[18], g_hw, CurrentTime);
            Atom types[3] = {A[22], atom(c), atom(c)};
            XChangeProperty(g_hd, g_hw, A[19], XA_ATOM, 32, PropModeReplace, (unsigned char *)types, 3);
            ev.xclient.type = ClientMessage;
            ev.xclient.window = g_target;
            ev.xclient.format = 32;
            ev.xclient.message_type = A[12];
            ev.xclient.data.l[0] = (long)g_hw;
            ev.xclient.data.l[1] = (long)(5u << 24 | (u8(c) & 1u));
            ev.xclient.data.l[2] = (long)A[22];
            XSendEvent(g_hd, g_target, False, 0, &ev);
            ev.xclient.message_type = A[13];
            ev.xclient.data.l[1] = 0;
            ev.xclient.data.l[2] = (long)(u32(c) & 0xFFFF) << 16 | (long)(u32(c) & 0xFFFF);
            ev.xclient.data.l[3] = (long)CurrentTime;
            ev.xclient.data.l[4] = (long)A[20];
            XSendEvent(g_hd, g_target, False, 0, &ev);
            if (u8(c) & 3)
            {
                ev.xclient.message_type = A[16];
                ev.xclient.data.l[2] = (long)CurrentTime;
                XSendEvent(g_hd, g_target, False, 0, &ev);
            }
            break;
        }
        case 2: /* a SelectionNotify the library did not ask for, or did */
            ev.xselection.type = SelectionNotify;
            ev.xselection.requestor = g_target;
            ev.xselection.selection = atom(c);
            ev.xselection.target = atom(c);
            ev.xselection.property = atom(c);
            ev.xselection.time = u32(c);
            XSendEvent(g_hd, g_target, False, 0, &ev);
            break;
        case 3: /* a property on the window, of any type, format and size, that a SelectionNotify can then name */
        {
            size_t len = (size_t)u8(c) * 64 + u8(c);
            uint8_t *buf = malloc(len + 1);
            for (size_t i = 0; i < len; i++)
                buf[i] = u8(c);
            int fmt = (u8(c) % 3 + 1) * 8 / (u8(c) % 2 + 1);
            if (fmt != 8 && fmt != 16 && fmt != 32)
                fmt = 8;
            XChangeProperty(g_hd, g_target, atom(c), atom(c), fmt, PropModeReplace, buf, (int)(len / (size_t)(fmt / 8)));
            free(buf);
            break;
        }
        case 4: /* ConfigureNotify with sizes and positions from anywhere */
            ev.xconfigure.type = ConfigureNotify;
            ev.xconfigure.event = g_target;
            ev.xconfigure.window = g_target;
            ev.xconfigure.x = (int)u32(c);
            ev.xconfigure.y = (int)u32(c);
            ev.xconfigure.width = (int)u32(c);
            ev.xconfigure.height = (int)u32(c);
            ev.xconfigure.border_width = (int)u32(c);
            XSendEvent(g_hd, g_target, False, 0, &ev);
            break;
        case 5: /* a SelectionRequest and a SelectionClear to the program, as the owner of the clipboard */
            ev.xselectionrequest.type = SelectionRequest;
            ev.xselectionrequest.owner = g_target;
            ev.xselectionrequest.requestor = u8(c) & 1 ? g_hw : window_of(c);
            ev.xselectionrequest.selection = atom(c);
            ev.xselectionrequest.target = atom(c);
            ev.xselectionrequest.property = atom(c);
            ev.xselectionrequest.time = u32(c);
            XSendEvent(g_hd, g_target, False, 0, &ev);
            break;
        default: /* any other type, with any bytes in it */
        {
            uint8_t t = u8(c);
            for (size_t i = 0; i < sizeof ev; i++)
                ((uint8_t *)&ev)[i] = u8(c);
            ev.type = 2 + t % 34; /* KeyPress .. MappingNotify */
            if (ev.type == ClientMessage)
                ev.type = PropertyNotify;
            ev.xany.window = u8(c) & 1 ? g_target : window_of(c);
            ev.xany.send_event = True;
            XSendEvent(g_hd, g_target, False, 0, &ev);
            break;
        }
        }
        XFlush(g_hd);
        pump();
        serve(c);
        pump();
    }
    return 0;
}
