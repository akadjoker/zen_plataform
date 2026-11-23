/*
 * core.c - portable core. Owns the input state, the per-frame event list, the
 * key and char queues, and the touch slots. Knows nothing about the OS: every
 * platform detail goes through backend.h.
 */
#include "core_internal.h"
#include "error_internal.h"
#include "gamepad_internal.h"
#include "gesture_internal.h"
#include "backend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

/* ---- windows alive, and destruction that waits for the application's code to return ---- */

/* Every window made and not yet destroyed, newest first. window_destroy ignores a pointer that is not in the list
   (a second destroy of the same window, a destroy after platform_shutdown, which destroys what is left). */
static PlatformWindow *g_windows;
/* Above zero while the platform is running application code (an event hook) or a loop that calls it (window_begin_frame,
   app_run): the backend is in the middle of the window's events and still uses its data when the application returns. A
   window_destroy made then only marks the window; it goes when the outermost of those returns. Main thread only. */
static int g_depth;
static bool g_shutdown_wanted; /* platform_shutdown was called from application code, it runs when that returns */
static bool g_reap_due;        /* a window is marked dead or a shutdown waits: scope_leave has work to do */
static void platform_shutdown_now(void);

static void window_free(PlatformWindow *w)
{
    for (PlatformWindow **pp = &g_windows; *pp; pp = &(*pp)->next)
        if (*pp == w)
        {
            *pp = w->next;
            break;
        }
    backend_destroy(w->b);
    free(w);
}

/* The windows marked by window_destroy while the application's code ran, and a platform_shutdown asked for then. */
static void reap(void)
{
    if (g_depth > 0 || !g_reap_due)
        return;
    g_reap_due = false;
    for (PlatformWindow *w = g_windows; w;)
        if (w->dead)
        {
            window_free(w);
            w = g_windows; /* the list changed: look again from the start */
        }
        else
            w = w->next;
    if (g_shutdown_wanted)
    {
        g_shutdown_wanted = false;
        platform_shutdown_now();
    }
}

static void scope_leave(void)
{
    if (--g_depth == 0 && g_reap_due)
        reap();
}

void core_scope_enter(void)
{
    g_depth++;
}

void core_scope_leave(void)
{
    scope_leave();
}

int core_window_count(void)
{
    int n = 0;
    for (const PlatformWindow *w = g_windows; w; w = w->next)
        n++;
    return n;
}

/* ---- ring buffers ---- */

static void keycode_push(InputState *s, int key)
{
    int next = (s->kq_tail + 1) % KEYCODE_QUEUE_LEN;
    if (next == s->kq_head)
        return; /* full: drop the newest, keep order */
    s->keycode_q[s->kq_tail] = key;
    s->kq_tail = next;
}

static int keycode_pop(InputState *s)
{
    if (s->kq_head == s->kq_tail)
        return 0;
    int key = s->keycode_q[s->kq_head];
    s->kq_head = (s->kq_head + 1) % KEYCODE_QUEUE_LEN;
    return key;
}

static void char_push(InputState *s, uint32_t cp)
{
    int next = (s->cq_tail + 1) % CHAR_QUEUE_LEN;
    if (next == s->cq_head)
        return;
    s->char_q[s->cq_tail] = cp;
    s->cq_tail = next;
}

static uint32_t char_pop(InputState *s)
{
    if (s->cq_head == s->cq_tail)
        return 0;
    uint32_t cp = s->char_q[s->cq_head];
    s->cq_head = (s->cq_head + 1) % CHAR_QUEUE_LEN;
    return cp;
}

/* ---- touch slots, kept dense and matched by id ---- */

static int touch_find(InputState *s, int id)
{
    for (int i = 0; i < s->touch_count; i++)
        if (s->touch[i].id == id)
            return i;
    return -1;
}

static void touch_apply(InputState *s, int id, float x, float y, float pressure, TouchPhase phase)
{
    int i = touch_find(s, id);
    switch (phase)
    {
    case TOUCH_DOWN:
        if (i < 0 && s->touch_count < MAX_TOUCH_POINTS)
            i = s->touch_count++;
        if (i < 0)
            return;
        s->touch[i].id = id;
        s->touch[i].x = x;
        s->touch[i].y = y;
        s->touch[i].pressure = pressure;
        break;
    case TOUCH_MOVE:
        if (i < 0)
            return;
        s->touch[i].x = x;
        s->touch[i].y = y;
        s->touch[i].pressure = pressure;
        break;
    case TOUCH_UP:
    case TOUCH_CANCEL:
        if (i < 0)
            return;
        for (int j = i; j < s->touch_count - 1; j++)
            s->touch[j] = s->touch[j + 1];
        s->touch_count--;
        break;
    }
}


/* ---- gestures: fed from touch (and optionally the left mouse button) ---- */

static void gesture_feed_touch(Core *core, TouchPhase phase, int id, float x, float y)
{
    InputState *s = &core->in;
    GVec2 pos[MAX_TOUCH_POINTS];
    int n = 0;
    bool known = false;
    for (int i = 0; i < s->touch_count; i++)
    {
        bool me = s->touch[i].id == id;
        known |= me;
        pos[n++] = me ? (GVec2){x, y} : (GVec2){s->touch[i].x, s->touch[i].y};
    }
    /* A new finger is not in the slots yet; a lifted one still is. */
    if (!known && phase != TOUCH_UP && phase != TOUCH_CANCEL && n < MAX_TOUCH_POINTS)
        pos[n++] = (GVec2){x, y};

    GestureAction a = phase == TOUCH_DOWN   ? GESTURE_ACTION_DOWN
                      : phase == TOUCH_MOVE ? GESTURE_ACTION_MOVE
                      : phase == TOUCH_UP   ? GESTURE_ACTION_UP
                                            : GESTURE_ACTION_CANCEL;
    /* gesture size comes from the window through core_set_gesture_size */
    gesture_feed(&s->gesture, a, n, pos, s->gesture.width, s->gesture.height, time_seconds());
}


/* ========================================================================== */
/*  Backend -> core                                                           */
/* ========================================================================== */

void core_push_char(Core *core, uint32_t codepoint)
{
    if (core->in.text_input_off)
        return;
    /* Text is both an event, for poll_event, and an entry in the char queue; the
       EVENT_CHAR case of core_push_event does the queue. */
    Event e = {.type = EVENT_CHAR};
    e.data.codepoint = codepoint;
    core_push_event(core, &e);
}


/* Desktop has no touch device: with emulation on, the left button becomes one
   finger (TOUCH_ID_MOUSE) and goes through the same path as a real touch. */
static void mouse_touch_emit(Core *core, TouchPhase phase)
{
    InputState *s = &core->in;
    Event t = {.type = EVENT_TOUCH};
    t.data.touch.id = TOUCH_ID_MOUSE;
    t.data.touch.x = (float)s->mouse_x;
    t.data.touch.y = (float)s->mouse_y;
    t.data.touch.pressure = phase == TOUCH_UP ? 0.0f : 1.0f;
    t.data.touch.phase = phase;
    core_push_event(core, &t);
}

/* Events that only say "it is now here": the newest says all the older ones did. */
static bool latest_wins(EventType type)
{
    return type == EVENT_MOUSE_MOVE || type == EVENT_WINDOW_RESIZE || type == EVENT_WINDOW_FB_RESIZE ||
           type == EVENT_WINDOW_MOVE;
}

/* A frame that gathers more events than the list holds (a slow frame, a fast mouse, a window being dragged) keeps
   the ones that matter: the newest pointer, size or position event replaces the last of its kind, and a key, a
   button or text pushes out the oldest of those instead of being lost. The state arrays and the key and char
   queues are filled whatever happens here. */
/* Replaces an older event by a newer one of the same kind; a mouse move keeps the sum of both deltas. */
static void fold_move(Event *older, const Event *newer)
{
    Event e = *newer;
    if (e.type == EVENT_MOUSE_MOVE)
    {
        e.data.mouse.dx += older->data.mouse.dx;
        e.data.mouse.dy += older->data.mouse.dy;
    }
    *older = e;
}

static void frame_event_add(InputState *s, const Event *ev)
{
    if (s->fe_count < FRAME_EVENT_MAX)
    {
        s->frame_events[s->fe_count++] = *ev;
        return;
    }
    Event add = *ev;
    int victim = -1;
    if (!s->fe_movers_ok)
    {
        s->fe_movers = 0;
        for (int i = s->fe_cursor; i < s->fe_count; i++)
            s->fe_movers += latest_wins(s->frame_events[i].type);
        s->fe_movers_ok = true;
    }
    if (latest_wins(ev->type))
    {
        /* Folded into the newest one of its kind, as long as no event of another kind (a button, a key) came after it:
           folding past one would put this position before an event that really came earlier (a release ahead of the
           move that followed it). Past such an event the oldest one of the kind makes room instead. */
        for (int i = s->fe_count - 1; i >= s->fe_cursor; i--)
        {
            if (s->frame_events[i].type == ev->type)
            {
                fold_move(&s->frame_events[i], ev);
                return;
            }
            if (!latest_wins(s->frame_events[i].type))
                break;
        }
        for (int i = s->fe_cursor; i < s->fe_count; i++)
            if (s->frame_events[i].type == ev->type)
            {
                victim = i;
                break;
            }
    }
    if (victim < 0 && s->fe_movers > 0)
        for (int i = s->fe_cursor; i < s->fe_count; i++)
            if (latest_wins(s->frame_events[i].type))
            {
                victim = i;
                break;
            }
    if (victim < 0)
        return; /* nothing in the list can be spared */
    /* The relative motion of a dropped move goes into the next move, so captured-mouse deltas add up. */
    if (s->frame_events[victim].type == EVENT_MOUSE_MOVE)
    {
        Event *next = &add;
        for (int j = victim + 1; j < s->fe_count; j++)
            if (s->frame_events[j].type == EVENT_MOUSE_MOVE)
            {
                next = &s->frame_events[j];
                break;
            }
        if (next->type == EVENT_MOUSE_MOVE)
        {
            next->data.mouse.dx += s->frame_events[victim].data.mouse.dx;
            next->data.mouse.dy += s->frame_events[victim].data.mouse.dy;
        }
    }
    memmove(&s->frame_events[victim], &s->frame_events[victim + 1], (size_t)(s->fe_count - 1 - victim) * sizeof s->frame_events[0]);
    s->frame_events[s->fe_count - 1] = add;
    s->fe_movers += latest_wins(add.type) - 1; /* the victim went out, the new one came in */
}

void core_push_event(Core *core, const Event *ev)
{
    InputState *s = &core->in;

    /* While a finger is down the system's mouse events are its echo: drop them. */
    if (s->real_touch > 0 && !s->synth && (ev->type == EVENT_MOUSE_MOVE || ev->type == EVENT_MOUSE_BUTTON))
        return;

    /* A release of a button that is not held (its press went to another window, or the focus loss already let it go)
       says nothing new: every release a program sees has its press. */
    if (ev->type == EVENT_MOUSE_BUTTON && !ev->data.mouse.down && ev->data.mouse.button >= 0 &&
        ev->data.mouse.button < MOUSE_BUTTON_MAX && !s->mouse_down[ev->data.mouse.button])
        return;

    /* Losing the focus lets go of every button: the stream says so too, before the loss, so whoever follows the events
       (not only the polled state) does not keep a button down. */
    if (ev->type == EVENT_WINDOW_FOCUS && !ev->data.focus.gained)
    {
        for (int b = 0; b < MOUSE_BUTTON_MAX; b++)
        {
            if (!s->mouse_down[b])
                continue;
            Event up = {.type = EVENT_MOUSE_BUTTON};
            up.data.mouse.button = b;
            up.data.mouse.down = false;
            up.data.mouse.x = s->mouse_x;
            up.data.mouse.y = s->mouse_y;
            core_push_event(core, &up);
        }
    }

    if (core->hook)
    {
        /* Application code: a window_destroy in it waits (see window_destroy), the backend is still in the middle of
           this event. */
        g_depth++;
        core->hook(core->owner, ev, core->hook_user);
        g_depth--;
    }

    frame_event_add(s, ev);

    switch (ev->type)
    {
    case EVENT_KEY:
    {
        int k = ev->data.key.key;
        if (k > 0 && k < KEY_MAX)
        {
            s->key_down[k] = ev->data.key.down;
            if (ev->data.key.down && !ev->data.key.repeat)
                keycode_push(s, k);
        }
        break;
    }
    case EVENT_CHAR:
        char_push(s, ev->data.codepoint);
        break;
    case EVENT_TEXT_EDIT:
        snprintf(s->composition, sizeof s->composition, "%s", ev->data.edit.text);
        break;
    case EVENT_MOUSE_MOVE:
        s->mouse_x = ev->data.mouse.x;
        s->mouse_y = ev->data.mouse.y;
        if (s->mouse_touch && s->mouse_touch_down)
            mouse_touch_emit(core, TOUCH_MOVE);
        break;
    case EVENT_MOUSE_BUTTON:
    {
        int b = ev->data.mouse.button;
        if (b >= 0 && b < MOUSE_BUTTON_MAX)
        {
            if (ev->data.mouse.down)
            {
                s->mouse_down[b] = true;
                s->mouse_release_pending[b] = false;
            }
            else if (!s->mouse_prev[b])
                s->mouse_release_pending[b] = true; /* pressed in this very frame: stays down until the next */
            else
                s->mouse_down[b] = false;
        }
        if (s->mouse_touch && b == MOUSE_LEFT)
        {
            if (ev->data.mouse.down && !s->mouse_touch_down && s->touch_count == 0)
            {
                s->mouse_touch_down = true;
                mouse_touch_emit(core, TOUCH_DOWN);
            }
            else if (!ev->data.mouse.down && s->mouse_touch_down)
            {
                s->mouse_touch_down = false;
                mouse_touch_emit(core, TOUCH_UP);
            }
        }
        break;
    }
    case EVENT_MOUSE_WHEEL:
        s->wheel_x += ev->data.wheel.x;
        s->wheel_y += ev->data.wheel.y;
        break;
    case EVENT_TOUCH:
        gesture_feed_touch(core, ev->data.touch.phase, ev->data.touch.id, ev->data.touch.x,
                           ev->data.touch.y);
        touch_apply(s, ev->data.touch.id, ev->data.touch.x, ev->data.touch.y,
                    ev->data.touch.pressure, ev->data.touch.phase);
        break;
    case EVENT_WINDOW_RESIZE:
        if (ev->data.resize.w > 0 && ev->data.resize.h > 0)
        {
            s->gesture.width = (float)ev->data.resize.w;
            s->gesture.height = (float)ev->data.resize.h;
        }
        break;
    case EVENT_WINDOW_FOCUS:
        if (!ev->data.focus.gained)
        {
            memset(s->key_down, 0, sizeof s->key_down);
            memset(s->mouse_down, 0, sizeof s->mouse_down);
            memset(s->mouse_release_pending, 0, sizeof s->mouse_release_pending);
        }
        break;
    case EVENT_WINDOW_CLOSE:
        s->close_request = true;
        break;
    default:
        break;
    }
}


/* A finger's coordinate as the pixel of the mouse it drives (and touch_x / touch_y): the system sends finite values, a driver or
   a bug may not (NaN, infinity, 1e38), and a float that does not fit an int is undefined behaviour to convert. Out of range is
   clamped to what a window coordinate can mean, NaN is the origin. */
static int touch_pixel(float v)
{
    if (!(v == v))
        return 0;
    return v < -1e9f ? -1000000000 : v > 1e9f ? 1000000000 : (int)v;
}

void core_push_touch(Core *core, TouchPhase phase, int id, float x, float y, float pressure)
{
    InputState *s = &core->in;
    Event t = {.type = EVENT_TOUCH};
    t.data.touch.id = id;
    t.data.touch.x = x;
    t.data.touch.y = y;
    t.data.touch.pressure = pressure;
    t.data.touch.phase = phase;
    core_push_event(core, &t);

    bool down = phase == TOUCH_DOWN;
    bool up = phase == TOUCH_UP || phase == TOUCH_CANCEL;
    if (down)
        s->real_touch++;
    else if (up && s->real_touch > 0)
        s->real_touch--;

    /* the first finger is the mouse */
    s->synth = true;
    if (down && !s->primary_active)
    {
        s->primary_active = true;
        s->primary_touch = id;
        Event m = {.type = EVENT_MOUSE_MOVE};
        m.data.mouse.x = touch_pixel(x);
        m.data.mouse.y = touch_pixel(y);
        core_push_event(core, &m);
        Event b = {.type = EVENT_MOUSE_BUTTON};
        b.data.mouse.button = MOUSE_LEFT;
        b.data.mouse.down = true;
        b.data.mouse.x = touch_pixel(x);
        b.data.mouse.y = touch_pixel(y);
        core_push_event(core, &b);
    }
    else if (phase == TOUCH_MOVE && s->primary_active && s->primary_touch == id)
    {
        Event m = {.type = EVENT_MOUSE_MOVE};
        m.data.mouse.x = touch_pixel(x);
        m.data.mouse.y = touch_pixel(y);
        core_push_event(core, &m);
    }
    else if (up && s->primary_active && s->primary_touch == id)
    {
        s->primary_active = false;
        Event b = {.type = EVENT_MOUSE_BUTTON};
        b.data.mouse.button = MOUSE_LEFT;
        b.data.mouse.down = false;
        b.data.mouse.x = touch_pixel(x);
        b.data.mouse.y = touch_pixel(y);
        core_push_event(core, &b);
    }
    s->synth = false;
}

/* ========================================================================== */
/*  Lifecycle                                                                 */
/* ========================================================================== */

static uint64_t g_time_base; /* nanoseconds of the monotonic clock at the first use of this clock; 0 until then */

static uint64_t now_nanos(void)
{
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;
    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    uint64_t whole = (uint64_t)counter.QuadPart / (uint64_t)freq.QuadPart;
    uint64_t rest = (uint64_t)counter.QuadPart % (uint64_t)freq.QuadPart;
    return whole * 1000000000ull + rest * 1000000000ull / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* The zero of time_nanos and time_seconds: the first of platform_init and the first read of the clock, once for the life of the
   process (so a clock read before platform_init does not jump back, to zero, when it runs, nor at every platform_init after
   a shutdown). Atomic, because any thread may read the time first. */
static uint64_t time_base(void)
{
#if defined(_WIN32)
    uint64_t base = (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&g_time_base, 0, 0);
    if (base)
        return base;
    uint64_t now = now_nanos();
    base = (uint64_t)InterlockedCompareExchange64((volatile LONG64 *)&g_time_base, (LONG64)now, 0);
    return base ? base : now;
#else
    uint64_t base = __atomic_load_n(&g_time_base, __ATOMIC_RELAXED);
    if (base)
        return base;
    uint64_t now = now_nanos(), expected = 0;
    return __atomic_compare_exchange_n(&g_time_base, &expected, now, false, __ATOMIC_RELAXED, __ATOMIC_RELAXED) ? now : expected;
#endif
}

/* Whether platform_init has succeeded and platform_shutdown has not been called since. */
static bool g_ready;

#ifndef __EMSCRIPTEN__
bool platform_web_download(const char *path, const char *name)
{
    (void)path;
    (void)name;
    return false;
}
#endif

bool platform_init(void)
{
    if (g_ready)
        return true; /* a second call would open the display again and lose the first */
    time_base();
    if (!backend_init())
        return false;
    gamepad_init();
    g_ready = true;
    return true;
}

static void platform_shutdown_now(void)
{
    if (!g_ready)
        return; /* never up, failed to come up, or already down: there is nothing to take down */
    g_ready = false;
    /* A window the program still holds goes with the connection it lives on (it would be a leak and, for the backend,
       a handle to a display that is gone). The program's pointer is then stale, as after window_destroy. */
    while (g_windows)
        window_free(g_windows);
    gamepad_shutdown();
    backend_shutdown();
}

void platform_shutdown(void)
{
    if (g_depth > 0)
    {
        g_shutdown_wanted = true; /* from inside a hook or a frame: when that returns */
        g_reap_due = true;
    }
    else
        platform_shutdown_now();
}

bool core_platform_ready(void)
{
    return g_ready;
}

PlatformWindow *window_create(const WindowConfig *cfg)
{
    if (!cfg)
        return NULL;
    reap(); /* windows a hook asked to destroy, if no application code runs now */
    if (!g_ready)
    {
        /* the backends use their connection to the system, which is not there before platform_init succeeds */
        error_set("the platform is not initialized (platform_init has not succeeded)");
        return NULL;
    }
    const GLConfig *gl = &cfg->gl;
    if (gl->profile < GL_PROFILE_DEFAULT || gl->profile > GL_PROFILE_ES || gl->major < 0 ||
        gl->minor < 0 || gl->msaa < 0)
    {
        error_set("invalid OpenGL configuration");
        return NULL;
    }
    PlatformWindow *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->cfg = *cfg;
    if (w->cfg.monitor == MONITOR_MOUSE)
    {
        /* Under XWayland the X pointer only moves over X windows, so it is stale while the mouse
           is over a native Wayland window (the terminal, the editor). The compositor knows where
           the mouse is and places the new window there by itself (MONITOR_CURRENT). */
        int under = getenv("WAYLAND_DISPLAY") ? -1 : monitor_from_mouse();
        w->cfg.monitor = under >= 0 ? under : MONITOR_CURRENT;
    }
    w->core.owner = w;
    gesture_state_init(&w->core.in.gesture);
    if (cfg->width > 0 && cfg->height > 0)
    {
        w->core.in.gesture.width = (float)cfg->width;
        w->core.in.gesture.height = (float)cfg->height;
    }
    w->b = backend_create(&w->cfg);
    if (!w->b)
    {
        free(w);
        return NULL;
    }
    w->next = g_windows;
    g_windows = w;
    return w;
}

void window_destroy(PlatformWindow *w)
{
    if (!w)
        return;
    PlatformWindow *known = g_windows;
    while (known && known != w)
        known = known->next;
    if (!known || w->dead)
        return; /* not a window of this process (already destroyed, or the platform shut down) */
    if (g_depth > 0)
    {
        /* Called from a hook or a frame callback of this window or another: the backend is still using the window's
           data. It closes from now on (a loop on window_should_close ends) and goes when the code that called returns. */
        w->dead = true;
        w->should_close = true;
        g_reap_due = true;
        return;
    }
    window_free(w);
}

bool window_should_close(PlatformWindow *w)
{
    if (!w)
        return true;
    return w->should_close;
}
void window_set_should_close(PlatformWindow *w, bool v)
{
    if (!w)
        return;
    w->should_close = v;
}

void window_set_user_ptr(PlatformWindow *w, void *ptr)
{
    if (!w)
        return;
    w->user = ptr;
}
void *window_get_user_ptr(PlatformWindow *w)
{
    if (!w)
        return NULL;
    return w->user;
}

/* Tell this window about devices that came or went since it last looked. A slot
   whose generation changed is a different device: the old one went, a new one came. */
static void device_events(Core *core)
{
    InputState *s = &core->in;
    unsigned epoch = gamepad_internal_epoch();
    if (epoch == s->dev_epoch)
        return; /* no device came or went since this window looked: the common frame */
    s->dev_epoch = epoch;
    for (int i = 0; i < GAMEPAD_MAX; i++)
    {
        unsigned now = gamepad_connected(i) ? gamepad_internal_generation(i) : 0;
        if (s->pad_seen[i] && s->pad_seen[i] != now)
        {
            Event e = {.type = EVENT_GAMEPAD_DISCONNECTED};
            e.data.device.index = i;
            core_push_event(core, &e);
        }
        if (now && s->pad_seen[i] != now)
        {
            Event e = {.type = EVENT_GAMEPAD_CONNECTED};
            e.data.device.index = i;
            core_push_event(core, &e);
        }
        s->pad_seen[i] = now;
    }
    for (int i = 0; i < JOYSTICK_MAX; i++)
    {
        unsigned now = joystick_connected(i) ? joystick_internal_generation(i) : 0;
        if (s->joy_seen[i] && s->joy_seen[i] != now)
        {
            Event e = {.type = EVENT_JOYSTICK_DISCONNECTED};
            e.data.device.index = i;
            core_push_event(core, &e);
        }
        if (now && s->joy_seen[i] != now)
        {
            Event e = {.type = EVENT_JOYSTICK_CONNECTED};
            e.data.device.index = i;
            core_push_event(core, &e);
        }
        s->joy_seen[i] = now;
    }
}

void window_begin_frame(PlatformWindow *w)
{
    if (!w)
        return;
    InputState *s = &w->core.in;
    memcpy(s->key_prev, s->key_down, sizeof s->key_down);
    memcpy(s->mouse_prev, s->mouse_down, sizeof s->mouse_down);
    for (int b = 0; b < MOUSE_BUTTON_MAX; b++)
        if (s->mouse_release_pending[b])
        {
            s->mouse_release_pending[b] = false;
            s->mouse_down[b] = false; /* the release of last frame's click: seen as an edge now */
        }
    s->mouse_px = s->mouse_x;
    s->mouse_py = s->mouse_y;
    s->wheel_x = s->wheel_y = 0;
    s->fe_count = 0;
    s->fe_cursor = 0;
    s->fe_movers_ok = false;
    /* keycode_q and char_q are not cleared here: the consumer drains them */

    gamepad_poll();
    device_events(&w->core);
    /* Before the new events: last frame's TAP turns into HOLD and a SWIPE ends, so
       a gesture born in this frame's events is still visible after this call. */
    gesture_update(&s->gesture, time_seconds());
    g_depth++;
    backend_pump_events(w->b, &w->core);

    if (s->close_request)
    {
        s->close_request = false;
        window_set_should_close(w, true);
    }
    if (s->exit_key && key_pressed(w, s->exit_key))
        window_set_should_close(w, true);
    scope_leave();
}

void window_swap(PlatformWindow *w)
{
    if (!w)
        return;
    /* Wayland dispatches the compositor's events while it waits for the frame callback, and some of them run the application's
       hit test: a window destroyed there waits until the swap is done with it. */
    g_depth++;
    backend_swap(w->b);
    scope_leave();
}

void app_run(PlatformWindow *w, FrameCallback frame, void *user)
{
    if (!w || !frame)
        return;
#if defined(__EMSCRIPTEN__)
    /* the loop is the browser's and never comes back here: each frame is its own scope (backend_web.c main_tick) */
    backend_run(w->b, w, frame, user);
#else
    g_depth++; /* the loop and the frame callbacks: the window outlives a window_destroy made in them until this returns */
    backend_run(w->b, w, frame, user);
    scope_leave();
#endif
}

/* ---- waiting for events ---- */

/* The wake: one object for the whole life of the process, made the first time anyone needs it. platform_wake may be called by
   any thread at any time, also while another thread shuts the platform down, so it cannot be tied to platform_init: a
   descriptor closed with the platform and given to something else would be written to. The cost is one descriptor (two on a
   system without eventfd) that is never closed, the same however many times the platform is started and stopped. */
#if defined(_WIN32)

static HANDLE g_wake_event;

void *core_wake_handle(void)
{
    HANDLE h = InterlockedCompareExchangePointer(&g_wake_event, NULL, NULL);
    if (h)
        return h;
    h = CreateEventW(NULL, FALSE, FALSE, NULL); /* auto-reset: the wait that ends on it takes the wake */
    if (!h)
        return NULL;
    HANDLE first = InterlockedCompareExchangePointer(&g_wake_event, h, NULL);
    if (first)
    {
        CloseHandle(h);
        h = first;
    }
    return h;
}

static void wake_post(void)
{
    HANDLE h = core_wake_handle();
    if (h)
        SetEvent(h);
}

bool core_wake_take(void)
{
    HANDLE h = core_wake_handle();
    return h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0;
}

#elif defined(__EMSCRIPTEN__)

int core_wake_fd(void)
{
    return -1;
}

static void wake_post(void)
{
}

bool core_wake_take(void)
{
    return false;
}

#else

/* The two ends of the wake, one more than the descriptor in each half (so that zero means "not made yet") of one word that a
   compare-and-swap publishes: two threads that both make one never leave the readers with the read end of one and the write
   end of the other. On Linux an eventfd is both ends. */
static unsigned long long g_wake_pair;

static bool wake_ends(int *rd, int *wr)
{
    unsigned long long p = __atomic_load_n(&g_wake_pair, __ATOMIC_ACQUIRE);
    if (!p)
    {
        int fds[2];
#if defined(__linux__)
        fds[0] = fds[1] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fds[0] < 0)
            return false;
#else
        if (pipe(fds) < 0)
            return false;
        for (int i = 0; i < 2; i++)
        {
            fcntl(fds[i], F_SETFD, FD_CLOEXEC);
            fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        }
#endif
        unsigned long long mine = ((unsigned long long)(unsigned)(fds[1] + 1) << 32) | (unsigned)(fds[0] + 1), none = 0;
        if (__atomic_compare_exchange_n(&g_wake_pair, &none, mine, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            p = mine;
        else
        {
            p = none; /* another thread was first: use its descriptors, close ours */
            close(fds[0]);
            if (fds[1] != fds[0])
                close(fds[1]);
        }
    }
    *rd = (int)(p & 0xffffffffu) - 1;
    *wr = (int)(p >> 32) - 1;
    return true;
}

int core_wake_fd(void)
{
    int rd, wr;
    return wake_ends(&rd, &wr) ? rd : -1;
}

/* A write on a descriptor that exists: it is safe in a signal handler (errno is put back), which is the one thing a handler
   may need to do to get a blocked loop to look at a flag. The first call of all would make the descriptor, which is only
   reliable outside a handler; a program that wakes from a handler calls platform_wake once, or waits once, before. */
static void wake_post(void)
{
    int rd, wr, saved = errno;
    if (wake_ends(&rd, &wr))
    {
#if defined(__linux__)
        unsigned long long one = 1;
        ssize_t n = write(wr, &one, sizeof one);
#else
        ssize_t n = write(wr, "x", 1);
#endif
        (void)n; /* EAGAIN: a wake is already pending, which is all this one would have said */
    }
    errno = saved;
}

bool core_wake_take(void)
{
    int rd, wr;
    if (!wake_ends(&rd, &wr))
        return false;
    bool had = false;
    char buf[64];
    for (;;)
    {
        ssize_t n = read(rd, buf, sizeof buf);
        if (n <= 0)
            break;
        had = true;
#if defined(__linux__)
        break; /* an eventfd gives its whole count in one read */
#endif
    }
    return had;
}

#endif

int64_t core_wait_ms(int64_t ns)
{
    if (ns < 0)
        return -1;
    int64_t ms = ns / 1000000 + (ns % 1000000 != 0); /* never early: a deadline loop would spin on the part of a ms left */
    return ms > 0x7fffffff ? 0x7fffffff : ms;
}

bool core_wake_only_wait(int64_t ns)
{
#if defined(_WIN32)
    HANDLE h = core_wake_handle();
    if (!h)
        return false;
    int64_t ms = core_wait_ms(ns);
    return WaitForSingleObject(h, ms < 0 ? INFINITE : (DWORD)ms) == WAIT_OBJECT_0;
#elif defined(__EMSCRIPTEN__)
    (void)ns;
    return true; /* nothing can be waited on in a browser: the caller goes on to its frame */
#else
    int fd = core_wake_fd();
    if (fd < 0)
        return false; /* no way to wait at all: better to return than to sleep for ever */
    struct pollfd p = {.fd = fd, .events = POLLIN};
    return poll(&p, 1, (int)core_wait_ms(ns)) > 0 && core_wake_take();
#endif
}

void platform_wake(void)
{
    wake_post();
    backend_wake();
}

/* State that only the next window_begin_frame changes, so a loop that sleeps between events has to run it: the release of a
   click that came in the frame of its press (mouse_down stays true until then) and a gesture that turns from a tap into a hold
   or ends. A window that nobody pumps keeps this true, which is the caller's to avoid. */
static bool frame_due(void)
{
    for (PlatformWindow *w = g_windows; w; w = w->next)
    {
        if (w->dead)
            continue;
        const InputState *s = &w->core.in;
        for (int b = 0; b < MOUSE_BUTTON_MAX; b++)
            if (s->mouse_release_pending[b])
                return true;
        if (gesture_update_pending(&s->gesture))
            return true;
    }
    return false;
}

bool platform_wait_events(double timeout_seconds)
{
    int64_t ns;
    if (timeout_seconds != timeout_seconds)
        ns = 0; /* NaN */
    else if (timeout_seconds < 0 || timeout_seconds >= 3e9)
        ns = -1;
    else
        ns = (int64_t)(timeout_seconds * 1e9);
    if (!g_ready)
        return core_wake_only_wait(ns);
    if (frame_due())
        return true;
    return backend_wait_events(ns);
}

static bool g_frame_wanted;
static int64_t g_frame_at = -1; /* time_nanos() of the frame the application asked for, or -1 */

void app_request_frame(void)
{
    g_frame_wanted = true;
}

void app_request_frame_at(double seconds)
{
    if (seconds != seconds)
        return;
    int64_t at = seconds <= 0 ? 0 : seconds >= 9e9 ? INT64_MAX : (int64_t)(seconds * 1e9);
    if (g_frame_at < 0 || at < g_frame_at)
        g_frame_at = at;
}

void app_run_events(PlatformWindow *w, FrameCallback frame, void *user)
{
    if (!w || !frame)
        return;
#if defined(__EMSCRIPTEN__)
    backend_run(w->b, w, frame, user); /* the browser decides when a frame is due; there is nothing to sleep on */
#else
    g_depth++;
    g_frame_wanted = true; /* the first frame draws what is there */
    while (!window_should_close(w))
    {
        double wait = -1;
        if (g_frame_wanted)
            wait = 0;
        else if (g_frame_at >= 0)
        {
            int64_t left = g_frame_at - (int64_t)time_nanos();
            wait = left > 0 ? (double)left / 1e9 : 0;
        }
        bool arrived = platform_wait_events(wait);
        window_begin_frame(w);
        bool due = g_frame_wanted || (g_frame_at >= 0 && (int64_t)time_nanos() >= g_frame_at);
        if (!arrived && !due && w->core.in.fe_count == 0)
            continue; /* a service tick or a wait that ran out with the deadline still ahead: nothing to show */
        g_frame_wanted = false;
        if (g_frame_at >= 0 && (int64_t)time_nanos() >= g_frame_at)
            g_frame_at = -1;
        frame(w, user);
        window_swap(w);
    }
    scope_leave();
#endif
}

bool poll_event(PlatformWindow *w, Event *out)
{
    if (!w)
        return false;
    InputState *s = &w->core.in;
    if (s->fe_cursor >= s->fe_count)
        return false;
    *out = s->frame_events[s->fe_cursor++];
    return true;
}

/* ========================================================================== */
/*  OpenGL context                                                            */
/* ========================================================================== */

void window_make_current(PlatformWindow *w)
{
    if (!w)
        return;
    backend_make_current(w->b);
}
void window_make_current_on(PlatformWindow *target, PlatformWindow *context)
{
    if (!target || !context)
        return;
    backend_make_current_on(target->b, context->b);
}
void window_set_event_hook(PlatformWindow *w, EventHook hook, void *user)
{
    if (!w)
        return;
    w->core.hook = hook;
    w->core.hook_user = user;
}

void window_text_input_start(PlatformWindow *w)
{
    if (!w)
        return;
    w->core.in.text_input_off = false;
    backend_set_text_input(w->b, true);
}

void window_text_input_stop(PlatformWindow *w)
{
    if (!w)
        return;
    w->core.in.text_input_off = true;
    w->core.in.composition[0] = '\0';
    backend_set_text_input(w->b, false);
}

bool window_text_input_active(PlatformWindow *w)
{
    if (!w)
        return false;
    return !w->core.in.text_input_off;
}

void window_set_text_input_rect(PlatformWindow *w, int x, int y, int width, int height)
{
    if (!w)
        return;
    backend_set_text_input_rect(w->b, x, y, width, height);
}

const char *window_text_composition(PlatformWindow *w)
{
    if (!w)
        return "";
    return w->core.in.composition;
}

void window_set_decorated(PlatformWindow *w, bool on)
{
    if (!w)
        return;
    backend_set_decorated(w->b, on);
}

void window_set_hit_test(PlatformWindow *w, HitTestFunc fn, void *user)
{
    if (!w)
        return;
    backend_set_hit_test(w->b, w, fn, user);
}

PlatformCursor *cursor_create(const uint32_t *argb, int width, int height, int hot_x, int hot_y)
{
    if (!argb || width < 1 || height < 1 || width > 256 || height > 256 || hot_x < 0 || hot_y < 0 ||
        hot_x >= width || hot_y >= height)
    {
        error_set("invalid cursor image");
        return NULL;
    }
    if (!g_ready)
    {
        error_set("the platform is not initialized (platform_init has not succeeded)");
        return NULL;
    }
    return backend_cursor_create(argb, width, height, hot_x, hot_y);
}

void cursor_destroy(PlatformCursor *cursor)
{
    if (cursor && g_ready) /* after platform_shutdown the connection it belonged to is gone */
        backend_cursor_destroy(cursor);
}

void mouse_set_cursor_image(PlatformWindow *w, PlatformCursor *cursor)
{
    if (!w)
        return;
    backend_set_cursor_image(w->b, cursor);
}

void window_set_live_callback(PlatformWindow *w, FrameCallback cb, void *user)
{
    if (!w)
        return;
    backend_set_live_callback(w->b, w, cb, user);
}

bool mouse_capture(PlatformWindow *w, bool on)
{
    if (!w)
        return false;
    return backend_mouse_capture(w->b, on);
}

void *window_native_handle(PlatformWindow *w, NativeHandleType type)
{
    if (!w)
        return NULL;
    return backend_native_handle(w->b, type);
}

void window_set_vsync(PlatformWindow *w, bool on)
{
    if (!w)
        return;
    backend_set_vsync(w->b, on);
}
void *gl_proc_address(const char *name)
{
    if (!g_ready || !name)
        return NULL;
    return backend_gl_proc_address(name);
}
GLWorker *gl_worker_create(PlatformWindow *w)
{
    if (!w)
    {
        error_set("no window (NULL)");
        return NULL;
    }
    return (GLWorker *)backend_gl_worker_create(w->b);
}
bool gl_worker_make_current(GLWorker *worker, bool current)
{
    return worker && backend_gl_worker_make_current((BackendGLWorker *)worker, current);
}
void gl_worker_destroy(GLWorker *worker)
{
    if (worker)
        backend_gl_worker_destroy((BackendGLWorker *)worker);
}

bool window_lock_pixels(PlatformWindow *w, Framebuffer *out)
{
    if (!w)
    {
        error_set("no window (NULL)");
        return false;
    }
    return backend_lock_pixels(w->b, out);
}
void window_present_pixels(PlatformWindow *w)
{
    if (!w)
        return;
    backend_present_pixels(w->b);
}

/* ========================================================================== */
/*  Geometry                                                                  */
/* ========================================================================== */

void window_get_size(PlatformWindow *w, int *width, int *height)
{
    if (!w)
    {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
        return;
    }
    backend_get_size(w->b, width, height);
}
void window_set_size(PlatformWindow *w, int width, int height)
{
    if (!w)
        return;
    backend_set_size(w->b, width, height);
}
void window_get_framebuffer_size(PlatformWindow *w, int *width, int *height)
{
    if (!w)
    {
        if (width)
            *width = 0;
        if (height)
            *height = 0;
        return;
    }
    backend_get_fb_size(w->b, width, height);
}
void window_get_position(PlatformWindow *w, int *x, int *y)
{
    if (!w)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    backend_get_pos(w->b, x, y);
}
void window_set_position(PlatformWindow *w, int x, int y)
{
    if (!w)
        return;
    backend_set_pos(w->b, x, y);
}
void window_set_title(PlatformWindow *w, const char *title)
{
    if (!w)
        return;
    backend_set_title(w->b, title);
}
void window_set_size_limits(PlatformWindow *w, int minw, int minh, int maxw, int maxh)
{
    if (!w)
        return;
    backend_set_size_limits(w->b, minw, minh, maxw, maxh);
}
float window_content_scale(PlatformWindow *w)
{
    if (!w)
        return 1.0f;
    return backend_content_scale(w->b);
}

void window_center_on_monitor(PlatformWindow *w, int monitor)
{
    if (!w)
        return;
    MonitorInfo m;
    if (!monitor_get_info(monitor, &m))
        return;
    int ww, wh;
    window_get_size(w, &ww, &wh);
    window_set_position(w, m.x + (m.width - ww) / 2, m.y + (m.height - wh) / 2);
}

void window_set_monitor(PlatformWindow *w, int monitor)
{
    if (!w)
        return;
    MonitorInfo m;
    if (!monitor_get_info(monitor, &m))
        return;
    window_set_position(w, m.x, m.y);
}

/* ========================================================================== */
/*  PlatformWindow state                                                              */
/* ========================================================================== */

void window_minimize(PlatformWindow *w)
{
    if (!w)
        return;
    backend_minimize(w->b);
}
void window_maximize(PlatformWindow *w)
{
    if (!w)
        return;
    backend_maximize(w->b);
}
void window_restore(PlatformWindow *w)
{
    if (!w)
        return;
    backend_restore(w->b);
}
void window_show(PlatformWindow *w)
{
    if (!w)
        return;
    backend_show(w->b);
}
void window_hide(PlatformWindow *w)
{
    if (!w)
        return;
    backend_hide(w->b);
}
void window_focus(PlatformWindow *w)
{
    if (!w)
        return;
    backend_focus(w->b);
}
void window_request_attention(PlatformWindow *w)
{
    if (!w)
        return;
    backend_request_attention(w->b);
}

bool window_is_focused(PlatformWindow *w)
{
    if (!w)
        return false;
    return backend_get_flag(w->b, WIN_FLAG_FOCUSED);
}
bool window_is_minimized(PlatformWindow *w)
{
    if (!w)
        return false;
    return backend_get_flag(w->b, WIN_FLAG_MINIMIZED);
}
bool window_is_maximized(PlatformWindow *w)
{
    if (!w)
        return false;
    return backend_get_flag(w->b, WIN_FLAG_MAXIMIZED);
}
bool window_is_visible(PlatformWindow *w)
{
    if (!w)
        return false;
    return backend_get_flag(w->b, WIN_FLAG_VISIBLE);
}
bool window_is_hovered(PlatformWindow *w)
{
    if (!w)
        return false;
    return backend_get_flag(w->b, WIN_FLAG_HOVERED);
}

void window_set_mode(PlatformWindow *w, WindowMode mode, int monitor)
{
    if (!w)
        return;
    backend_set_mode(w->b, mode, monitor);
}
WindowMode window_get_mode(PlatformWindow *w)
{
    if (!w)
        return WINDOW_WINDOWED;
    return backend_get_mode(w->b);
}

void window_set_icon(PlatformWindow *w, int width, int height, const uint8_t *rgba)
{
    if (!w)
        return;
    backend_set_icon(w->b, width, height, rgba);
}
void window_set_opacity(PlatformWindow *w, float alpha)
{
    if (!w)
        return;
    backend_set_opacity(w->b, alpha);
}
void window_set_always_on_top(PlatformWindow *w, bool on)
{
    if (!w)
        return;
    backend_set_always_on_top(w->b, on);
}

/* ========================================================================== */
/*  Monitors                                                                  */
/* ========================================================================== */

int monitor_count(void)
{
    return g_ready ? backend_monitor_count() : 0;
}
bool monitor_get_info(int index, MonitorInfo *out)
{
    return g_ready && out && backend_monitor_info(index, out);
}

int monitor_from_point(int x, int y)
{
    int n = monitor_count();
    for (int i = 0; i < n; i++)
    {
        MonitorInfo m;
        if (!monitor_get_info(i, &m))
            continue;
        if (x >= m.x && x < m.x + m.width && y >= m.y && y < m.y + m.height)
            return i;
    }
    return -1;
}

bool mouse_global_position(int *x, int *y)
{
    int px = 0, py = 0;
    bool known = g_ready && backend_mouse_global_position(&px, &py);
    if (x)
        *x = known ? px : 0;
    if (y)
        *y = known ? py : 0;
    return known;
}

int monitor_from_mouse(void)
{
    int x, y;
    if (!mouse_global_position(&x, &y))
        return -1;
    return monitor_from_point(x, y);
}

int monitor_from_window(PlatformWindow *w)
{
    if (!w)
        return -1;
    int x, y, ww, wh;
    window_get_position(w, &x, &y);
    window_get_size(w, &ww, &wh);
    int m = monitor_from_point(x + ww / 2, y + wh / 2);
    return m < 0 ? 0 : m;
}

/* ========================================================================== */
/*  Keyboard                                                                  */
/* ========================================================================== */

int key_mods(PlatformWindow *w)
{
    if (!w)
        return 0;
    const bool *k = w->core.in.key_down;
    int mods = 0;
    if (k[KEY_LEFT_SHIFT] || k[KEY_RIGHT_SHIFT])
        mods |= KEYMOD_SHIFT;
    if (k[KEY_LEFT_CONTROL] || k[KEY_RIGHT_CONTROL])
        mods |= KEYMOD_CTRL;
    if (k[KEY_LEFT_ALT] || k[KEY_RIGHT_ALT])
        mods |= KEYMOD_ALT;
    if (k[KEY_LEFT_SUPER] || k[KEY_RIGHT_SUPER])
        mods |= KEYMOD_SUPER;
    return mods | backend_lock_state();
}

bool key_down(PlatformWindow *w, int key)
{
    if (!w)
        return false;
    return key > 0 && key < KEY_MAX && w->core.in.key_down[key];
}
bool key_up(PlatformWindow *w, int key)
{
    if (!w)
        return false;
    return !key_down(w, key);
}
bool key_pressed(PlatformWindow *w, int key)
{
    if (!w)
        return false;
    return key > 0 && key < KEY_MAX && w->core.in.key_down[key] && !w->core.in.key_prev[key];
}
bool key_released(PlatformWindow *w, int key)
{
    if (!w)
        return false;
    return key > 0 && key < KEY_MAX && !w->core.in.key_down[key] && w->core.in.key_prev[key];
}

int key_get_pressed(PlatformWindow *w)
{
    if (!w)
        return 0;
    return keycode_pop(&w->core.in);
}
uint32_t char_get_pressed(PlatformWindow *w)
{
    if (!w)
        return 0;
    return char_pop(&w->core.in);
}

void key_set_exit(PlatformWindow *w, int key)
{
    if (!w)
        return;
    w->core.in.exit_key = key;
}

/* ========================================================================== */
/*  Mouse                                                                     */
/* ========================================================================== */

bool mouse_button_down(PlatformWindow *w, int button)
{
    if (!w)
        return false;
    return button >= 0 && button < MOUSE_BUTTON_MAX && w->core.in.mouse_down[button];
}
bool mouse_button_up(PlatformWindow *w, int button)
{
    if (!w)
        return false;
    return !mouse_button_down(w, button);
}
bool mouse_button_pressed(PlatformWindow *w, int button)
{
    if (!w)
        return false;
    return button >= 0 && button < MOUSE_BUTTON_MAX &&
           w->core.in.mouse_down[button] && !w->core.in.mouse_prev[button];
}
bool mouse_button_released(PlatformWindow *w, int button)
{
    if (!w)
        return false;
    return button >= 0 && button < MOUSE_BUTTON_MAX &&
           !w->core.in.mouse_down[button] && w->core.in.mouse_prev[button];
}

int mouse_x(PlatformWindow *w)
{
    if (!w)
        return 0;
    return w->core.in.mouse_x;
}
int mouse_y(PlatformWindow *w)
{
    if (!w)
        return 0;
    return w->core.in.mouse_y;
}
void mouse_position(PlatformWindow *w, int *x, int *y)
{
    if (!w)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    if (x)
        *x = w->core.in.mouse_x;
    if (y)
        *y = w->core.in.mouse_y;
}
void mouse_delta(PlatformWindow *w, int *dx, int *dy)
{
    if (!w)
    {
        if (dx)
            *dx = 0;
        if (dy)
            *dy = 0;
        return;
    }
    if (dx)
        *dx = w->core.in.mouse_x - w->core.in.mouse_px;
    if (dy)
        *dy = w->core.in.mouse_y - w->core.in.mouse_py;
}
float mouse_wheel(PlatformWindow *w)
{
    if (!w)
        return 0;
    return w->core.in.wheel_y;
}
void mouse_wheel_v(PlatformWindow *w, float *x, float *y)
{
    if (!w)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    if (x)
        *x = w->core.in.wheel_x;
    if (y)
        *y = w->core.in.wheel_y;
}

void mouse_set_position(PlatformWindow *w, int x, int y)
{
    if (!w)
        return;
    w->core.in.mouse_x = x;
    w->core.in.mouse_y = y;
    backend_set_mouse_pos(w->b, x, y);
}
void mouse_set_cursor(PlatformWindow *w, int cursor)
{
    if (!w)
        return;
    backend_set_cursor(w->b, cursor);
}
void mouse_set_mode(PlatformWindow *w, int mode)
{
    if (!w)
        return;
    backend_set_mouse_mode(w->b, mode);
}

/* ========================================================================== */
/*  Touch                                                                     */
/* ========================================================================== */

int touch_count(PlatformWindow *w)
{
    if (!w)
        return 0;
    return w->core.in.touch_count;
}
int touch_x(PlatformWindow *w, int index)
{
    if (!w)
        return 0;
    return (index >= 0 && index < w->core.in.touch_count) ? touch_pixel(w->core.in.touch[index].x) : 0;
}
int touch_y(PlatformWindow *w, int index)
{
    if (!w)
        return 0;
    return (index >= 0 && index < w->core.in.touch_count) ? touch_pixel(w->core.in.touch[index].y) : 0;
}
void touch_position(PlatformWindow *w, int index, float *x, float *y)
{
    if (!w)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    if (index < 0 || index >= w->core.in.touch_count)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    if (x)
        *x = w->core.in.touch[index].x;
    if (y)
        *y = w->core.in.touch[index].y;
}
/* ---- gestures ---- */

void gesture_set_enabled(PlatformWindow *w, unsigned flags)
{
    if (!w)
        return;
    w->core.in.gesture.enabled = flags;
}
unsigned gesture_detected(PlatformWindow *w)
{
    if (!w)
        return 0;
    return gesture_state_detected(&w->core.in.gesture);
}
bool gesture_is_detected(PlatformWindow *w, unsigned gesture)
{
    if (!w)
        return false;
    return gesture != GESTURE_NONE && gesture_detected(w) == gesture;
}
float gesture_hold_duration(PlatformWindow *w)
{
    if (!w)
        return 0;
    return gesture_state_hold_duration(&w->core.in.gesture, time_seconds());
}
void gesture_drag_vector(PlatformWindow *w, float *x, float *y)
{
    if (!w)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    if (x)
        *x = w->core.in.gesture.drag_vector.x;
    if (y)
        *y = w->core.in.gesture.drag_vector.y;
}
float gesture_drag_angle(PlatformWindow *w)
{
    if (!w)
        return 0;
    return w->core.in.gesture.drag_angle;
}
void gesture_pinch_vector(PlatformWindow *w, float *x, float *y)
{
    if (!w)
    {
        if (x)
            *x = 0;
        if (y)
            *y = 0;
        return;
    }
    if (x)
        *x = w->core.in.gesture.pinch_vector.x;
    if (y)
        *y = w->core.in.gesture.pinch_vector.y;
}
float gesture_pinch_angle(PlatformWindow *w)
{
    if (!w)
        return 0;
    return w->core.in.gesture.pinch_angle;
}
void touch_set_mouse_emulation(PlatformWindow *w, bool on)
{
    if (!w)
        return;
    InputState *s = &w->core.in;
    if (!on && s->mouse_touch_down)
    {
        s->mouse_touch_down = false;
        mouse_touch_emit(&w->core, TOUCH_UP);
    }
    s->mouse_touch = on;
}

int touch_id(PlatformWindow *w, int index)
{
    if (!w)
        return -1;
    return (index >= 0 && index < w->core.in.touch_count) ? w->core.in.touch[index].id : -1;
}

/* ========================================================================== */
/*  Time                                                                      */
/* ========================================================================== */

uint64_t time_nanos(void)
{
    const uint64_t base = time_base(); /* first: the clock is read after the zero is fixed, never before it */
    return now_nanos() - base;
}
double time_seconds(void)
{
    return (double)time_nanos() / 1e9;
}

void time_sleep(uint32_t milliseconds)
{
#if defined(_WIN32)
    /* A high-resolution timer of its own per call: one shared timer would be reset
       by a second thread sleeping at the same time, and the first would wait for ever. */
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x2 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    if (timer)
    {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)milliseconds * 10000;
        bool waited = SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
                      WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
        CloseHandle(timer);
        if (waited)
            return;
    }
    Sleep(milliseconds);
#else
    struct timespec ts = {(time_t)(milliseconds / 1000), (long)(milliseconds % 1000) * 1000000L};
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR)
    {
    }
#endif
}

