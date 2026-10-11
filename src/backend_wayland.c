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
#include "xdg-shell-client-protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
} g;

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

    /* A hook may reach back into the backend, so the queue can grow underneath. */
    for (int i = 0; i < b->queue_n; i++)
    {
        Event e = b->queue[i];
        core_push_event(core, &e);
    }
    b->queue_n = 0;
}

/* ========================================================================== */
/*  GL context glue                                                           */
/* ========================================================================== */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

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
    return 0;
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
