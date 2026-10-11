/*
 * test_input_wayland.c - keyboard, pointer and window configure of the Wayland
 * backend, against a small compositor of its own (libwayland-server, in a
 * thread). What it sends is scripted, so layouts, repeats, compose, scrolling and
 * states are checked without a real keyboard or a display. Skips (77) when a
 * piece is missing.
 */
#define _GNU_SOURCE
#include "platform.h"
#include "xdg-shell-server-protocol.h"
#ifdef ZEN_WL_POINTER_LOCK
#include "pointer-constraints-unstable-v1-server-protocol.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#endif

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

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

/* ========================================================================== */
/*  the compositor                                                            */
/* ========================================================================== */

typedef struct
{
    struct wl_resource *res, *xdg, *toplevel;
    struct wl_resource *frames[16];
    int frame_n;
    struct wl_resource *buffer;
    bool configured;
    bool shown; /* a buffer was committed */
    int shown_w, shown_h;
    uint32_t shown_pixel;
} Surface;

static struct
{
    struct wl_display *display;
    struct wl_event_source *cmd_source;
    int cmd[2];
    pthread_t thread;
    struct wl_resource *keyboard, *pointer;
    Surface *surface;
    uint32_t serial;
    struct xkb_context *xkb;
    struct xkb_keymap *keymap;
    struct xkb_state *state;

    /* the last wl_pointer.set_cursor, and what the cursor surface showed with it */
    int cursor_requests;
    uint32_t cursor_serial;
    bool cursor_has_surface;
    int cursor_hot_x, cursor_hot_y, cursor_w, cursor_h;
    uint32_t cursor_pixel; /* the first pixel of the buffer, as stored */
    struct wl_resource *cursor_surface;

    uint32_t enter_serial;
#ifdef ZEN_WL_POINTER_LOCK
    struct wl_resource *locked, *relative;
    int lock_requests;
    bool lock_destroyed;
    int hint_x, hint_y;
#endif
} S;

static const struct
{
    const char *layout, *variant;
} layouts[] = {{"us", ""}, {"fr", ""}, {"us", "intl"}};

enum
{
    CMD_LAYOUT,
    CMD_KB_ENTER,
    CMD_KB_LEAVE,
    CMD_KEY,
    CMD_PTR_ENTER,
    CMD_PTR_LEAVE,
    CMD_PTR_MOVE,
    CMD_PTR_BUTTON,
    CMD_PTR_AXIS,
    CMD_PTR_V120,
    CMD_PTR_FRAME,
    CMD_CONFIGURE,
    CMD_LOCK,
    CMD_UNLOCK,
    CMD_REL_MOTION,
    CMD_QUIT
};

typedef struct
{
    int type, a, b, c, d;
} Cmd;

static void unused_request(void) {}

/* ---- wl_surface ---- */

static void surface_destroy_req(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void surface_attach(struct wl_client *c, struct wl_resource *r, struct wl_resource *buffer, int32_t x,
                           int32_t y)
{
    Surface *s = wl_resource_get_user_data(r);
    (void)c;
    (void)x;
    (void)y;
    s->buffer = buffer;
}

static void surface_frame(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    Surface *s = wl_resource_get_user_data(r);
    struct wl_resource *cb = wl_resource_create(c, &wl_callback_interface, 1, id);
    if (cb && s->frame_n < 16)
        s->frames[s->frame_n++] = cb;
}

static void send_configure(Surface *s, int w, int h, uint32_t states)
{
    struct wl_array arr;
    wl_array_init(&arr);
    static const uint32_t order[][2] = {{1, XDG_TOPLEVEL_STATE_MAXIMIZED},
                                        {2, XDG_TOPLEVEL_STATE_FULLSCREEN},
                                        {4, XDG_TOPLEVEL_STATE_ACTIVATED}};
    for (int i = 0; i < 3; i++)
    {
        if (states & order[i][0])
        {
            uint32_t *p = wl_array_add(&arr, sizeof *p);
            *p = order[i][1];
        }
    }
    xdg_toplevel_send_configure(s->toplevel, w, h, &arr);
    xdg_surface_send_configure(s->xdg, ++S.serial);
    wl_array_release(&arr);
}

static void surface_commit(struct wl_client *c, struct wl_resource *r)
{
    Surface *s = wl_resource_get_user_data(r);
    (void)c;
    if (s->xdg && s->toplevel && !s->configured)
    {
        s->configured = true;
        send_configure(s, 0, 0, 0);
    }
    if (s->buffer)
    {
        struct wl_shm_buffer *shm = wl_shm_buffer_get(s->buffer);
        if (shm)
        {
            s->shown = true;
            s->shown_w = wl_shm_buffer_get_width(shm);
            s->shown_h = wl_shm_buffer_get_height(shm);
            wl_shm_buffer_begin_access(shm);
            s->shown_pixel = *(uint32_t *)wl_shm_buffer_get_data(shm);
            wl_shm_buffer_end_access(shm);
        }
        wl_buffer_send_release(s->buffer);
        s->buffer = NULL;
    }
    for (int i = 0; i < s->frame_n; i++)
    {
        wl_callback_send_done(s->frames[i], 0);
        wl_resource_destroy(s->frames[i]);
    }
    s->frame_n = 0;
}

static const struct wl_surface_interface surface_impl = {
    .destroy = surface_destroy_req,
    .attach = surface_attach,
    .damage = (void *)unused_request,
    .frame = surface_frame,
    .set_opaque_region = (void *)unused_request,
    .set_input_region = (void *)unused_request,
    .commit = surface_commit,
    .set_buffer_transform = (void *)unused_request,
    .set_buffer_scale = (void *)unused_request,
    .damage_buffer = (void *)unused_request,
};

static void surface_gone(struct wl_resource *r)
{
    Surface *s = wl_resource_get_user_data(r);
    if (S.surface == s)
        S.surface = NULL;
    free(s);
}

/* ---- wl_compositor, wl_region ---- */

static void region_destroy_req(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct wl_region_interface region_impl = {
    .destroy = region_destroy_req,
    .add = (void *)unused_request,
    .subtract = (void *)unused_request,
};

static void compositor_create_surface(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    Surface *s = calloc(1, sizeof *s);
    s->res = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->res, &surface_impl, s, surface_gone);
    if (!S.surface)
        S.surface = s;
}

static void compositor_create_region(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *reg = wl_resource_create(c, &wl_region_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(reg, &region_impl, NULL, NULL);
}

static const struct wl_compositor_interface compositor_impl = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)version, id);
    wl_resource_set_implementation(r, &compositor_impl, NULL, NULL);
}

/* ---- xdg-shell ---- */

static void xdg_destroy_req(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct xdg_toplevel_interface toplevel_impl = {
    .destroy = xdg_destroy_req,
    .set_parent = (void *)unused_request,
    .set_title = (void *)unused_request,
    .set_app_id = (void *)unused_request,
    .show_window_menu = (void *)unused_request,
    .move = (void *)unused_request,
    .resize = (void *)unused_request,
    .set_max_size = (void *)unused_request,
    .set_min_size = (void *)unused_request,
    .set_maximized = (void *)unused_request,
    .unset_maximized = (void *)unused_request,
    .set_fullscreen = (void *)unused_request,
    .unset_fullscreen = (void *)unused_request,
    .set_minimized = (void *)unused_request,
};

static void xdg_surface_get_toplevel_req(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    Surface *s = wl_resource_get_user_data(r);
    s->toplevel = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->toplevel, &toplevel_impl, s, NULL);
}

static const struct xdg_surface_interface xdg_surface_impl = {
    .destroy = xdg_destroy_req,
    .get_toplevel = xdg_surface_get_toplevel_req,
    .get_popup = (void *)unused_request,
    .set_window_geometry = (void *)unused_request,
    .ack_configure = (void *)unused_request,
};

static void wm_get_xdg_surface(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *surface)
{
    Surface *s = wl_resource_get_user_data(surface);
    s->xdg = wl_resource_create(c, &xdg_surface_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(s->xdg, &xdg_surface_impl, s, NULL);
}

static const struct xdg_wm_base_interface wm_base_impl = {
    .destroy = xdg_destroy_req,
    .create_positioner = (void *)unused_request,
    .get_xdg_surface = wm_get_xdg_surface,
    .pong = (void *)unused_request,
};

static void wm_base_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, (int)version, id);
    wl_resource_set_implementation(r, &wm_base_impl, NULL, NULL);
}

/* ---- wl_seat ---- */

static void release_req(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static const struct wl_keyboard_interface keyboard_impl = {.release = release_req};
static void pointer_set_cursor(struct wl_client *c, struct wl_resource *r, uint32_t serial,
                               struct wl_resource *surface, int32_t hot_x, int32_t hot_y)
{
    (void)c;
    (void)r;
    S.cursor_requests++;
    S.cursor_serial = serial;
    S.cursor_has_surface = surface != NULL;
    S.cursor_hot_x = hot_x;
    S.cursor_hot_y = hot_y;
    S.cursor_surface = surface;
    S.cursor_w = S.cursor_h = 0;
    S.cursor_pixel = 0;
    if (surface)
    {
        Surface *s = wl_resource_get_user_data(surface);
        if (s->shown)
        {
            S.cursor_w = s->shown_w;
            S.cursor_h = s->shown_h;
            S.cursor_pixel = s->shown_pixel;
        }
    }
}

static const struct wl_pointer_interface pointer_impl = {.set_cursor = pointer_set_cursor,
                                                         .release = release_req};

static void keyboard_gone(struct wl_resource *r)
{
    if (S.keyboard == r)
        S.keyboard = NULL;
}

static void pointer_gone(struct wl_resource *r)
{
    if (S.pointer == r)
        S.pointer = NULL;
}

static void send_keymap(void)
{
    if (!S.keyboard || !S.keymap)
        return;
    char *text = xkb_keymap_get_as_string(S.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(text) + 1;
    int fd = memfd_create("keymap", MFD_CLOEXEC);
    if (fd >= 0 && write(fd, text, size) == (ssize_t)size)
        wl_keyboard_send_keymap(S.keyboard, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)size);
    if (fd >= 0)
        close(fd);
    free(text);
}

static void send_mods(void)
{
    if (!S.keyboard || !S.state)
        return;
    wl_keyboard_send_modifiers(S.keyboard, ++S.serial, xkb_state_serialize_mods(S.state, XKB_STATE_MODS_DEPRESSED),
                               xkb_state_serialize_mods(S.state, XKB_STATE_MODS_LATCHED),
                               xkb_state_serialize_mods(S.state, XKB_STATE_MODS_LOCKED),
                               xkb_state_serialize_layout(S.state, XKB_STATE_LAYOUT_EFFECTIVE));
}

static bool set_layout(int i)
{
    struct xkb_rule_names names = {.rules = "evdev", .model = "pc105", .layout = layouts[i].layout,
                                   .variant = layouts[i].variant};
    struct xkb_keymap *map = xkb_keymap_new_from_names(S.xkb, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!map)
        return false;
    struct xkb_state *state = xkb_state_new(map);
    xkb_state_unref(S.state);
    xkb_keymap_unref(S.keymap);
    S.keymap = map;
    S.state = state;
    send_keymap();
    return true;
}

static void seat_get_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    S.pointer = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(S.pointer, &pointer_impl, NULL, pointer_gone);
}

static void seat_get_keyboard(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    S.keyboard = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(S.keyboard, &keyboard_impl, NULL, keyboard_gone);
    send_keymap();
    if (wl_resource_get_version(S.keyboard) >= WL_KEYBOARD_REPEAT_INFO_SINCE_VERSION)
        wl_keyboard_send_repeat_info(S.keyboard, 40, 120); /* 40 per second after 120 ms */
}

static const struct wl_seat_interface seat_impl = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = (void *)unused_request,
    .release = release_req,
};

static void seat_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_seat_interface, (int)version, id);
    wl_resource_set_implementation(r, &seat_impl, NULL, NULL);
    wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}

#ifdef ZEN_WL_POINTER_LOCK
static void locked_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    S.lock_destroyed = true;
    S.locked = NULL;
    wl_resource_destroy(r);
}

static void locked_set_hint(struct wl_client *c, struct wl_resource *r, wl_fixed_t x, wl_fixed_t y)
{
    (void)c;
    (void)r;
    S.hint_x = wl_fixed_to_int(x);
    S.hint_y = wl_fixed_to_int(y);
}

static const struct zwp_locked_pointer_v1_interface locked_impl = {
    .destroy = locked_destroy,
    .set_cursor_position_hint = locked_set_hint,
    .set_region = (void *)unused_request,
};

static void constraints_lock_pointer(struct wl_client *c, struct wl_resource *r, uint32_t id,
                                     struct wl_resource *surface, struct wl_resource *pointer,
                                     struct wl_resource *region, uint32_t lifetime)
{
    (void)surface;
    (void)pointer;
    (void)region;
    (void)lifetime;
    S.lock_requests++;
    S.lock_destroyed = false;
    S.locked = wl_resource_create(c, &zwp_locked_pointer_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(S.locked, &locked_impl, NULL, NULL);
}

static const struct zwp_pointer_constraints_v1_interface constraints_impl = {
    .destroy = release_req,
    .lock_pointer = constraints_lock_pointer,
    .confine_pointer = (void *)unused_request,
};

static void constraints_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_pointer_constraints_v1_interface, (int)version, id);
    wl_resource_set_implementation(r, &constraints_impl, NULL, NULL);
}

static void relative_gone(struct wl_resource *r)
{
    if (S.relative == r)
        S.relative = NULL;
}

static const struct zwp_relative_pointer_v1_interface relative_impl = {.destroy = release_req};

static void manager_get_relative(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *pointer)
{
    (void)pointer;
    S.relative = wl_resource_create(c, &zwp_relative_pointer_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(S.relative, &relative_impl, NULL, relative_gone);
}

static const struct zwp_relative_pointer_manager_v1_interface manager_impl = {
    .destroy = release_req,
    .get_relative_pointer = manager_get_relative,
};

static void manager_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_relative_pointer_manager_v1_interface, (int)version, id);
    wl_resource_set_implementation(r, &manager_impl, NULL, NULL);
}
#endif

/* ---- the script ---- */

static void run_cmd(const Cmd *c)
{
    struct wl_array none;
    switch (c->type)
    {
    case CMD_LAYOUT:
        set_layout(c->a);
        break;
    case CMD_KB_ENTER:
        if (S.keyboard && S.surface)
        {
            wl_array_init(&none);
            wl_keyboard_send_enter(S.keyboard, ++S.serial, S.surface->res, &none);
            wl_array_release(&none);
            send_mods();
        }
        break;
    case CMD_KB_LEAVE:
        if (S.keyboard && S.surface)
            wl_keyboard_send_leave(S.keyboard, ++S.serial, S.surface->res);
        break;
    case CMD_KEY:
        if (S.keyboard && S.state)
        {
            wl_keyboard_send_key(S.keyboard, ++S.serial, 0, (uint32_t)c->a,
                                 c->b ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED);
            xkb_state_update_key(S.state, (xkb_keycode_t)c->a + 8, c->b ? XKB_KEY_DOWN : XKB_KEY_UP);
            send_mods();
        }
        break;
    case CMD_PTR_ENTER:
        if (S.pointer && S.surface)
        {
            S.enter_serial = ++S.serial;
            wl_pointer_send_enter(S.pointer, S.enter_serial, S.surface->res, wl_fixed_from_int(c->a),
                                  wl_fixed_from_int(c->b));
        }
        break;
    case CMD_PTR_LEAVE:
        if (S.pointer && S.surface)
            wl_pointer_send_leave(S.pointer, ++S.serial, S.surface->res);
        break;
    case CMD_PTR_MOVE:
        if (S.pointer)
            wl_pointer_send_motion(S.pointer, 0, wl_fixed_from_int(c->a), wl_fixed_from_int(c->b));
        break;
    case CMD_PTR_BUTTON:
        if (S.pointer)
            wl_pointer_send_button(S.pointer, ++S.serial, 0, (uint32_t)c->a,
                                   c->b ? WL_POINTER_BUTTON_STATE_PRESSED : WL_POINTER_BUTTON_STATE_RELEASED);
        break;
    case CMD_PTR_AXIS:
        if (S.pointer)
            wl_pointer_send_axis(S.pointer, 0, (uint32_t)c->a, wl_fixed_from_int(c->b));
        break;
    case CMD_PTR_V120:
        if (S.pointer)
            wl_pointer_send_axis_value120(S.pointer, (uint32_t)c->a, c->b);
        break;
    case CMD_PTR_FRAME:
        if (S.pointer)
            wl_pointer_send_frame(S.pointer);
        break;
    case CMD_CONFIGURE:
        if (S.surface && S.surface->configured)
            send_configure(S.surface, c->a, c->b, (uint32_t)c->c);
        break;
#ifdef ZEN_WL_POINTER_LOCK
    case CMD_LOCK:
        if (S.locked)
            zwp_locked_pointer_v1_send_locked(S.locked);
        break;
    case CMD_UNLOCK:
        if (S.locked)
            zwp_locked_pointer_v1_send_unlocked(S.locked);
        break;
    case CMD_REL_MOTION:
        if (S.relative)
            zwp_relative_pointer_v1_send_relative_motion(S.relative, 0, 0, wl_fixed_from_int(c->a * 2),
                                                         wl_fixed_from_int(c->b * 2), wl_fixed_from_int(c->a),
                                                         wl_fixed_from_int(c->b));
        break;
#endif
    case CMD_QUIT:
        wl_display_terminate(S.display);
        break;
    }
}

static int on_cmd(int fd, uint32_t mask, void *data)
{
    Cmd c;
    (void)mask;
    (void)data;
    while (read(fd, &c, sizeof c) == (ssize_t)sizeof c)
        run_cmd(&c);
    return 0;
}

static void *server_main(void *arg)
{
    (void)arg;
    wl_display_run(S.display);
    return NULL;
}

static void cmd(int type, int a, int b, int c, int d)
{
    Cmd m = {type, a, b, c, d};
    if (write(S.cmd[1], &m, sizeof m) != (ssize_t)sizeof m)
        g_fail++;
}

static bool start_compositor(void)
{
    if (!getenv("XDG_RUNTIME_DIR"))
    {
        char tmpl[] = "/tmp/zen-wl-XXXXXX";
        char *dir = mkdtemp(tmpl);
        if (!dir)
            return false;
        setenv("XDG_RUNTIME_DIR", dir, 1);
    }
    S.xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    S.display = wl_display_create();
    if (!S.xkb || !S.display || !set_layout(0))
        return false;
    static char name[64];
    snprintf(name, sizeof name, "zen-test-%d", (int)getpid());
    if (wl_display_add_socket(S.display, name) != 0 || wl_display_init_shm(S.display) != 0)
        return false;

    wl_global_create(S.display, &wl_compositor_interface, 4, NULL, compositor_bind);
    wl_global_create(S.display, &xdg_wm_base_interface, 5, NULL, wm_base_bind);
    wl_global_create(S.display, &wl_seat_interface, 8, NULL, seat_bind);
#ifdef ZEN_WL_POINTER_LOCK
    wl_global_create(S.display, &zwp_pointer_constraints_v1_interface, 1, NULL, constraints_bind);
    wl_global_create(S.display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, manager_bind);
#endif

    if (pipe2(S.cmd, O_NONBLOCK | O_CLOEXEC) != 0)
        return false;
    S.cmd_source = wl_event_loop_add_fd(wl_display_get_event_loop(S.display), S.cmd[0], WL_EVENT_READABLE, on_cmd,
                                        NULL);

    setenv("WAYLAND_DISPLAY", name, 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1); /* this compositor has no buffer sharing, only wl_shm */
    setenv("LC_ALL", "en_US.UTF-8", 1);      /* the Compose table of the test */
    return pthread_create(&S.thread, NULL, server_main, NULL) == 0;
}

/* ========================================================================== */
/*  what the client heard                                                     */
/* ========================================================================== */

typedef struct
{
    EventType type;
    int key, scancode, mods, button, x, y, w, h;
    bool down, repeat, flag;
    uint32_t cp;
    float wx, wy;
} Rec;

#define REC_MAX 2048
static Rec rec[REC_MAX];
static int nrec;
static int marker = 600;

static void frame(PlatformWindow *w)
{
    window_begin_frame(w);
    Event e;
    while (poll_event(w, &e))
    {
        if (nrec >= REC_MAX)
            continue;
        Rec *r = &rec[nrec++];
        memset(r, 0, sizeof *r);
        r->type = e.type;
        switch (e.type)
        {
        case EVENT_KEY:
            r->key = e.data.key.key;
            r->scancode = e.data.key.scancode;
            r->mods = e.data.key.mods;
            r->down = e.data.key.down;
            r->repeat = e.data.key.repeat;
            break;
        case EVENT_CHAR:
            r->cp = e.data.codepoint;
            break;
        case EVENT_MOUSE_MOVE:
            r->x = e.data.mouse.x;
            r->y = e.data.mouse.y;
            break;
        case EVENT_MOUSE_BUTTON:
            r->button = e.data.mouse.button;
            r->down = e.data.mouse.down;
            r->x = e.data.mouse.x;
            r->y = e.data.mouse.y;
            break;
        case EVENT_MOUSE_WHEEL:
            r->wx = e.data.wheel.x;
            r->wy = e.data.wheel.y;
            break;
        case EVENT_WINDOW_ENTER:
            r->flag = e.data.enter.entered;
            break;
        case EVENT_WINDOW_FOCUS:
            r->flag = e.data.focus.gained;
            break;
        case EVENT_WINDOW_RESIZE:
        case EVENT_WINDOW_FB_RESIZE:
            r->w = e.data.resize.w;
            r->h = e.data.resize.h;
            break;
        default:
            break;
        }
    }
}

static void pump(PlatformWindow *w, int ms)
{
    for (int t = 0; t < ms; t += 2)
    {
        frame(w);
        usleep(2000);
    }
}

/* One round: everything sent before is heard once a configure of a size nobody
   else uses comes back, since the connection keeps the order. */
static bool round_trip(PlatformWindow *w)
{
    int size = ++marker;
    cmd(CMD_CONFIGURE, size, size - 100, 4, 0);
    for (int t = 0; t < 3000; t += 2)
    {
        frame(w);
        for (int i = 0; i < nrec; i++)
        {
            if (rec[i].type == EVENT_WINDOW_RESIZE && rec[i].w == size)
                return true;
        }
        usleep(2000);
    }
    return false;
}

/* Two rounds: what the client sent in answer to the first (a cursor request) has
   been read by the compositor thread once the second comes back. */
static bool settle(PlatformWindow *w)
{
    return round_trip(w) && round_trip(w);
}

static void reset(void)
{
    nrec = 0;
}

static int count(EventType type)
{
    int n = 0;
    for (int i = 0; i < nrec; i++)
        n += rec[i].type == type;
    return n;
}

/* The n-th record of a type, or NULL. */
static const Rec *nth(EventType type, int n)
{
    for (int i = 0; i < nrec; i++)
    {
        if (rec[i].type == type && n-- == 0)
            return &rec[i];
    }
    return NULL;
}

static int count_key(int key, bool down, bool repeat)
{
    int n = 0;
    for (int i = 0; i < nrec; i++)
        n += rec[i].type == EVENT_KEY && rec[i].key == key && rec[i].down == down && rec[i].repeat == repeat;
    return n;
}

static int count_char(uint32_t cp)
{
    int n = 0;
    for (int i = 0; i < nrec; i++)
        n += rec[i].type == EVENT_CHAR && rec[i].cp == cp;
    return n;
}

/* evdev codes */
enum
{
    EV_ESC = 1, EV_1 = 2, EV_APOSTROPHE = 40, EV_A = 30, EV_Q = 16, EV_LCTRL = 29, EV_LSHIFT = 42,
    EV_CAPS = 58, EV_F1 = 59, EV_LEFT = 105, EV_KP5 = 76, EV_NUMLOCK = 69
};

static void tap(int code)
{
    cmd(CMD_KEY, code, 1, 0, 0);
    cmd(CMD_KEY, code, 0, 0, 0);
}

/* ========================================================================== */
/*  tests                                                                     */
/* ========================================================================== */

static void test_keys(PlatformWindow *w)
{
    reset();
    cmd(CMD_LAYOUT, 0, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    tap(EV_A);
    CHECK(settle(w));
    const Rec *k = nth(EVENT_KEY, 0);
    CHECK(k && k->key == KEY_A && k->down && !k->repeat && k->mods == 0);
    CHECK(k && k->scancode == EV_A + 8); /* the X11 numbering */
    CHECK(count_key(KEY_A, false, false) == 1);
    CHECK(count_char('a') == 1);

    /* Shift is a modifier of the keys after it, and moves the text, not the key */
    reset();
    cmd(CMD_KEY, EV_LSHIFT, 1, 0, 0);
    tap(EV_A);
    tap(EV_1);
    cmd(CMD_KEY, EV_LSHIFT, 0, 0, 0);
    CHECK(settle(w));
    CHECK(count_key(KEY_LEFT_SHIFT, true, false) == 1);
    const Rec *shifted = NULL;
    for (int i = 0; i < nrec; i++)
    {
        if (rec[i].type == EVENT_KEY && rec[i].key == KEY_A && rec[i].down)
            shifted = &rec[i];
    }
    CHECK(shifted && (shifted->mods & KEYMOD_SHIFT));
    CHECK(count_char('A') == 1);
    CHECK(count_key(KEY_ZERO + 1, true, false) == 1);
    CHECK(count_char('!') == 1);

    /* Control makes no text */
    reset();
    cmd(CMD_KEY, EV_LCTRL, 1, 0, 0);
    tap(EV_A);
    cmd(CMD_KEY, EV_LCTRL, 0, 0, 0);
    CHECK(settle(w));
    CHECK(count(EVENT_CHAR) == 0);
    const Rec *ctrl_a = NULL;
    for (int i = 0; i < nrec; i++)
    {
        if (rec[i].type == EVENT_KEY && rec[i].key == KEY_A && rec[i].down)
            ctrl_a = &rec[i];
    }
    CHECK(ctrl_a && (ctrl_a->mods & KEYMOD_CTRL));

    /* the keys that are not letters */
    reset();
    tap(EV_ESC);
    tap(EV_F1);
    tap(EV_LEFT);
    tap(EV_KP5);
    CHECK(settle(w));
    CHECK(count_key(KEY_ESCAPE, true, false) == 1);
    CHECK(count_key(KEY_F1, true, false) == 1);
    CHECK(count_key(KEY_LEFT, true, false) == 1);
    CHECK(count_key(KEY_KP_5, true, false) == 1);
    CHECK(count(EVENT_CHAR) == 0 || count_char('5') == 1);

    /* a key held in the window is down for the core as well */
    reset();
    cmd(CMD_KEY, EV_A, 1, 0, 0);
    CHECK(settle(w));
    CHECK(key_down(w, KEY_A));
    cmd(CMD_KEY, EV_A, 0, 0, 0);
    CHECK(settle(w));
    CHECK(!key_down(w, KEY_A));
}

static void test_layout(PlatformWindow *w)
{
    /* AZERTY: the key at the Q position of a US board is an A */
    reset();
    cmd(CMD_LAYOUT, 1, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    tap(EV_Q);
    tap(EV_A);
    CHECK(settle(w));
    CHECK(count_key(KEY_A, true, false) == 1);
    CHECK(count_key(KEY_Q, true, false) == 1);
    CHECK(count_char('a') == 1);
    CHECK(count_char('q') == 1);
    const Rec *first = nth(EVENT_KEY, 0);
    CHECK(first && first->scancode == EV_Q + 8); /* the position is the scancode */

    cmd(CMD_LAYOUT, 0, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    CHECK(settle(w));
}

static void test_repeat(PlatformWindow *w)
{
    /* the compositor repeats nothing: 40 per second after 120 ms is made here */
    reset();
    cmd(CMD_KEY, EV_A, 1, 0, 0);
    pump(w, 500);
    int repeats = count_key(KEY_A, true, true);
    CHECK(count_key(KEY_A, true, false) == 1);
    CHECK(repeats >= 5);
    CHECK(count_char('a') == 1 + repeats);
    const Rec *first = nth(EVENT_KEY, 0);
    CHECK(first && !first->repeat);

    cmd(CMD_KEY, EV_A, 0, 0, 0);
    CHECK(settle(w));
    int after_release = count_key(KEY_A, true, true);
    pump(w, 150);
    CHECK(count_key(KEY_A, true, true) == after_release); /* the release ends it */

    /* a modifier does not repeat */
    reset();
    cmd(CMD_KEY, EV_LSHIFT, 1, 0, 0);
    pump(w, 400);
    CHECK(count_key(KEY_LEFT_SHIFT, true, true) == 0);
    cmd(CMD_KEY, EV_LSHIFT, 0, 0, 0);
    CHECK(settle(w));

    /* the repeat stops when the window loses the keyboard */
    reset();
    cmd(CMD_KEY, EV_A, 1, 0, 0);
    pump(w, 300);
    CHECK(count_key(KEY_A, true, true) >= 1);
    cmd(CMD_KB_LEAVE, 0, 0, 0, 0);
    CHECK(settle(w));
    int at_leave = count_key(KEY_A, true, true);
    pump(w, 200);
    CHECK(count_key(KEY_A, true, true) == at_leave);
    cmd(CMD_KEY, EV_A, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    CHECK(settle(w));
}

static void test_compose(PlatformWindow *w)
{
    if (access("/usr/share/X11/locale/en_US.UTF-8/Compose", R_OK) != 0)
        return;
    /* US international: the apostrophe key is a dead acute, and acute + a is an a-acute */
    reset();
    cmd(CMD_LAYOUT, 2, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    tap(EV_APOSTROPHE);
    tap(EV_A);
    CHECK(settle(w));
    CHECK(count_char(0xE1) == 1);
    CHECK(count_char('a') == 0);
    CHECK(count(EVENT_CHAR) == 1);

    cmd(CMD_LAYOUT, 0, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    CHECK(settle(w));
}

static void test_locks(PlatformWindow *w)
{
    CHECK(!(key_mods(w) & KEYMOD_CAPS_LOCK));
    reset();
    tap(EV_CAPS);
    CHECK(settle(w));
    CHECK(key_mods(w) & KEYMOD_CAPS_LOCK);

    tap(EV_A);
    CHECK(settle(w));
    CHECK(count_char('A') == 1);

    tap(EV_CAPS);
    CHECK(settle(w));
    CHECK(!(key_mods(w) & KEYMOD_CAPS_LOCK));

    tap(EV_NUMLOCK);
    CHECK(settle(w));
    CHECK(key_mods(w) & KEYMOD_NUM_LOCK);
    tap(EV_NUMLOCK);
    CHECK(settle(w));
    CHECK(!(key_mods(w) & KEYMOD_NUM_LOCK));
}

#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#define BTN_SIDE 0x113
#define BTN_EXTRA 0x114

static void test_pointer(PlatformWindow *w)
{
    reset();
    cmd(CMD_PTR_ENTER, 10, 20, 0, 0);
    cmd(CMD_PTR_MOVE, 30, 40, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_LEFT, 1, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_LEFT, 0, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_RIGHT, 1, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_MIDDLE, 1, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_SIDE, 1, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_EXTRA, 1, 0, 0);
    CHECK(settle(w));

    const Rec *in = nth(EVENT_WINDOW_ENTER, 0);
    CHECK(in && in->flag);
    CHECK(window_is_hovered(w));
    const Rec *m0 = nth(EVENT_MOUSE_MOVE, 0);
    const Rec *m1 = nth(EVENT_MOUSE_MOVE, 1);
    CHECK(m0 && m0->x == 10 && m0->y == 20); /* the entry is a move too */
    CHECK(m1 && m1->x == 30 && m1->y == 40);
    CHECK(mouse_x(w) == 30 && mouse_y(w) == 40);

    const Rec *b0 = nth(EVENT_MOUSE_BUTTON, 0);
    const Rec *b1 = nth(EVENT_MOUSE_BUTTON, 1);
    CHECK(b0 && b0->button == MOUSE_LEFT && b0->down && b0->x == 30 && b0->y == 40);
    CHECK(b1 && b1->button == MOUSE_LEFT && !b1->down);
    const Rec *b2 = nth(EVENT_MOUSE_BUTTON, 2);
    const Rec *b3 = nth(EVENT_MOUSE_BUTTON, 3);
    const Rec *b4 = nth(EVENT_MOUSE_BUTTON, 4);
    const Rec *b5 = nth(EVENT_MOUSE_BUTTON, 5);
    CHECK(b2 && b2->button == MOUSE_RIGHT);
    CHECK(b3 && b3->button == MOUSE_MIDDLE);
    CHECK(b4 && b4->button == MOUSE_X1);
    CHECK(b5 && b5->button == MOUSE_X2);

    reset();
    cmd(CMD_PTR_BUTTON, BTN_RIGHT, 0, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_MIDDLE, 0, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_SIDE, 0, 0, 0);
    cmd(CMD_PTR_BUTTON, BTN_EXTRA, 0, 0, 0);
    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0);
    cmd(CMD_PTR_MOVE, 99, 99, 0, 0); /* after the leave: nobody has the pointer */
    CHECK(settle(w));
    const Rec *out = nth(EVENT_WINDOW_ENTER, 0);
    CHECK(out && !out->flag);
    CHECK(!window_is_hovered(w));
    CHECK(count(EVENT_MOUSE_MOVE) == 0);
}

static void test_wheel(PlatformWindow *w)
{
    cmd(CMD_PTR_ENTER, 5, 5, 0, 0);
    CHECK(settle(w));

    /* a notch up: one step, positive, as on X11 */
    reset();
    cmd(CMD_PTR_V120, 0, -120, 0, 0);
    cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(count(EVENT_MOUSE_WHEEL) == 1);
    const Rec *up = nth(EVENT_MOUSE_WHEEL, 0);
    CHECK(up && up->wy == 1.0f && up->wx == 0.0f);

    /* a notch down, and one to the right */
    reset();
    cmd(CMD_PTR_V120, 0, 120, 0, 0);
    cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
    cmd(CMD_PTR_V120, 1, 120, 0, 0);
    cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
    CHECK(settle(w));
    const Rec *down = nth(EVENT_MOUSE_WHEEL, 0);
    const Rec *right = nth(EVENT_MOUSE_WHEEL, 1);
    CHECK(down && down->wy == -1.0f);
    CHECK(right && right->wx == 1.0f && right->wy == 0.0f);

    /* a wheel sends the axis value and the notch of the same movement: counted once */
    reset();
    cmd(CMD_PTR_AXIS, 0, 10, 0, 0);
    cmd(CMD_PTR_V120, 0, 120, 0, 0);
    cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(count(EVENT_MOUSE_WHEEL) == 1);
    const Rec *both = nth(EVENT_MOUSE_WHEEL, 0);
    CHECK(both && both->wy == -1.0f);

    /* a touchpad: continuous, 10 units to the step */
    reset();
    cmd(CMD_PTR_AXIS, 0, 25, 0, 0);
    cmd(CMD_PTR_FRAME, 0, 0, 0, 0);
    CHECK(settle(w));
    const Rec *smooth = nth(EVENT_MOUSE_WHEEL, 0);
    CHECK(smooth && smooth->wy == -2.5f);

    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0);
    CHECK(settle(w));
}

/* What the client says about the cursor is read from the compositor's side once
   everything before it has been delivered. The server threads write these, so a
   settled client is the point at which they are read. */
static void test_cursor(PlatformWindow *w)
{
    /* entering the window sets a cursor, quoting that enter */
    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0);
    CHECK(settle(w));
    int before = S.cursor_requests;
    mouse_set_cursor(w, CURSOR_DEFAULT);
    cmd(CMD_PTR_ENTER, 5, 5, 0, 0);
    CHECK(settle(w));
    CHECK(S.cursor_requests > before);
    CHECK(S.cursor_serial == S.enter_serial);
    /* the theme may be missing on the machine; when it is there the cursor is a surface */
    bool theme = S.cursor_has_surface;
    if (theme)
        CHECK(S.cursor_w > 0 && S.cursor_h > 0);

    /* a cursor from an image: its size, hot spot and premultiplied pixels */
    uint32_t px[4 * 3];
    for (int i = 0; i < 12; i++)
        px[i] = 0x80FF0000u; /* half-transparent red, not premultiplied */
    PlatformCursor *c = cursor_create(px, 4, 3, 1, 2);
    CHECK(c != NULL);
    if (c)
    {
        mouse_set_cursor_image(w, c);
        CHECK(settle(w));
        CHECK(S.cursor_has_surface);
        CHECK(S.cursor_hot_x == 1 && S.cursor_hot_y == 2);
        CHECK(S.cursor_w == 4 && S.cursor_h == 3);
        CHECK(S.cursor_pixel == 0x80800000u);
        CHECK(S.cursor_serial == S.enter_serial);

        /* a shape takes the place of the image, and the image can be asked back */
        mouse_set_cursor(w, CURSOR_IBEAM);
        CHECK(settle(w));
        if (theme)
            CHECK(!(S.cursor_w == 4 && S.cursor_h == 3));
        mouse_set_cursor_image(w, c);
        CHECK(settle(w));
        CHECK(S.cursor_w == 4 && S.cursor_h == 3);
        mouse_set_cursor_image(w, NULL);
        CHECK(settle(w));
        cursor_destroy(c);
    }

    /* hidden: the pointer is given no surface */
    mouse_set_mode(w, MOUSE_MODE_HIDDEN);
    CHECK(settle(w));
    CHECK(!S.cursor_has_surface);
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    CHECK(settle(w));
    if (theme)
        CHECK(S.cursor_has_surface);

    /* a new enter asks again */
    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0);
    cmd(CMD_PTR_ENTER, 6, 6, 0, 0);
    before = S.cursor_requests;
    CHECK(settle(w));
    CHECK(S.cursor_requests > before);
    CHECK(S.cursor_serial == S.enter_serial);
    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0);
    CHECK(settle(w));
}

#ifdef ZEN_WL_POINTER_LOCK
/* Captured: hidden and locked, and the motion comes as raw deltas on a position
   that does not stop at an edge. */
static void test_captured(PlatformWindow *w)
{
    cmd(CMD_PTR_ENTER, 100, 80, 0, 0);
    CHECK(settle(w));

    int before = S.lock_requests;
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    CHECK(settle(w));
    CHECK(S.lock_requests == before + 1);
    CHECK(S.locked != NULL && S.relative != NULL);
    CHECK(!S.cursor_has_surface); /* hidden */

    /* motion before the compositor says locked is still absolute */
    cmd(CMD_LOCK, 0, 0, 0, 0);
    CHECK(settle(w));

    reset();
    cmd(CMD_REL_MOTION, 7, -3, 0, 0);
    cmd(CMD_REL_MOTION, 5, 2, 0, 0);
    CHECK(settle(w));
    CHECK(count(EVENT_MOUSE_MOVE) == 2);
    const Rec *m0 = nth(EVENT_MOUSE_MOVE, 0);
    const Rec *m1 = nth(EVENT_MOUSE_MOVE, 1);
    CHECK(m0 && m0->x == 107 && m0->y == 77);
    CHECK(m1 && m1->x == 112 && m1->y == 79);
    CHECK(mouse_x(w) == 112 && mouse_y(w) == 79);

    /* far past the window: there is no edge to stop it */
    reset();
    for (int i = 0; i < 30; i++)
        cmd(CMD_REL_MOTION, 50, 50, 0, 0);
    CHECK(settle(w));
    CHECK(mouse_x(w) == 112 + 1500 && mouse_y(w) == 79 + 1500);

    /* leaving the mode gives the pointer back, at the place the lock began */
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    CHECK(settle(w));
    CHECK(S.lock_destroyed);
    CHECK(S.hint_x == 100 && S.hint_y == 80);

    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0);
    CHECK(settle(w));
}
#endif

enum
{
    ST_MAXIMIZED = 1,
    ST_FULLSCREEN = 2,
    ST_ACTIVATED = 4
};

/* What the compositor says about the window is what the application hears. */
static void test_configure(PlatformWindow *w)
{
    int w0, h0;
    window_get_size(w, &w0, &h0);

    reset();
    cmd(CMD_CONFIGURE, 500, 400, ST_MAXIMIZED | ST_ACTIVATED, 0);
    pump(w, 100);
    CHECK(window_is_maximized(w));
    CHECK(count(EVENT_WINDOW_MAXIMIZE) == 1);
    int cw, ch;
    window_get_size(w, &cw, &ch);
    CHECK(cw == 500 && ch == 400);
    window_get_framebuffer_size(w, &cw, &ch);
    CHECK(cw == 500 && ch == 400);
    const Rec *fb = nth(EVENT_WINDOW_FB_RESIZE, 0);
    CHECK(fb && fb->w == 500 && fb->h == 400);

    /* a zero size is the client's choice: the one from before the maximize */
    reset();
    cmd(CMD_CONFIGURE, 0, 0, ST_ACTIVATED, 0);
    pump(w, 100);
    CHECK(!window_is_maximized(w));
    CHECK(count(EVENT_WINDOW_RESTORE) == 1);
    window_get_size(w, &cw, &ch);
    CHECK(cw == w0 && ch == h0);

    reset();
    cmd(CMD_CONFIGURE, 800, 600, ST_FULLSCREEN | ST_ACTIVATED, 0);
    pump(w, 100);
    CHECK(window_get_mode(w) == WINDOW_FULLSCREEN); /* the compositor did it, not the application */
    window_get_size(w, &cw, &ch);
    CHECK(cw == 800 && ch == 600);

    cmd(CMD_CONFIGURE, 0, 0, ST_ACTIVATED, 0);
    pump(w, 100);
    CHECK(window_get_mode(w) == WINDOW_WINDOWED);
    window_get_size(w, &cw, &ch);
    CHECK(cw == w0 && ch == h0);

    /* focus follows the activated state */
    reset();
    cmd(CMD_CONFIGURE, w0, h0, 0, 0);
    pump(w, 100);
    CHECK(!window_is_focused(w));
    const Rec *lost = nth(EVENT_WINDOW_FOCUS, 0);
    CHECK(lost && !lost->flag);
    cmd(CMD_CONFIGURE, w0, h0, ST_ACTIVATED, 0);
    pump(w, 100);
    CHECK(window_is_focused(w));
}

int main(void)
{
    if (!start_compositor())
    {
        printf("SKIP the test compositor did not start\n");
        return 77;
    }
    if (!platform_init())
    {
        printf("SKIP %s\n", platform_get_error());
        return 77;
    }

    WindowConfig cfg = {.title = "test_input_wayland", .width = 320, .height = 240, .resizable = true};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("FAIL window_create: %s\n", platform_get_error());
        return 1;
    }
    for (int i = 0; i < 20; i++)
    {
        frame(w);
        usleep(2000);
    }

    test_keys(w);
    test_layout(w);
    test_repeat(w);
    test_compose(w);
    test_locks(w);
    test_pointer(w);
    test_wheel(w);
    test_cursor(w);
#ifdef ZEN_WL_POINTER_LOCK
    test_captured(w);
#endif
    test_configure(w);

    window_destroy(w);
    platform_shutdown();
    cmd(CMD_QUIT, 0, 0, 0, 0);
    pthread_join(S.thread, NULL);
    wl_event_source_remove(S.cmd_source);
    wl_display_destroy(S.display);
    close(S.cmd[0]);
    close(S.cmd[1]);
    xkb_state_unref(S.state);
    xkb_keymap_unref(S.keymap);
    xkb_context_unref(S.xkb);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
