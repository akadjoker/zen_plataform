/*
 * fuzz_touch.c - the touch path that XInput 2 (XI_TouchBegin / Update / End on a desktop touch screen), WM_TOUCH and the
 * Android and web touch events feed: core_push_touch and EVENT_TOUCH with any finger id, any phase and any coordinates
 * (NaN, infinity, 1e38: a server only sends finite values, a driver or a bug may send others), mixed with the mouse, the
 * mouse emulation of the first finger, window resizes, frames, and the gesture and touch queries a program makes. The XI 2
 * events themselves cannot be reached from another client (XSendEvent refuses generic events, and the Xvfb has no touch
 * device); this is what the backend hands the core once it has unpacked one. Over the fake backend: no display. The input is a
 * script (byte 0 of each step picks the operation, the bytes after it fill it, a script that runs out reads zeros).
 * libFuzzer entry point; tools/fuzz.sh builds it with the sanitizers, tests/fuzz/standalone_main.c runs the corpus.
 */
#include "platform.h"
#include "backend_fake.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct
{
    const uint8_t *p, *end;
} Cur;

static uint8_t u8(Cur *c)
{
    return c->p < c->end ? *c->p++ : 0;
}

static uint32_t u32(Cur *c)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | u8(c);
    return v;
}

/* the bits of a float: every value there is, NaN and the infinities included, but weighted to the plain ones */
static float f32(Cur *c)
{
    uint8_t kind = u8(c);
    if (kind < 160)
        return (float)(int)(u32(c) % 4000) - 1000.0f;
    uint32_t bits = u32(c);
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

static PlatformWindow *g_win;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc, (void)argv;
    return platform_init() ? 0 : 77;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 8192)
        return 0;
    WindowConfig cfg = {.title = "fuzz", .width = 320, .height = 240};
    g_win = window_create(&cfg);
    if (!g_win)
        return 0;
    PlatformWindow *w = g_win;
    Cur c = {data, data + size};
    int steps = 0;
    while (c.p < c.end && steps++ < 400)
    {
        switch (u8(&c) % 10)
        {
        case 0:
        case 1: /* a finger as a desktop touch screen sends it: it also drives the mouse */
        {
            int id = (int)u32(&c);
            float x = f32(&c), y = f32(&c);
            fake_real_touch(w, id, x, y, (TouchPhase)(u8(&c) % 4));
            break;
        }
        case 2: /* the raw event of the touch backends (Android, the web) */
        {
            int id = (int)u32(&c);
            float x = f32(&c), y = f32(&c);
            fake_touch(w, id, x, y, (TouchPhase)(u8(&c) % 4));
            break;
        }
        case 3:
            fake_mouse_move(w, (int)u32(&c), (int)u32(&c));
            break;
        case 4:
            fake_mouse_button(w, u8(&c) % 9, u8(&c) & 1);
            break;
        case 5:
            touch_set_mouse_emulation(w, u8(&c) & 1);
            break;
        case 6:
            fake_resize(w, (int)(u32(&c) % 5000) - 100, (int)(u32(&c) % 5000) - 100);
            break;
        case 7:
            gesture_set_enabled(w, u32(&c));
            break;
        default: /* a frame, and everything a program reads after it */
        {
            window_begin_frame(w);
            Event e;
            while (poll_event(w, &e))
                if (e.type == EVENT_TOUCH)
                    (void)e.data.touch.id;
            volatile unsigned acc = gesture_detected(w);
            int n = touch_count(w);
            for (int i = -1; i <= n; i++)
            {
                float x, y;
                touch_position(w, i, &x, &y);
                acc += (unsigned)touch_x(w, i) + (unsigned)touch_y(w, i) + (unsigned)touch_id(w, i);
            }
            float a, b;
            gesture_drag_vector(w, &a, &b);
            gesture_pinch_vector(w, &a, &b);
            /* the angles and the hold time can be NaN for a finger at NaN (garbage in): read the bits, a float to unsigned
               conversion of a NaN is undefined, and it is the harness that would do it */
            float vals[3] = {gesture_drag_angle(w), gesture_pinch_angle(w), gesture_hold_duration(w)};
            for (int i = 0; i < 3; i++)
            {
                unsigned bits;
                memcpy(&bits, &vals[i], sizeof bits);
                acc += bits;
            }
            (void)acc;
            break;
        }
        }
    }
    window_begin_frame(w);
    window_destroy(w);
    return 0;
}
