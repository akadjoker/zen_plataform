/* bench_sdl2.c - SDL2 side of the comparison. The SDL_PollEvent loop is the turn of the event loop. */
#include "bench.h"
#include <SDL.h>
#include <SDL_syswm.h>

const char *lib_name(void)
{
    return "sdl2";
}

int lib_init(void)
{
    return SDL_Init(SDL_INIT_VIDEO) == 0;
}

typedef struct
{
    SDL_Window *win;
    SDL_GLContext ctx;
} Win;

void *lib_window(int gl, int x, int y, int w, int h)
{
    Uint32 flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (gl)
    {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
        flags |= SDL_WINDOW_OPENGL;
    }
    static Win slot;
    SDL_Window *win = SDL_CreateWindow("bench", x < 0 ? SDL_WINDOWPOS_UNDEFINED : x, y < 0 ? SDL_WINDOWPOS_UNDEFINED : y, w, h, flags);
    if (!win)
        return NULL;
    slot.win = win;
    slot.ctx = NULL;
    if (gl)
    {
        slot.ctx = SDL_GL_CreateContext(win);
        if (!slot.ctx)
        {
            SDL_DestroyWindow(win);
            return NULL;
        }
        SDL_GL_SetSwapInterval(0);
    }
    return &slot;
}

void lib_window_free(void *p)
{
    Win *w = p;
    if (w->ctx)
        SDL_GL_DeleteContext(w->ctx);
    SDL_DestroyWindow(w->win);
}

void lib_exit(void)
{
    SDL_Quit();
}

void lib_poll(void *p, Counts *c)
{
    (void)p;
    SDL_Event e;
    while (SDL_PollEvent(&e))
    {
        switch (e.type)
        {
        case SDL_MOUSEMOTION:
            c->motion++;
            break;
        case SDL_KEYDOWN:
            if (!e.key.repeat)
                c->key_down++;
            break;
        case SDL_KEYUP:
            c->key_up++;
            break;
        case SDL_TEXTINPUT:
            c->text++;
            break;
        default:
            c->other++;
        }
    }
}

static Uint32 g_wake_type;

int lib_wait(void *p, double seconds, Counts *c)
{
    (void)p;
    SDL_Event e;
    int got = SDL_WaitEventTimeout(&e, (int)(seconds * 1000.0 + 0.5));
    if (got)
    {
        if (e.type == g_wake_type)
            c->other++;
        else if (e.type == SDL_MOUSEMOTION)
            c->motion++;
        else
            c->other++;
        lib_poll(p, c);
    }
    return got;
}

void lib_wake(void)
{
    if (!g_wake_type)
        g_wake_type = SDL_RegisterEvents(1);
    SDL_Event e;
    SDL_zero(e);
    e.type = g_wake_type;
    SDL_PushEvent(&e);
}

static int info(void *p, SDL_SysWMinfo *i)
{
    Win *w = p;
    SDL_VERSION(&i->version);
    return SDL_GetWindowWMInfo(w->win, i);
}

Display *lib_display(void *p)
{
    SDL_SysWMinfo i;
    return info(p, &i) ? i.info.x11.display : NULL;
}

unsigned long lib_xwin(void *p)
{
    SDL_SysWMinfo i;
    return info(p, &i) ? i.info.x11.window : 0;
}
