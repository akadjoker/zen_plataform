/*
 * backend_wayland.c - Wayland + EGL backend. One xdg-shell toplevel and one
 * wl_egl_window per window; everything the compositor says is normalized into
 * core_push_event. No input logic lives here; that is the core's job.
 *
 * Built in phases (notes/docs/WAYLAND.md). What is not here yet answers like a
 * platform that lacks the feature, so the library links and the core keeps
 * working.
 */
#define _GNU_SOURCE
#include "core_internal.h"
#include "backend.h"
#include "error_internal.h"

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include "xdg-shell-client-protocol.h"
#ifdef ZEN_WL_POINTER_LOCK
#include "pointer-constraints-unstable-v1-client-protocol.h"
#include "relative-pointer-unstable-v1-client-protocol.h"
#endif
#ifdef ZEN_WL_XDG_DECORATION
#include "xdg-decoration-unstable-v1-client-protocol.h"
#endif

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

/* linux/input-event-codes.h, the buttons wl_pointer reports */
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114

/* ---- global connection ---- */

enum
{
    DECOR_TOP,
    DECOR_LEFT,
    DECOR_RIGHT,
    DECOR_BOTTOM,
    DECOR_COUNT
};

typedef struct DecorPart
{
    struct wl_surface *surface;
    struct wl_subsurface *sub;
    struct wl_buffer *buffer; /* the one on the surface, until the compositor lets go */
    BackendWindow *owner;
    int part;
} DecorPart;

static struct
{
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct xdg_wm_base *wm_base;

    EGLDisplay egl;
    bool egl_create_context; /* version and profile attributes: EGL 1.5 or the KHR extension */
    bool lost;               /* the connection broke; the close event was sent */

    BackendWindow *windows; /* every window, to find the one a surface belongs to */

    struct wl_seat *seat;
    struct wl_keyboard *keyboard;
    struct wl_pointer *pointer;
    BackendWindow *keyboard_focus, *pointer_focus;
    uint32_t serial; /* of the last key or button, which clipboard and move requests quote */
    uint32_t enter_serial; /* of the pointer's last enter, which a cursor request quotes */

    struct wl_subcompositor *subcompositor;
#ifdef ZEN_WL_XDG_DECORATION
    struct zxdg_decoration_manager_v1 *deco_manager;
#endif
    DecorPart *pointer_decor; /* the part of a frame the pointer is on, if any */
    int decor_x, decor_y;     /* where, in that part */
    int decor_shape;          /* the cursor it shows: asked for only when it changes */

    struct wl_shm *shm;
    struct wl_cursor_theme *theme; /* loaded on first use */
    struct wl_surface *cursor_surface;
#ifdef ZEN_WL_POINTER_LOCK
    struct zwp_pointer_constraints_v1 *constraints;
    struct zwp_relative_pointer_manager_v1 *relative_manager;
#endif

    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *xkb_state;
    struct xkb_compose_state *compose;
    xkb_mod_index_t mod_shift, mod_ctrl, mod_alt, mod_super, mod_caps, mod_num;

    /* The compositor sends no repeats: the client makes them from repeat_info. */
    int repeat_rate;  /* per second, 0 is off */
    int repeat_delay; /* ms before the first one */
    xkb_keycode_t repeat_key;
    double repeat_next; /* ms */

    int pointer_x, pointer_y;
    double axis_x, axis_y; /* continuous scroll of the current pointer frame */
    int v120_x, v120_y;    /* wheel notches of it, in 1/120 */
} g;

static const struct wl_seat_listener seat_listener;
static void apply_cursor(BackendWindow *b);
static void set_pointer_cursor(int shape, struct PlatformCursor *image, bool hidden);
static void apply_limits(BackendWindow *b);
static DecorPart *decor_for_surface(struct wl_surface *surface);
static void decor_enter(DecorPart *d, int x, int y);
static void decor_motion(DecorPart *d, int x, int y);
static void decor_leave(DecorPart *d);
static void decor_button(DecorPart *d, uint32_t serial, uint32_t button, bool down);
static void decor_extents(const BackendWindow *b, bool maximized, bool fullscreen, int *l, int *t, int *r, int *bt);
static void start_move_resize(BackendWindow *b, HitTestResult r, uint32_t serial);
static int hit_cursor_for(HitTestResult r);

struct BackendWindow
{
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    EGLContext context;
    EGLConfig config; /* what a new surface is made with when the window is shown again */

    RenderMode render;
    int width, height; /* screen coordinates */
    int restore_w, restore_h; /* the size before maximize or fullscreen took it */
    char *title;
    int min_w, min_h, max_w, max_h; /* 0 is no limit */

    /* what the last toplevel.configure said, applied when its xdg_surface.configure arrives */
    int pending_w, pending_h; /* 0 leaves the choice to the client */
    bool pending_focused, pending_maximized, pending_fullscreen;
    bool configured;

    bool focused, maximized, fullscreen; /* as applied */
    bool want_maximized;                 /* asked for; also what a hidden window comes back as */
    bool visible;                        /* false between hide and show: the shell objects are gone */
    WindowMode mode;                     /* as asked for: fullscreen and borderless are the same here */

    bool vsync;
    struct wl_callback *frame_cb; /* the frame callback of the last swap, until the compositor answers */

    bool hovered;
    BackendWindow *next; /* g.windows */

    int cursor_idx;                /* the CURSOR_* shape */
    struct PlatformCursor *cursor_image;
    int mouse_mode;                /* MOUSE_MODE_* */
#ifdef ZEN_WL_POINTER_LOCK
    struct zwp_locked_pointer_v1 *locked_pointer;
    struct zwp_relative_pointer_v1 *relative_pointer;
#endif
    DecorPart decor[DECOR_COUNT];
    bool decorated;     /* asked for */
    bool server_side;   /* the compositor said it draws the frame */
    bool csd;           /* decorated, and drawn here */
    bool decor_dirty;
    int decor_ext[4];   /* the frame as last sent: left, top, right, bottom */
    int hover_button, press_button;
    double title_click_ms;
    HitTestFunc hit_cb; /* a title bar and edges of the application's own */
    PlatformWindow *hit_w;
    void *hit_user;
    bool hit_press;
    int hit_cursor;     /* the resize cursor the hit test asks for, or -1 */
#ifdef ZEN_WL_XDG_DECORATION
    struct zxdg_toplevel_decoration_v1 *xdg_deco;
#endif
    bool locked;                   /* the compositor holds the pointer still */
    double virt_x, virt_y;         /* where the pointer would be: moved by the raw deltas while locked */
    int lock_x, lock_y;            /* where it was when the lock began */

    /* events the listeners raised, handed to the core by the window's own pump:
       the connection is shared, so any window's pump may dispatch for another */
    Event *queue;
    int queue_n, queue_cap;
};

static bool has_word(const char *list, const char *word)
{
    size_t n = strlen(word);
    for (const char *p = list; p && *p;)
    {
        const char *end = strchr(p, ' ');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && strncmp(p, word, n) == 0)
            return true;
        p = end ? end + 1 : p + len;
    }
    return false;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static BackendWindow *window_for_surface(struct wl_surface *surface)
{
    for (BackendWindow *w = g.windows; w; w = w->next)
    {
        if (w->surface == surface)
            return w;
    }
    return NULL;
}

static void push(BackendWindow *b, const Event *e)
{
    if (b->queue_n == b->queue_cap)
    {
        int cap = b->queue_cap ? b->queue_cap * 2 : 16;
        Event *q = realloc(b->queue, (size_t)cap * sizeof *q);
        if (!q)
            return;
        b->queue = q;
        b->queue_cap = cap;
    }
    b->queue[b->queue_n++] = *e;
}

/* ========================================================================== */
/*  registry                                                                  */
/* ========================================================================== */

static void registry_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t version)
{
    (void)data;
    if (strcmp(iface, wl_compositor_interface.name) == 0)
        g.compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(iface, xdg_wm_base_interface.name) == 0)
        g.wm_base = wl_registry_bind(reg, name, &xdg_wm_base_interface, version < 5 ? version : 5);
    else if (strcmp(iface, wl_shm_interface.name) == 0)
        g.shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    else if (strcmp(iface, wl_subcompositor_interface.name) == 0)
        g.subcompositor = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
#ifdef ZEN_WL_XDG_DECORATION
    else if (strcmp(iface, zxdg_decoration_manager_v1_interface.name) == 0)
        g.deco_manager = wl_registry_bind(reg, name, &zxdg_decoration_manager_v1_interface, 1);
#endif
#ifdef ZEN_WL_POINTER_LOCK
    else if (strcmp(iface, zwp_pointer_constraints_v1_interface.name) == 0)
        g.constraints = wl_registry_bind(reg, name, &zwp_pointer_constraints_v1_interface, 1);
    else if (strcmp(iface, zwp_relative_pointer_manager_v1_interface.name) == 0)
        g.relative_manager = wl_registry_bind(reg, name, &zwp_relative_pointer_manager_v1_interface, 1);
#endif
    else if (strcmp(iface, wl_seat_interface.name) == 0 && !g.seat)
    {
        g.seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 8 ? version : 8);
        wl_seat_add_listener(g.seat, &seat_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
    (void)data;
    (void)reg;
    (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* A compositor that gets no pong in time calls the client unresponsive. */
static void wm_base_ping(void *data, struct xdg_wm_base *wm, uint32_t serial)
{
    (void)data;
    xdg_wm_base_pong(wm, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = {
    .ping = wm_base_ping,
};

/* ========================================================================== */
/*  init / shutdown                                                           */
/* ========================================================================== */

static EGLDisplay open_egl_display(void)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display = NULL;
    const char *ext = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (ext && has_word(ext, "EGL_EXT_platform_base"))
        *(void **)&get_platform_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display)
        return get_platform_display(EGL_PLATFORM_WAYLAND_EXT, g.display, NULL);
    return eglGetDisplay((EGLNativeDisplayType)g.display);
}

bool backend_init(void)
{
    g.display = wl_display_connect(NULL);
    if (!g.display)
        return error_set("cannot connect to the Wayland display");

    g.repeat_rate = 25;
    g.repeat_delay = 600;
    g.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (g.xkb)
    {
        const char *locale = getenv("LC_ALL");
        if (!locale || !*locale)
            locale = getenv("LC_CTYPE");
        if (!locale || !*locale)
            locale = getenv("LANG");
        struct xkb_compose_table *table =
            xkb_compose_table_new_from_locale(g.xkb, locale && *locale ? locale : "C", XKB_COMPOSE_COMPILE_NO_FLAGS);
        if (table)
        {
            g.compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
            xkb_compose_table_unref(table);
        }
    }
    else
    {
        log_warn("Wayland: xkbcommon did not start, there will be no keyboard");
    }

    g.registry = wl_display_get_registry(g.display);
    wl_registry_add_listener(g.registry, &registry_listener, NULL);
    wl_display_roundtrip(g.display);
    if (!g.compositor || !g.wm_base)
    {
        error_set("the compositor has no %s", g.compositor ? "xdg_wm_base" : "wl_compositor");
        backend_shutdown();
        return false;
    }
    xdg_wm_base_add_listener(g.wm_base, &wm_base_listener, NULL);
    if (g.seat)
    {
        wl_display_roundtrip(g.display); /* the seat's capabilities, and so the keyboard and pointer */
        wl_display_roundtrip(g.display); /* the keymap and repeat_info of the keyboard */
    }

    g.egl = open_egl_display();
    EGLint major = 0, minor = 0;
    if (g.egl == EGL_NO_DISPLAY || !eglInitialize(g.egl, &major, &minor))
    {
        error_set("cannot initialize EGL on the Wayland display");
        backend_shutdown();
        return false;
    }
    g.egl_create_context = major > 1 || minor >= 5 ||
                           has_word(eglQueryString(g.egl, EGL_EXTENSIONS), "EGL_KHR_create_context");
    return true;
}

void backend_shutdown(void)
{
    if (g.egl)
        eglTerminate(g.egl);
    if (g.theme)
        wl_cursor_theme_destroy(g.theme);
    if (g.cursor_surface)
        wl_surface_destroy(g.cursor_surface);
#ifdef ZEN_WL_POINTER_LOCK
    if (g.relative_manager)
        zwp_relative_pointer_manager_v1_destroy(g.relative_manager);
    if (g.constraints)
        zwp_pointer_constraints_v1_destroy(g.constraints);
#endif
    if (g.shm)
        wl_shm_destroy(g.shm);
    if (g.keyboard)
        wl_keyboard_release(g.keyboard);
    if (g.pointer)
        wl_pointer_release(g.pointer);
    if (g.seat)
        wl_seat_destroy(g.seat);
    xkb_compose_state_unref(g.compose);
    xkb_state_unref(g.xkb_state);
    xkb_keymap_unref(g.keymap);
    xkb_context_unref(g.xkb);
    if (g.wm_base)
        xdg_wm_base_destroy(g.wm_base);
    if (g.compositor)
        wl_compositor_destroy(g.compositor);
    if (g.registry)
        wl_registry_destroy(g.registry);
    if (g.display)
    {
        wl_display_flush(g.display);
        wl_display_disconnect(g.display);
    }
    memset(&g, 0, sizeof g);
}

/* ========================================================================== */
/*  xdg-shell listeners                                                       */
/* ========================================================================== */

static void set_opaque_region(BackendWindow *b)
{
    /* The framebuffer has an alpha channel; without this the compositor blends the
       window with what is behind it wherever the application left alpha at zero. */
    struct wl_region *r = wl_compositor_create_region(g.compositor);
    wl_region_add(r, 0, 0, b->width, b->height);
    wl_surface_set_opaque_region(b->surface, r);
    wl_region_destroy(r);
}

static void apply_size(BackendWindow *b, int w, int h)
{
    if (w == b->width && h == b->height)
        return;
    b->width = w;
    b->height = h;
    if (b->egl_window)
        wl_egl_window_resize(b->egl_window, w, h, 0, 0);
    set_opaque_region(b);

    Event r = {.type = EVENT_WINDOW_RESIZE};
    r.data.resize.w = w;
    r.data.resize.h = h;
    push(b, &r);
    Event fbr = {.type = EVENT_WINDOW_FB_RESIZE};
    fbr.data.resize.w = w; /* no buffer scale yet: fb == size */
    fbr.data.resize.h = h;
    push(b, &fbr);
}

/* The state arrives in toplevel.configure and takes effect with the xdg_surface
   configure that follows it, once acknowledged. */
static void toplevel_configure(void *data, struct xdg_toplevel *t, int32_t w, int32_t h, struct wl_array *states)
{
    BackendWindow *b = data;
    (void)t;
    b->pending_w = w;
    b->pending_h = h;

    b->pending_focused = b->pending_maximized = b->pending_fullscreen = false;
    uint32_t *s;
    wl_array_for_each(s, states)
    {
        switch (*s)
        {
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            b->pending_focused = true;
            break;
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            b->pending_maximized = true;
            break;
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            b->pending_fullscreen = true;
            break;
        }
    }
}

static void toplevel_close(void *data, struct xdg_toplevel *t)
{
    (void)t;
    Event e = {.type = EVENT_WINDOW_CLOSE};
    push(data, &e);
}

static void toplevel_configure_bounds(void *data, struct xdg_toplevel *t, int32_t w, int32_t h)
{
    (void)data;
    (void)t;
    (void)w;
    (void)h;
}

static void toplevel_wm_capabilities(void *data, struct xdg_toplevel *t, struct wl_array *caps)
{
    (void)data;
    (void)t;
    (void)caps;
}

static const struct xdg_toplevel_listener toplevel_listener = {
    .configure = toplevel_configure,
    .close = toplevel_close,
    .configure_bounds = toplevel_configure_bounds,
    .wm_capabilities = toplevel_wm_capabilities,
};

static void surface_configure(void *data, struct xdg_surface *s, uint32_t serial)
{
    BackendWindow *b = data;
    xdg_surface_ack_configure(s, serial);

    bool was_tiled = b->maximized || b->fullscreen;
    bool now_tiled = b->pending_maximized || b->pending_fullscreen;
    if (!was_tiled && now_tiled)
    {
        b->restore_w = b->width;
        b->restore_h = b->height;
    }

    /* A zero size says the client picks. Leaving maximized or fullscreen that is
       the size it had before; otherwise the one it has. */
    int w = b->pending_w, h = b->pending_h;
    if (w <= 0 || h <= 0)
    {
        if (was_tiled && !now_tiled && b->restore_w > 0)
        {
            w = b->restore_w;
            h = b->restore_h;
        }
        else
        {
            w = b->width;
            h = b->height;
        }
    }
    else
    {
        /* the size the compositor gives is the window's, frame included */
        int l, t, r, bt;
        decor_extents(b, b->pending_maximized, b->pending_fullscreen, &l, &t, &r, &bt);
        w -= l + r;
        h -= t + bt;
        if (w < 1)
            w = 1;
        if (h < 1)
            h = 1;
    }
    apply_size(b, w, h);
    b->decor_dirty = true;

    if (b->pending_maximized != b->maximized)
    {
        b->maximized = b->pending_maximized;
        Event e = {.type = b->maximized ? EVENT_WINDOW_MAXIMIZE : EVENT_WINDOW_RESTORE};
        push(b, &e);
    }
    if (b->pending_fullscreen != b->fullscreen)
    {
        /* Someone else took the window in or out of fullscreen. */
        if (!b->pending_fullscreen)
            b->mode = WINDOW_WINDOWED;
        else if (b->mode == WINDOW_WINDOWED)
            b->mode = WINDOW_FULLSCREEN;
    }
    b->fullscreen = b->pending_fullscreen;
    if (b->pending_focused != b->focused)
    {
        b->focused = b->pending_focused;
        Event e = {.type = EVENT_WINDOW_FOCUS};
        e.data.focus.gained = b->focused;
        push(b, &e);
    }
    b->configured = true;
}

static const struct xdg_surface_listener surface_listener = {
    .configure = surface_configure,
};

/* ========================================================================== */
/*  seat: keyboard and pointer                                                */
/* ========================================================================== */

static int translate_keysym(xkb_keysym_t ks)
{
    if (ks >= XKB_KEY_a && ks <= XKB_KEY_z)
        return KEY_A + (int)(ks - XKB_KEY_a);
    if (ks >= XKB_KEY_A && ks <= XKB_KEY_Z)
        return KEY_A + (int)(ks - XKB_KEY_A);
    if (ks >= XKB_KEY_0 && ks <= XKB_KEY_9)
        return KEY_ZERO + (int)(ks - XKB_KEY_0);
    if (ks >= XKB_KEY_F1 && ks <= XKB_KEY_F12)
        return KEY_F1 + (int)(ks - XKB_KEY_F1);
    if (ks >= XKB_KEY_KP_0 && ks <= XKB_KEY_KP_9)
        return KEY_KP_0 + (int)(ks - XKB_KEY_KP_0);

    switch (ks)
    {
    case XKB_KEY_space:
        return KEY_SPACE;
    case XKB_KEY_apostrophe:
        return KEY_APOSTROPHE;
    case XKB_KEY_comma:
        return KEY_COMMA;
    case XKB_KEY_minus:
        return KEY_MINUS;
    case XKB_KEY_period:
        return KEY_PERIOD;
    case XKB_KEY_slash:
        return KEY_SLASH;
    case XKB_KEY_semicolon:
        return KEY_SEMICOLON;
    case XKB_KEY_equal:
        return KEY_EQUAL;
    case XKB_KEY_bracketleft:
        return KEY_LEFT_BRACKET;
    case XKB_KEY_backslash:
        return KEY_BACKSLASH;
    case XKB_KEY_bracketright:
        return KEY_RIGHT_BRACKET;
    case XKB_KEY_grave:
        return KEY_GRAVE;
    case XKB_KEY_Escape:
        return KEY_ESCAPE;
    case XKB_KEY_Return:
        return KEY_ENTER;
    case XKB_KEY_Tab:
        return KEY_TAB;
    case XKB_KEY_BackSpace:
        return KEY_BACKSPACE;
    case XKB_KEY_Insert:
        return KEY_INSERT;
    case XKB_KEY_Delete:
        return KEY_DELETE;
    case XKB_KEY_Right:
        return KEY_RIGHT;
    case XKB_KEY_Left:
        return KEY_LEFT;
    case XKB_KEY_Down:
        return KEY_DOWN;
    case XKB_KEY_Up:
        return KEY_UP;
    case XKB_KEY_Page_Up:
        return KEY_PAGE_UP;
    case XKB_KEY_Page_Down:
        return KEY_PAGE_DOWN;
    case XKB_KEY_Home:
        return KEY_HOME;
    case XKB_KEY_End:
        return KEY_END;
    case XKB_KEY_Caps_Lock:
        return KEY_CAPS_LOCK;
    case XKB_KEY_Num_Lock:
        return KEY_NUM_LOCK;
    case XKB_KEY_Print:
        return KEY_PRINT_SCREEN;
    case XKB_KEY_Pause:
        return KEY_PAUSE;
    case XKB_KEY_Scroll_Lock:
        return KEY_SCROLL_LOCK;
    case XKB_KEY_Menu:
        return KEY_MENU;
    case XKB_KEY_KP_Insert:
        return KEY_KP_0;
    case XKB_KEY_KP_End:
        return KEY_KP_1;
    case XKB_KEY_KP_Down:
        return KEY_KP_2;
    case XKB_KEY_KP_Page_Down:
        return KEY_KP_3;
    case XKB_KEY_KP_Left:
        return KEY_KP_4;
    case XKB_KEY_KP_Begin:
        return KEY_KP_5;
    case XKB_KEY_KP_Right:
        return KEY_KP_6;
    case XKB_KEY_KP_Home:
        return KEY_KP_7;
    case XKB_KEY_KP_Up:
        return KEY_KP_8;
    case XKB_KEY_KP_Page_Up:
        return KEY_KP_9;
    case XKB_KEY_KP_Delete:
    case XKB_KEY_KP_Decimal:
    case XKB_KEY_KP_Separator:
        return KEY_KP_DECIMAL;
    case XKB_KEY_KP_Divide:
        return KEY_KP_DIVIDE;
    case XKB_KEY_KP_Multiply:
        return KEY_KP_MULTIPLY;
    case XKB_KEY_KP_Subtract:
        return KEY_KP_SUBTRACT;
    case XKB_KEY_KP_Add:
        return KEY_KP_ADD;
    case XKB_KEY_KP_Enter:
        return KEY_KP_ENTER;
    case XKB_KEY_KP_Equal:
        return KEY_KP_EQUAL;
    case XKB_KEY_Shift_L:
        return KEY_LEFT_SHIFT;
    case XKB_KEY_Control_L:
        return KEY_LEFT_CONTROL;
    case XKB_KEY_Alt_L:
        return KEY_LEFT_ALT;
    case XKB_KEY_Super_L:
        return KEY_LEFT_SUPER;
    case XKB_KEY_Shift_R:
        return KEY_RIGHT_SHIFT;
    case XKB_KEY_Control_R:
        return KEY_RIGHT_CONTROL;
    case XKB_KEY_Alt_R:
        return KEY_RIGHT_ALT;
    case XKB_KEY_Super_R:
        return KEY_RIGHT_SUPER;
    default:
        return KEY_NULL;
    }
}

static int current_mods(void)
{
    static const struct
    {
        xkb_mod_index_t *index;
        int mod;
    } map[] = {{&g.mod_shift, KEYMOD_SHIFT}, {&g.mod_ctrl, KEYMOD_CTRL},
               {&g.mod_alt, KEYMOD_ALT},     {&g.mod_super, KEYMOD_SUPER}};
    int mods = 0;
    if (!g.xkb_state)
        return 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++)
    {
        if (xkb_state_mod_index_is_active(g.xkb_state, *map[i].index, XKB_STATE_MODS_EFFECTIVE) > 0)
            mods |= map[i].mod;
    }
    return mods;
}

static void push_text(BackendWindow *b, const char *utf8)
{
    for (const unsigned char *p = (const unsigned char *)utf8; *p;)
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
        {
            Event e = {.type = EVENT_CHAR};
            e.data.codepoint = cp;
            push(b, &e);
        }
        p += len;
    }
}

/* Dead keys and Compose sequences go through the compose state; a key in the
   middle of one gives no text yet. */
static void key_text(BackendWindow *b, xkb_keycode_t kc)
{
    char buf[32];
    if (g.compose)
    {
        xkb_keysym_t sym = xkb_state_key_get_one_sym(g.xkb_state, kc);
        if (sym != XKB_KEY_NoSymbol && xkb_compose_state_feed(g.compose, sym) == XKB_COMPOSE_FEED_ACCEPTED)
        {
            switch (xkb_compose_state_get_status(g.compose))
            {
            case XKB_COMPOSE_COMPOSING:
            case XKB_COMPOSE_CANCELLED:
                return;
            case XKB_COMPOSE_COMPOSED:
                if (xkb_compose_state_get_utf8(g.compose, buf, sizeof buf) > 0)
                    push_text(b, buf);
                xkb_compose_state_reset(g.compose);
                return;
            default:
                break;
            }
        }
    }
    if (xkb_state_key_get_utf8(g.xkb_state, kc, buf, sizeof buf) > 0)
        push_text(b, buf);
}

static void key_event(BackendWindow *b, xkb_keycode_t kc, bool down, bool repeat)
{
    /* The key is the first layout's first level, as XLookupKeysym(0) gives on X11:
       it does not move with Shift or with a switch of layout. */
    const xkb_keysym_t *syms = NULL;
    int n = xkb_keymap_key_get_syms_by_level(g.keymap, kc, 0, 0, &syms);

    Event e = {.type = EVENT_KEY};
    e.data.key.key = n > 0 ? translate_keysym(syms[0]) : KEY_NULL;
    e.data.key.scancode = (int)kc;
    e.data.key.down = down;
    e.data.key.repeat = repeat;
    e.data.key.mods = current_mods();
    push(b, &e);

    if (down)
        key_text(b, kc);
}

/* A held key repeats while its window has the focus, and the pump is what drives
   it, so a late frame gets the repeats it missed (a few at most). */
static void repeat_keys(BackendWindow *b)
{
    if (!g.repeat_key || g.keyboard_focus != b || g.repeat_rate <= 0)
        return;
    double interval = 1000.0 / g.repeat_rate;
    double now = now_ms();
    for (int i = 0; i < 8 && now >= g.repeat_next; i++)
    {
        key_event(b, g.repeat_key, true, true);
        g.repeat_next += interval;
    }
    if (now >= g.repeat_next)
        g.repeat_next = now + interval;
}

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data;
    (void)kb;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !g.xkb)
    {
        close(fd);
        return;
    }
    char *text = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (text == MAP_FAILED)
        return;
    struct xkb_keymap *keymap =
        xkb_keymap_new_from_string(g.xkb, text, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(text, size);
    if (!keymap)
        return;
    struct xkb_state *state = xkb_state_new(keymap);
    if (!state)
    {
        xkb_keymap_unref(keymap);
        return;
    }
    xkb_state_unref(g.xkb_state);
    xkb_keymap_unref(g.keymap);
    g.keymap = keymap;
    g.xkb_state = state;
    g.mod_shift = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_SHIFT);
    g.mod_ctrl = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CTRL);
    g.mod_alt = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_ALT);
    g.mod_super = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_LOGO);
    g.mod_caps = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_CAPS);
    g.mod_num = xkb_keymap_mod_get_index(keymap, XKB_MOD_NAME_NUM);
    g.repeat_key = 0;
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface,
                           struct wl_array *keys)
{
    (void)data;
    (void)kb;
    (void)keys;
    g.serial = serial;
    g.keyboard_focus = window_for_surface(surface);
}

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)kb;
    (void)surface;
    g.serial = serial;
    g.keyboard_focus = NULL;
    g.repeat_key = 0;
    if (g.compose)
        xkb_compose_state_reset(g.compose);
}

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t time, uint32_t key,
                         uint32_t state)
{
    (void)data;
    (void)kb;
    (void)time;
    g.serial = serial;
    BackendWindow *b = g.keyboard_focus;
    if (!b || !g.xkb_state)
        return;

    xkb_keycode_t kc = key + 8; /* the evdev code plus 8, the number X11 reports as well */
    bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    key_event(b, kc, down, false);
    if (down)
    {
        if (g.repeat_rate > 0 && xkb_keymap_key_repeats(g.keymap, kc))
        {
            g.repeat_key = kc;
            g.repeat_next = now_ms() + g.repeat_delay;
        }
    }
    else if (g.repeat_key == kc)
    {
        g.repeat_key = 0;
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial, uint32_t depressed,
                               uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data;
    (void)kb;
    (void)serial;
    if (g.xkb_state)
        xkb_state_update_mask(g.xkb_state, depressed, latched, locked, 0, 0, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{
    (void)data;
    (void)kb;
    g.repeat_rate = rate;
    g.repeat_delay = delay;
}

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void pointer_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data;
    (void)p;
    g.serial = serial;
    g.enter_serial = serial;
    DecorPart *d = decor_for_surface(surface);
    if (d)
    {
        g.pointer_focus = NULL;
        decor_enter(d, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
        return;
    }
    BackendWindow *b = window_for_surface(surface);
    g.pointer_focus = b;
    if (!b)
        return;
    g.pointer_x = wl_fixed_to_int(sx);
    g.pointer_y = wl_fixed_to_int(sy);
    b->hovered = true;
    apply_cursor(b);

    Event in = {.type = EVENT_WINDOW_ENTER};
    in.data.enter.entered = true;
    push(b, &in);
    Event mv = {.type = EVENT_MOUSE_MOVE};
    mv.data.mouse.x = g.pointer_x;
    mv.data.mouse.y = g.pointer_y;
    push(b, &mv);
}

static void pointer_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *surface)
{
    (void)data;
    (void)p;
    (void)surface;
    g.serial = serial;
    if (g.pointer_decor)
    {
        decor_leave(g.pointer_decor);
        return;
    }
    BackendWindow *b = g.pointer_focus;
    g.pointer_focus = NULL;
    if (!b)
        return;
    b->hovered = false;
    Event e = {.type = EVENT_WINDOW_ENTER};
    e.data.enter.entered = false;
    push(b, &e);
}

static void pointer_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)data;
    (void)p;
    (void)time;
    if (g.pointer_decor)
    {
        decor_motion(g.pointer_decor, wl_fixed_to_int(sx), wl_fixed_to_int(sy));
        return;
    }
    BackendWindow *b = g.pointer_focus;
    if (!b || b->locked)
        return;
    g.pointer_x = wl_fixed_to_int(sx);
    g.pointer_y = wl_fixed_to_int(sy);
    Event e = {.type = EVENT_MOUSE_MOVE};
    e.data.mouse.x = g.pointer_x;
    e.data.mouse.y = g.pointer_y;
    push(b, &e);

    /* The pointer over a resize edge of the application's own shows its cursor. */
    if (b->hit_cb && b->mouse_mode == MOUSE_MODE_NORMAL)
    {
        int want = hit_cursor_for(b->hit_cb(b->hit_w, g.pointer_x, g.pointer_y, b->hit_user));
        if (want != b->hit_cursor)
        {
            b->hit_cursor = want;
            apply_cursor(b);
        }
    }
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)data;
    (void)p;
    (void)time;
    g.serial = serial;
    if (g.pointer_decor)
    {
        decor_button(g.pointer_decor, serial, button, state == WL_POINTER_BUTTON_STATE_PRESSED);
        return;
    }
    BackendWindow *b = g.pointer_focus;
    if (!b)
        return;

    /* A press on a title bar or edge of the application's own is the window
       manager's, and the release that goes with it is nobody's. */
    if (b->hit_cb && button == BTN_LEFT)
    {
        if (state == WL_POINTER_BUTTON_STATE_PRESSED)
        {
            HitTestResult r = b->hit_cb(b->hit_w, g.pointer_x, g.pointer_y, b->hit_user);
            if (r != HIT_NORMAL)
            {
                b->hit_press = true;
                start_move_resize(b, r, serial);
                return;
            }
        }
        else if (b->hit_press)
        {
            b->hit_press = false;
            return;
        }
    }

    int mb;
    switch (button)
    {
    case BTN_LEFT:
        mb = MOUSE_LEFT;
        break;
    case BTN_RIGHT:
        mb = MOUSE_RIGHT;
        break;
    case BTN_MIDDLE:
        mb = MOUSE_MIDDLE;
        break;
    case BTN_SIDE:
        mb = MOUSE_X1;
        break;
    case BTN_EXTRA:
        mb = MOUSE_X2;
        break;
    default:
        return;
    }
    Event e = {.type = EVENT_MOUSE_BUTTON};
    e.data.mouse.button = mb;
    e.data.mouse.down = state == WL_POINTER_BUTTON_STATE_PRESSED;
    e.data.mouse.x = g.pointer_x;
    e.data.mouse.y = g.pointer_y;
    push(b, &e);
}

/* One wheel notch is 1.0, as on X11. A notch arrives as value120 when the
   compositor has it, else as a continuous axis value of about 10 per notch, which
   is also what a touchpad gives. The pointer frame closes a group of them, and
   wheel up is a negative vertical value on the wire. */
static void flush_scroll(void)
{
    BackendWindow *b = g.pointer_focus;
    float x, y;
    if (g.v120_x || g.v120_y)
    {
        x = (float)g.v120_x / 120.0f;
        y = (float)g.v120_y / 120.0f;
    }
    else
    {
        x = (float)(g.axis_x / 10.0);
        y = (float)(g.axis_y / 10.0);
    }
    g.axis_x = g.axis_y = 0.0;
    g.v120_x = g.v120_y = 0;
    if (!b || (x == 0.0f && y == 0.0f))
        return;
    Event e = {.type = EVENT_MOUSE_WHEEL};
    e.data.wheel.x = x;
    e.data.wheel.y = -y;
    push(b, &e);
}

static void pointer_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    (void)data;
    (void)time;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        g.axis_y += wl_fixed_to_double(value);
    else
        g.axis_x += wl_fixed_to_double(value);
    if (wl_pointer_get_version(p) < WL_POINTER_FRAME_SINCE_VERSION)
        flush_scroll();
}

static void pointer_frame(void *data, struct wl_pointer *p)
{
    (void)data;
    (void)p;
    flush_scroll();
}

static void pointer_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
    (void)data;
    (void)p;
    (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
    (void)data;
    (void)p;
    (void)time;
    (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
    (void)data;
    /* Version 8 sends value120 as well, and that is the one counted. */
    if (wl_pointer_get_version(p) >= 8)
        return;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        g.v120_y += discrete * 120;
    else
        g.v120_x += discrete * 120;
}

static void pointer_axis_value120(void *data, struct wl_pointer *p, uint32_t axis, int32_t value120)
{
    (void)data;
    (void)p;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
        g.v120_y += value120;
    else
        g.v120_x += value120;
}

#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
static void pointer_axis_relative_direction(void *data, struct wl_pointer *p, uint32_t axis, uint32_t direction)
{
    (void)data;
    (void)p;
    (void)axis;
    (void)direction;
}
#endif

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
    .axis_value120 = pointer_axis_value120,
#ifdef WL_POINTER_AXIS_RELATIVE_DIRECTION_SINCE_VERSION
    .axis_relative_direction = pointer_axis_relative_direction,
#endif
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps)
{
    (void)data;
    bool has_pointer = caps & WL_SEAT_CAPABILITY_POINTER;
    bool has_keyboard = caps & WL_SEAT_CAPABILITY_KEYBOARD;

    if (has_pointer && !g.pointer)
    {
        g.pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(g.pointer, &pointer_listener, NULL);
    }
    else if (!has_pointer && g.pointer)
    {
        wl_pointer_release(g.pointer);
        g.pointer = NULL;
        g.pointer_focus = NULL;
    }

    if (has_keyboard && !g.keyboard)
    {
        g.keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(g.keyboard, &keyboard_listener, NULL);
    }
    else if (!has_keyboard && g.keyboard)
    {
        wl_keyboard_release(g.keyboard);
        g.keyboard = NULL;
        g.keyboard_focus = NULL;
        g.repeat_key = 0;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name)
{
    (void)data;
    (void)seat;
    (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
};

/* ========================================================================== */
/*  decorations                                                               */
/* ========================================================================== */

/* GNOME draws no frame for a client: a title bar and a border are four wl_shm
   surfaces around the content, subsurfaces of it, and the window geometry says
   the frame is part of the window. Where the compositor draws the frame itself
   (xdg-decoration answers server-side) none of this exists. The border is mostly
   empty: it is there to be grabbed. */
#define TITLE_H 32
#define BORDER 6
#define BUTTON_W 40

enum
{
    TB_NONE = -1,
    TB_MINIMIZE,
    TB_MAXIMIZE,
    TB_CLOSE
};

static const uint32_t bar_color[2] = {0xFF232323, 0xFF2B2B2B}; /* unfocused, focused */
static const uint32_t line_color = 0xFF111111;
static const uint32_t icon_color[2] = {0xFF8A8A8A, 0xFFE6E6E6};
static const uint32_t hover_color = 0xFF3D3D3D;
static const uint32_t close_hover_color = 0xFFC42B1C;

static void buffer_release(void *data, struct wl_buffer *buffer)
{
    DecorPart *p = data;
    if (p && p->buffer == buffer)
        p->buffer = NULL;
    wl_buffer_destroy(buffer);
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

/* A buffer that holds its own copy of the pixels: the pool and the file are gone
   as soon as it exists. With a part, it is destroyed when the compositor lets go
   of it. */
static struct wl_buffer *shm_buffer(const uint32_t *px, int w, int h, DecorPart *part)
{
    size_t size = (size_t)w * (size_t)h * 4;
    int fd = memfd_create("zen-buffer", MFD_CLOEXEC);
    if (fd < 0)
        return NULL;
    if (write(fd, px, size) != (ssize_t)size)
    {
        close(fd);
        return NULL;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(g.shm, fd, (int32_t)size);
    close(fd);
    if (!pool)
        return NULL;
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    if (buffer && part)
        wl_buffer_add_listener(buffer, &buffer_listener, part);
    return buffer;
}

static void paint_rect(uint32_t *px, int stride, int x, int y, int w, int h, uint32_t c)
{
    for (int j = y; j < y + h; j++)
    {
        for (int i = x; i < x + w; i++)
            px[j * stride + i] = c;
    }
}

static void paint_outline(uint32_t *px, int stride, int x, int y, int w, int h, uint32_t c)
{
    paint_rect(px, stride, x, y, w, 1, c);
    paint_rect(px, stride, x, y + h - 1, w, 1, c);
    paint_rect(px, stride, x, y, 1, h, c);
    paint_rect(px, stride, x + w - 1, y, 1, h, c);
}

/* The title is a 5x7 bitmap font of the printable ASCII range at twice its size:
   the platform has no font of its own (the TrueType one is zen_ui's, above this
   layer). The Latin-1 letters lose their mark and the rest outside ASCII is drawn
   as '?'. One byte a column, the top row the lowest bit. */
#define TITLE_SCALE 2
#define TITLE_PAD 12

static const uint8_t font5x7[95][5] = {
    {0x00, 0x00, 0x00, 0x00, 0x00}, {0x00, 0x00, 0x5F, 0x00, 0x00}, {0x00, 0x07, 0x00, 0x07, 0x00},
    {0x14, 0x7F, 0x14, 0x7F, 0x14}, {0x24, 0x2A, 0x7F, 0x2A, 0x12}, {0x23, 0x13, 0x08, 0x64, 0x62},
    {0x36, 0x49, 0x55, 0x22, 0x50}, {0x00, 0x05, 0x03, 0x00, 0x00}, {0x00, 0x1C, 0x22, 0x41, 0x00},
    {0x00, 0x41, 0x22, 0x1C, 0x00}, {0x14, 0x08, 0x3E, 0x08, 0x14}, {0x08, 0x08, 0x3E, 0x08, 0x08},
    {0x00, 0x50, 0x30, 0x00, 0x00}, {0x08, 0x08, 0x08, 0x08, 0x08}, {0x00, 0x60, 0x60, 0x00, 0x00},
    {0x20, 0x10, 0x08, 0x04, 0x02}, {0x3E, 0x51, 0x49, 0x45, 0x3E}, {0x00, 0x42, 0x7F, 0x40, 0x00},
    {0x42, 0x61, 0x51, 0x49, 0x46}, {0x21, 0x41, 0x45, 0x4B, 0x31}, {0x18, 0x14, 0x12, 0x7F, 0x10},
    {0x27, 0x45, 0x45, 0x45, 0x39}, {0x3C, 0x4A, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03},
    {0x36, 0x49, 0x49, 0x49, 0x36}, {0x06, 0x49, 0x49, 0x29, 0x1E}, {0x00, 0x36, 0x36, 0x00, 0x00},
    {0x00, 0x56, 0x36, 0x00, 0x00}, {0x08, 0x14, 0x22, 0x41, 0x00}, {0x14, 0x14, 0x14, 0x14, 0x14},
    {0x00, 0x41, 0x22, 0x14, 0x08}, {0x02, 0x01, 0x51, 0x09, 0x06}, {0x32, 0x49, 0x79, 0x41, 0x3E},
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36}, {0x3E, 0x41, 0x41, 0x41, 0x22},
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F}, {0x00, 0x41, 0x7F, 0x41, 0x00},
    {0x20, 0x40, 0x41, 0x3F, 0x01}, {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F}, {0x3E, 0x41, 0x41, 0x41, 0x3E},
    {0x7F, 0x09, 0x09, 0x09, 0x06}, {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
    {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01}, {0x3F, 0x40, 0x40, 0x40, 0x3F},
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
    {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}, {0x00, 0x7F, 0x41, 0x41, 0x00},
    {0x02, 0x04, 0x08, 0x10, 0x20}, {0x00, 0x41, 0x41, 0x7F, 0x00}, {0x04, 0x02, 0x01, 0x02, 0x04},
    {0x40, 0x40, 0x40, 0x40, 0x40}, {0x00, 0x01, 0x02, 0x04, 0x00}, {0x20, 0x54, 0x54, 0x54, 0x78},
    {0x7F, 0x48, 0x44, 0x44, 0x38}, {0x38, 0x44, 0x44, 0x44, 0x20}, {0x38, 0x44, 0x44, 0x48, 0x7F},
    {0x38, 0x54, 0x54, 0x54, 0x18}, {0x08, 0x7E, 0x09, 0x01, 0x02}, {0x0C, 0x52, 0x52, 0x52, 0x3E},
    {0x7F, 0x08, 0x04, 0x04, 0x78}, {0x00, 0x44, 0x7D, 0x40, 0x00}, {0x20, 0x40, 0x44, 0x3D, 0x00},
    {0x7F, 0x10, 0x28, 0x44, 0x00}, {0x00, 0x41, 0x7F, 0x40, 0x00}, {0x7C, 0x04, 0x18, 0x04, 0x78},
    {0x7C, 0x08, 0x04, 0x04, 0x78}, {0x38, 0x44, 0x44, 0x44, 0x38}, {0x7C, 0x14, 0x14, 0x14, 0x08},
    {0x08, 0x14, 0x14, 0x18, 0x7C}, {0x7C, 0x08, 0x04, 0x04, 0x08}, {0x48, 0x54, 0x54, 0x54, 0x20},
    {0x04, 0x3F, 0x44, 0x40, 0x20}, {0x3C, 0x40, 0x40, 0x20, 0x7C}, {0x1C, 0x20, 0x40, 0x20, 0x1C},
    {0x3C, 0x40, 0x30, 0x40, 0x3C}, {0x44, 0x28, 0x10, 0x28, 0x44}, {0x0C, 0x50, 0x50, 0x50, 0x3C},
    {0x44, 0x64, 0x54, 0x4C, 0x44}, {0x00, 0x08, 0x36, 0x41, 0x00}, {0x00, 0x00, 0x7F, 0x00, 0x00},
    {0x00, 0x41, 0x36, 0x08, 0x00}, {0x10, 0x08, 0x08, 0x10, 0x08},
};

static void paint_glyph(uint32_t *px, int stride, int x, int y, uint32_t cp, uint32_t c)
{
    /* U+00C0..U+00FF as the letter without its mark: a title in Portuguese stays readable */
    static const char latin1_base[] = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYTs"
                                      "aaaaaaaceeeeiiiidnooooo/ouuuuyty";
    if (cp >= 0xC0 && cp <= 0xFF)
        cp = (unsigned char)latin1_base[cp - 0xC0];
    const uint8_t *col = font5x7[(cp >= 0x20 && cp <= 0x7E ? cp : '?') - 0x20];
    for (int i = 0; i < 5; i++)
    {
        for (int j = 0; j < 7; j++)
        {
            if (col[i] & (1 << j))
                paint_rect(px, stride, x + i * TITLE_SCALE, y + j * TITLE_SCALE, TITLE_SCALE, TITLE_SCALE, c);
        }
    }
}

static uint32_t next_codepoint(const char **s)
{
    const unsigned char *p = (const unsigned char *)*s;
    uint32_t c = *p++;
    int more = 0;
    if (c >= 0xF0)
    {
        c &= 0x07;
        more = 3;
    }
    else if (c >= 0xE0)
    {
        c &= 0x0F;
        more = 2;
    }
    else if (c >= 0xC0)
    {
        c &= 0x1F;
        more = 1;
    }
    else if (c >= 0x80)
    {
        c = '?'; /* a continuation byte with nothing before it */
    }
    for (; more > 0 && (*p & 0xC0) == 0x80; more--)
        c = (c << 6) | (*p++ & 0x3F);
    *s = (const char *)p;
    return c;
}

/* The title, from x and centred on mid, in avail pixels: cut with "..." when it
   does not fit. */
static void paint_title(uint32_t *px, int stride, int x, int mid, int avail, const char *title, uint32_t c)
{
    const int cell = 6 * TITLE_SCALE;
    int fit = avail / cell;
    if (fit <= 0)
        return;
    int count = 0;
    for (const char *s = title; *s;)
    {
        next_codepoint(&s);
        count++;
    }
    bool cut = count > fit;
    int shown = cut ? (fit > 3 ? fit - 3 : 0) : count;
    int y = mid - 7 * TITLE_SCALE / 2;

    const char *s = title;
    for (int i = 0; i < shown; i++)
        paint_glyph(px, stride, x + i * cell, y, next_codepoint(&s), c);
    if (cut && fit > 3)
    {
        for (int i = 0; i < 3; i++)
            paint_glyph(px, stride, x + (shown + i) * cell, y, '.', c);
    }
}

static void paint_icon(uint32_t *px, int stride, int cx, int cy, int which, bool maximized, uint32_t c)
{
    switch (which)
    {
    case TB_MINIMIZE:
        paint_rect(px, stride, cx - 5, cy + 2, 10, 1, c);
        break;
    case TB_MAXIMIZE:
        if (maximized)
        {
            paint_outline(px, stride, cx - 5, cy - 2, 8, 8, c);
            paint_outline(px, stride, cx - 3, cy - 4, 8, 8, c);
        }
        else
        {
            paint_outline(px, stride, cx - 5, cy - 5, 10, 10, c);
        }
        break;
    case TB_CLOSE:
        for (int i = 0; i < 10; i++)
        {
            px[(cy - 5 + i) * stride + cx - 5 + i] = c;
            px[(cy - 5 + i) * stride + cx + 4 - i] = c;
        }
        break;
    }
}

static void decor_extents(const BackendWindow *b, bool maximized, bool fullscreen, int *l, int *t, int *r, int *bt)
{
    int side = 0, top = 0;
    if (b->csd && !fullscreen)
    {
        top = TITLE_H;
        side = maximized ? 0 : BORDER;
    }
    *l = *r = *bt = side;
    *t = top;
}

/* The frame is drawn here when it is asked for, the compositor does not draw it,
   and there are subsurfaces and shared memory to draw it with. */
static bool wants_own_frame(const BackendWindow *b)
{
    return b->decorated && !b->server_side && g.subcompositor && g.shm;
}

static bool decor_resizable(const BackendWindow *b)
{
    return !(b->min_w > 0 && b->min_w == b->max_w && b->min_h == b->max_h);
}

/* What the pointer is over, in the coordinates of one of the four surfaces: an
   edge or corner to resize by, HIT_DRAG on the title bar, or HIT_NORMAL with the
   button it is on. */
static HitTestResult decor_hit(BackendWindow *b, int part, int x, int y, int *button)
{
    int l, t, r, bt;
    decor_extents(b, b->maximized, b->fullscreen, &l, &t, &r, &bt);
    int width = b->width + l + r;
    int corner = 2 * BORDER;
    bool resize = decor_resizable(b) && !b->maximized && !b->fullscreen;
    *button = TB_NONE;

    switch (part)
    {
    case DECOR_TOP:
        if (resize)
        {
            if (y < BORDER)
                return x < corner ? HIT_RESIZE_TOPLEFT : x >= width - corner ? HIT_RESIZE_TOPRIGHT : HIT_RESIZE_TOP;
            if (x < BORDER)
                return y < corner ? HIT_RESIZE_TOPLEFT : HIT_RESIZE_LEFT;
            if (x >= width - BORDER)
                return y < corner ? HIT_RESIZE_TOPRIGHT : HIT_RESIZE_RIGHT;
        }
        for (int i = 0; i < 3; i++)
        {
            int x0 = width - r - (3 - i) * BUTTON_W;
            if (x >= x0 && x < x0 + BUTTON_W)
            {
                *button = i;
                return HIT_NORMAL;
            }
        }
        return HIT_DRAG;
    case DECOR_LEFT:
        if (!resize)
            return HIT_NORMAL;
        return y >= b->height - corner ? HIT_RESIZE_BOTTOMLEFT : HIT_RESIZE_LEFT;
    case DECOR_RIGHT:
        if (!resize)
            return HIT_NORMAL;
        return y >= b->height - corner ? HIT_RESIZE_BOTTOMRIGHT : HIT_RESIZE_RIGHT;
    case DECOR_BOTTOM:
        if (!resize)
            return HIT_NORMAL;
        return x < corner ? HIT_RESIZE_BOTTOMLEFT : x >= width - corner ? HIT_RESIZE_BOTTOMRIGHT : HIT_RESIZE_BOTTOM;
    }
    return HIT_NORMAL;
}

static int hit_cursor_for(HitTestResult r)
{
    switch (r)
    {
    case HIT_RESIZE_TOPLEFT:
    case HIT_RESIZE_BOTTOMRIGHT:
        return CURSOR_RESIZE_NWSE;
    case HIT_RESIZE_TOPRIGHT:
    case HIT_RESIZE_BOTTOMLEFT:
        return CURSOR_RESIZE_NESW;
    case HIT_RESIZE_TOP:
    case HIT_RESIZE_BOTTOM:
        return CURSOR_RESIZE_NS;
    case HIT_RESIZE_LEFT:
    case HIT_RESIZE_RIGHT:
        return CURSOR_RESIZE_EW;
    default:
        return -1;
    }
}

static uint32_t resize_edge(HitTestResult r)
{
    switch (r)
    {
    case HIT_RESIZE_TOPLEFT:
        return XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
    case HIT_RESIZE_TOP:
        return XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    case HIT_RESIZE_TOPRIGHT:
        return XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
    case HIT_RESIZE_RIGHT:
        return XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
    case HIT_RESIZE_BOTTOMRIGHT:
        return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    case HIT_RESIZE_BOTTOM:
        return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    case HIT_RESIZE_BOTTOMLEFT:
        return XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    default:
        return XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
    }
}

/* The window manager takes over the pointer from here: a move or a resize. */
static void start_move_resize(BackendWindow *b, HitTestResult r, uint32_t serial)
{
    if (!b->toplevel || !g.seat)
        return;
    if (r == HIT_DRAG)
        xdg_toplevel_move(b->toplevel, g.seat, serial);
    else
        xdg_toplevel_resize(b->toplevel, g.seat, serial, resize_edge(r));
}

static void paint_part(BackendWindow *b, int part, uint32_t *px, int w, int h, int l, int r)
{
    bool f = b->focused;
    memset(px, 0, (size_t)w * (size_t)h * 4);
    switch (part)
    {
    case DECOR_TOP:
    {
        int x0 = l, bar_w = w - l - r;
        paint_rect(px, w, x0, 0, bar_w, h, bar_color[f]);
        paint_rect(px, w, x0, h - 1, bar_w, 1, line_color);
        paint_rect(px, w, x0, 0, bar_w, 1, line_color);
        if (l)
        {
            paint_rect(px, w, l - 1, 0, 1, h, line_color);
            paint_rect(px, w, w - r, 0, 1, h, line_color);
        }
        int title_x = x0 + TITLE_PAD;
        int title_w = w - r - 3 * BUTTON_W - TITLE_PAD / 2 - title_x;
        if (b->title && title_w > 0)
            paint_title(px, w, title_x, h / 2, title_w, b->title, icon_color[f]);
        for (int i = 0; i < 3; i++)
        {
            int bx = w - r - (3 - i) * BUTTON_W;
            if (bx < l || (i == TB_MAXIMIZE && !decor_resizable(b)))
                continue; /* does not fit, or not for a window of one size */
            if (i == b->hover_button)
                paint_rect(px, w, bx, 1, BUTTON_W, h - 2, i == TB_CLOSE ? close_hover_color : hover_color);
            paint_icon(px, w, bx + BUTTON_W / 2, h / 2, i, b->maximized, icon_color[f]);
        }
        break;
    }
    case DECOR_LEFT:
        paint_rect(px, w, w - 1, 0, 1, h, line_color);
        break;
    case DECOR_RIGHT:
        paint_rect(px, w, 0, 0, 1, h, line_color);
        break;
    case DECOR_BOTTOM:
        paint_rect(px, w, l - 1 < 0 ? 0 : l - 1, 0, w - (l ? 2 * (l - 1) : 0), 1, line_color);
        break;
    }
}

/* Say the frame to the compositor, in the commit the swap is about to make: the
   window geometry, the four surfaces and their pixels all arrive together with
   the content, so a resize never shows one at the old size. */
static void decor_flush(BackendWindow *b)
{
    if (!b->decor_dirty || !b->xdg_surface || !g.shm || !g.subcompositor)
        return;
    b->decor_dirty = false;

    int l, t, r, bt;
    decor_extents(b, b->maximized, b->fullscreen, &l, &t, &r, &bt);
    int cw = b->width, ch = b->height;
    int ext[4] = {l, t, r, bt};
    if (memcmp(ext, b->decor_ext, sizeof ext) != 0)
    {
        memcpy(b->decor_ext, ext, sizeof ext);
        apply_limits(b); /* the limits are of the whole window */
    }
    xdg_surface_set_window_geometry(b->xdg_surface, -l, -t, cw + l + r, ch + t + bt);

    const int rect[DECOR_COUNT][4] = {
        {-l, -t, cw + l + r, t}, /* x, y, w, h: top, left, right, bottom */
        {-l, 0, l, ch},
        {cw, 0, r, ch},
        {-l, ch, cw + l + r, bt},
    };
    for (int i = 0; i < DECOR_COUNT; i++)
    {
        DecorPart *p = &b->decor[i];
        int w = rect[i][2], h = rect[i][3];
        if (w <= 0 || h <= 0)
        {
            if (p->surface)
            {
                wl_surface_attach(p->surface, NULL, 0, 0);
                wl_surface_commit(p->surface);
            }
            continue;
        }
        if (!p->surface)
        {
            p->owner = b;
            p->part = i;
            p->surface = wl_compositor_create_surface(g.compositor);
            p->sub = wl_subcompositor_get_subsurface(g.subcompositor, p->surface, b->surface);
        }
        wl_subsurface_set_position(p->sub, rect[i][0], rect[i][1]);

        uint32_t *px = malloc((size_t)w * (size_t)h * 4);
        if (!px)
            continue;
        paint_part(b, i, px, w, h, l, r);
        struct wl_buffer *buffer = shm_buffer(px, w, h, p);
        free(px);
        if (!buffer)
            continue;
        if (p->buffer)
            wl_buffer_set_user_data(p->buffer, NULL); /* destroyed when it is released, whoever is left */
        p->buffer = buffer;
        wl_surface_attach(p->surface, buffer, 0, 0);
        wl_surface_damage(p->surface, 0, 0, w, h);
        wl_surface_commit(p->surface);
    }
}

/* A change that does not wait for the next swap: a button lighting up. */
static void decor_flush_now(BackendWindow *b)
{
    b->decor_dirty = true;
    decor_flush(b);
    wl_surface_commit(b->surface);
}

static DecorPart *decor_for_surface(struct wl_surface *surface)
{
    /* An enter can name a surface that is already gone; the parts that have none
       yet must not answer to it. */
    if (!surface)
        return NULL;
    for (BackendWindow *w = g.windows; w; w = w->next)
    {
        for (int i = 0; i < DECOR_COUNT; i++)
        {
            if (w->decor[i].surface == surface)
                return &w->decor[i];
        }
    }
    return NULL;
}

static void decor_motion(DecorPart *d, int x, int y)
{
    BackendWindow *b = d->owner;
    int button;
    HitTestResult e = decor_hit(b, d->part, x, y, &button);
    g.decor_x = x;
    g.decor_y = y;

    int shape = hit_cursor_for(e);
    if (shape < 0)
        shape = CURSOR_DEFAULT;
    if (shape != g.decor_shape)
    {
        g.decor_shape = shape;
        set_pointer_cursor(shape, NULL, false);
    }
    if (button != b->hover_button)
    {
        b->hover_button = button;
        decor_flush_now(b);
    }
}

static void decor_enter(DecorPart *d, int x, int y)
{
    g.pointer_decor = d;
    g.decor_shape = -1;
    decor_motion(d, x, y);
}

static void decor_leave(DecorPart *d)
{
    BackendWindow *b = d->owner;
    g.pointer_decor = NULL;
    if (b->hover_button != TB_NONE)
    {
        b->hover_button = TB_NONE;
        decor_flush_now(b);
    }
}

static void decor_button(DecorPart *d, uint32_t serial, uint32_t button, bool down)
{
    BackendWindow *b = d->owner;
    int pressed;
    HitTestResult e = decor_hit(b, d->part, g.decor_x, g.decor_y, &pressed);

    if (button == BTN_LEFT)
    {
        if (down)
        {
            if (pressed != TB_NONE)
            {
                b->press_button = pressed;
            }
            else if (e == HIT_DRAG)
            {
                double now = now_ms();
                if (now - b->title_click_ms < 400.0 && decor_resizable(b))
                {
                    b->title_click_ms = 0.0;
                    if (b->maximized)
                        backend_restore(b);
                    else
                        backend_maximize(b);
                }
                else
                {
                    b->title_click_ms = now;
                    start_move_resize(b, HIT_DRAG, serial);
                }
            }
            else if (e != HIT_NORMAL)
            {
                start_move_resize(b, e, serial);
            }
        }
        else if (b->press_button != TB_NONE)
        {
            int which = b->press_button;
            b->press_button = TB_NONE;
            if (which == pressed)
            {
                if (which == TB_CLOSE)
                {
                    Event close = {.type = EVENT_WINDOW_CLOSE};
                    push(b, &close);
                }
                else if (which == TB_MAXIMIZE && decor_resizable(b))
                {
                    if (b->maximized)
                        backend_restore(b);
                    else
                        backend_maximize(b);
                }
                else if (which == TB_MINIMIZE)
                {
                    backend_minimize(b);
                }
            }
        }
    }
    else if (button == BTN_RIGHT && down && e == HIT_DRAG && b->toplevel && g.seat)
    {
        int l, t, r, bt;
        decor_extents(b, b->maximized, b->fullscreen, &l, &t, &r, &bt);
        xdg_toplevel_show_window_menu(b->toplevel, g.seat, serial, g.decor_x - l, g.decor_y - t);
    }
}

/* ========================================================================== */
/*  cursor and pointer lock                                                   */
/* ========================================================================== */

struct PlatformCursor
{
    struct wl_buffer *buffer;
    int w, h, hot_x, hot_y;
};

/* The names a shape goes by in the themes, the traditional X ones first. */
static const char *const cursor_names[CURSOR_COUNT][6] = {
    [CURSOR_DEFAULT] = {"left_ptr", "default", "arrow"},
    [CURSOR_ARROW] = {"left_ptr", "default", "arrow"},
    [CURSOR_IBEAM] = {"xterm", "text", "ibeam"},
    [CURSOR_CROSSHAIR] = {"crosshair", "cross"},
    [CURSOR_HAND] = {"hand2", "pointer", "hand1", "pointing_hand"},
    [CURSOR_RESIZE_EW] = {"sb_h_double_arrow", "ew-resize", "h_double_arrow", "col-resize", "size_hor"},
    [CURSOR_RESIZE_NS] = {"sb_v_double_arrow", "ns-resize", "v_double_arrow", "row-resize", "size_ver"},
    [CURSOR_NOT_ALLOWED] = {"crossed_circle", "not-allowed", "forbidden", "circle", "X_cursor"},
    [CURSOR_RESIZE_NWSE] = {"nwse-resize", "size_fdiag", "bd_double_arrow", "top_left_corner"},
    [CURSOR_RESIZE_NESW] = {"nesw-resize", "size_bdiag", "fd_double_arrow", "top_right_corner"},
    [CURSOR_RESIZE_ALL] = {"fleur", "all-scroll", "move", "size_all"},
};

static struct wl_cursor *theme_cursor(int shape)
{
    if (!g.theme)
    {
        const char *size = getenv("XCURSOR_SIZE");
        g.theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size && atoi(size) > 0 ? atoi(size) : 24, g.shm);
        if (!g.theme)
            return NULL;
    }
    for (int i = 0; i < 6 && cursor_names[shape][i]; i++)
    {
        struct wl_cursor *c = wl_cursor_theme_get_cursor(g.theme, cursor_names[shape][i]);
        if (c && c->image_count > 0)
            return c;
    }
    return NULL;
}

/* What the pointer shows over this window: nothing, a cursor from an image, or
   the shape from the theme. It has to be said again at every enter, quoting the
   serial of that enter. */
static void set_pointer_cursor(int shape, struct PlatformCursor *image, bool hidden)
{
    if (!g.pointer)
        return;
    if (hidden)
    {
        wl_pointer_set_cursor(g.pointer, g.enter_serial, NULL, 0, 0);
        return;
    }

    struct wl_buffer *buffer = NULL;
    int w = 0, h = 0, hot_x = 0, hot_y = 0;
    if (image)
    {
        buffer = image->buffer;
        w = image->w;
        h = image->h;
        hot_x = image->hot_x;
        hot_y = image->hot_y;
    }
    else if (g.shm)
    {
        struct wl_cursor *c = theme_cursor(shape);
        if (c)
        {
            struct wl_cursor_image *img = c->images[0];
            buffer = wl_cursor_image_get_buffer(img);
            w = (int)img->width;
            h = (int)img->height;
            hot_x = (int)img->hotspot_x;
            hot_y = (int)img->hotspot_y;
        }
    }
    if (!buffer)
        return;

    if (!g.cursor_surface)
        g.cursor_surface = wl_compositor_create_surface(g.compositor);
    wl_surface_attach(g.cursor_surface, buffer, 0, 0);
    wl_surface_damage(g.cursor_surface, 0, 0, w, h);
    wl_surface_commit(g.cursor_surface);
    wl_pointer_set_cursor(g.pointer, g.enter_serial, g.cursor_surface, hot_x, hot_y);
}

/* What the pointer shows over this window: nothing, a cursor from an image, a
   resize cursor the hit test wants, or the shape from the theme. It has to be
   said again at every enter, quoting the serial of that enter. */
static void apply_cursor(BackendWindow *b)
{
    if (!g.pointer || g.pointer_focus != b)
        return;
    bool normal = b->mouse_mode == MOUSE_MODE_NORMAL;
    if (normal && b->hit_cursor >= 0)
        set_pointer_cursor(b->hit_cursor, NULL, false);
    else
        set_pointer_cursor(b->cursor_idx, normal ? b->cursor_image : NULL, !normal);
}

#ifdef ZEN_WL_POINTER_LOCK
static void locked_pointer_locked(void *data, struct zwp_locked_pointer_v1 *lp)
{
    BackendWindow *b = data;
    (void)lp;
    b->locked = true;
    b->lock_x = b->virt_x = g.pointer_x;
    b->lock_y = b->virt_y = g.pointer_y;
}

static void locked_pointer_unlocked(void *data, struct zwp_locked_pointer_v1 *lp)
{
    BackendWindow *b = data;
    (void)lp;
    b->locked = false;
}

static const struct zwp_locked_pointer_v1_listener locked_pointer_listener = {
    .locked = locked_pointer_locked,
    .unlocked = locked_pointer_unlocked,
};

/* The pointer does not move while it is locked, so the motion the core sees is a
   position that moves with the raw (unaccelerated) deltas and never stops at an
   edge: what mouse_delta reads from it is the relative motion. */
static void relative_pointer_motion(void *data, struct zwp_relative_pointer_v1 *rp, uint32_t hi, uint32_t lo,
                                    wl_fixed_t dx, wl_fixed_t dy, wl_fixed_t dx_unaccel, wl_fixed_t dy_unaccel)
{
    BackendWindow *b = data;
    (void)rp;
    (void)hi;
    (void)lo;
    (void)dx;
    (void)dy;
    if (!b->locked)
        return;
    double ux = wl_fixed_to_double(dx_unaccel), uy = wl_fixed_to_double(dy_unaccel);
    b->virt_x += ux;
    b->virt_y += uy;
    g.pointer_x = (int)b->virt_x;
    g.pointer_y = (int)b->virt_y;

    Event e = {.type = EVENT_MOUSE_MOVE};
    e.data.mouse.x = g.pointer_x;
    e.data.mouse.y = g.pointer_y;
    e.data.mouse.dx = (int)ux;
    e.data.mouse.dy = (int)uy;
    push(b, &e);
}

static const struct zwp_relative_pointer_v1_listener relative_pointer_listener = {
    .relative_motion = relative_pointer_motion,
};
#endif

static void lock_pointer(BackendWindow *b)
{
#ifdef ZEN_WL_POINTER_LOCK
    if (b->locked_pointer || !g.constraints || !g.relative_manager || !g.pointer)
        return;
    /* Persistent: the compositor locks again whenever the window gets the pointer back. */
    b->locked_pointer = zwp_pointer_constraints_v1_lock_pointer(
        g.constraints, b->surface, g.pointer, NULL, ZWP_POINTER_CONSTRAINTS_V1_LIFETIME_PERSISTENT);
    zwp_locked_pointer_v1_add_listener(b->locked_pointer, &locked_pointer_listener, b);
    b->relative_pointer = zwp_relative_pointer_manager_v1_get_relative_pointer(g.relative_manager, g.pointer);
    zwp_relative_pointer_v1_add_listener(b->relative_pointer, &relative_pointer_listener, b);
#else
    (void)b;
#endif
}

static void unlock_pointer(BackendWindow *b)
{
#ifdef ZEN_WL_POINTER_LOCK
    if (!b->locked_pointer)
        return;
    /* The cursor comes back where it was when the lock began. */
    if (b->locked)
    {
        zwp_locked_pointer_v1_set_cursor_position_hint(b->locked_pointer, wl_fixed_from_int(b->lock_x),
                                                       wl_fixed_from_int(b->lock_y));
        wl_surface_commit(b->surface);
    }
    zwp_locked_pointer_v1_destroy(b->locked_pointer);
    zwp_relative_pointer_v1_destroy(b->relative_pointer);
    b->locked_pointer = NULL;
    b->relative_pointer = NULL;
    b->locked = false;
#else
    (void)b;
#endif
}

PlatformCursor *backend_cursor_create(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    if (!g.shm)
    {
        error_set("the compositor has no wl_shm, which a cursor from an image needs");
        return NULL;
    }
    uint32_t *px = malloc((size_t)w * (size_t)h * 4);
    if (!px)
        return NULL;
    for (int i = 0; i < w * h; i++) /* wl_shm wants premultiplied alpha */
    {
        uint32_t p = argb[i], a = p >> 24;
        uint32_t r = ((p >> 16) & 0xFF) * a / 255, gr = ((p >> 8) & 0xFF) * a / 255, bl = (p & 0xFF) * a / 255;
        px[i] = (a << 24) | (r << 16) | (gr << 8) | bl;
    }
    PlatformCursor *c = calloc(1, sizeof *c);
    if (c)
        c->buffer = shm_buffer(px, w, h, NULL);
    free(px);
    if (!c || !c->buffer)
    {
        free(c);
        error_set("cannot create the cursor");
        return NULL;
    }
    c->w = w;
    c->h = h;
    c->hot_x = hot_x;
    c->hot_y = hot_y;
    return c;
}

void backend_cursor_destroy(PlatformCursor *c)
{
    if (!c)
        return;
    wl_buffer_destroy(c->buffer);
    free(c);
}

void backend_set_cursor(BackendWindow *b, int cursor)
{
    if (cursor < 0 || cursor >= CURSOR_COUNT)
        cursor = CURSOR_DEFAULT;
    b->cursor_idx = cursor;
    b->cursor_image = NULL;
    apply_cursor(b);
}

void backend_set_cursor_image(BackendWindow *b, PlatformCursor *c)
{
    b->cursor_image = c;
    apply_cursor(b);
}

void backend_set_mouse_mode(BackendWindow *b, int mode)
{
    if (mode == b->mouse_mode)
        return;
    b->mouse_mode = mode;
    if (mode == MOUSE_MODE_CAPTURED)
        lock_pointer(b);
    else
        unlock_pointer(b);
    apply_cursor(b);
}

/* A client cannot move the pointer; the one place it can say where it is is a
   lock, and that only for where the cursor appears after it. */
void backend_set_mouse_pos(BackendWindow *b, int x, int y)
{
#ifdef ZEN_WL_POINTER_LOCK
    if (b->locked)
    {
        b->virt_x = b->lock_x = x;
        b->virt_y = b->lock_y = y;
        g.pointer_x = x;
        g.pointer_y = y;
    }
#else
    (void)b;
    (void)x;
    (void)y;
#endif
}

/* While a button is held the compositor already sends the pointer to the window
   that took the press, outside it too, which is the case this is for. */
bool backend_mouse_capture(BackendWindow *b, bool on)
{
    (void)b;
    (void)on;
    return true;
}

/* ========================================================================== */
/*  EGL context                                                               */
/* ========================================================================== */

static EGLConfig choose_config(const GLConfig *gl, GLProfile profile, int major)
{
    EGLint renderable = EGL_OPENGL_BIT;
    if (profile == GL_PROFILE_ES)
        renderable = major >= 3 ? EGL_OPENGL_ES3_BIT : EGL_OPENGL_ES2_BIT;

    const EGLint attribs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, renderable,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_SAMPLE_BUFFERS, gl->msaa > 0 ? 1 : 0,
        EGL_SAMPLES, gl->msaa,
        EGL_NONE};

    EGLConfig config;
    EGLint count = 0;
    if (!eglChooseConfig(g.egl, attribs, &config, 1, &count) || count == 0)
    {
        error_set("no framebuffer configuration with RGBA8, depth 24, stencil 8 and %d samples", gl->msaa);
        return NULL;
    }
    return config;
}

static EGLContext create_context(EGLConfig config, const GLConfig *gl, GLProfile profile, int major, int minor)
{
    const char *name = "core";
    EGLint mask = EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT;
    if (profile == GL_PROFILE_COMPAT)
    {
        name = "compatibility";
        mask = EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT;
    }
    else if (profile == GL_PROFILE_ES)
    {
        name = "ES";
    }
    if (!g.egl_create_context)
    {
        error_set("EGL_KHR_create_context is not supported");
        return EGL_NO_CONTEXT;
    }

    if (!eglBindAPI(profile == GL_PROFILE_ES ? EGL_OPENGL_ES_API : EGL_OPENGL_API))
    {
        error_set("cannot create an OpenGL %s %d.%d context: the API is not there", name, major, minor);
        return EGL_NO_CONTEXT;
    }

    EGLint attribs[11];
    int n = 0;
    attribs[n++] = EGL_CONTEXT_MAJOR_VERSION;
    attribs[n++] = major;
    attribs[n++] = EGL_CONTEXT_MINOR_VERSION;
    attribs[n++] = minor;
    if (profile != GL_PROFILE_ES)
    {
        attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
        attribs[n++] = mask;
    }
    if (gl->debug)
    {
        attribs[n++] = EGL_CONTEXT_OPENGL_DEBUG;
        attribs[n++] = EGL_TRUE;
    }
    attribs[n] = EGL_NONE;

    EGLContext c = eglCreateContext(g.egl, config, EGL_NO_CONTEXT, attribs);
    if (c == EGL_NO_CONTEXT)
    {
        error_set("cannot create an OpenGL %s %d.%d context%s", name, major, minor, gl->debug ? " with debug" : "");
        return EGL_NO_CONTEXT;
    }
    return c;
}

/* ========================================================================== */
/*  create / destroy                                                          */
/* ========================================================================== */

static void apply_limits(BackendWindow *b)
{
    if (!b->toplevel)
        return;
    int l, t, r, bt;
    decor_extents(b, b->maximized, b->fullscreen, &l, &t, &r, &bt);
    int min_w = b->min_w ? b->min_w + l + r : 0, min_h = b->min_h ? b->min_h + t + bt : 0;
    int max_w = b->max_w ? b->max_w + l + r : 0, max_h = b->max_h ? b->max_h + t + bt : 0;
    /* room for the three buttons, unless the window cannot be that wide */
    if (b->csd && min_w < 3 * BUTTON_W + 2 * BORDER)
        min_w = 3 * BUTTON_W + 2 * BORDER;
    if (max_w && min_w > max_w)
        min_w = max_w;
    xdg_toplevel_set_min_size(b->toplevel, min_w, min_h);
    xdg_toplevel_set_max_size(b->toplevel, max_w, max_h);
}

#ifdef ZEN_WL_XDG_DECORATION
static void deco_configure(void *data, struct zxdg_toplevel_decoration_v1 *deco, uint32_t mode)
{
    BackendWindow *b = data;
    (void)deco;
    b->server_side = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    b->csd = wants_own_frame(b);
    b->decor_dirty = true;
}

static const struct zxdg_toplevel_decoration_v1_listener deco_listener = {
    .configure = deco_configure,
};
#endif

/* The xdg role of the window: made at creation and again by show after hide.
   Returns once the compositor has sent the first configure, since nothing may be
   attached before that one is acknowledged. */
static bool create_shell(BackendWindow *b)
{
    b->xdg_surface = xdg_wm_base_get_xdg_surface(g.wm_base, b->surface);
    xdg_surface_add_listener(b->xdg_surface, &surface_listener, b);
    b->toplevel = xdg_surface_get_toplevel(b->xdg_surface);
    xdg_toplevel_add_listener(b->toplevel, &toplevel_listener, b);

    xdg_toplevel_set_title(b->toplevel, b->title);
    xdg_toplevel_set_app_id(b->toplevel, program_invocation_short_name);
#ifdef ZEN_WL_XDG_DECORATION
    if (g.deco_manager)
    {
        b->xdg_deco = zxdg_decoration_manager_v1_get_toplevel_decoration(g.deco_manager, b->toplevel);
        zxdg_toplevel_decoration_v1_add_listener(b->xdg_deco, &deco_listener, b);
        zxdg_toplevel_decoration_v1_set_mode(b->xdg_deco, b->decorated
                                                             ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                             : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    }
#endif
    apply_limits(b);
    if (b->want_maximized)
        xdg_toplevel_set_maximized(b->toplevel);
    if (b->mode != WINDOW_WINDOWED)
        xdg_toplevel_set_fullscreen(b->toplevel, NULL);
    set_opaque_region(b);

    b->configured = false;
    wl_surface_commit(b->surface);
    while (!b->configured)
    {
        if (wl_display_dispatch(g.display) < 0)
            return false;
    }
    return true;
}

static void destroy_shell(BackendWindow *b)
{
#ifdef ZEN_WL_XDG_DECORATION
    if (b->xdg_deco)
        zxdg_toplevel_decoration_v1_destroy(b->xdg_deco);
    b->xdg_deco = NULL;
#endif
    if (b->toplevel)
        xdg_toplevel_destroy(b->toplevel);
    if (b->xdg_surface)
        xdg_surface_destroy(b->xdg_surface);
    b->toplevel = NULL;
    b->xdg_surface = NULL;
}

/* The four surfaces of the frame and the buffers on them. They are subsurfaces of
   the window's, so they go before it does and are made again by the next swap. */
static void decor_destroy(BackendWindow *b)
{
    for (int i = 0; i < DECOR_COUNT; i++)
    {
        DecorPart *d = &b->decor[i];
        if (g.pointer_decor == d)
            g.pointer_decor = NULL;
        if (d->buffer)
            wl_buffer_destroy(d->buffer);
        if (d->sub)
            wl_subsurface_destroy(d->sub);
        if (d->surface)
            wl_surface_destroy(d->surface);
        memset(d, 0, sizeof *d);
    }
    b->decor_dirty = true;
}

BackendWindow *backend_create(const WindowConfig *cfg)
{
    if (cfg->render != RENDER_GL)
    {
        error_set("the Wayland backend has only RENDER_GL windows so far");
        return NULL;
    }

    GLProfile profile = cfg->gl.profile == GL_PROFILE_DEFAULT ? GL_PROFILE_CORE : cfg->gl.profile;
    int major = cfg->gl.major;
    int minor = cfg->gl.minor;
    if (major == 0)
    {
        major = 3;
        minor = profile == GL_PROFILE_ES ? 0 : 3;
    }

    /* The context needs no window, so a configuration the driver refuses fails
       before anything is shown. */
    EGLConfig config = choose_config(&cfg->gl, profile, major);
    if (!config)
        return NULL;
    EGLContext context = create_context(config, &cfg->gl, profile, major, minor);
    if (context == EGL_NO_CONTEXT)
        return NULL;

    BackendWindow *b = calloc(1, sizeof *b);
    if (!b)
    {
        eglDestroyContext(g.egl, context);
        return NULL;
    }
    b->context = context;
    b->config = config;
    b->render = cfg->render;
    b->mode = cfg->mode;
    b->visible = true;
    b->width = cfg->width > 0 ? cfg->width : 640;
    b->height = cfg->height > 0 ? cfg->height : 480;
    b->title = strdup(cfg->title ? cfg->title : "");
    b->hover_button = b->press_button = TB_NONE;
    b->hit_cursor = -1;
    b->decorated = !cfg->undecorated && cfg->kind != WINDOW_KIND_POPUP && cfg->kind != WINDOW_KIND_TOOLTIP;
    b->csd = wants_own_frame(b); /* until the compositor says it draws the frame */
    b->decor_dirty = true;
    if (!cfg->resizable)
    {
        b->min_w = b->max_w = b->width;
        b->min_h = b->max_h = b->height;
    }

    b->surface = wl_compositor_create_surface(g.compositor);
    b->next = g.windows;
    g.windows = b;
    if (!b->title || !create_shell(b))
    {
        error_set("the Wayland connection broke while the window was being created");
        backend_destroy(b);
        return NULL;
    }

    b->egl_window = wl_egl_window_create(b->surface, b->width, b->height);
    if (b->egl_window)
        b->egl_surface = eglCreateWindowSurface(g.egl, config, (EGLNativeWindowType)b->egl_window, NULL);
    if (b->egl_surface == EGL_NO_SURFACE || !eglMakeCurrent(g.egl, b->egl_surface, b->egl_surface, b->context))
    {
        error_set("cannot create the EGL surface of the window");
        backend_destroy(b);
        return NULL;
    }
    /* EGL's own default is 1; the pacing is backend_swap's (see there). */
    eglSwapInterval(g.egl, 0);
    b->vsync = cfg->vsync;
    return b;
}

void backend_destroy(BackendWindow *b)
{
    if (!b)
        return;
    /* Leave another window's context current, unless it draws to this window. */
    if (b->egl_surface != EGL_NO_SURFACE && (eglGetCurrentContext() == b->context ||
                                             eglGetCurrentSurface(EGL_DRAW) == b->egl_surface))
        eglMakeCurrent(g.egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (b->egl_surface)
        eglDestroySurface(g.egl, b->egl_surface);
    if (b->context)
        eglDestroyContext(g.egl, b->context);
    if (b->egl_window)
        wl_egl_window_destroy(b->egl_window);
    for (BackendWindow **p = &g.windows; *p; p = &(*p)->next)
    {
        if (*p == b)
        {
            *p = b->next;
            break;
        }
    }
    if (g.keyboard_focus == b)
    {
        g.keyboard_focus = NULL;
        g.repeat_key = 0;
    }
    if (g.pointer_focus == b)
        g.pointer_focus = NULL;
    decor_destroy(b);
    unlock_pointer(b);
    destroy_shell(b);
    if (b->frame_cb)
        wl_callback_destroy(b->frame_cb);
    if (b->surface)
        wl_surface_destroy(b->surface);
    free(b->title);
    free(b->queue);
    free(b);
}

/* ========================================================================== */
/*  event pump                                                                */
/* ========================================================================== */

/* Read what the compositor sent, waiting up to timeout_ms for it. */
static void dispatch(int timeout_ms)
{
    while (wl_display_prepare_read(g.display) != 0)
        wl_display_dispatch_pending(g.display);

    if (wl_display_flush(g.display) < 0 && errno != EAGAIN)
    {
        wl_display_cancel_read(g.display);
        return;
    }
    struct pollfd pfd = {.fd = wl_display_get_fd(g.display), .events = POLLIN};
    if (poll(&pfd, 1, timeout_ms) > 0)
        wl_display_read_events(g.display);
    else
        wl_display_cancel_read(g.display);
    wl_display_dispatch_pending(g.display);
}

void backend_pump_events(BackendWindow *b, Core *core)
{
    dispatch(0);

    if (!g.lost && wl_display_get_error(g.display) != 0)
    {
        g.lost = true;
        log_error("Wayland: the connection to the compositor broke (error %d)", wl_display_get_error(g.display));
        Event e = {.type = EVENT_WINDOW_CLOSE};
        push(b, &e);
    }

    repeat_keys(b);

    /* A hook may reach back into the backend, so the queue can grow underneath. */
    for (int i = 0; i < b->queue_n; i++)
    {
        Event e = b->queue[i];
        if (e.type == EVENT_CHAR)
            core_push_char(core, e.data.codepoint);
        else
            core_push_event(core, &e);
    }
    b->queue_n = 0;
}

/* ========================================================================== */
/*  GL context glue                                                           */
/* ========================================================================== */

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    BackendWindow *b = data;
    (void)time;
    wl_callback_destroy(cb);
    b->frame_cb = NULL;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_done,
};

/* The swap interval is done here, not by EGL: Mesa's swap with an interval waits
   for the compositor's frame callback without a limit, and a minimized or covered
   window never gets one, so the loop would stop inside the swap and the pings go
   unanswered. Waiting here keeps the connection served, and gives up after a
   moment: such a window then runs slowly instead of not at all. */
#define FRAME_WAIT_MS 50

void backend_swap(BackendWindow *b)
{
    if (b->render != RENDER_GL || !b->visible)
        return;

    if (b->vsync)
    {
        double deadline = now_ms() + FRAME_WAIT_MS;
        for (double left = FRAME_WAIT_MS; b->frame_cb && left > 0; left = deadline - now_ms())
            dispatch((int)left + 1);
        if (!b->frame_cb)
        {
            b->frame_cb = wl_surface_frame(b->surface);
            wl_callback_add_listener(b->frame_cb, &frame_listener, b);
        }
    }
    decor_flush(b);
    eglSwapBuffers(g.egl, b->egl_surface);
}

void backend_make_current(BackendWindow *b)
{
    if (b->render == RENDER_GL)
        eglMakeCurrent(g.egl, b->egl_surface, b->egl_surface, b->context);
}

void backend_make_current_on(BackendWindow *target, BackendWindow *context)
{
    if (target->render == RENDER_GL && context->render == RENDER_GL)
        eglMakeCurrent(g.egl, target->egl_surface, target->egl_surface, context->context);
}

void backend_set_vsync(BackendWindow *b, bool on)
{
    b->vsync = on;
}

void *backend_gl_proc_address(const char *name)
{
    return (void *)eglGetProcAddress(name);
}

BackendGLWorker *backend_gl_worker_create(BackendWindow *b)
{
    (void)b;
    return NULL;
}
bool backend_gl_worker_make_current(BackendGLWorker *worker, bool current)
{
    (void)worker;
    (void)current;
    return false;
}
void backend_gl_worker_destroy(BackendGLWorker *worker)
{
    (void)worker;
}

/* ========================================================================== */
/*  geometry and state                                                        */
/* ========================================================================== */

void backend_get_size(BackendWindow *b, int *w, int *h)
{
    if (w)
        *w = b->width;
    if (h)
        *h = b->height;
}

/* A client sets the size of a floating window by attaching a buffer of that size;
   the resize reaches the compositor with the next swap. */
void backend_set_size(BackendWindow *b, int w, int h)
{
    if (w > 0 && h > 0 && !b->maximized && !b->fullscreen)
        apply_size(b, w, h);
}

void backend_get_fb_size(BackendWindow *b, int *w, int *h)
{
    if (w)
        *w = b->width;
    if (h)
        *h = b->height;
}

/* Wayland tells a client nothing about where its window is. */
void backend_get_pos(BackendWindow *b, int *x, int *y)
{
    (void)b;
    if (x)
        *x = 0;
    if (y)
        *y = 0;
}

void backend_set_pos(BackendWindow *b, int x, int y)
{
    (void)b;
    (void)x;
    (void)y;
}

float backend_content_scale(BackendWindow *b)
{
    (void)b;
    return 1.0f;
}

void backend_set_title(BackendWindow *b, const char *title)
{
    char *copy = strdup(title ? title : "");
    if (!copy)
        return;
    if (b->title && strcmp(b->title, copy) != 0)
        b->decor_dirty = true; /* drawn on the bar, by the next swap */
    free(b->title);
    b->title = copy;
    if (b->toplevel)
        xdg_toplevel_set_title(b->toplevel, b->title);
}

void backend_set_size_limits(BackendWindow *b, int minw, int minh, int maxw, int maxh)
{
    b->min_w = minw > 0 ? minw : 0;
    b->min_h = minh > 0 ? minh : 0;
    b->max_w = maxw > 0 ? maxw : 0;
    b->max_h = maxh > 0 ? maxh : 0;
    if (!b->toplevel)
        return;
    apply_limits(b);
    wl_surface_commit(b->surface); /* the limits are double-buffered state */
}

/* ========================================================================== */
/*  window state                                                              */
/* ========================================================================== */

/* The compositor never says a window is minimized, so there is no event and no
   flag for it; the request is all a client can make, and there is no way back
   from a client either (activation needs a token, see request_attention). */
void backend_minimize(BackendWindow *b)
{
    if (b->toplevel)
        xdg_toplevel_set_minimized(b->toplevel);
}

void backend_maximize(BackendWindow *b)
{
    b->want_maximized = true;
    if (b->toplevel)
        xdg_toplevel_set_maximized(b->toplevel);
}

void backend_restore(BackendWindow *b)
{
    b->want_maximized = false;
    if (b->toplevel)
        xdg_toplevel_unset_maximized(b->toplevel);
}

/* A toplevel has no mapped state of its own: hiding it takes its shell objects
   away and attaches nothing, showing it makes them again and waits for the
   compositor to configure the window anew. */
void backend_hide(BackendWindow *b)
{
    if (!b->visible)
        return;
    destroy_shell(b);
    if (b->frame_cb)
    {
        wl_callback_destroy(b->frame_cb);
        b->frame_cb = NULL;
    }
    wl_surface_attach(b->surface, NULL, 0, 0);
    wl_surface_commit(b->surface);
    b->visible = false;
    if (b->focused)
    {
        b->focused = false;
        Event e = {.type = EVENT_WINDOW_FOCUS};
        push(b, &e);
    }
}

/* A window comes back on a wl_surface of its own, with a new wl_egl_window and EGL
   surface. A new role on the surface the old one had gives the compositor the same
   surface to show twice while it is still playing the closing of the first: GNOME
   Shell 46 asserts in clutter_actor_insert_child_at_index and then dies with
   SIGSEGV in its destroy animation (reproduced with test_hide_show), and with it
   the whole session. Nothing the compositor holds has been told about this one.
   The window stays hidden, on its old objects, when any step fails. */
static bool show_on_new_surface(BackendWindow *b)
{
    struct wl_surface *surface = wl_compositor_create_surface(g.compositor);
    if (!surface)
        return false;

    unlock_pointer(b); /* it is bound to the surface that goes away */
    decor_destroy(b);
    b->hover_button = b->press_button = TB_NONE;

    struct wl_surface *old_surface = b->surface;
    struct wl_egl_window *old_window = b->egl_window;
    EGLSurface old_egl = b->egl_surface;
    b->surface = surface;
    b->egl_window = NULL; /* the configure that comes is the new one's */
    b->egl_surface = EGL_NO_SURFACE;

    EGLSurface egl = EGL_NO_SURFACE;
    if (create_shell(b))
    {
        b->egl_window = wl_egl_window_create(surface, b->width, b->height);
        if (b->egl_window)
            egl = eglCreateWindowSurface(g.egl, b->config, (EGLNativeWindowType)b->egl_window, NULL);
    }
    if (egl == EGL_NO_SURFACE)
    {
        if (b->egl_window)
            wl_egl_window_destroy(b->egl_window);
        destroy_shell(b);
        wl_surface_destroy(surface);
        b->surface = old_surface;
        b->egl_window = old_window;
        b->egl_surface = old_egl;
        return false;
    }
    b->egl_surface = egl;

    /* What the context draws to moves along; the swap interval is the surface's own. */
    EGLContext cur = eglGetCurrentContext();
    EGLSurface draw = eglGetCurrentSurface(EGL_DRAW);
    EGLSurface read = eglGetCurrentSurface(EGL_READ);
    if (eglMakeCurrent(g.egl, egl, egl, b->context))
        eglSwapInterval(g.egl, 0);
    if (cur != EGL_NO_CONTEXT)
        eglMakeCurrent(g.egl, draw == old_egl ? egl : draw, read == old_egl ? egl : read, cur);
    else
        eglMakeCurrent(g.egl, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    eglDestroySurface(g.egl, old_egl);
    if (old_window)
        wl_egl_window_destroy(old_window);
    wl_surface_destroy(old_surface);

    if (b->mouse_mode == MOUSE_MODE_CAPTURED)
        lock_pointer(b);
    return true;
}

void backend_show(BackendWindow *b)
{
    if (b->visible)
        return;
    if (!show_on_new_surface(b))
    {
        log_error("Wayland: cannot show the window again");
        return;
    }
    b->visible = true;
    b->decor_dirty = true; /* the geometry went with the old shell */
}

void backend_set_mode(BackendWindow *b, WindowMode mode, int monitor)
{
    (void)monitor; /* the compositor picks the output until the monitors are known */
    b->mode = mode;
    if (!b->toplevel)
        return;
    if (mode != WINDOW_WINDOWED)
        xdg_toplevel_set_fullscreen(b->toplevel, NULL);
    else
        xdg_toplevel_unset_fullscreen(b->toplevel);
}

WindowMode backend_get_mode(BackendWindow *b)
{
    return b->mode;
}

bool backend_get_flag(BackendWindow *b, int flag)
{
    switch (flag)
    {
    case WIN_FLAG_FOCUSED:
        return b->focused;
    case WIN_FLAG_MAXIMIZED:
        return b->maximized;
    case WIN_FLAG_VISIBLE:
        return b->visible;
    case WIN_FLAG_HOVERED:
        return b->hovered;
    }
    return false;
}

/* ========================================================================== */
/*  not there yet                                                             */
/* ========================================================================== */

void backend_focus(BackendWindow *b) { (void)b; }
void backend_request_attention(BackendWindow *b) { (void)b; }

void backend_set_icon(BackendWindow *b, int w, int h, const uint8_t *rgba)
{
    (void)b; (void)w; (void)h; (void)rgba;
}

void backend_set_opacity(BackendWindow *b, float a) { (void)b; (void)a; }
void backend_set_always_on_top(BackendWindow *b, bool on) { (void)b; (void)on; }
void backend_set_decorated(BackendWindow *b, bool on)
{
    b->decorated = on;
#ifdef ZEN_WL_XDG_DECORATION
    if (b->xdg_deco)
        zxdg_toplevel_decoration_v1_set_mode(b->xdg_deco, on ? ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE
                                                             : ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
#endif
    b->csd = wants_own_frame(b);
    b->decor_dirty = true;
    if (b->visible && b->xdg_surface)
        decor_flush_now(b);
}

void backend_set_hit_test(BackendWindow *b, PlatformWindow *w, HitTestFunc fn, void *user)
{
    b->hit_cb = fn;
    b->hit_w = w;
    b->hit_user = user;
    if (!fn && b->hit_cursor >= 0)
    {
        b->hit_cursor = -1;
        apply_cursor(b);
    }
}

void *backend_native_handle(BackendWindow *b, NativeHandleType type)
{
    switch (type)
    {
    case NATIVE_DISPLAY:
        return g.display;
    case NATIVE_WINDOW:
        return b->surface;
    case NATIVE_GL_CONTEXT:
        return b->context;
    }
    return NULL;
}

const char *const *backend_vulkan_extensions(uint32_t *count)
{
    *count = 0;
    return NULL;
}

bool backend_vulkan_create_surface(BackendWindow *b, void *instance, const void *allocator, uint64_t *out_surface)
{
    (void)b; (void)instance; (void)allocator; (void)out_surface;
    return error_set("Vulkan is not supported by the Wayland backend yet");
}

bool backend_lock_pixels(BackendWindow *b, Framebuffer *out)
{
    (void)b; (void)out;
    return false;
}

void backend_present_pixels(BackendWindow *b) { (void)b; }

void backend_set_live_callback(BackendWindow *b, PlatformWindow *w, FrameCallback cb, void *user)
{
    (void)b; (void)w; (void)cb; (void)user;
}

int backend_lock_state(void)
{
    int locks = 0;
    if (!g.xkb_state)
        return 0;
    if (xkb_state_mod_index_is_active(g.xkb_state, g.mod_caps, XKB_STATE_MODS_LOCKED) > 0)
        locks |= KEYMOD_CAPS_LOCK;
    if (xkb_state_mod_index_is_active(g.xkb_state, g.mod_num, XKB_STATE_MODS_LOCKED) > 0)
        locks |= KEYMOD_NUM_LOCK;
    return locks;
}

void backend_set_text_input(BackendWindow *b, bool on) { (void)b; (void)on; }

void backend_set_text_input_rect(BackendWindow *b, int x, int y, int w, int h)
{
    (void)b; (void)x; (void)y; (void)w; (void)h;
}

bool backend_clipboard_set(const ClipboardItem *items, int count)
{
    (void)items; (void)count;
    return false;
}

bool backend_clipboard_has(const char *mime)
{
    (void)mime;
    return false;
}

void *backend_clipboard_get(const char *mime, size_t *size)
{
    (void)mime;
    if (size)
        *size = 0;
    return NULL;
}

int backend_monitor_count(void)
{
    return 0;
}

bool backend_monitor_info(int index, MonitorInfo *out)
{
    (void)index; (void)out;
    return false;
}

/* Wayland has no global pointer position either. */
bool backend_mouse_global_position(int *x, int *y)
{
    (void)x; (void)y;
    return false;
}

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
