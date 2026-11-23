/*
 * fuzz_wayland.c - a hostile compositor. The small compositor of tests/test_input_wayland.c (included below, its main renamed)
 * runs in a thread of the process and the library is its client; the input is a script of what the compositor does: keys
 * with any code, key focus and pointer focus coming and going in any order and twice in a row, pointer motion, buttons, axes
 * and wheel notches with any values, configures with any size and any state bits (zero, negative, 2^31 - 1), a layout change
 * in the middle, a pointer lock that is granted and taken back. Every message is legal as the protocol sees it: libwayland
 * on the client side drops a connection that sends or receives a malformed one, so what can hurt the library is a compositor
 * that is wrong in meaning, not in bytes. After each few steps the client runs frames, as a program does. A failure is a
 * sanitizer report, a hang, or the client's connection going down with an error (the library reports it and carries on:
 * not a failure; a crash is). libFuzzer entry point; tools/fuzz.sh builds it with the sanitizers, tests/fuzz/standalone_main.c
 * runs the corpus as an ordinary test.
 */
#define main wayland_input_main
#include "../test_input_wayland.c"
#undef main

typedef struct
{
    const uint8_t *p, *end;
} Cur;

static uint8_t u8(Cur *c)
{
    return c->p < c->end ? *c->p++ : 0;
}

static int32_t i32(Cur *c)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++)
        v = v << 8 | u8(c);
    return (int32_t)v;
}

/* a value that is mostly plain and sometimes not */
static int32_t odd(Cur *c)
{
    uint8_t k = u8(c);
    switch (k % 8)
    {
    case 0: return 0;
    case 1: return -1;
    case 2: return INT32_MAX;
    case 3: return INT32_MIN;
    case 4: return 65535;
    case 5: return 65536;
    default: return (int32_t)(i32(c) % 3000);
    }
}

/* wl_fixed_t holds 24 bits of integer part (the relative motion is doubled before that): the test compositor builds its pointer values with wl_fixed_from_int, which is a
   multiplication by 256 that overflows an int for what odd() may give (the signed overflow is in the compositor, not in the
   library). Pointer coordinates and axis values are kept inside what a fixed-point number can say; sizes are not. */
static int32_t fixed(int32_t v)
{
    return v > 4000000 ? 4000000 : v < -4000000 ? -4000000 : v;
}

static PlatformWindow *g_win;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc, (void)argv;
    if (!start_compositor() || !platform_init())
        return 77;
    WindowConfig cfg = {.title = "fuzz", .width = 320, .height = 240, .resizable = true};
    g_win = window_create(&cfg);
    if (!g_win)
        return 77;
    for (int i = 0; i < 20; i++)
    {
        frame(g_win);
        usleep(2000);
    }
    settle(g_win);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 4096 || !g_win)
        return 0;
    reset();
    Cur c = {data, data + size};
    int steps = 0;
    while (c.p < c.end && steps++ < 200)
    {
        int op = u8(&c) % 16;
        switch (op)
        {
        case 0: cmd(CMD_LAYOUT, u8(&c) % 3, u8(&c) & 1, 0, 0); break;
        case 1: cmd(CMD_KB_ENTER, 0, 0, 0, 0); break;
        case 2: cmd(CMD_KB_LEAVE, 0, 0, 0, 0); break;
        case 3:
        case 4: cmd(CMD_KEY, (int)((uint32_t)i32(&c) % 70000), u8(&c) & 1, 0, 0); break;
        case 5: cmd(CMD_PTR_ENTER, fixed(odd(&c)), fixed(odd(&c)), 0, 0); break;
        case 6: cmd(CMD_PTR_LEAVE, 0, 0, 0, 0); break;
        case 7:
        case 8: cmd(CMD_PTR_MOVE, fixed(odd(&c)), fixed(odd(&c)), 0, 0); break;
        case 9: cmd(CMD_PTR_BUTTON, i32(&c), u8(&c) & 1, 0, 0); break;
        case 10: cmd(CMD_PTR_AXIS, u8(&c) % 3, fixed(odd(&c)), 0, 0); break;
        case 11: cmd(CMD_PTR_V120, u8(&c) % 3, fixed(odd(&c)), 0, 0); break;
        case 12: cmd(CMD_PTR_FRAME, 0, 0, 0, 0); break;
        case 13: cmd(CMD_CONFIGURE, odd(&c), odd(&c), (int)(u8(&c) & 7), 0); break;
#ifdef ZEN_WL_POINTER_LOCK
        case 14: cmd(u8(&c) & 1 ? CMD_LOCK : CMD_UNLOCK, 0, 0, 0, 0); break;
        case 15: cmd(CMD_REL_MOTION, fixed(odd(&c)), fixed(odd(&c)), 0, 0); break;
#else
        default: break;
#endif
        }
        if (steps % 6 == 0)
        {
            frame(g_win);
            usleep(500);
        }
    }
    for (int i = 0; i < 4; i++)
    {
        frame(g_win);
        usleep(1000);
    }
    return 0;
}
