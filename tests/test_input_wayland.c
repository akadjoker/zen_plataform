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
#ifdef ZEN_WL_PRIMARY
#include "primary-selection-unstable-v1-server-protocol.h"
#endif

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include <errno.h>
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
    bool bare; /* send the keymap with no NUL, padded to whole pages */
    struct xkb_state *state;

    /* the last wl_pointer.set_cursor, and what the cursor surface showed with it */
    int cursor_requests;
    uint32_t cursor_serial;
    bool cursor_has_surface;
    int cursor_hot_x, cursor_hot_y, cursor_w, cursor_h;
    uint32_t cursor_pixel; /* the first pixel of the buffer, as stored */
    struct wl_resource *cursor_surface;

    uint32_t enter_serial;
#ifdef ZEN_WL_PRIMARY
    /* the primary selection: the compositor is the other program. p_text is what it offers as the selection of "another
       client"; what the client offers comes back by the pipe p_fetch asks for. p_lock guards what the test thread reads. */
    bool p_advertise;
    struct wl_resource *p_device, *p_source;
    int p_sets, p_clears, p_receives;
    uint32_t p_serial;
    int p_mode; /* 0: serve p_text, 1: take the pipe and never write or close it */
    int p_held_fd;
    char *p_text;
    size_t p_text_len;
    char p_mime[64];
    pthread_mutex_t p_lock;
    char *p_got;
    size_t p_got_len;
    bool p_got_done;
    int p_got_fd;
    struct wl_event_source *p_got_src;
    char p_sent_mime[64];
#endif

    /* a drag of files over the window: the client's data device, the offer in hand, and what the client asked of it */
    struct wl_resource *data_device, *offer;
    int dnd_accepts, dnd_receives, dnd_finished, dnd_offers_gone;
    char dnd_accepted[64]; /* the MIME type of the last accept, "" for a refusal */
    uint32_t dnd_actions, dnd_preferred;
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
    CMD_PRIMARY_OFFER, /* another program takes the selection: a = mode (0 serve, 1 stall) */
    CMD_PRIMARY_DROP,  /* the selection has no owner */
    CMD_PRIMARY_FETCH, /* read what the client offers, through a pipe */
    CMD_PRIMARY_RELEASE, /* close the pipe a stalling offer held */
    CMD_DND_ENTER, /* a, b: where; c: the offer has text/uri-list */
    CMD_DND_MOVE,
    CMD_DND_DROP,
    CMD_DND_LEAVE,
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

/* The protocol does not say the keymap ends with a NUL: with S.bare it is sent without one, its size a whole number of
   pages (padded with newlines, which xkb ignores), so that nothing follows it in the client's mapping either. */
static void send_keymap(void)
{
    if (!S.keyboard || !S.keymap)
        return;
    char *text = xkb_keymap_get_as_string(S.keymap, XKB_KEYMAP_FORMAT_TEXT_V1);
    size_t size = strlen(text) + 1;
    if (S.bare)
    {
        const size_t page = 4096;
        size = (strlen(text) + page - 1) / page * page;
        char *padded = malloc(size);
        memset(padded, '\n', size);
        memcpy(padded, text, strlen(text));
        free(text);
        text = padded;
    }
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

/* ---- wl_data_device_manager: enough for files dragged into the window ---- */

/* One line of comment, two local files (one escaped) and a web link: only the two local ones are paths. */
static const char DND_URIS[] = "# a comment\r\n"
                               "file:///tmp/zen%20drop/a%C3%A1.txt\r\n"
                               "http://example.com/x\r\n"
                               "file://localhost/tmp/b.png\r\n";

static void offer_accept(struct wl_client *c, struct wl_resource *r, uint32_t serial, const char *mime)
{
    (void)c;
    (void)r;
    (void)serial;
    S.dnd_accepts++;
    snprintf(S.dnd_accepted, sizeof S.dnd_accepted, "%s", mime ? mime : "");
}

/* The source's side of a receive: the list goes into the pipe and the pipe is closed. */
static void offer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)c;
    (void)r;
    S.dnd_receives++;
    if (mime && strcmp(mime, "text/uri-list") == 0 && write(fd, DND_URIS, strlen(DND_URIS)) != (ssize_t)strlen(DND_URIS))
        g_fail++;
    close(fd);
}

static void offer_finish(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    (void)r;
    S.dnd_finished++;
}

static void offer_set_actions(struct wl_client *c, struct wl_resource *r, uint32_t actions, uint32_t preferred)
{
    (void)c;
    (void)r;
    S.dnd_actions = actions;
    S.dnd_preferred = preferred;
}

static const struct wl_data_offer_interface offer_impl = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = release_req,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

static void offer_gone(struct wl_resource *r)
{
    if (S.offer == r)
        S.offer = NULL;
    S.dnd_offers_gone++;
}

static void data_device_gone(struct wl_resource *r)
{
    if (S.data_device == r)
        S.data_device = NULL;
}

static const struct wl_data_device_interface data_device_impl = {
    .start_drag = (void *)unused_request,
    .set_selection = (void *)unused_request,
    .release = release_req,
};

static void data_manager_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    S.data_device = wl_resource_create(c, &wl_data_device_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(S.data_device, &data_device_impl, NULL, data_device_gone);
}

static const struct wl_data_device_manager_interface data_manager_impl = {
    .create_data_source = (void *)unused_request,
    .get_data_device = data_manager_get_device,
};

static void data_manager_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_data_device_manager_interface, (int)version, id);
    wl_resource_set_implementation(r, &data_manager_impl, NULL, NULL);
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

#ifdef ZEN_WL_PRIMARY
/* ---- the primary selection ---- */

static void psource_offer_req(struct wl_client *c, struct wl_resource *r, const char *mime)
{
    (void)c, (void)r, (void)mime;
}

static void psource_gone(struct wl_resource *r)
{
    if (S.p_source == r)
        S.p_source = NULL;
}

static const struct zwp_primary_selection_source_v1_interface psource_impl = {.offer = psource_offer_req,
                                                                              .destroy = release_req};

static void pdevice_set_selection(struct wl_client *c, struct wl_resource *r, struct wl_resource *source, uint32_t serial)
{
    (void)c, (void)r;
    S.p_serial = serial;
    if (S.p_source && S.p_source != source)
        zwp_primary_selection_source_v1_send_cancelled(S.p_source);
    S.p_source = source;
    if (source)
        S.p_sets++;
    else
        S.p_clears++;
}

static void pdevice_gone(struct wl_resource *r)
{
    if (S.p_device == r)
        S.p_device = NULL;
}

static const struct zwp_primary_selection_device_v1_interface pdevice_impl = {.set_selection = pdevice_set_selection,
                                                                              .destroy = release_req};

static void poffer_receive(struct wl_client *c, struct wl_resource *r, const char *mime, int32_t fd)
{
    (void)c, (void)r;
    snprintf(S.p_mime, sizeof S.p_mime, "%s", mime);
    S.p_receives++;
    if (S.p_mode == 1)
    {
        if (S.p_held_fd >= 0)
            close(S.p_held_fd);
        S.p_held_fd = fd; /* nothing is written and it stays open: a source that does not answer */
        return;
    }
    const char *p = S.p_text;
    size_t n = S.p_text_len;
    while (n)
    {
        ssize_t w = write(fd, p, n);
        if (w <= 0)
            break; /* the client reads in its own thread: a pipe that is full is not a deadlock */
        p += w;
        n -= (size_t)w;
    }
    close(fd);
}

static const struct zwp_primary_selection_offer_v1_interface poffer_impl = {.receive = poffer_receive,
                                                                            .destroy = release_req};

static void pmanager_create_source(struct wl_client *c, struct wl_resource *r, uint32_t id)
{
    struct wl_resource *src = wl_resource_create(c, &zwp_primary_selection_source_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(src, &psource_impl, NULL, psource_gone);
}

static void pmanager_get_device(struct wl_client *c, struct wl_resource *r, uint32_t id, struct wl_resource *seat)
{
    (void)seat;
    S.p_device = wl_resource_create(c, &zwp_primary_selection_device_v1_interface, wl_resource_get_version(r), id);
    wl_resource_set_implementation(S.p_device, &pdevice_impl, NULL, pdevice_gone);
}

static const struct zwp_primary_selection_device_manager_v1_interface pmanager_impl = {
    .create_source = pmanager_create_source,
    .get_device = pmanager_get_device,
    .destroy = release_req,
};

static void pmanager_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zwp_primary_selection_device_manager_v1_interface, (int)version, id);
    wl_resource_set_implementation(r, &pmanager_impl, NULL, NULL);
}

/* what the client's source writes into the pipe, read without blocking the compositor */
static int pfetch_readable(int fd, uint32_t mask, void *data)
{
    (void)mask, (void)data;
    char buf[65536];
    for (;;)
    {
        ssize_t n = read(fd, buf, sizeof buf);
        if (n > 0)
        {
            pthread_mutex_lock(&S.p_lock);
            S.p_got = realloc(S.p_got, S.p_got_len + (size_t)n + 1);
            memcpy(S.p_got + S.p_got_len, buf, (size_t)n);
            S.p_got_len += (size_t)n;
            S.p_got[S.p_got_len] = '\0';
            pthread_mutex_unlock(&S.p_lock);
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EINTR))
            return 0;
        break;
    }
    wl_event_source_remove(S.p_got_src);
    S.p_got_src = NULL;
    close(fd);
    pthread_mutex_lock(&S.p_lock);
    S.p_got_done = true;
    pthread_mutex_unlock(&S.p_lock);
    return 0;
}

static void primary_cmd(const Cmd *c)
{
    switch (c->type)
    {
    case CMD_PRIMARY_OFFER:
        if (S.p_device)
        {
            if (S.p_source) /* the new owner takes it from the client's source */
            {
                zwp_primary_selection_source_v1_send_cancelled(S.p_source);
                S.p_source = NULL;
            }
            S.p_mode = c->a;
            struct wl_client *client = wl_resource_get_client(S.p_device);
            struct wl_resource *offer = wl_resource_create(client, &zwp_primary_selection_offer_v1_interface,
                                                           wl_resource_get_version(S.p_device), 0);
            wl_resource_set_implementation(offer, &poffer_impl, NULL, NULL);
            zwp_primary_selection_device_v1_send_data_offer(S.p_device, offer);
            zwp_primary_selection_offer_v1_send_offer(offer, "image/png"); /* a type that is not text, listed first */
            zwp_primary_selection_offer_v1_send_offer(offer, "STRING");
            zwp_primary_selection_offer_v1_send_offer(offer, "text/plain;charset=utf-8");
            zwp_primary_selection_device_v1_send_selection(S.p_device, offer);
        }
        break;
    case CMD_PRIMARY_DROP:
        if (S.p_device)
            zwp_primary_selection_device_v1_send_selection(S.p_device, NULL);
        break;
    case CMD_PRIMARY_FETCH:
        if (S.p_source)
        {
            int fds[2];
            if (pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0)
                break;
            S.p_got_fd = fds[0];
            S.p_got_src = wl_event_loop_add_fd(wl_display_get_event_loop(S.display), fds[0], WL_EVENT_READABLE, pfetch_readable, NULL);
            snprintf(S.p_sent_mime, sizeof S.p_sent_mime, "%s", c->a == 1 ? "STRING" : c->a == 2 ? "image/png" : "text/plain;charset=utf-8");
            zwp_primary_selection_source_v1_send_send(S.p_source, S.p_sent_mime, fds[1]);
            close(fds[1]);
        }
        break;
    case CMD_PRIMARY_RELEASE:
        if (S.p_held_fd >= 0)
            close(S.p_held_fd);
        S.p_held_fd = -1;
        break;
    }
}
#endif

/* ---- the script ---- */

static void run_cmd(const Cmd *c)
{
    struct wl_array none;
    switch (c->type)
    {
    case CMD_LAYOUT:
        S.bare = c->b != 0;
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
#ifdef ZEN_WL_PRIMARY
    case CMD_PRIMARY_OFFER:
    case CMD_PRIMARY_DROP:
    case CMD_PRIMARY_FETCH:
    case CMD_PRIMARY_RELEASE:
        primary_cmd(c);
        break;
#endif
    case CMD_DND_ENTER:
        if (S.data_device && S.surface)
        {
            /* A new offer with its types, then the drag enters the window with it. */
            S.offer = wl_resource_create(wl_resource_get_client(S.data_device), &wl_data_offer_interface,
                                         wl_resource_get_version(S.data_device), 0);
            wl_resource_set_implementation(S.offer, &offer_impl, NULL, offer_gone);
            wl_data_device_send_data_offer(S.data_device, S.offer);
            wl_data_offer_send_offer(S.offer, "text/plain");
            if (c->c)
                wl_data_offer_send_offer(S.offer, "text/uri-list");
            if (wl_resource_get_version(S.offer) >= WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
                wl_data_offer_send_source_actions(S.offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY | WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE);
            wl_data_device_send_enter(S.data_device, ++S.serial, S.surface->res, wl_fixed_from_int(c->a), wl_fixed_from_int(c->b),
                                      S.offer);
        }
        break;
    case CMD_DND_MOVE:
        if (S.data_device)
            wl_data_device_send_motion(S.data_device, 0, wl_fixed_from_int(c->a), wl_fixed_from_int(c->b));
        break;
    case CMD_DND_DROP:
        if (S.data_device)
            wl_data_device_send_drop(S.data_device);
        break;
    case CMD_DND_LEAVE:
        if (S.data_device)
            wl_data_device_send_leave(S.data_device);
        break;
    case CMD_QUIT:
        wl_display_terminate(S.display);
        break;
    }
}

/* What the client has already sent is read before a command is carried out. The commands come through a pipe and the
   client's requests through its socket, and when both are ready the loop may take either first: the configure that
   ends a settle could be answered before a request the client sent earlier was read, and the test would look at the
   compositor's counters too soon (a few runs in 30 on a loaded machine). */
static int on_cmd(int fd, uint32_t mask, void *data)
{
    static bool draining;
    Cmd c;
    (void)mask;
    (void)data;
    if (draining) /* the dispatch below reports this pipe again: not now */
        return 0;
    draining = true;
    wl_event_loop_dispatch(wl_display_get_event_loop(S.display), 0);
    draining = false;
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
    wl_global_create(S.display, &wl_data_device_manager_interface, 3, NULL, data_manager_bind);
#ifdef ZEN_WL_POINTER_LOCK
    wl_global_create(S.display, &zwp_pointer_constraints_v1_interface, 1, NULL, constraints_bind);
    wl_global_create(S.display, &zwp_relative_pointer_manager_v1_interface, 1, NULL, manager_bind);
#endif
#ifdef ZEN_WL_PRIMARY
    S.p_held_fd = -1;
    pthread_mutex_init(&S.p_lock, NULL);
    if (S.p_advertise)
        wl_global_create(S.display, &zwp_primary_selection_device_manager_v1_interface, 1, NULL, pmanager_bind);
#endif
#ifdef ZEN_WL_EXTRA_GLOBALS
    ZEN_WL_EXTRA_GLOBALS(S.display); /* bench/compare/wl_compositor.c adds a wl_output, which SDL2 needs */
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
    int n;              /* EVENT_WINDOW_DROP: how many paths, and the first two */
    char paths[2][96];
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
        case EVENT_WINDOW_DROP:
            r->n = e.data.drop.count;
            for (int i = 0; i < r->n && i < 2; i++)
                snprintf(r->paths[i], sizeof r->paths[i], "%s", e.data.drop.paths[i]);
            mouse_position(w, &r->x, &r->y); /* as an application reads it when the event comes */
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
    /* A request the client made while handling the last events (the cursor it sets on an enter) sits in its output
       buffer until the next dispatch writes it out: one frame first, so that it is on the socket before the marker
       is asked for, and the compositor reads it first. */
    frame(w);
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
    EV_CAPS = 58, EV_F1 = 59, EV_LEFT = 105, EV_KP5 = 76, EV_NUMLOCK = 69, EV_MINUS = 12, EV_COMMA = 51, EV_SPACE = 57,
    EV_5 = 6, EV_RALT = 100, EV_LSUPER = 125, EV_INSERT = 110, EV_DELETE = 111
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

/* The rule (platform.h): a key pressed while Ctrl or Super is held is a shortcut and types nothing. xkbcommon turns
   Ctrl+letter into a control character, which is dropped, but gives Ctrl+1, Ctrl+minus and Ctrl+comma as the plain
   character and Ctrl+space as a space. */
static void test_chord_text(PlatformWindow *w)
{
    static const struct
    {
        int hold, key;
        int mod;
    } chords[] = {{EV_LCTRL, EV_A, KEYMOD_CTRL},     {EV_LCTRL, EV_1, KEYMOD_CTRL},     {EV_LCTRL, EV_MINUS, KEYMOD_CTRL},
                  {EV_LCTRL, EV_COMMA, KEYMOD_CTRL}, {EV_LCTRL, EV_SPACE, KEYMOD_CTRL}, {EV_LSUPER, EV_A, KEYMOD_SUPER},
                  {EV_LSUPER, EV_1, KEYMOD_SUPER}};
    cmd(CMD_LAYOUT, 0, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    CHECK(settle(w));
    for (size_t i = 0; i < sizeof chords / sizeof chords[0]; i++)
    {
        reset();
        cmd(CMD_KEY, chords[i].hold, 1, 0, 0);
        tap(chords[i].key);
        cmd(CMD_KEY, chords[i].hold, 0, 0, 0);
        CHECK(settle(w));
        if (count(EVENT_CHAR))
            printf("  chord %zu typed U+%04X\n", i, (unsigned)nth(EVENT_CHAR, 0)->cp);
        CHECK(count(EVENT_CHAR) == 0);
        const Rec *pressed = NULL;
        for (int j = 0; j < nrec; j++)
            if (rec[j].type == EVENT_KEY && rec[j].down && rec[j].scancode == chords[i].key + 8)
                pressed = &rec[j];
        CHECK(pressed && (pressed->mods & chords[i].mod));
    }
    /* a dead key pending is not finished by a shortcut: Ctrl+a does not become an a-acute, and the a after it is a plain a */
    if (access("/usr/share/X11/locale/en_US.UTF-8/Compose", R_OK) == 0)
    {
        cmd(CMD_LAYOUT, 2, 0, 0, 0);
        cmd(CMD_KB_ENTER, 0, 0, 0, 0);
        reset();
        tap(EV_APOSTROPHE);
        cmd(CMD_KEY, EV_LCTRL, 1, 0, 0);
        tap(EV_A);
        cmd(CMD_KEY, EV_LCTRL, 0, 0, 0);
        tap(EV_A);
        CHECK(settle(w));
        CHECK(count_char(0xE1) == 0);
        CHECK(count_char('a') == 1);
        /* AltGr is a level of the key: what the layout puts on it is text (AltGr+5 is the euro sign), with no Ctrl */
        reset();
        cmd(CMD_KEY, EV_RALT, 1, 0, 0);
        tap(EV_5);
        cmd(CMD_KEY, EV_RALT, 0, 0, 0);
        CHECK(settle(w));
        CHECK(count_char(0x20AC) == 1);
        CHECK(count(EVENT_CHAR) == 1);
        for (int j = 0; j < nrec; j++)
            if (rec[j].type == EVENT_KEY)
                CHECK(!(rec[j].mods & KEYMOD_CTRL));
    }
    cmd(CMD_LAYOUT, 0, 0, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    CHECK(settle(w));
}

/* Shift+Insert, Ctrl+Insert and Shift+Delete, the clipboard shortcuts of old: the key and its modifier, no text. */
static void test_clipboard_shortcuts(PlatformWindow *w)
{
    static const struct
    {
        int hold, key, want_key, mod;
    } k[] = {{EV_LSHIFT, EV_INSERT, KEY_INSERT, KEYMOD_SHIFT},
             {EV_LCTRL, EV_INSERT, KEY_INSERT, KEYMOD_CTRL},
             {EV_LSHIFT, EV_DELETE, KEY_DELETE, KEYMOD_SHIFT}};
    for (size_t i = 0; i < 3; i++)
    {
        reset();
        cmd(CMD_KEY, k[i].hold, 1, 0, 0);
        tap(k[i].key);
        cmd(CMD_KEY, k[i].hold, 0, 0, 0);
        CHECK(settle(w));
        const Rec *pressed = NULL;
        for (int j = 0; j < nrec; j++)
            if (rec[j].type == EVENT_KEY && rec[j].down && rec[j].key == k[i].want_key)
                pressed = &rec[j];
        CHECK(pressed && pressed->mods == k[i].mod);
        CHECK(count(EVENT_CHAR) == 0);
    }
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

    /* the same layout sent as a keymap with no NUL at the end and a size of whole pages: the client must read it by its
       size, not by looking for a NUL past the end of its mapping */
    reset();
    cmd(CMD_LAYOUT, 1, 1, 0, 0);
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    tap(EV_Q);
    CHECK(settle(w));
    CHECK(count_key(KEY_A, true, false) == 1);
    CHECK(count_char('a') == 1);
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
/* Files dragged in from another client: the offer with text/uri-list is accepted as a copy, the pointer's place comes as
   pointer moves while it is held over the window, and the drop gives the local paths where the pointer was. */
static void test_drop(PlatformWindow *w)
{
    reset();
    cmd(CMD_PTR_LEAVE, 0, 0, 0, 0); /* a drag takes the pointer away from the window first */
    CHECK(settle(w));
    reset();
    cmd(CMD_DND_ENTER, 40, 30, 1, 0);
    cmd(CMD_DND_MOVE, 55, 44, 0, 0);
    CHECK(settle(w));
    CHECK(S.dnd_accepts == 1 && strcmp(S.dnd_accepted, "text/uri-list") == 0);
    CHECK(S.dnd_actions == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY && S.dnd_preferred == WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
    const Rec *m0 = nth(EVENT_MOUSE_MOVE, 0);
    const Rec *m1 = nth(EVENT_MOUSE_MOVE, 1);
    CHECK(m0 && m0->x == 40 && m0->y == 30);
    CHECK(m1 && m1->x == 55 && m1->y == 44);
    CHECK(count(EVENT_WINDOW_DROP) == 0);
    int mx = -1, my = -1;
    mouse_position(w, &mx, &my);
    CHECK(mx == 55 && my == 44);

    reset();
    cmd(CMD_DND_MOVE, 120, 77, 0, 0);
    cmd(CMD_DND_DROP, 0, 0, 0, 0);
    CHECK(settle(w));
    const Rec *d = nth(EVENT_WINDOW_DROP, 0);
    CHECK(d && d->n == 2);
    CHECK(d && strcmp(d->paths[0], "/tmp/zen drop/a\xC3\xA1.txt") == 0);
    CHECK(d && strcmp(d->paths[1], "/tmp/b.png") == 0);
    CHECK(d && d->x == 120 && d->y == 77); /* where the files were let go */
    CHECK(count(EVENT_WINDOW_DROP) == 1);
    CHECK(S.dnd_receives == 1 && S.dnd_finished == 1);
    CHECK(S.dnd_offers_gone == 1 && !S.offer); /* the client let go of the offer */

    /* A drag with no files in it (text from an editor) is refused, and its drop gives nothing. */
    reset();
    cmd(CMD_DND_ENTER, 10, 10, 0, 0);
    cmd(CMD_DND_MOVE, 20, 20, 0, 0);
    CHECK(settle(w));
    CHECK(S.dnd_accepts == 2 && S.dnd_accepted[0] == 0);
    CHECK(S.dnd_actions == WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE);
    CHECK(count(EVENT_MOUSE_MOVE) == 0);
    cmd(CMD_DND_DROP, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(count(EVENT_WINDOW_DROP) == 0 && S.dnd_receives == 1);
    CHECK(S.dnd_offers_gone == 2);

    /* A drag that leaves without a drop: the offer is let go, and the next drag works. */
    reset();
    cmd(CMD_DND_ENTER, 5, 6, 1, 0);
    cmd(CMD_DND_LEAVE, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(S.dnd_offers_gone == 3 && count(EVENT_WINDOW_DROP) == 0);
    cmd(CMD_DND_ENTER, 70, 80, 1, 0);
    cmd(CMD_DND_DROP, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(count(EVENT_WINDOW_DROP) == 1 && S.dnd_receives == 2 && S.dnd_offers_gone == 4);
    const Rec *d2 = nth(EVENT_WINDOW_DROP, 0);
    CHECK(d2 && d2->x == 70 && d2->y == 80);
}

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


/* ---- the primary selection ---- */

#ifdef ZEN_WL_PRIMARY
static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static char *make_text(size_t n)
{
    char *p = malloc(n + 1);
    for (size_t i = 0; i < n; i++)
        p[i] = (char)('a' + (i * 7 + i / 26) % 26);
    p[n] = '\0';
    return p;
}

/* the compositor asks the client for the text it offers, and the client writes it from its frame */
static bool fetch_from_client(PlatformWindow *w, int kind, char **got, size_t *len)
{
    pthread_mutex_lock(&S.p_lock); /* cleared here, before the compositor is asked: it must not find the last answer */
    free(S.p_got);
    S.p_got = NULL;
    S.p_got_len = 0;
    S.p_got_done = false;
    pthread_mutex_unlock(&S.p_lock);
    cmd(CMD_PRIMARY_FETCH, kind, 0, 0, 0);
    for (int t = 0; t < 4000; t += 2)
    {
        frame(w);
        pthread_mutex_lock(&S.p_lock);
        bool done = S.p_got_done;
        if (done)
        {
            *got = S.p_got ? strdup(S.p_got) : strdup("");
            *len = S.p_got_len;
        }
        pthread_mutex_unlock(&S.p_lock);
        if (done)
            return true;
        usleep(2000);
    }
    return false;
}

static void set_foreign_text(const char *text)
{
    free(S.p_text);
    S.p_text = strdup(text);
    S.p_text_len = strlen(text);
}

static void test_primary_we_own(PlatformWindow *w)
{
    CHECK(clipboard_has_primary_support());
    cmd(CMD_KB_ENTER, 0, 0, 0, 0);
    tap(EV_A); /* a key press: the serial that a selection is taken on */
    CHECK(settle(w));

    CHECK(clipboard_set_primary("ol\xC3\xA1 zen"));
    CHECK(settle(w));
    CHECK(S.p_sets == 1 && S.p_serial != 0);
    CHECK(strcmp(clipboard_get_primary(), "ol\xC3\xA1 zen") == 0); /* ours: no round trip */

    char *got = NULL;
    size_t len = 0;
    CHECK(fetch_from_client(w, 0, &got, &len));
    CHECK(got && len == strlen("ol\xC3\xA1 zen") && strcmp(got, "ol\xC3\xA1 zen") == 0);
    CHECK(strcmp(S.p_sent_mime, "text/plain;charset=utf-8") == 0);
    free(got);
    /* a type we do not offer gets nothing, and the pipe is closed */
    CHECK(fetch_from_client(w, 2, &got, &len) && len == 0);
    free(got);

    /* a text larger than a pipe holds: the client writes it as the compositor reads, without blocking for good */
    size_t big_n = 3u << 20;
    char *big = make_text(big_n);
    CHECK(clipboard_set_primary(big));
    CHECK(settle(w));
    CHECK(S.p_sets == 2);
    double t0 = now_s();
    CHECK(fetch_from_client(w, 0, &got, &len));
    CHECK(len == big_n && got && memcmp(got, big, big_n) == 0);
    CHECK(now_s() - t0 < 5.0);
    free(got);
    free(big);

    /* given up with NULL and with "": the compositor is told, once, and what is read is empty */
    CHECK(clipboard_set_primary(NULL));
    CHECK(settle(w));
    CHECK(S.p_clears == 1);
    CHECK(strcmp(clipboard_get_primary(), "") == 0);
    CHECK(clipboard_set_primary("again"));
    CHECK(clipboard_set_primary(""));
    CHECK(settle(w));
    CHECK(S.p_clears == 2);
    CHECK(clipboard_set_primary(NULL)); /* nothing to give up */
    CHECK(settle(w));
    CHECK(S.p_clears == 2);
}

static void test_primary_foreign(PlatformWindow *w)
{
    set_foreign_text("from another program \xC3\xA7");
    cmd(CMD_PRIMARY_OFFER, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(strcmp(clipboard_get_primary(), "from another program \xC3\xA7") == 0);
    CHECK(strcmp(S.p_mime, "text/plain;charset=utf-8") == 0); /* the best of the types it listed (STRING and image/png too) */

    /* 2 MB: more than a pipe holds, written by the other side while this one reads */
    size_t big_n = 2u << 20;
    char *big = make_text(big_n);
    set_foreign_text(big);
    cmd(CMD_PRIMARY_OFFER, 0, 0, 0, 0);
    CHECK(settle(w));
    double t0 = now_s();
    const char *got = clipboard_get_primary();
    CHECK(strlen(got) == big_n && memcmp(got, big, big_n) == 0);
    CHECK(now_s() - t0 < 5.0);
    free(big);

    /* the selection passes from us to another program: our source is cancelled, what we read is theirs, and giving up a
       selection we no longer own leaves theirs alone */
    CHECK(clipboard_set_primary("mine"));
    CHECK(settle(w));
    int clears = S.p_clears;
    set_foreign_text("theirs");
    cmd(CMD_PRIMARY_OFFER, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(strcmp(clipboard_get_primary(), "theirs") == 0);
    CHECK(clipboard_set_primary(NULL));
    CHECK(settle(w));
    CHECK(S.p_clears == clears);
    CHECK(strcmp(clipboard_get_primary(), "theirs") == 0);

    /* and from another program back to us */
    CHECK(clipboard_set_primary("mine again"));
    CHECK(settle(w));
    CHECK(strcmp(clipboard_get_primary(), "mine again") == 0);
    CHECK(clipboard_set_primary(NULL));
    CHECK(settle(w));

    /* the owner goes away: nothing to read */
    set_foreign_text("soon gone");
    cmd(CMD_PRIMARY_OFFER, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(strcmp(clipboard_get_primary(), "soon gone") == 0);
    cmd(CMD_PRIMARY_DROP, 0, 0, 0, 0);
    CHECK(settle(w));
    CHECK(strcmp(clipboard_get_primary(), "") == 0);

    /* an owner that takes the pipe and never writes or closes it: a step is one second, and it is not asked again at once */
    set_foreign_text("never delivered");
    cmd(CMD_PRIMARY_OFFER, 1, 0, 0, 0);
    CHECK(settle(w));
    t0 = now_s();
    CHECK(strcmp(clipboard_get_primary(), "") == 0);
    double first = now_s() - t0;
    t0 = now_s();
    CHECK(strcmp(clipboard_get_primary(), "") == 0);
    CHECK(strcmp(clipboard_get_primary(), "") == 0);
    double rest = now_s() - t0;
    printf("  a primary selection whose owner never writes: gave up after %.2f s, the next two calls took %.3f s\n", first, rest);
    CHECK(first > 0.5 && first < 4.0);
    CHECK(rest < 0.2);
    cmd(CMD_PRIMARY_RELEASE, 0, 0, 0, 0);
    cmd(CMD_PRIMARY_DROP, 0, 0, 0, 0);
    CHECK(settle(w));
}
#endif

/* The compositor has no primary selection protocol: the calls say there is none, without an error. */
static void test_primary_unsupported(void)
{
    platform_clear_error();
    CHECK(!clipboard_has_primary_support());
    CHECK(!clipboard_set_primary("text"));
    CHECK(!clipboard_set_primary(NULL));
    CHECK(strcmp(clipboard_get_primary(), "") == 0);
    CHECK(platform_get_error()[0] == '\0');
}

int main(int argc, char **argv)
{
#ifdef ZEN_WL_PRIMARY
    S.p_advertise = !(argc > 1 && strcmp(argv[1], "noprimary") == 0);
#else
    (void)argc, (void)argv;
#endif
    const bool only_unsupported = argc > 1 && strcmp(argv[1], "noprimary") == 0;
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

    if (only_unsupported)
    {
        test_primary_unsupported();
        window_destroy(w);
        platform_shutdown();
        cmd(CMD_QUIT, 0, 0, 0, 0);
        pthread_join(S.thread, NULL);
        printf("%d passed, %d failed\n", g_pass, g_fail);
        return g_fail ? 1 : 0;
    }
    test_keys(w);
    test_chord_text(w);
    test_clipboard_shortcuts(w);
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
    test_drop(w);
    test_configure(w);
#ifdef ZEN_WL_PRIMARY
    test_primary_we_own(w);
    test_primary_foreign(w);
#else
    test_primary_unsupported();
#endif

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
