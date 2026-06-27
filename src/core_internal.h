/*
 * core_internal.h - structures shared between core.c and the backends.
 * Not part of the public contract. Public names live in platform.h.
 */
#ifndef CORE_INTERNAL_H
#define CORE_INTERNAL_H

#include "platform.h"

typedef struct BackendWindow BackendWindow;

/* PlatformWindow flags queried through backend_get_flag. */
enum
{
    WIN_FLAG_FOCUSED = 0,
    WIN_FLAG_MINIMIZED,
    WIN_FLAG_MAXIMIZED,
    WIN_FLAG_VISIBLE,
    WIN_FLAG_HOVERED
};

#define KEYCODE_QUEUE_LEN 16
#define CHAR_QUEUE_LEN 32
#define FRAME_EVENT_MAX 256

typedef struct
{
    bool key_down[KEY_MAX];
    bool key_prev[KEY_MAX]; /* previous-frame snapshot, drives edges */
    bool mouse_down[MOUSE_BUTTON_MAX];
    bool mouse_prev[MOUSE_BUTTON_MAX];
    int mouse_x, mouse_y, mouse_px, mouse_py;
    float wheel_x, wheel_y;

    int keycode_q[KEYCODE_QUEUE_LEN];
    int kq_head, kq_tail;
    uint32_t char_q[CHAR_QUEUE_LEN];
    int cq_head, cq_tail;

    Event frame_events[FRAME_EVENT_MAX];
    int fe_count;
    int fe_cursor;

    /* Active points kept dense in [0, touch_count); matched by OS id. */
    struct
    {
        int id;
        float x, y, pressure;
    } touch[MAX_TOUCH_POINTS];
    int touch_count;

    bool close_request; /* set by EVENT_WINDOW_CLOSE, drained into should_close */
    int exit_key;
} InputState;

typedef struct Core
{
    InputState in;
} Core;

struct PlatformWindow
{
    Core core;
    BackendWindow *b;
    bool should_close;
    void *user;
    WindowConfig cfg;
};

/* Backend -> core. core_push_event fills the state arrays and the per-frame
   event list in one call; core_push_char only touches the text queue. */
void core_push_event(Core *core, const Event *ev);
void core_push_char(Core *core, uint32_t codepoint);

#endif /* CORE_INTERNAL_H */
