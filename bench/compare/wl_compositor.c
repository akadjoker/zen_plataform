/* wl_compositor.c - the small compositor of tests/test_input_wayland.c (wl_compositor, xdg_wm_base, wl_seat, wl_shm; it
   configures every toplevel and takes the buffers) as a program of its own, for the Wayland tables of
   tools/bench_wayland.sh: it prints the name of its socket on stdout and serves until it is killed. XDG_RUNTIME_DIR must be
   set (the socket is made there). It also has one wl_output. No window is ever shown: nothing is drawn, the buffers are
   read and dropped. */
struct wl_display;
static void add_output(struct wl_display *display);
#define ZEN_WL_EXTRA_GLOBALS add_output
#define main wayland_input_main
#include "../../tests/test_input_wayland.c"
#undef main

/* One 1280x800 output: SDL2 refuses to start without a display, and a real compositor always has one. */
static void output_bind(struct wl_client *client, void *data, uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource *r = wl_resource_create(client, &wl_output_interface, version > 2 ? 2 : (int)version, id);
    if (!r)
    {
        wl_client_post_no_memory(client);
        return;
    }
    wl_output_send_geometry(r, 0, 0, 340, 270, WL_OUTPUT_SUBPIXEL_UNKNOWN, "bench", "bench", WL_OUTPUT_TRANSFORM_NORMAL);
    wl_output_send_mode(r, WL_OUTPUT_MODE_CURRENT | WL_OUTPUT_MODE_PREFERRED, 1280, 800, 60000);
    if (version >= 2)
    {
        wl_output_send_scale(r, 1);
        wl_output_send_done(r);
    }
}

static void add_output(struct wl_display *display)
{
    wl_global_create(display, &wl_output_interface, 2, NULL, output_bind);
}

int main(void)
{
    if (!start_compositor())
    {
        fprintf(stderr, "wl_compositor: could not start\n");
        return 1;
    }
    printf("%s\n", getenv("WAYLAND_DISPLAY"));
    fflush(stdout);
    pthread_join(S.thread, NULL);
    return 0;
}
