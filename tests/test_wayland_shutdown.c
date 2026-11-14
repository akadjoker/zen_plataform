/*
 * test_wayland_shutdown.c - platform_init / platform_shutdown over and over against a small compositor of its own
 * (libwayland-server, in a thread) that offers every optional global the backend binds (wl_subcompositor and the
 * decoration manager among them). Each global the backend binds has to be destroyed at shutdown, or every cycle
 * leaks a proxy: the leak is on the client's side and the compositor cannot tell (it frees the client's objects when
 * the connection closes), so this shows up under LeakSanitizer (cmake -DPLATFORM_SANITIZE=ON) and as a clean exit.
 * Skips (77) when a piece is missing.
 */
#define _GNU_SOURCE
#include "platform.h"
#include "xdg-shell-server-protocol.h"
#ifdef ZEN_WL_XDG_DECORATION
#include "xdg-decoration-unstable-v1-server-protocol.h"
#endif

#include <wayland-server.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static struct wl_display *g_display;
static pthread_t g_thread;
static int g_binds;

static void count(int *counter)
{
    __atomic_add_fetch(counter, 1, __ATOMIC_SEQ_CST);
}

static int counted(int *counter)
{
    return __atomic_load_n(counter, __ATOMIC_SEQ_CST);
}

static void unused_request(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_post_error(r, 0, "this compositor does not do that");
}

static void compositor_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_compositor_interface, (int)version, id);
    static const struct wl_compositor_interface impl = {.create_surface = (void *)unused_request, .create_region = (void *)unused_request};
    wl_resource_set_implementation(r, &impl, NULL, NULL);
    count(&g_binds);
}

static void wm_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void wm_base_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &xdg_wm_base_interface, (int)version, id);
    static const struct xdg_wm_base_interface impl = {
        .destroy = wm_destroy,
        .create_positioner = (void *)unused_request,
        .get_xdg_surface = (void *)unused_request,
        .pong = (void *)unused_request,
    };
    wl_resource_set_implementation(r, &impl, NULL, NULL);
}

static void subcompositor_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void subcompositor_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &wl_subcompositor_interface, (int)version, id);
    static const struct wl_subcompositor_interface impl = {.destroy = subcompositor_destroy, .get_subsurface = (void *)unused_request};
    wl_resource_set_implementation(r, &impl, NULL, NULL);
}

#ifdef ZEN_WL_XDG_DECORATION
static void deco_destroy(struct wl_client *c, struct wl_resource *r)
{
    (void)c;
    wl_resource_destroy(r);
}

static void deco_bind(struct wl_client *c, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(c, &zxdg_decoration_manager_v1_interface, (int)version, id);
    static const struct zxdg_decoration_manager_v1_interface impl = {.destroy = deco_destroy, .get_toplevel_decoration = (void *)unused_request};
    wl_resource_set_implementation(r, &impl, NULL, NULL);
}
#endif

static void *server_main(void *arg)
{
    (void)arg;
    wl_display_run(g_display);
    return NULL;
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
    g_display = wl_display_create();
    if (!g_display)
        return false;
    static char name[64];
    snprintf(name, sizeof name, "zen-shutdown-%d", (int)getpid());
    if (wl_display_add_socket(g_display, name) != 0 || wl_display_init_shm(g_display) != 0)
        return false;
    wl_global_create(g_display, &wl_compositor_interface, 4, NULL, compositor_bind);
    wl_global_create(g_display, &xdg_wm_base_interface, 5, NULL, wm_base_bind);
    wl_global_create(g_display, &wl_subcompositor_interface, 1, NULL, subcompositor_bind);
#ifdef ZEN_WL_XDG_DECORATION
    wl_global_create(g_display, &zxdg_decoration_manager_v1_interface, 1, NULL, deco_bind);
#endif
    setenv("WAYLAND_DISPLAY", name, 1);
    setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
    return pthread_create(&g_thread, NULL, server_main, NULL) == 0;
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
    platform_shutdown();

    const int cycles = 8;
    for (int i = 1; i < cycles; i++)
    {
        if (!platform_init())
        {
            CHECK(false);
            break;
        }
        platform_shutdown();
    }
    CHECK(counted(&g_binds) == cycles);

    wl_display_terminate(g_display);
    pthread_join(g_thread, NULL);
    wl_display_destroy(g_display);
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
