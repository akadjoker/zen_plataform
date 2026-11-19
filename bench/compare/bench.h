/* bench.h - what a library file (bench_zp.c, bench_glfw.c, bench_sdl2.c) gives bench_main.c. The modes are
   written once in bench_main.c, so the three libraries do the same work by construction. */
#ifndef BENCH_H
#define BENCH_H

#ifndef BENCH_WAYLAND /* a Wayland build has no X11 at all */
#include <X11/Xlib.h>
#endif

typedef struct
{
    long motion;   /* pointer moves the application saw */
    long key_down; /* key presses (not repeats) */
    long key_up;
    long text;     /* text input events (EVENT_CHAR, charmods, SDL_TEXTINPUT) */
    long other;
} Counts;

const char *lib_name(void);
int lib_init(void);
/* a visible window of w x h at (x, y); gl: a 3.3 core context current on it, no vsync */
void *lib_window(int gl, int x, int y, int w, int h);
void lib_window_free(void *win);
void lib_exit(void);
/* one turn of the event loop: pump and drain, adding to c */
void lib_poll(void *win, Counts *c);
/* Sleep until an event, a lib_wake or the timeout (seconds), and handle what came the way lib_poll does: the same work a
   program that sleeps between events does per wake-up (zen_platform: platform_wait_events and the begin_frame that follows;
   GLFW: glfwWaitEventsTimeout; SDL: SDL_WaitEventTimeout and the events behind it). Returns 1 if it woke before the timeout. */
int lib_wait(void *win, double seconds, Counts *c);
/* Wake a lib_wait, from any thread (zen_platform: platform_wake, GLFW: glfwPostEmptyEvent, SDL: SDL_PushEvent of a user event) */
void lib_wake(void);
#ifndef BENCH_WAYLAND
Display *lib_display(void *win);
unsigned long lib_xwin(void *win);
#endif

#endif
