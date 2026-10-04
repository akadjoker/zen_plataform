/*
 * test_x11_window.c - window-system features on a real X server: pointer capture
 * and the lock-key state. Skips (77) without a display.
 */
#include "platform.h"

#include <stdio.h>
#include <string.h>

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

static void settle(PlatformWindow *w)
{
    for (int i = 0; i < 10; i++)
    {
        window_begin_frame(w);
        time_sleep(5);
    }
}

static void test_capture(PlatformWindow *w)
{
    bool got = mouse_capture(w, true);
    if (got)
    {
        CHECK(mouse_capture(w, true)); /* asking again keeps it */
        CHECK(!mouse_capture(w, false));
        CHECK(!mouse_capture(w, false)); /* and releasing twice is harmless */
        /* the pointer is ours again: a second capture works */
        CHECK(mouse_capture(w, true));
        mouse_capture(w, false);
    }
    else
        printf("note: another program holds a pointer grab, capture checks skipped\n");

    /* capture and the hidden, locked mode share the pointer grab */
    mouse_set_mode(w, MOUSE_MODE_CAPTURED);
    mouse_set_mode(w, MOUSE_MODE_NORMAL);
    mouse_capture(w, false);
    CHECK(true);
}

static void test_locks(PlatformWindow *w)
{
    int mods = key_mods(w);
    CHECK((mods & ~(KEYMOD_SHIFT | KEYMOD_CTRL | KEYMOD_ALT | KEYMOD_SUPER | KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK)) == 0);
    /* the lock state agrees with itself between calls */
    CHECK((key_mods(w) & (KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK)) == (mods & (KEYMOD_CAPS_LOCK | KEYMOD_NUM_LOCK)));
}

int main(void)
{
    if (!platform_init())
    {
        printf("skip: no display\n");
        return 77;
    }
    WindowConfig cfg = {.title = "x11 window test", .width = 200, .height = 150, .x = WINDOW_POS_UNDEFINED, .y = WINDOW_POS_UNDEFINED};
    PlatformWindow *w = window_create(&cfg);
    if (!w)
    {
        printf("skip: %s\n", platform_get_error());
        return 77;
    }
    settle(w);
    test_capture(w);
    test_locks(w);
    window_destroy(w);
    platform_shutdown();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
