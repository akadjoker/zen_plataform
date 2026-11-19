/* bench.h - what a library file (bench_zp.c, bench_glfw.c, bench_sdl2.c) gives bench_main.c. The modes are
   written once in bench_main.c, so the three libraries do the same work by construction. */
#ifndef BENCH_H
#define BENCH_H

#include <X11/Xlib.h>

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
Display *lib_display(void *win);
unsigned long lib_xwin(void *win);

#endif
