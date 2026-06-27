/*
 * backend_x11.c - X11 + GLX backend. Creates the window and GL context, pumps the
 * Xlib event queue, and normalizes everything into core_push_event / core_push_char.
 * No input logic lives here; that is the core's job.
 *
 * Naming note: the public opaque type is PlatformWindow precisely so Xlib's
 * 'Window' (an XID) can be used here without a clash. In this file 'Window' is
 * always Xlib's; our per-window handle is BackendWindow.
 */
#include "core_internal.h"
#include "backend.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/keysym.h>
#include <X11/cursorfont.h>
#include <X11/extensions/Xrandr.h>
#include <X11/Xresource.h> 
#include <GL/glx.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef GLXContext (*glXCreateContextAttribsARBProc)(Display *, GLXFBConfig, GLXContext, Bool, const int *);
typedef void (*glXSwapIntervalEXTProc)(Display *, GLXDrawable, int);
typedef int (*glXSwapIntervalMESAProc)(unsigned int);
typedef int (*glXSwapIntervalSGIProc)(int);

#define GLX_CONTEXT_MAJOR_VERSION_ARB 0x2091
#define GLX_CONTEXT_MINOR_VERSION_ARB 0x2092
#define GLX_CONTEXT_PROFILE_MASK_ARB 0x9126
#define GLX_CONTEXT_CORE_PROFILE_BIT_ARB 0x00000001

/* ---- global connection ---- */

static struct
{
    Display *dpy;
    int screen;
    Window root;
    XContext ctx; /* xid -> BackendWindow* */
    XIM xim;

    Atom WM_PROTOCOLS, WM_DELETE_WINDOW, NET_WM_NAME, UTF8_STRING;
    Atom NET_WM_STATE, NET_WM_STATE_FULLSCREEN, NET_WM_STATE_MAXIMIZED_VERT,
        NET_WM_STATE_MAXIMIZED_HORZ, NET_WM_STATE_HIDDEN, NET_WM_STATE_ABOVE,
        NET_WM_STATE_DEMANDS_ATTENTION, NET_ACTIVE_WINDOW, NET_WM_ICON,
        NET_WM_WINDOW_OPACITY, MOTIF_WM_HINTS;
    Atom CLIPBOARD, TARGETS, XSEL_DATA;

    glXCreateContextAttribsARBProc create_context;
    glXSwapIntervalEXTProc swap_ext;
    glXSwapIntervalMESAProc swap_mesa;
    glXSwapIntervalSGIProc swap_sgi;

    Window helper;        /* unmapped window that owns the CLIPBOARD selection */
    char *clipboard_text; /* what we last set; also the get() return buffer */
} g;

struct BackendWindow
{
    Window win;
    GLXContext glc;
    Colormap colormap;
    XIC xic;
    Core *core; /* set at the start of each pump so handlers can reach the core */

    WindowMode mode;
    int width, height;
    int pos_x, pos_y;
    int saved_x, saved_y, saved_w, saved_h; /* restore from fullscreen */
    bool focused, minimized, maximized, visible, hovered;

    int cursor_mode; /* MOUSE_MODE_* */
    Cursor active_cursor;
    Cursor hidden_cursor;
    bool grabbed;

    RenderMode render;
    GC gc;         /* pixel mode: blits the XImage to the window */
    XImage *image; /* pixel mode: the CPU framebuffer */
};

/* ========================================================================== */
/*  keysym + modifier translation                                             */
/* ========================================================================== */

static int translate_keysym(KeySym ks)
{
    if (ks >= XK_a && ks <= XK_z)
        return KEY_A + (int)(ks - XK_a);
    if (ks >= XK_A && ks <= XK_Z)
        return KEY_A + (int)(ks - XK_A);
    if (ks >= XK_0 && ks <= XK_9)
        return KEY_ZERO + (int)(ks - XK_0);
    if (ks >= XK_F1 && ks <= XK_F12)
        return KEY_F1 + (int)(ks - XK_F1);

    switch (ks)
    {
    case XK_space:
        return KEY_SPACE;
    case XK_apostrophe:
        return KEY_APOSTROPHE;
    case XK_comma:
        return KEY_COMMA;
    case XK_minus:
        return KEY_MINUS;
    case XK_period:
        return KEY_PERIOD;
    case XK_slash:
        return KEY_SLASH;
    case XK_semicolon:
        return KEY_SEMICOLON;
    case XK_equal:
        return KEY_EQUAL;
    case XK_bracketleft:
        return KEY_LEFT_BRACKET;
    case XK_backslash:
        return KEY_BACKSLASH;
    case XK_bracketright:
        return KEY_RIGHT_BRACKET;
    case XK_grave:
        return KEY_GRAVE;
    case XK_Escape:
        return KEY_ESCAPE;
    case XK_Return:
        return KEY_ENTER;
    case XK_Tab:
        return KEY_TAB;
    case XK_BackSpace:
        return KEY_BACKSPACE;
    case XK_Insert:
        return KEY_INSERT;
    case XK_Delete:
        return KEY_DELETE;
    case XK_Right:
        return KEY_RIGHT;
    case XK_Left:
        return KEY_LEFT;
    case XK_Down:
        return KEY_DOWN;
    case XK_Up:
        return KEY_UP;
    case XK_Page_Up:
        return KEY_PAGE_UP;
    case XK_Page_Down:
        return KEY_PAGE_DOWN;
    case XK_Home:
        return KEY_HOME;
    case XK_End:
        return KEY_END;
    case XK_Caps_Lock:
        return KEY_CAPS_LOCK;
    case XK_Num_Lock:
        return KEY_NUM_LOCK;
    case XK_Print:
        return KEY_PRINT_SCREEN;
    case XK_Pause:
        return KEY_PAUSE;
    case XK_Shift_L:
        return KEY_LEFT_SHIFT;
    case XK_Control_L:
        return KEY_LEFT_CONTROL;
    case XK_Alt_L:
        return KEY_LEFT_ALT;
    case XK_Super_L:
        return KEY_LEFT_SUPER;
    case XK_Shift_R:
        return KEY_RIGHT_SHIFT;
    case XK_Control_R:
        return KEY_RIGHT_CONTROL;
    case XK_Alt_R:
        return KEY_RIGHT_ALT;
    case XK_Super_R:
        return KEY_RIGHT_SUPER;
    default:
        return KEY_NULL;
    }
}

static int translate_mods(unsigned int state)
{
    int mods = 0;
    if (state & ShiftMask)
        mods |= MOD_SHIFT;
    if (state & ControlMask)
        mods |= MOD_CTRL;
    if (state & Mod1Mask)
        mods |= MOD_ALT;
    if (state & Mod4Mask)
        mods |= MOD_SUPER;
    return mods;
}

/* ========================================================================== */
/*  _NET_WM_STATE helpers                                                     */
/* ========================================================================== */

static void send_wm_state(BackendWindow *b, int action, Atom a, Atom bb)
{
    XEvent e = {0};
    e.type = ClientMessage;
    e.xclient.window = b->win;
    e.xclient.message_type = g.NET_WM_STATE;
    e.xclient.format = 32;
    e.xclient.data.l[0] = action; /* 0 remove, 1 add, 2 toggle */
    e.xclient.data.l[1] = (long)a;
    e.xclient.data.l[2] = (long)bb;
    e.xclient.data.l[3] = 1;
    XSendEvent(g.dpy, g.root, False,
               SubstructureNotifyMask | SubstructureRedirectMask, &e);
}

static void refresh_wm_state(BackendWindow *b)
{
    Atom type;
    int fmt;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (XGetWindowProperty(g.dpy, b->win, g.NET_WM_STATE, 0, 64, False, XA_ATOM,
                           &type, &fmt, &n, &after, &data) != Success ||
        !data)
        return;

    Atom *states = (Atom *)data;
    bool maxv = false, maxh = false, hidden = false;
    for (unsigned long i = 0; i < n; i++)
    {
        if (states[i] == g.NET_WM_STATE_MAXIMIZED_VERT)
            maxv = true;
        else if (states[i] == g.NET_WM_STATE_MAXIMIZED_HORZ)
            maxh = true;
        else if (states[i] == g.NET_WM_STATE_HIDDEN)
            hidden = true;
    }
    b->maximized = maxv && maxh;
    b->minimized = hidden;
    XFree(data);
}

/* ========================================================================== */
/*  GL context                                                                */
/* ========================================================================== */

static GLXFBConfig choose_fbconfig(const WindowConfig *cfg)
{
    int attribs[] = {
        GLX_X_RENDERABLE, True,
        GLX_DRAWABLE_TYPE, GLX_WINDOW_BIT,
        GLX_RENDER_TYPE, GLX_RGBA_BIT,
        GLX_X_VISUAL_TYPE, GLX_TRUE_COLOR,
        GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8,
        GLX_DEPTH_SIZE, 24, GLX_STENCIL_SIZE, 8,
        GLX_DOUBLEBUFFER, True,
        GLX_SAMPLE_BUFFERS, cfg->msaa > 0 ? 1 : 0,
        GLX_SAMPLES, cfg->msaa > 0 ? cfg->msaa : 0,
        None};

    int count = 0;
    GLXFBConfig *configs = glXChooseFBConfig(g.dpy, g.screen, attribs, &count);
    if (!configs || count == 0)
        return NULL;
    GLXFBConfig chosen = configs[0];
    XFree(configs);
    return chosen;
}

static GLXContext create_context(GLXFBConfig fb, int major, int minor)
{
    if (g.create_context)
    {
        int attribs[] = {
            GLX_CONTEXT_MAJOR_VERSION_ARB, major > 0 ? major : 3,
            GLX_CONTEXT_MINOR_VERSION_ARB, major > 0 ? minor : 3,
            GLX_CONTEXT_PROFILE_MASK_ARB, GLX_CONTEXT_CORE_PROFILE_BIT_ARB,
            None};
        GLXContext c = g.create_context(g.dpy, fb, NULL, True, attribs);
        if (c)
            return c;
    }
    return glXCreateNewContext(g.dpy, fb, GLX_RGBA_TYPE, NULL, True);
}

/* ========================================================================== */
/*  init / shutdown                                                           */
/* ========================================================================== */

bool backend_init(void)
{
    g.dpy = XOpenDisplay(NULL);
    if (!g.dpy)
        return false;
    g.screen = DefaultScreen(g.dpy);
    g.root = RootWindow(g.dpy, g.screen);
    g.ctx = XUniqueContext();

    g.WM_PROTOCOLS = XInternAtom(g.dpy, "WM_PROTOCOLS", False);
    g.WM_DELETE_WINDOW = XInternAtom(g.dpy, "WM_DELETE_WINDOW", False);
    g.NET_WM_NAME = XInternAtom(g.dpy, "_NET_WM_NAME", False);
    g.UTF8_STRING = XInternAtom(g.dpy, "UTF8_STRING", False);
    g.NET_WM_STATE = XInternAtom(g.dpy, "_NET_WM_STATE", False);
    g.NET_WM_STATE_FULLSCREEN = XInternAtom(g.dpy, "_NET_WM_STATE_FULLSCREEN", False);
    g.NET_WM_STATE_MAXIMIZED_VERT = XInternAtom(g.dpy, "_NET_WM_STATE_MAXIMIZED_VERT", False);
    g.NET_WM_STATE_MAXIMIZED_HORZ = XInternAtom(g.dpy, "_NET_WM_STATE_MAXIMIZED_HORZ", False);
    g.NET_WM_STATE_HIDDEN = XInternAtom(g.dpy, "_NET_WM_STATE_HIDDEN", False);
    g.NET_WM_STATE_ABOVE = XInternAtom(g.dpy, "_NET_WM_STATE_ABOVE", False);
    g.NET_WM_STATE_DEMANDS_ATTENTION = XInternAtom(g.dpy, "_NET_WM_STATE_DEMANDS_ATTENTION", False);
    g.NET_ACTIVE_WINDOW = XInternAtom(g.dpy, "_NET_ACTIVE_WINDOW", False);
    g.NET_WM_ICON = XInternAtom(g.dpy, "_NET_WM_ICON", False);
    g.NET_WM_WINDOW_OPACITY = XInternAtom(g.dpy, "_NET_WM_WINDOW_OPACITY", False);
    g.MOTIF_WM_HINTS = XInternAtom(g.dpy, "_MOTIF_WM_HINTS", False);
    g.CLIPBOARD = XInternAtom(g.dpy, "CLIPBOARD", False);
    g.TARGETS = XInternAtom(g.dpy, "TARGETS", False);
    g.XSEL_DATA = XInternAtom(g.dpy, "PLATFORM_CLIPBOARD", False);

    /* A 1x1 unmapped window owns the CLIPBOARD selection and answers conversion
       requests, so clipboard ownership outlives any single visible window. */
    g.helper = XCreateSimpleWindow(g.dpy, g.root, -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(g.dpy, g.helper, PropertyChangeMask);

    g.xim = XOpenIM(g.dpy, NULL, NULL, NULL);

    g.create_context = (glXCreateContextAttribsARBProc)glXGetProcAddressARB((const GLubyte *)"glXCreateContextAttribsARB");
    g.swap_ext = (glXSwapIntervalEXTProc)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalEXT");
    g.swap_mesa = (glXSwapIntervalMESAProc)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalMESA");
    g.swap_sgi = (glXSwapIntervalSGIProc)glXGetProcAddressARB((const GLubyte *)"glXSwapIntervalSGI");
    return true;
}

void backend_shutdown(void)
{
    free(g.clipboard_text);
    if (g.helper)
        XDestroyWindow(g.dpy, g.helper);
    if (g.xim)
        XCloseIM(g.xim);
    if (g.dpy)
        XCloseDisplay(g.dpy);
    memset(&g, 0, sizeof g);
}

/* ========================================================================== */
/*  create / destroy                                                          */
/* ========================================================================== */

static void set_title(BackendWindow *b, const char *title)
{
    if (!title)
        title = "";
    XStoreName(g.dpy, b->win, title);
    XChangeProperty(g.dpy, b->win, g.NET_WM_NAME, g.UTF8_STRING, 8,
                    PropModeReplace, (const unsigned char *)title, (int)strlen(title));
}

BackendWindow *backend_create(const WindowConfig *cfg)
{
    /* Pixel windows take the default TrueColor visual (no GLX involved); GL
       windows take the visual of a chosen framebuffer config. */
    GLXFBConfig fb = NULL;
    XVisualInfo vinfo;
    XVisualInfo *vi;
    if (cfg->render == RENDER_PIXELS)
    {
        vi = &vinfo;
        if (!XMatchVisualInfo(g.dpy, g.screen, DefaultDepth(g.dpy, g.screen), TrueColor, vi))
            return NULL;
    }
    else
    {
        fb = choose_fbconfig(cfg);
        if (!fb)
            return NULL;
        vi = glXGetVisualFromFBConfig(g.dpy, fb);
        if (!vi)
            return NULL;
    }

    BackendWindow *b = calloc(1, sizeof *b);
    if (!b)
    {
        if (cfg->render == RENDER_GL)
            XFree(vi);
        return NULL;
    }
    b->render = cfg->render;

    b->colormap = XCreateColormap(g.dpy, g.root, vi->visual, AllocNone);

    XSetWindowAttributes swa = {0};
    swa.colormap = b->colormap;
    swa.background_pixel = 0;
    swa.border_pixel = 0;
    swa.event_mask = ExposureMask | KeyPressMask | KeyReleaseMask |
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask |
                     StructureNotifyMask | FocusChangeMask | EnterWindowMask |
                     LeaveWindowMask | PropertyChangeMask;

    int x = (cfg->x == WINDOW_POS_CENTERED || cfg->x == WINDOW_POS_UNDEFINED) ? 0 : cfg->x;
    int y = (cfg->y == WINDOW_POS_CENTERED || cfg->y == WINDOW_POS_UNDEFINED) ? 0 : cfg->y;
    int w = cfg->width > 0 ? cfg->width : 640;
    int h = cfg->height > 0 ? cfg->height : 480;

    b->win = XCreateWindow(g.dpy, g.root, x, y, (unsigned)w, (unsigned)h, 0,
                           vi->depth, InputOutput, vi->visual,
                           CWColormap | CWEventMask | CWBackPixel | CWBorderPixel, &swa);
    if (cfg->render == RENDER_GL)
        XFree(vi);
    if (!b->win)
    {
        free(b);
        return NULL;
    }

    b->width = w;
    b->height = h;
    b->pos_x = x;
    b->pos_y = y;
    b->mode = cfg->mode;
    b->cursor_mode = MOUSE_MODE_NORMAL;

    XSaveContext(g.dpy, b->win, g.ctx, (XPointer)b);
    XSetWMProtocols(g.dpy, b->win, &g.WM_DELETE_WINDOW, 1);
    set_title(b, cfg->title);

    XClassHint *cls = XAllocClassHint();
    if (cls)
    {
        cls->res_name = (char *)(cfg->title ? cfg->title : "platform");
        cls->res_class = (char *)"Platform";
        XSetClassHint(g.dpy, b->win, cls);
        XFree(cls);
    }

    XSizeHints *hints = XAllocSizeHints();
    if (hints)
    {
        if (!cfg->resizable)
        {
            hints->flags |= PMinSize | PMaxSize;
            hints->min_width = hints->max_width = w;
            hints->min_height = hints->max_height = h;
        }
        if (cfg->x != WINDOW_POS_UNDEFINED && cfg->y != WINDOW_POS_UNDEFINED)
        {
            hints->flags |= PPosition;
            hints->x = x;
            hints->y = y;
        }
        XSetWMNormalHints(g.dpy, b->win, hints);
        XFree(hints);
    }

    if (g.xim)
        b->xic = XCreateIC(g.xim, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                           XNClientWindow, b->win, XNFocusWindow, b->win, NULL);

    if (cfg->render == RENDER_PIXELS)
    {
        b->gc = XCreateGC(g.dpy, b->win, 0, NULL);
    }
    else
    {
        b->glc = create_context(fb, cfg->gl_major, cfg->gl_minor);
        if (!b->glc)
        {
            XDestroyWindow(g.dpy, b->win);
            free(b);
            return NULL;
        }
        glXMakeCurrent(g.dpy, b->win, b->glc);
    }

    XMapWindow(g.dpy, b->win);

    /* Honour cfg->monitor when centering or going fullscreen.
       monitor < 0 means MONITOR_CURRENT – resolve to whichever monitor
       contains the window now (after XMapWindow placed it). */
    int target_monitor = cfg->monitor;
    if (target_monitor < 0)
    {
        /* Find which monitor the window landed on. */
        int nmon = backend_monitor_count();
        for (int i = 0; i < nmon; i++)
        {
            MonitorInfo mi;
            if (backend_monitor_info(i, &mi))
            {
                if (b->pos_x >= mi.x && b->pos_x < mi.x + mi.width &&
                    b->pos_y >= mi.y && b->pos_y < mi.y + mi.height)
                {
                    target_monitor = i;
                    break;
                }
            }
        }
        if (target_monitor < 0)
            target_monitor = 0;
    }

    if (cfg->x == WINDOW_POS_CENTERED || cfg->y == WINDOW_POS_CENTERED)
    {
        MonitorInfo m;
        if (backend_monitor_info(target_monitor, &m))
        {
            int cx = cfg->x == WINDOW_POS_CENTERED ? m.x + (m.width - w) / 2 : x;
            int cy = cfg->y == WINDOW_POS_CENTERED ? m.y + (m.height - h) / 2 : y;
            XMoveWindow(g.dpy, b->win, cx, cy);
            b->pos_x = cx;
            b->pos_y = cy;
        }
    }
    if (cfg->vsync && b->render == RENDER_GL)
        backend_set_vsync(b, true);
    if (cfg->mode != WINDOW_WINDOWED)
        backend_set_mode(b, cfg->mode, target_monitor);

    XFlush(g.dpy);
    return b;
}

void backend_destroy(BackendWindow *b)
{
    if (!b)
        return;
    if (b->grabbed)
        XUngrabPointer(g.dpy, CurrentTime);
    if (b->xic)
        XDestroyIC(b->xic);
    if (b->image)
        XDestroyImage(b->image);
    if (b->gc)
        XFreeGC(g.dpy, b->gc);
    if (b->render == RENDER_GL)
    {
        glXMakeCurrent(g.dpy, None, NULL);
        if (b->glc)
            glXDestroyContext(g.dpy, b->glc);
    }
    XDeleteContext(g.dpy, b->win, g.ctx);
    if (b->hidden_cursor)
        XFreeCursor(g.dpy, b->hidden_cursor);
    if (b->active_cursor)
        XFreeCursor(g.dpy, b->active_cursor);
    XDestroyWindow(g.dpy, b->win);
    XFreeColormap(g.dpy, b->colormap);
    free(b);
}

/* ========================================================================== */
/*  event pump                                                                */
/* ========================================================================== */

static void push(BackendWindow *b, Event *e)
{
    core_push_event(b->core, e);
}

static void handle_key(BackendWindow *b, XEvent *ev, bool down)
{
    KeySym ks = XLookupKeysym(&ev->xkey, 0);
    Event e = {.type = EVENT_KEY};
    e.data.key.key = translate_keysym(ks);
    e.data.key.scancode = (int)ev->xkey.keycode;
    e.data.key.down = down;
    e.data.key.repeat = false;
    e.data.key.mods = translate_mods(ev->xkey.state);
    push(b, &e);

    if (!down)
        return;

    /* text: feed the IM and decode UTF-8 into codepoints */
    if (b->xic)
    {
        char buf[64];
        KeySym sym;
        Status status;
        int n = Xutf8LookupString(b->xic, &ev->xkey, buf, sizeof buf - 1, &sym, &status);
        if (n > 0)
        {
            buf[n] = '\0';
            for (const unsigned char *p = (unsigned char *)buf; *p;)
            {
                uint32_t cp;
                int len;
                if (*p < 0x80)
                {
                    cp = *p;
                    len = 1;
                }
                else if ((*p >> 5) == 0x6)
                {
                    cp = *p & 0x1F;
                    len = 2;
                }
                else if ((*p >> 4) == 0xE)
                {
                    cp = *p & 0x0F;
                    len = 3;
                }
                else if ((*p >> 3) == 0x1E)
                {
                    cp = *p & 0x07;
                    len = 4;
                }
                else
                {
                    p++;
                    continue;
                }
                for (int i = 1; i < len && p[i]; i++)
                    cp = (cp << 6) | (p[i] & 0x3F);
                if (cp >= 0x20 && cp != 0x7F)
                    core_push_char(b->core, cp);
                p += len;
            }
        }
    }
}

static void handle_button(BackendWindow *b, XEvent *ev, bool down)
{
    unsigned int btn = ev->xbutton.button;
    if (btn == Button4 || btn == Button5 || btn == 6 || btn == 7)
    {
        if (!down)
            return;
        Event e = {.type = EVENT_MOUSE_WHEEL};
        if (btn == Button4)
            e.data.wheel.y = 1.0f;
        else if (btn == Button5)
            e.data.wheel.y = -1.0f;
        else if (btn == 6)
            e.data.wheel.x = -1.0f;
        else
            e.data.wheel.x = 1.0f;
        push(b, &e);
        return;
    }

    int mb;
    switch (btn)
    {
    case Button1:
        mb = MOUSE_LEFT;
        break;
    case Button2:
        mb = MOUSE_MIDDLE;
        break;
    case Button3:
        mb = MOUSE_RIGHT;
        break;
    case 8:
        mb = MOUSE_X1;
        break;
    case 9:
        mb = MOUSE_X2;
        break;
    default:
        return;
    }
    Event e = {.type = EVENT_MOUSE_BUTTON};
    e.data.mouse.button = mb;
    e.data.mouse.down = down;
    e.data.mouse.x = ev->xbutton.x;
    e.data.mouse.y = ev->xbutton.y;
    push(b, &e);
}

static void process_event(BackendWindow *b, XEvent *ev)
{
    switch (ev->type)
    {
    case ClientMessage:
        if ((Atom)ev->xclient.data.l[0] == g.WM_DELETE_WINDOW)
        {
            Event e = {.type = EVENT_WINDOW_CLOSE};
            push(b, &e);
        }
        break;

    case ConfigureNotify:
    {
        int nw = ev->xconfigure.width, nh = ev->xconfigure.height;
        if (nw != b->width || nh != b->height)
        {
            b->width = nw;
            b->height = nh;
            Event r = {.type = EVENT_WINDOW_RESIZE};
            r.data.resize.w = nw;
            r.data.resize.h = nh;
            push(b, &r);
            Event fbr = {.type = EVENT_WINDOW_FB_RESIZE};
            fbr.data.resize.w = nw; /* X11 has no per-window scaling: fb == size */
            fbr.data.resize.h = nh;
            push(b, &fbr);
        }
        if (ev->xconfigure.x != b->pos_x || ev->xconfigure.y != b->pos_y)
        {
            b->pos_x = ev->xconfigure.x;
            b->pos_y = ev->xconfigure.y;
            Event m = {.type = EVENT_WINDOW_MOVE};
            m.data.move.x = b->pos_x;
            m.data.move.y = b->pos_y;
            push(b, &m);
        }
        break;
    }

    case KeyPress:
        handle_key(b, ev, true);
        break;
    case KeyRelease:
        /* swallow the synthetic release of an auto-repeat pair */
        if (XEventsQueued(g.dpy, QueuedAfterReading))
        {
            XEvent next;
            XPeekEvent(g.dpy, &next);
            if (next.type == KeyPress && next.xkey.time == ev->xkey.time &&
                next.xkey.keycode == ev->xkey.keycode)
            {
                XNextEvent(g.dpy, &next);
                Event e = {.type = EVENT_KEY};
                e.data.key.key = translate_keysym(XLookupKeysym(&next.xkey, 0));
                e.data.key.scancode = (int)next.xkey.keycode;
                e.data.key.down = true;
                e.data.key.repeat = true;
                e.data.key.mods = translate_mods(next.xkey.state);
                push(b, &e);
                break;
            }
        }
        handle_key(b, ev, false);
        break;

    case ButtonPress:
        handle_button(b, ev, true);
        break;
    case ButtonRelease:
        handle_button(b, ev, false);
        break;

    case MotionNotify:
    {
        Event e = {.type = EVENT_MOUSE_MOVE};
        e.data.mouse.x = ev->xmotion.x;
        e.data.mouse.y = ev->xmotion.y;
        push(b, &e);
        break;
    }

    case EnterNotify:
    case LeaveNotify:
    {
        b->hovered = ev->type == EnterNotify;
        Event e = {.type = EVENT_WINDOW_ENTER};
        e.data.enter.entered = b->hovered;
        push(b, &e);
        break;
    }

    case FocusIn:
    case FocusOut:
    {
        b->focused = ev->type == FocusIn;
        if (b->xic)
            (b->focused ? XSetICFocus : XUnsetICFocus)(b->xic);
        Event e = {.type = EVENT_WINDOW_FOCUS};
        e.data.focus.gained = b->focused;
        push(b, &e);
        break;
    }

    case MapNotify:
        b->visible = true;
        break;
    case UnmapNotify:
        b->visible = false;
        break;
    case PropertyNotify:
        if (ev->xproperty.atom == g.NET_WM_STATE)
            refresh_wm_state(b);
        break;
    }
}

/* Answer a request for our CLIPBOARD selection (we are the owner). */
static void answer_selection_request(XSelectionRequestEvent *req)
{
    XSelectionEvent resp = {0};
    resp.type = SelectionNotify;
    resp.display = req->display;
    resp.requestor = req->requestor;
    resp.selection = req->selection;
    resp.target = req->target;
    resp.time = req->time;
    resp.property = None;

    if (req->target == g.TARGETS)
    {
        Atom targets[] = {g.TARGETS, g.UTF8_STRING, XA_STRING};
        XChangeProperty(g.dpy, req->requestor, req->property, XA_ATOM, 32,
                        PropModeReplace, (unsigned char *)targets, 3);
        resp.property = req->property;
    }
    else if ((req->target == g.UTF8_STRING || req->target == XA_STRING) && g.clipboard_text)
    {
        XChangeProperty(g.dpy, req->requestor, req->property, req->target, 8,
                        PropModeReplace, (unsigned char *)g.clipboard_text,
                        (int)strlen(g.clipboard_text));
        resp.property = req->property;
    }
    XSendEvent(g.dpy, req->requestor, True, NoEventMask, (XEvent *)&resp);
}

void backend_pump_events(BackendWindow *b, Core *core)
{
    b->core = core;
    while (XPending(g.dpy))
    {
        XEvent ev;
        XNextEvent(g.dpy, &ev);
        if (XFilterEvent(&ev, None))
            continue;
        if (ev.type == SelectionRequest)
        {
            answer_selection_request(&ev.xselectionrequest);
            continue;
        }
        if (ev.type == SelectionClear)
        {
            free(g.clipboard_text);
            g.clipboard_text = NULL;
            continue;
        }
        if (ev.xany.window != b->win)
            continue; /* MVP is single-window; other windows are not dispatched here */
        process_event(b, &ev);
    }
}

void backend_swap(BackendWindow *b)
{
    if (b->render == RENDER_GL)
        glXSwapBuffers(g.dpy, b->win);
}

/* ========================================================================== */
/*  GL context glue                                                           */
/* ========================================================================== */

void backend_make_current(BackendWindow *b)
{
    if (b->render == RENDER_GL)
        glXMakeCurrent(g.dpy, b->win, b->glc);
}

void backend_set_vsync(BackendWindow *b, bool on)
{
    int interval = on ? 1 : 0;
    if (g.swap_ext)
        g.swap_ext(g.dpy, b->win, interval);
    else if (g.swap_mesa)
        g.swap_mesa((unsigned)interval);
    else if (g.swap_sgi)
        g.swap_sgi(interval);
}

void *backend_gl_proc_address(const char *name)
{
    return (void *)glXGetProcAddressARB((const GLubyte *)name);
}

/* ========================================================================== */
/*  pixel surface (XImage, 32-bit)                                            */
/* ========================================================================== */

bool backend_lock_pixels(BackendWindow *b, Framebuffer *out)
{
    if (b->render != RENDER_PIXELS || !out)
        return false;

    if (!b->image || b->image->width != b->width || b->image->height != b->height)
    {
        if (b->image)
            XDestroyImage(b->image);
        int depth = DefaultDepth(g.dpy, g.screen);
        Visual *visual = DefaultVisual(g.dpy, g.screen);
        uint32_t *data = malloc((size_t)b->width * b->height * 4);
        if (!data)
            return false;
        b->image = XCreateImage(g.dpy, visual, depth, ZPixmap, 0, (char *)data,
                                b->width, b->height, 32, 0);
        if (!b->image)
        {
            free(data);
            return false;
        }
    }

    out->pixels = (uint32_t *)b->image->data;
    out->width = b->image->width;
    out->height = b->image->height;
    out->stride = b->image->bytes_per_line / 4;
    return true;
}

void backend_present_pixels(BackendWindow *b)
{
    if (b->render != RENDER_PIXELS || !b->image)
        return;
    XPutImage(g.dpy, b->win, b->gc, b->image, 0, 0, 0, 0,
              (unsigned)b->image->width, (unsigned)b->image->height);
    XFlush(g.dpy);
}

/* ========================================================================== */
/*  geometry                                                                  */
/* ========================================================================== */

void backend_get_size(BackendWindow *b, int *w, int *h)
{
    if (w)
        *w = b->width;
    if (h)
        *h = b->height;
}

void backend_set_size(BackendWindow *b, int w, int h)
{
    XResizeWindow(g.dpy, b->win, (unsigned)w, (unsigned)h);
}

void backend_get_fb_size(BackendWindow *b, int *w, int *h)
{
    if (w)
        *w = b->width; /* no per-window scaling on X11 */
    if (h)
        *h = b->height;
}

void backend_get_pos(BackendWindow *b, int *x, int *y)
{
    Window child;
    int rx, ry;
    XTranslateCoordinates(g.dpy, b->win, g.root, 0, 0, &rx, &ry, &child);
    if (x)
        *x = rx;
    if (y)
        *y = ry;
}

void backend_set_pos(BackendWindow *b, int x, int y)
{
    XMoveWindow(g.dpy, b->win, x, y);
}

float backend_content_scale(BackendWindow *b)
{
    (void)b;
    return 1.0f;
}

void backend_set_title(BackendWindow *b, const char *title)
{
    set_title(b, title);
}

void backend_set_size_limits(BackendWindow *b, int minw, int minh, int maxw, int maxh)
{
    XSizeHints *hints = XAllocSizeHints();
    if (!hints) return;

    hints->flags = 0;

    if (minw > 0 || minh > 0) {
        hints->flags |= PMinSize;
        hints->min_width  = minw > 0 ? minw : 1;
        hints->min_height = minh > 0 ? minh : 1;
    }

    if (maxw > 0 || maxh > 0) {
        hints->flags |= PMaxSize;
        hints->max_width  = maxw > 0 ? maxw : 32767;
        hints->max_height = maxh > 0 ? maxh : 32767;
    }

    XSetWMNormalHints(g.dpy, b->win, hints);
    XFree(hints);
}

/* ========================================================================== */
/*  window state                                                              */
/* ========================================================================== */

void backend_minimize(BackendWindow *b)
{
    XIconifyWindow(g.dpy, b->win, g.screen);
}

void backend_maximize(BackendWindow *b)
{
    send_wm_state(b, 1, g.NET_WM_STATE_MAXIMIZED_VERT, g.NET_WM_STATE_MAXIMIZED_HORZ);
}

void backend_restore(BackendWindow *b)
{
    send_wm_state(b, 0, g.NET_WM_STATE_MAXIMIZED_VERT, g.NET_WM_STATE_MAXIMIZED_HORZ);
    XMapWindow(g.dpy, b->win);
}

void backend_show(BackendWindow *b)
{
    XMapWindow(g.dpy, b->win);
}
void backend_hide(BackendWindow *b)
{
    XUnmapWindow(g.dpy, b->win);
}

void backend_focus(BackendWindow *b)
{
    XRaiseWindow(g.dpy, b->win);
    XSetInputFocus(g.dpy, b->win, RevertToParent, CurrentTime);
}

void backend_request_attention(BackendWindow *b)
{
    send_wm_state(b, 1, g.NET_WM_STATE_DEMANDS_ATTENTION, 0);
}

bool backend_get_flag(BackendWindow *b, int flag)
{
    switch (flag)
    {
    case WIN_FLAG_FOCUSED:
        return b->focused;
    case WIN_FLAG_MINIMIZED:
        return b->minimized;
    case WIN_FLAG_MAXIMIZED:
        return b->maximized;
    case WIN_FLAG_VISIBLE:
        return b->visible;
    case WIN_FLAG_HOVERED:
        return b->hovered;
    default:
        return false;
    }
}

void backend_set_mode(BackendWindow *b, WindowMode mode, int monitor)
{
    bool fs = mode != WINDOW_WINDOWED;

    if (fs && monitor >= 0)
    {
        /* Move to the target monitor before going fullscreen so the WM
           fullscreens on the correct head. */
        MonitorInfo m;
        if (backend_monitor_info(monitor, &m))
        {
            /* Place it roughly centred so it's clearly on that monitor. */
            int cx = m.x + (m.width - b->width) / 2;
            int cy = m.y + (m.height - b->height) / 2;
            XMoveWindow(g.dpy, b->win, cx, cy);
            XFlush(g.dpy);
        }
    }

    send_wm_state(b, fs ? 1 : 0, g.NET_WM_STATE_FULLSCREEN, 0);
    b->mode = mode;
}

WindowMode backend_get_mode(BackendWindow *b)
{
    return b->mode;
}

void backend_set_icon(BackendWindow *b, int w, int h, const uint8_t *rgba)
{
    if (!rgba || w <= 0 || h <= 0)
    {
        XDeleteProperty(g.dpy, b->win, g.NET_WM_ICON);
        return;
    }
    /* _NET_WM_ICON is width, height, then w*h ARGB longs. */
    int n = 2 + w * h;
    long *buf = malloc((size_t)n * sizeof(long));
    if (!buf)
        return;
    buf[0] = w;
    buf[1] = h;
    for (int i = 0; i < w * h; i++)
    {
        const uint8_t *p = rgba + i * 4;
        buf[2 + i] = ((long)p[3] << 24) | ((long)p[0] << 16) | ((long)p[1] << 8) | (long)p[2];
    }
    XChangeProperty(g.dpy, b->win, g.NET_WM_ICON, XA_CARDINAL, 32, PropModeReplace,
                    (unsigned char *)buf, n);
    free(buf);
}

void backend_set_opacity(BackendWindow *b, float a)
{
    if (a < 0.0f) a = 0.0f;
    if (a > 1.0f) a = 1.0f;
    uint32_t value = (uint32_t)((double)a * 4294967295.0);
    XChangeProperty(g.dpy, b->win, g.NET_WM_WINDOW_OPACITY, XA_CARDINAL, 32,
                    PropModeReplace, (unsigned char *)&value, 1);
}

void backend_set_always_on_top(BackendWindow *b, bool on)
{
    send_wm_state(b, on ? 1 : 0, g.NET_WM_STATE_ABOVE, 0);
}

/* ========================================================================== */
/*  mouse                                                                     */
/* ========================================================================== */

void backend_set_mouse_pos(BackendWindow *b, int x, int y)
{
    XWarpPointer(g.dpy, None, b->win, 0, 0, 0, 0, x, y);
    XFlush(g.dpy);
}

static Cursor make_hidden_cursor(BackendWindow *b)
{
    if (b->hidden_cursor)
        return b->hidden_cursor;
    char data[1] = {0};
    Pixmap pm = XCreateBitmapFromData(g.dpy, b->win, data, 1, 1);
    XColor c = {0};
    b->hidden_cursor = XCreatePixmapCursor(g.dpy, pm, pm, &c, &c, 0, 0);
    XFreePixmap(g.dpy, pm);
    return b->hidden_cursor;
}

void backend_set_cursor(BackendWindow *b, int cursor)
{
    unsigned int shape;
    switch (cursor)
    {
    case CURSOR_IBEAM:
        shape = XC_xterm;
        break;
    case CURSOR_CROSSHAIR:
        shape = XC_crosshair;
        break;
    case CURSOR_HAND:
        shape = XC_hand2;
        break;
    case CURSOR_RESIZE_EW:
        shape = XC_sb_h_double_arrow;
        break;
    case CURSOR_RESIZE_NS:
        shape = XC_sb_v_double_arrow;
        break;
    case CURSOR_NOT_ALLOWED:
        shape = XC_X_cursor;
        break;
    default:
        shape = XC_left_ptr;
        break;
    }
    if (b->active_cursor)
        XFreeCursor(g.dpy, b->active_cursor);
    b->active_cursor = XCreateFontCursor(g.dpy, shape);
    if (b->cursor_mode == MOUSE_MODE_NORMAL)
        XDefineCursor(g.dpy, b->win, b->active_cursor);
}

void backend_set_mouse_mode(BackendWindow *b, int mode)
{
    b->cursor_mode = mode;
    if (mode == MOUSE_MODE_NORMAL)
    {
        if (b->grabbed)
        {
            XUngrabPointer(g.dpy, CurrentTime);
            b->grabbed = false;
        }
        XDefineCursor(g.dpy, b->win, b->active_cursor ? b->active_cursor : None);
    }
    else
    {
        XDefineCursor(g.dpy, b->win, make_hidden_cursor(b));
        if (mode == MOUSE_MODE_CAPTURED && !b->grabbed)
        {
            XGrabPointer(g.dpy, b->win, True,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                         GrabModeAsync, GrabModeAsync, b->win, None, CurrentTime);
            b->grabbed = true;
        }
    }
    XFlush(g.dpy);
}

/* ========================================================================== */
/*  clipboard (CLIPBOARD selection)                                           */
/* ========================================================================== */

void backend_clipboard_set(const char *text)
{
    free(g.clipboard_text);
    g.clipboard_text = text ? strdup(text) : NULL;
    XSetSelectionOwner(g.dpy, g.CLIPBOARD, g.helper, CurrentTime);
    XFlush(g.dpy);
}

const char *backend_clipboard_get(void)
{
    /* If we own the selection, skip the round trip. */
    if (XGetSelectionOwner(g.dpy, g.CLIPBOARD) == g.helper)
        return g.clipboard_text ? g.clipboard_text : "";

    XConvertSelection(g.dpy, g.CLIPBOARD, g.UTF8_STRING, g.XSEL_DATA, g.helper, CurrentTime);
    XFlush(g.dpy);

    /* Wait briefly for the reply without consuming the window's events. */
    for (int i = 0; i < 100; i++)
    {
        XEvent ev;
        if (XCheckTypedWindowEvent(g.dpy, g.helper, SelectionNotify, &ev))
        {
            free(g.clipboard_text);
            g.clipboard_text = NULL;
            if (ev.xselection.property == None)
                return "";

            Atom type;
            int fmt;
            unsigned long n, after;
            unsigned char *data = NULL;
            XGetWindowProperty(g.dpy, g.helper, g.XSEL_DATA, 0, ~0L, False,
                               AnyPropertyType, &type, &fmt, &n, &after, &data);
            if (data)
            {
                g.clipboard_text = malloc(n + 1);
                if (g.clipboard_text)
                {
                    memcpy(g.clipboard_text, data, n);
                    g.clipboard_text[n] = '\0';
                }
                XFree(data);
            }
            XDeleteProperty(g.dpy, g.helper, g.XSEL_DATA);
            return g.clipboard_text ? g.clipboard_text : "";
        }
        struct timespec ts = {0, 1000000}; /* 1 ms */
        nanosleep(&ts, NULL);
    }
    return "";
}

/* ========================================================================== */
/*  monitors (XRandR, one virtual coordinate space)                           */
/* ========================================================================== */

int backend_monitor_count(void)
{
    int n = 0;
    XRRMonitorInfo *m = XRRGetMonitors(g.dpy, g.root, True, &n);
    if (m)
        XRRFreeMonitors(m);
    return n > 0 ? n : 1;
}

bool backend_monitor_info(int index, MonitorInfo *out)
{
    if (!out)
        return false;
    int n = 0;
    XRRMonitorInfo *m = XRRGetMonitors(g.dpy, g.root, True, &n);
    if (!m || index < 0 || index >= n)
    {
        if (m)
            XRRFreeMonitors(m);
        return false;
    }
    XRRMonitorInfo *mi = &m[index];
    memset(out, 0, sizeof *out);
    out->index = index;
    out->x = mi->x;
    out->y = mi->y;
    out->width = mi->width;
    out->height = mi->height;
    out->work_x = mi->x;
    out->work_y = mi->y;
    out->work_w = mi->width;
    out->work_h = mi->height;
    out->phys_width_mm = mi->mwidth;
    out->phys_height_mm = mi->mheight;
    out->refresh_hz = 60;
    /* content_scale stays 1.0: X11 has no logical/pixel split, fb == window size */
    out->content_scale = 1.0f;
    out->primary = mi->primary != 0;

    /* Refresh from the CRTC mode of the monitor's first output. */
    if (mi->noutput > 0)
    {
        XRRScreenResources *res = XRRGetScreenResourcesCurrent(g.dpy, g.root);
        if (res)
        {
            XRROutputInfo *oi = XRRGetOutputInfo(g.dpy, res, mi->outputs[0]);
            if (oi && oi->crtc)
            {
                XRRCrtcInfo *ci = XRRGetCrtcInfo(g.dpy, res, oi->crtc);
                if (ci)
                {
                    for (int i = 0; i < res->nmode; i++)
                        if (res->modes[i].id == ci->mode)
                        {
                            XRRModeInfo *mode = &res->modes[i];
                            unsigned long total = (unsigned long)mode->hTotal * mode->vTotal;
                            if (total)
                                out->refresh_hz = (int)((double)mode->dotClock / total + 0.5);
                            break;
                        }
                    XRRFreeCrtcInfo(ci);
                }
            }
            if (oi)
                XRRFreeOutputInfo(oi);
            XRRFreeScreenResources(res);
        }
    }
    char *name = XGetAtomName(g.dpy, mi->name);
    static char namebuf[64];
    snprintf(namebuf, sizeof namebuf, "%s", name ? name : "monitor");
    if (name)
        XFree(name);
    out->name = namebuf;
    XRRFreeMonitors(m);
    return true;
}

/* ========================================================================== */
/*  loop                                                                      */
/* ========================================================================== */

void backend_run(BackendWindow *b, PlatformWindow *w, FrameCallback frame, void *user)
{
    (void)b;
    while (!window_should_close(w))
    {
        window_begin_frame(w);
        frame(w, user);
        window_swap(w);
    }
}
