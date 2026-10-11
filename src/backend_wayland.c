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
#include <wayland-egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-compose.h>
#include "xdg-shell-client-protocol.h"

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

struct BackendWindow
{
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *toplevel;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    EGLContext context;

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
    apply_size(b, w, h);

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
    BackendWindow *b = window_for_surface(surface);
    g.pointer_focus = b;
    if (!b)
        return;
    g.pointer_x = wl_fixed_to_int(sx);
    g.pointer_y = wl_fixed_to_int(sy);
    b->hovered = true;

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
    BackendWindow *b = g.pointer_focus;
    if (!b)
        return;
    g.pointer_x = wl_fixed_to_int(sx);
    g.pointer_y = wl_fixed_to_int(sy);
    Event e = {.type = EVENT_MOUSE_MOVE};
    e.data.mouse.x = g.pointer_x;
    e.data.mouse.y = g.pointer_y;
    push(b, &e);
}

static void pointer_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state)
{
    (void)data;
    (void)p;
    (void)time;
    g.serial = serial;
    BackendWindow *b = g.pointer_focus;
    if (!b)
        return;

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
    xdg_toplevel_set_min_size(b->toplevel, b->min_w, b->min_h);
    xdg_toplevel_set_max_size(b->toplevel, b->max_w, b->max_h);
}

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
    if (b->toplevel)
        xdg_toplevel_destroy(b->toplevel);
    if (b->xdg_surface)
        xdg_surface_destroy(b->xdg_surface);
    b->toplevel = NULL;
    b->xdg_surface = NULL;
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
    b->render = cfg->render;
    b->mode = cfg->mode;
    b->visible = true;
    b->width = cfg->width > 0 ? cfg->width : 640;
    b->height = cfg->height > 0 ? cfg->height : 480;
    b->title = strdup(cfg->title ? cfg->title : "");
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

void backend_show(BackendWindow *b)
{
    if (b->visible)
        return;
    if (!create_shell(b))
    {
        destroy_shell(b);
        log_error("Wayland: cannot show the window again");
        return;
    }
    b->visible = true;
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
void backend_set_decorated(BackendWindow *b, bool on) { (void)b; (void)on; }

void backend_set_hit_test(BackendWindow *b, PlatformWindow *w, HitTestFunc fn, void *user)
{
    (void)b; (void)w; (void)fn; (void)user;
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

bool backend_mouse_capture(BackendWindow *b, bool on)
{
    (void)b; (void)on;
    return false;
}

PlatformCursor *backend_cursor_create(const uint32_t *argb, int w, int h, int hot_x, int hot_y)
{
    (void)argb; (void)w; (void)h; (void)hot_x; (void)hot_y;
    return NULL;
}

void backend_cursor_destroy(PlatformCursor *c) { (void)c; }
void backend_set_cursor_image(BackendWindow *b, PlatformCursor *c) { (void)b; (void)c; }
void backend_set_mouse_pos(BackendWindow *b, int x, int y) { (void)b; (void)x; (void)y; }
void backend_set_cursor(BackendWindow *b, int cursor) { (void)b; (void)cursor; }
void backend_set_mouse_mode(BackendWindow *b, int mode) { (void)b; (void)mode; }

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
