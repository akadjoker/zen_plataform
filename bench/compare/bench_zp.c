/* bench_zp.c - zen_platform side of the comparison. A window_begin_frame and the poll_event loop is the turn of
   the event loop (the frame list is filled by the begin_frame, poll_event only reads it). */
#include "bench.h"
#include "platform.h"

const char *lib_name(void)
{
    return "zen_platform";
}

int lib_init(void)
{
    return platform_init();
}

void *lib_window(int gl, int x, int y, int w, int h)
{
    WindowConfig cfg = {.title = "bench", .width = w, .height = h, .x = x < 0 ? WINDOW_POS_UNDEFINED : x, .y = y < 0 ? WINDOW_POS_UNDEFINED : y, .monitor = MONITOR_CURRENT,
                        .render = gl ? RENDER_GL : RENDER_PIXELS, .resizable = true};
    cfg.gl = (GLConfig){.profile = GL_PROFILE_CORE, .major = 3, .minor = 3};
    PlatformWindow *win = window_create(&cfg);
    if (win && gl)
        window_set_vsync(win, false);
    return win;
}

void lib_window_free(void *win)
{
    window_destroy(win);
}

void lib_exit(void)
{
    platform_shutdown();
}

void lib_poll(void *win, Counts *c)
{
    Event e;
    window_begin_frame(win);
    while (poll_event(win, &e))
    {
        switch (e.type)
        {
        case EVENT_MOUSE_MOVE:
            c->motion++;
            break;
        case EVENT_KEY:
            if (e.data.key.down)
                c->key_down++;
            else
                c->key_up++;
            break;
        case EVENT_CHAR:
            c->text++;
            break;
        default:
            c->other++;
        }
    }
}

int lib_wait(void *win, double seconds, Counts *c)
{
    int woke = platform_wait_events(seconds);
    lib_poll(win, c);
    return woke;
}

void lib_wake(void)
{
    platform_wake();
}

Display *lib_display(void *win)
{
    return window_native_handle(win, NATIVE_DISPLAY);
}

unsigned long lib_xwin(void *win)
{
    return (unsigned long)(uintptr_t)window_native_handle(win, NATIVE_WINDOW);
}
