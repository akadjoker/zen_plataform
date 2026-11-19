/*
 * test_null_handles.c - the contract for a NULL window, and for calls made when the platform is not up.
 *
 * Every public function that takes a window answers a NULL one with a neutral value and no effect (platform.h, "Handles"):
 * false, 0, NULL, an empty string, a no-op, zeros through its out pointers; window_should_close(NULL) is true (a window
 * that does not exist is closed), touch_id(NULL, ...) and monitor_from_window(NULL) are -1, window_content_scale(NULL) 1.
 * The table below has one row per such function of the header, each called with NULL in a child process of its own, so a
 * crash names the function; a second check reads include/platform.h and fails when a function that takes a window has no
 * row, so a new one cannot go in without its NULL case. The dialogs take the window only as a parent and accept NULL by
 * design (they are not in the table). The second half of the file calls the API before platform_init, after
 * platform_shutdown, twice in a row, and destroys a window twice and after the shutdown.
 * Built on core.c and the fake backend: the guards are in core.c, which every backend shares.
 */
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

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

static void noop_frame(PlatformWindow *w, void *user)
{
    (void)w;
    (void)user;
}

/* what a case may need: out pointers that start at 7, to see them zeroed */
#define CASE_VARS                      \
    int ia[2] = {7, 7};                \
    float fa[2] = {7, 7};              \
    Event ev;                          \
    Framebuffer fb;                    \
    uint64_t u64 = 0;                  \
    memset(&ev, 0, sizeof ev);         \
    memset(&fb, 0, sizeof fb);         \
    (void)ia, (void)fa, (void)ev, (void)fb, (void)u64

static bool c_window_destroy(void)
{
    CASE_VARS;
    return (window_destroy(NULL), true);
}

static bool c_window_should_close(void)
{
    CASE_VARS;
    return (window_should_close(NULL)) == true;
}

static bool c_window_set_should_close(void)
{
    CASE_VARS;
    return (window_set_should_close(NULL, false), true);
}

static bool c_window_begin_frame(void)
{
    CASE_VARS;
    return (window_begin_frame(NULL), true);
}

static bool c_window_swap(void)
{
    CASE_VARS;
    return (window_swap(NULL), true);
}

static bool c_app_run_events(void)
{
    return (app_run_events(NULL, noop_frame, NULL), true);
}

static bool c_app_run(void)
{
    CASE_VARS;
    return (app_run(NULL, noop_frame, NULL), true);
}

static bool c_poll_event(void)
{
    CASE_VARS;
    return !poll_event(NULL, &ev);
}

static bool c_window_set_event_hook(void)
{
    CASE_VARS;
    return (window_set_event_hook(NULL, NULL, NULL), true);
}

static bool c_window_set_live_callback(void)
{
    CASE_VARS;
    return (window_set_live_callback(NULL, noop_frame, NULL), true);
}

static bool c_window_set_user_ptr(void)
{
    CASE_VARS;
    return (window_set_user_ptr(NULL, NULL), true);
}

static bool c_window_get_user_ptr(void)
{
    CASE_VARS;
    return window_get_user_ptr(NULL) == NULL;
}

static bool c_window_make_current(void)
{
    CASE_VARS;
    return (window_make_current(NULL), true);
}

static bool c_window_make_current_on(void)
{
    CASE_VARS;
    return (window_make_current_on(NULL, NULL), true);
}

static bool c_window_set_vsync(void)
{
    CASE_VARS;
    return (window_set_vsync(NULL, false), true);
}

static bool c_gl_worker_create(void)
{
    CASE_VARS;
    return gl_worker_create(NULL) == NULL;
}

static bool c_window_native_handle(void)
{
    CASE_VARS;
    return window_native_handle(NULL, NATIVE_WINDOW) == NULL;
}

static bool c_vulkan_create_surface(void)
{
    CASE_VARS;
    return !vulkan_create_surface(NULL, NULL, NULL, &u64);
}

static bool c_window_lock_pixels(void)
{
    CASE_VARS;
    return !window_lock_pixels(NULL, &fb);
}

static bool c_window_present_pixels(void)
{
    CASE_VARS;
    return (window_present_pixels(NULL), true);
}

static bool c_window_get_size(void)
{
    CASE_VARS;
    return (window_get_size(NULL, &ia[0], &ia[1]), true) && ia[0] == 0 && ia[1] == 0;
}

static bool c_window_set_size(void)
{
    CASE_VARS;
    return (window_set_size(NULL, 0, 0), true);
}

static bool c_window_get_framebuffer_size(void)
{
    CASE_VARS;
    return (window_get_framebuffer_size(NULL, &ia[0], &ia[1]), true) && ia[0] == 0 && ia[1] == 0;
}

static bool c_window_get_position(void)
{
    CASE_VARS;
    return (window_get_position(NULL, &ia[0], &ia[1]), true) && ia[0] == 0 && ia[1] == 0;
}

static bool c_window_set_position(void)
{
    CASE_VARS;
    return (window_set_position(NULL, 0, 0), true);
}

static bool c_window_set_title(void)
{
    CASE_VARS;
    return (window_set_title(NULL, "x"), true);
}

static bool c_window_set_size_limits(void)
{
    CASE_VARS;
    return (window_set_size_limits(NULL, 0, 0, 0, 0), true);
}

static bool c_window_content_scale(void)
{
    CASE_VARS;
    return (window_content_scale(NULL)) == 1.0f;
}

static bool c_window_center_on_monitor(void)
{
    CASE_VARS;
    return (window_center_on_monitor(NULL, 0), true);
}

static bool c_window_set_monitor(void)
{
    CASE_VARS;
    return (window_set_monitor(NULL, 0), true);
}

static bool c_window_minimize(void)
{
    CASE_VARS;
    return (window_minimize(NULL), true);
}

static bool c_window_maximize(void)
{
    CASE_VARS;
    return (window_maximize(NULL), true);
}

static bool c_window_restore(void)
{
    CASE_VARS;
    return (window_restore(NULL), true);
}

static bool c_window_show(void)
{
    CASE_VARS;
    return (window_show(NULL), true);
}

static bool c_window_hide(void)
{
    CASE_VARS;
    return (window_hide(NULL), true);
}

static bool c_window_focus(void)
{
    CASE_VARS;
    return (window_focus(NULL), true);
}

static bool c_window_request_attention(void)
{
    CASE_VARS;
    return (window_request_attention(NULL), true);
}

static bool c_window_is_focused(void)
{
    CASE_VARS;
    return !window_is_focused(NULL);
}

static bool c_window_is_minimized(void)
{
    CASE_VARS;
    return !window_is_minimized(NULL);
}

static bool c_window_is_maximized(void)
{
    CASE_VARS;
    return !window_is_maximized(NULL);
}

static bool c_window_is_visible(void)
{
    CASE_VARS;
    return !window_is_visible(NULL);
}

static bool c_window_is_hovered(void)
{
    CASE_VARS;
    return !window_is_hovered(NULL);
}

static bool c_window_set_mode(void)
{
    CASE_VARS;
    return (window_set_mode(NULL, WINDOW_WINDOWED, 0), true);
}

static bool c_window_get_mode(void)
{
    CASE_VARS;
    return (window_get_mode(NULL)) == WINDOW_WINDOWED;
}

static bool c_window_set_icon(void)
{
    CASE_VARS;
    return (window_set_icon(NULL, 0, 0, NULL), true);
}

static bool c_window_set_opacity(void)
{
    CASE_VARS;
    return (window_set_opacity(NULL, 0.0f), true);
}

static bool c_window_set_always_on_top(void)
{
    CASE_VARS;
    return (window_set_always_on_top(NULL, false), true);
}

static bool c_window_set_decorated(void)
{
    CASE_VARS;
    return (window_set_decorated(NULL, false), true);
}

static bool c_window_set_hit_test(void)
{
    CASE_VARS;
    return (window_set_hit_test(NULL, NULL, NULL), true);
}

static bool c_monitor_from_window(void)
{
    CASE_VARS;
    return (monitor_from_window(NULL)) == -1;
}

static bool c_key_down(void)
{
    CASE_VARS;
    return !key_down(NULL, 0);
}

static bool c_key_up(void)
{
    CASE_VARS;
    return !key_up(NULL, 0);
}

static bool c_key_pressed(void)
{
    CASE_VARS;
    return !key_pressed(NULL, 0);
}

static bool c_key_released(void)
{
    CASE_VARS;
    return !key_released(NULL, 0);
}

static bool c_key_get_pressed(void)
{
    CASE_VARS;
    return key_get_pressed(NULL) == 0;
}

static bool c_key_mods(void)
{
    CASE_VARS;
    return key_mods(NULL) == 0;
}

static bool c_char_get_pressed(void)
{
    CASE_VARS;
    return char_get_pressed(NULL) == 0;
}

static bool c_window_text_input_start(void)
{
    CASE_VARS;
    return (window_text_input_start(NULL), true);
}

static bool c_window_text_input_stop(void)
{
    CASE_VARS;
    return (window_text_input_stop(NULL), true);
}

static bool c_window_text_input_active(void)
{
    CASE_VARS;
    return !window_text_input_active(NULL);
}

static bool c_window_set_text_input_rect(void)
{
    CASE_VARS;
    return (window_set_text_input_rect(NULL, 0, 0, 0, 0), true);
}

static bool c_window_text_composition(void)
{
    CASE_VARS;
    return window_text_composition(NULL) != NULL && window_text_composition(NULL)[0] == 0;
}

static bool c_key_set_exit(void)
{
    CASE_VARS;
    return (key_set_exit(NULL, 0), true);
}

static bool c_mouse_button_down(void)
{
    CASE_VARS;
    return !mouse_button_down(NULL, 0);
}

static bool c_mouse_button_up(void)
{
    CASE_VARS;
    return !mouse_button_up(NULL, 0);
}

static bool c_mouse_button_pressed(void)
{
    CASE_VARS;
    return !mouse_button_pressed(NULL, 0);
}

static bool c_mouse_button_released(void)
{
    CASE_VARS;
    return !mouse_button_released(NULL, 0);
}

static bool c_mouse_x(void)
{
    CASE_VARS;
    return mouse_x(NULL) == 0;
}

static bool c_mouse_y(void)
{
    CASE_VARS;
    return mouse_y(NULL) == 0;
}

static bool c_mouse_position(void)
{
    CASE_VARS;
    return (mouse_position(NULL, &ia[0], &ia[1]), true) && ia[0] == 0 && ia[1] == 0;
}

static bool c_mouse_delta(void)
{
    CASE_VARS;
    return (mouse_delta(NULL, &ia[0], &ia[1]), true) && ia[0] == 0 && ia[1] == 0;
}

static bool c_mouse_wheel(void)
{
    CASE_VARS;
    return mouse_wheel(NULL) == 0;
}

static bool c_mouse_wheel_v(void)
{
    CASE_VARS;
    return (mouse_wheel_v(NULL, &fa[0], &fa[1]), true) && fa[0] == 0 && fa[1] == 0;
}

static bool c_mouse_set_position(void)
{
    CASE_VARS;
    return (mouse_set_position(NULL, 0, 0), true);
}

static bool c_mouse_set_cursor(void)
{
    CASE_VARS;
    return (mouse_set_cursor(NULL, 0), true);
}

static bool c_mouse_set_cursor_image(void)
{
    CASE_VARS;
    return (mouse_set_cursor_image(NULL, NULL), true);
}

static bool c_mouse_set_mode(void)
{
    CASE_VARS;
    return (mouse_set_mode(NULL, 0), true);
}

static bool c_mouse_capture(void)
{
    CASE_VARS;
    return !mouse_capture(NULL, false);
}

static bool c_touch_count(void)
{
    CASE_VARS;
    return touch_count(NULL) == 0;
}

static bool c_touch_x(void)
{
    CASE_VARS;
    return touch_x(NULL, 0) == 0;
}

static bool c_touch_y(void)
{
    CASE_VARS;
    return touch_y(NULL, 0) == 0;
}

static bool c_touch_position(void)
{
    CASE_VARS;
    return (touch_position(NULL, 0, &fa[0], &fa[1]), true) && fa[0] == 0 && fa[1] == 0;
}

static bool c_touch_id(void)
{
    CASE_VARS;
    return (touch_id(NULL, 0)) == -1;
}

static bool c_touch_set_mouse_emulation(void)
{
    CASE_VARS;
    return (touch_set_mouse_emulation(NULL, false), true);
}

static bool c_gesture_set_enabled(void)
{
    CASE_VARS;
    return (gesture_set_enabled(NULL, 0), true);
}

static bool c_gesture_detected(void)
{
    CASE_VARS;
    return gesture_detected(NULL) == 0;
}

static bool c_gesture_is_detected(void)
{
    CASE_VARS;
    return !gesture_is_detected(NULL, 0);
}

static bool c_gesture_hold_duration(void)
{
    CASE_VARS;
    return gesture_hold_duration(NULL) == 0;
}

static bool c_gesture_drag_vector(void)
{
    CASE_VARS;
    return (gesture_drag_vector(NULL, &fa[0], &fa[1]), true) && fa[0] == 0 && fa[1] == 0;
}

static bool c_gesture_drag_angle(void)
{
    CASE_VARS;
    return gesture_drag_angle(NULL) == 0;
}

static bool c_gesture_pinch_vector(void)
{
    CASE_VARS;
    return (gesture_pinch_vector(NULL, &fa[0], &fa[1]), true) && fa[0] == 0 && fa[1] == 0;
}

static bool c_gesture_pinch_angle(void)
{
    CASE_VARS;
    return gesture_pinch_angle(NULL) == 0;
}

typedef struct
{
    const char *name;
    bool (*fn)(void);
} Case;

static const Case cases[] = {
    {"window_destroy", c_window_destroy},
    {"window_should_close", c_window_should_close},
    {"window_set_should_close", c_window_set_should_close},
    {"window_begin_frame", c_window_begin_frame},
    {"window_swap", c_window_swap},
    {"app_run", c_app_run},
    {"app_run_events", c_app_run_events},
    {"poll_event", c_poll_event},
    {"window_set_event_hook", c_window_set_event_hook},
    {"window_set_live_callback", c_window_set_live_callback},
    {"window_set_user_ptr", c_window_set_user_ptr},
    {"window_get_user_ptr", c_window_get_user_ptr},
    {"window_make_current", c_window_make_current},
    {"window_make_current_on", c_window_make_current_on},
    {"window_set_vsync", c_window_set_vsync},
    {"gl_worker_create", c_gl_worker_create},
    {"window_native_handle", c_window_native_handle},
    {"vulkan_create_surface", c_vulkan_create_surface},
    {"window_lock_pixels", c_window_lock_pixels},
    {"window_present_pixels", c_window_present_pixels},
    {"window_get_size", c_window_get_size},
    {"window_set_size", c_window_set_size},
    {"window_get_framebuffer_size", c_window_get_framebuffer_size},
    {"window_get_position", c_window_get_position},
    {"window_set_position", c_window_set_position},
    {"window_set_title", c_window_set_title},
    {"window_set_size_limits", c_window_set_size_limits},
    {"window_content_scale", c_window_content_scale},
    {"window_center_on_monitor", c_window_center_on_monitor},
    {"window_set_monitor", c_window_set_monitor},
    {"window_minimize", c_window_minimize},
    {"window_maximize", c_window_maximize},
    {"window_restore", c_window_restore},
    {"window_show", c_window_show},
    {"window_hide", c_window_hide},
    {"window_focus", c_window_focus},
    {"window_request_attention", c_window_request_attention},
    {"window_is_focused", c_window_is_focused},
    {"window_is_minimized", c_window_is_minimized},
    {"window_is_maximized", c_window_is_maximized},
    {"window_is_visible", c_window_is_visible},
    {"window_is_hovered", c_window_is_hovered},
    {"window_set_mode", c_window_set_mode},
    {"window_get_mode", c_window_get_mode},
    {"window_set_icon", c_window_set_icon},
    {"window_set_opacity", c_window_set_opacity},
    {"window_set_always_on_top", c_window_set_always_on_top},
    {"window_set_decorated", c_window_set_decorated},
    {"window_set_hit_test", c_window_set_hit_test},
    {"monitor_from_window", c_monitor_from_window},
    {"key_down", c_key_down},
    {"key_up", c_key_up},
    {"key_pressed", c_key_pressed},
    {"key_released", c_key_released},
    {"key_get_pressed", c_key_get_pressed},
    {"key_mods", c_key_mods},
    {"char_get_pressed", c_char_get_pressed},
    {"window_text_input_start", c_window_text_input_start},
    {"window_text_input_stop", c_window_text_input_stop},
    {"window_text_input_active", c_window_text_input_active},
    {"window_set_text_input_rect", c_window_set_text_input_rect},
    {"window_text_composition", c_window_text_composition},
    {"key_set_exit", c_key_set_exit},
    {"mouse_button_down", c_mouse_button_down},
    {"mouse_button_up", c_mouse_button_up},
    {"mouse_button_pressed", c_mouse_button_pressed},
    {"mouse_button_released", c_mouse_button_released},
    {"mouse_x", c_mouse_x},
    {"mouse_y", c_mouse_y},
    {"mouse_position", c_mouse_position},
    {"mouse_delta", c_mouse_delta},
    {"mouse_wheel", c_mouse_wheel},
    {"mouse_wheel_v", c_mouse_wheel_v},
    {"mouse_set_position", c_mouse_set_position},
    {"mouse_set_cursor", c_mouse_set_cursor},
    {"mouse_set_cursor_image", c_mouse_set_cursor_image},
    {"mouse_set_mode", c_mouse_set_mode},
    {"mouse_capture", c_mouse_capture},
    {"touch_count", c_touch_count},
    {"touch_x", c_touch_x},
    {"touch_y", c_touch_y},
    {"touch_position", c_touch_position},
    {"touch_id", c_touch_id},
    {"touch_set_mouse_emulation", c_touch_set_mouse_emulation},
    {"gesture_set_enabled", c_gesture_set_enabled},
    {"gesture_detected", c_gesture_detected},
    {"gesture_is_detected", c_gesture_is_detected},
    {"gesture_hold_duration", c_gesture_hold_duration},
    {"gesture_drag_vector", c_gesture_drag_vector},
    {"gesture_drag_angle", c_gesture_drag_angle},
    {"gesture_pinch_vector", c_gesture_pinch_vector},
    {"gesture_pinch_angle", c_gesture_pinch_angle},
};

/* A function of the header taking a window as its first parameter, that is not in the table: reads platform.h */
static void test_every_function_has_a_row(void)
{
    FILE *f = fopen(ZP_HEADER, "r");
    CHECK(f != NULL);
    if (!f)
        return;
    /* by design they take the window as a parent that may be NULL, or are not about an existing window */
    static const char *const by_design[] = {"window_create", "message_box", "confirm_box", "dialog_open_file",
                                            "dialog_save_file", "dialog_pick_folder", "dialog_open_files"};
    char line[1024], pending[2048] = "";
    int declared = 0, missing = 0;
    while (fgets(line, sizeof line, f))
    {
        char *c = strstr(line, "/*");
        if (c && !strstr(line, "PLATFORM_API"))
            continue;
        if (strstr(line, "PLATFORM_API") == NULL && pending[0] == 0)
            continue;
        strncat(pending, line, sizeof pending - strlen(pending) - 1);
        if (!strchr(line, ';'))
            continue; /* the prototype goes on */
        char *api = strstr(pending, "PLATFORM_API");
        char *paren = api ? strchr(api, '(') : NULL;
        if (api && paren && strncmp(paren + 1, "PlatformWindow *", 16) == 0)
        {
            char *e = paren;
            while (e > api && (e[-1] == '_' || (e[-1] >= 'a' && e[-1] <= 'z') || (e[-1] >= '0' && e[-1] <= '9')))
                e--;
            char name[96];
            size_t n = (size_t)(paren - e);
            if (n < sizeof name)
            {
                memcpy(name, e, n);
                name[n] = 0;
                bool known = false;
                for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
                    known |= strcmp(cases[i].name, name) == 0;
                for (unsigned i = 0; i < sizeof by_design / sizeof by_design[0]; i++)
                    known |= strcmp(by_design[i], name) == 0;
                declared++;
                if (!known)
                {
                    missing++;
                    printf("no NULL case for %s\n", name);
                }
            }
        }
        pending[0] = 0;
    }
    fclose(f);
    printf("  %d functions of the header take a window, %d without a row\n", declared, missing);
    CHECK(declared >= 90);
    CHECK(missing == 0);
}

static void test_null_window_table(void)
{
    int crashed = 0, wrong = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++)
    {
        fflush(stdout);
        pid_t pid = fork();
        if (pid == 0)
        {
            alarm(10);
            _exit(cases[i].fn() ? 0 : 1);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        if (WIFSIGNALED(st) || (WIFEXITED(st) && WEXITSTATUS(st) > 1))
        {
            crashed++;
            printf("FAIL %s(NULL, ...) crashed (status %d)\n", cases[i].name, st);
        }
        else if (WEXITSTATUS(st) != 0)
        {
            wrong++;
            printf("FAIL %s(NULL, ...) did not return the neutral value\n", cases[i].name);
        }
    }
    printf("  %u functions called with a NULL window, %d crashed, %d wrong\n", (unsigned)(sizeof cases / sizeof cases[0]),
           crashed, wrong);
    CHECK(crashed == 0);
    CHECK(wrong == 0);
}

static WindowConfig small_config(void)
{
    WindowConfig cfg = {.title = "null", .width = 64, .height = 48, .render = RENDER_PIXELS, .resizable = true};
    cfg.x = WINDOW_POS_UNDEFINED;
    cfg.y = WINDOW_POS_UNDEFINED;
    return cfg;
}

/* What needs no window, with the platform not up. */
static void calls_without_platform(bool expect_clean)
{
    (void)expect_clean;
    MonitorInfo mi;
    int x = 7, y = 7;
    CHECK(monitor_count() == 0);
    CHECK(!monitor_get_info(0, &mi));
    CHECK(monitor_from_point(10, 10) == -1);
    CHECK(monitor_from_mouse() == -1);
    CHECK(!mouse_global_position(&x, &y) && x == 0 && y == 0);
    CHECK(gl_proc_address("glClear") == NULL);
    uint32_t px[4] = {0, 0, 0, 0};
    CHECK(cursor_create(px, 2, 2, 0, 0) == NULL);
    cursor_destroy(NULL);
    const char *t = clipboard_get();
    CHECK(t != NULL && t[0] == 0);
    clipboard_set("x");
    clipboard_set(NULL);
    CHECK(!clipboard_set_data("text/plain", "x", 1));
    CHECK(!clipboard_has_data("text/plain"));
    size_t n = 5;
    CHECK(clipboard_get_data("text/plain", &n) == NULL && n == 0);
    Framebuffer fb;
    CHECK(!clipboard_get_image(&fb));
    WindowConfig cfg = small_config();
    CHECK(window_create(&cfg) == NULL);
    CHECK(window_create(NULL) == NULL);
    CHECK(time_nanos() >= 0 && time_seconds() >= 0); /* the clock needs nothing */
    platform_shutdown();                              /* with the platform not up: nothing to do */
}

static void test_lifecycle(void)
{
    calls_without_platform(true); /* before platform_init */

    CHECK(platform_init());
    CHECK(platform_init()); /* up already: nothing happens, still true */
    WindowConfig cfg = small_config();
    PlatformWindow *w = window_create(&cfg);
    CHECK(w != NULL);
    if (!w)
        return;
    window_destroy(w);
    window_destroy(w); /* the second one is not a window of the process any more: nothing happens */
    PlatformWindow *w2 = window_create(&cfg);
    CHECK(w2 != NULL);
    platform_shutdown(); /* w2 is still there: goes with the platform */
    platform_shutdown(); /* twice: nothing */
    window_destroy(w2);  /* after the shutdown: a stale pointer is ignored, not used */
    calls_without_platform(false);

    /* the platform comes up again, with working windows */
    CHECK(platform_init());
    PlatformWindow *w3 = window_create(&cfg);
    CHECK(w3 != NULL);
    if (w3)
    {
        window_begin_frame(w3);
        CHECK(!window_should_close(w3));
        window_destroy(w3);
    }
    platform_shutdown();
}

int main(void)
{
    test_every_function_has_a_row();
    test_null_window_table();
    test_lifecycle();
    printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
