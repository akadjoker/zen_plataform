/*
 * platform.h - window, input and OpenGL context, cross-platform.
 *
 * One public contract for every backend. The backend pushes normalized events
 * into a queue; the portable core builds per-frame input state from that queue.
 * Geometry uses two spaces: window position and size are in screen coordinates,
 * the framebuffer is in pixels, and content_scale bridges them.
 */
#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "platform_export.h"

#ifdef __cplusplus
extern "C"
{
#endif

    /* ========================================================================== */
    /*  Types                                                                     */
    /* ========================================================================== */

    typedef struct PlatformWindow PlatformWindow; /* opaque; defined by the active backend */

    typedef enum
    {
        WINDOW_WINDOWED,
        WINDOW_FULLSCREEN,
        WINDOW_FULLSCREEN_BORDERLESS
    } WindowMode;

    /* How the window is drawn to. RENDER_GL gives a GL context (window_swap);
       RENDER_PIXELS gives a CPU framebuffer blitted by the backend, no GL at all
       (window_lock_pixels / window_present_pixels); RENDER_VULKAN gives a bare
       window with no GL context, for the application to draw on through a
       VkSurfaceKHR (see Vulkan below; window_swap does nothing). Chosen once, at
       creation. */
    typedef enum
    {
        RENDER_GL,
        RENDER_PIXELS,
        RENDER_VULKAN
    } RenderMode;

#define WINDOW_POS_CENTERED (-1)  /* place centered on the chosen monitor */
#define WINDOW_POS_UNDEFINED (-2) /* let the system choose the position   */
#define MONITOR_CURRENT (-1)      /* the monitor the window is mostly on  */

    typedef enum
    {
        GL_PROFILE_DEFAULT, /* core on desktop, ES on web and android */
        GL_PROFILE_CORE,
        GL_PROFILE_COMPAT,
        GL_PROFILE_ES
    } GLProfile;

    /* Zero means default: 3.3 core on desktop, ES 3.0 on web and android, no MSAA,
       no debug. The framebuffer is always RGBA8 with 24-bit depth, 8-bit stencil
       and double buffering. A request the platform cannot satisfy makes
       window_create fail (see platform_get_error); it is never downgraded.
       debug is ignored on the web. */
    typedef struct
    {
        GLProfile profile;
        int major, minor;
        int msaa; /* sample count, 0 to disable */
        bool debug;
    } GLConfig;

    typedef struct
    {
        const char *title;
        int width, height; /* screen coordinates, not pixels */
        int x, y;          /* WINDOW_POS_CENTERED / _UNDEFINED, or virtual coord */
        int monitor;       /* index, or MONITOR_CURRENT */
        WindowMode mode;
        RenderMode render; /* RENDER_GL (default), RENDER_PIXELS or RENDER_VULKAN */
        GLConfig gl;
        bool resizable;
        bool vsync;
    } WindowConfig;

    typedef struct
    {
        int index;
        const char *name;
        int x, y;                           /* origin in the shared virtual coordinate space */
        int width, height;                  /* resolution in screen coordinates */
        int work_x, work_y, work_w, work_h; /* usable area, excluding taskbars */
        int phys_width_mm, phys_height_mm;
        int refresh_hz;
        float content_scale; /* pixels per screen coordinate for this monitor */
        bool primary;
    } MonitorInfo;

    /* ========================================================================== */
    /*  Events (raw stream, drained once per frame by window_begin_frame)         */
    /* ========================================================================== */

    typedef enum
    {
        EVENT_NONE = 0,

        EVENT_WINDOW_CLOSE,
        EVENT_WINDOW_RESIZE,    /* size in screen coordinates */
        EVENT_WINDOW_FB_RESIZE, /* framebuffer size in pixels */
        EVENT_WINDOW_MOVE,
        EVENT_WINDOW_FOCUS,
        EVENT_WINDOW_MINIMIZE,
        EVENT_WINDOW_MAXIMIZE,
        EVENT_WINDOW_RESTORE,
        EVENT_WINDOW_ENTER,         /* pointer entered or left the window */
        EVENT_WINDOW_SCALE_CHANGED, /* moved to a monitor with a different scale */
        EVENT_WINDOW_DROP,          /* paths are valid only during this event */

        EVENT_KEY,  /* a physical key changed state */
        EVENT_CHAR, /* text input, one Unicode codepoint */
        EVENT_MOUSE_MOVE,
        EVENT_MOUSE_BUTTON,
        EVENT_MOUSE_WHEEL,
        EVENT_TOUCH
    } EventType;

    typedef enum
    {
        TOUCH_DOWN,
        TOUCH_MOVE,
        TOUCH_UP,
        TOUCH_CANCEL
    } TouchPhase;

    typedef struct
    {
        EventType type;
        bool consumed; /* a higher layer may set this; later handlers skip it */
        union
        {
            struct
            {
                int key;
                int scancode;
                bool down;
                bool repeat;
                int mods;
            } key;
            uint32_t codepoint;
            struct
            {
                int x, y, dx, dy;
                int button;
                bool down;
            } mouse;
            struct
            {
                float x, y;
            } wheel;
            struct
            {
                int id;
                float x, y, pressure;
                TouchPhase phase;
            } touch;
            struct
            {
                int w, h;
            } resize;
            struct
            {
                int x, y;
            } move;
            struct
            {
                bool gained;
            } focus;
            struct
            {
                bool entered;
            } enter;
            struct
            {
                float scale;
            } scale;
            struct
            {
                int count;
                const char **paths;
            } drop;
        } data;
    } Event;

    /* ========================================================================== */
    /*  Lifecycle                                                                 */
    /* ========================================================================== */

    /* Initialize global platform state. Required before monitor queries or window
       creation. Returns false if the platform could not be brought up. */
    PLATFORM_API bool platform_init(void);
    PLATFORM_API void platform_shutdown(void);

    PLATFORM_API const char *platform_get_error(void);
    PLATFORM_API void platform_clear_error(void);

    /* ========================================================================== */
    /*  Logging                                                                   */
    /* ========================================================================== */

    /* Messages below the current level are dropped (default LOGLEVEL_INFO). The
       default sink prints "[LEVEL] text" to stderr, to logcat on Android, and to
       the debugger output on Windows. log_set_callback replaces it, NULL restores
       it. The platform itself logs the errors it reports through
       platform_get_error at LOGLEVEL_DEBUG. Set the level and callback before
       starting threads; the text is cut at 1023 bytes. */
    typedef enum
    {
        LOGLEVEL_DEBUG,
        LOGLEVEL_INFO,
        LOGLEVEL_WARN,
        LOGLEVEL_ERROR,
        LOGLEVEL_OFF
    } LogLevel;

    typedef void (*LogCallback)(LogLevel level, const char *message, void *user);

#if defined(__GNUC__) || defined(__clang__)
#define PLATFORM_PRINTF(f, a) __attribute__((format(printf, f, a)))
#else
#define PLATFORM_PRINTF(f, a)
#endif

    PLATFORM_API void log_set_level(LogLevel level);
    PLATFORM_API LogLevel log_get_level(void);
    PLATFORM_API void log_set_callback(LogCallback cb, void *user);
    PLATFORM_API void log_message(LogLevel level, const char *fmt, ...) PLATFORM_PRINTF(2, 3);

#define log_debug(...) log_message(LOGLEVEL_DEBUG, __VA_ARGS__)
#define log_info(...) log_message(LOGLEVEL_INFO, __VA_ARGS__)
#define log_warn(...) log_message(LOGLEVEL_WARN, __VA_ARGS__)
#define log_error(...) log_message(LOGLEVEL_ERROR, __VA_ARGS__)

    PLATFORM_API PlatformWindow *window_create(const WindowConfig *cfg);
    PLATFORM_API void window_destroy(PlatformWindow *w);

    PLATFORM_API bool window_should_close(PlatformWindow *w);
    PLATFORM_API void window_set_should_close(PlatformWindow *w, bool value);

    /* Drain the OS into input state and the per-frame event queue. Call once at the
       top of each frame when driving the loop manually. */
    PLATFORM_API void window_begin_frame(PlatformWindow *w);
    PLATFORM_API void window_swap(PlatformWindow *w);

    /* Frame loop. The callback runs once per frame. On desktop this owns the while
       loop; on platforms where the system owns the loop it registers the callback.
       Use this instead of a manual loop when targeting every platform. */
    typedef void (*FrameCallback)(PlatformWindow *w, void *user);
    PLATFORM_API void app_run(PlatformWindow *w, FrameCallback frame, void *user);

    /* Iterate the events collected this frame. Returns false when the queue is
       empty. Does not clear input state, so state queries stay valid in the same
       frame. */
    PLATFORM_API bool poll_event(PlatformWindow *w, Event *out);

    PLATFORM_API void window_set_user_ptr(PlatformWindow *w, void *ptr);
    PLATFORM_API void *window_get_user_ptr(PlatformWindow *w);

    /* ========================================================================== */
    /*  OpenGL context                                                            */
    /* ========================================================================== */

    PLATFORM_API void window_make_current(PlatformWindow *w);
    PLATFORM_API void window_set_vsync(PlatformWindow *w, bool on);
    /* Address of a GL function for a loader, or NULL if unavailable. */
    PLATFORM_API void *gl_proc_address(const char *name);

    /* ========================================================================== */
    /*  Shared libraries                                                          */
    /* ========================================================================== */

    /* Load a library at run time and look up its symbols (dlopen / LoadLibrary).
       Pass a name the system resolves ("libvulkan.so.1", "vulkan-1.dll") or a
       path (UTF-8). library_open and library_symbol return NULL and set
       platform_get_error() on failure. A function pointer comes back as void*:
       cast it to the right type. Not supported on the web. */
    typedef struct SharedLibrary SharedLibrary;

    PLATFORM_API SharedLibrary *library_open(const char *path);
    PLATFORM_API void *library_symbol(SharedLibrary *lib, const char *name);
    PLATFORM_API void library_close(SharedLibrary *lib);

    /* ========================================================================== */
    /*  Native handles                                                            */
    /* ========================================================================== */

    /* The OS objects behind a window, for libraries that attach to them (a Dear
       ImGui platform backend, a video player, a Vulkan or Metal layer). Every value
       comes back as a pointer; an integer handle (an X11 Window) is cast to
       uintptr_t and then to void*. NULL when the platform has no such object.
       They belong to the window: do not destroy them, and they are gone after
       window_destroy.

                          NATIVE_DISPLAY       NATIVE_WINDOW      NATIVE_GL_CONTEXT
         X11              Display*             Window             GLXContext
         Win32            HINSTANCE            HWND               HGLRC
         Android          EGLDisplay           ANativeWindow*     EGLContext
         Web, fake        NULL                 NULL               NULL

       On Android the window comes and goes with the activity: ask again after
       each EVENT_WINDOW_FB_RESIZE that follows a resume. NATIVE_GL_CONTEXT is NULL
       unless the window is RENDER_GL. */
    typedef enum
    {
        NATIVE_DISPLAY,
        NATIVE_WINDOW,
        NATIVE_GL_CONTEXT
    } NativeHandleType;

    PLATFORM_API void *window_native_handle(PlatformWindow *w, NativeHandleType type);

    /* ========================================================================== */
    /*  Vulkan                                                                    */
    /* ========================================================================== */

    /* zen_platform does not link Vulkan and does not need its headers. It loads
       the loader at run time (vulkan-1.dll, libvulkan.so.1) and hands you what you
       need to start: the loader entry point, the instance extensions the window
       system requires, and a surface for a RENDER_VULKAN window. Handles are
       opaque here: a VkInstance is a void*, a VkSurfaceKHR a uint64_t.

         if (!vulkan_supported()) { ... fall back to GL ... }
         uint32_t n; const char *const *ext = vulkan_instance_extensions(&n);
         // pass ext/n in VkInstanceCreateInfo, then:
         PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)vulkan_get_proc_addr();
         ... vkCreateInstance ...
         uint64_t surface;
         if (!vulkan_create_surface(w, instance, NULL, &surface)) { platform_get_error(); }

       Size the swapchain from window_get_framebuffer_size. Supported on X11,
       Windows and Android; not on the web or in the fake backend. The surface must
       be destroyed (vkDestroySurfaceKHR) before the instance and the window. On
       Android a surface is invalid once the app is backgrounded: create it again
       when the window returns. */

    /* True when the Vulkan loader is present and exposes VK_KHR_surface and this
       platform's surface extension. Does not need a window. */
    PLATFORM_API bool vulkan_supported(void);

    /* The loader's vkGetInstanceProcAddr (cast it to PFN_vkGetInstanceProcAddr), or
       NULL when the loader cannot be loaded. */
    PLATFORM_API void *vulkan_get_proc_addr(void);

    /* Instance extensions the window system needs, count in *count. The array is
       static. NULL and 0 on a platform without Vulkan. */
    PLATFORM_API const char *const *vulkan_instance_extensions(uint32_t *count);

    /* Creates the VkSurfaceKHR of a RENDER_VULKAN window on `instance` (a
       VkInstance created with the extensions above). allocator is a
       const VkAllocationCallbacks* or NULL. Returns false and sets
       platform_get_error() on failure. */
    PLATFORM_API bool vulkan_create_surface(PlatformWindow *w, void *instance, const void *allocator,
                                            uint64_t *out_surface);

    /* ========================================================================== */
    /*  Pixel surface (software framebuffer,  RENDER_PIXELS windows only)    */
    /* ========================================================================== */

    typedef struct
    {
        uint32_t *pixels; /* writable; one pixel is 0xAARRGGBB (B,G,R,A in memory) */
        int width, height;
        int stride; /* pixels per row, >= width */
    } Framebuffer;

    /* Acquire the back framebuffer for this frame, sized to the window. Returns
       false on a RENDER_GL window or if no surface is available (e.g. minimized).
       Valid until window_present_pixels. */
    PLATFORM_API bool window_lock_pixels(PlatformWindow *w, Framebuffer *out);
    /* Blit the framebuffer to the window and present it. */
    PLATFORM_API void window_present_pixels(PlatformWindow *w);

    /* Software 2D primitives over any Framebuffer (the one from window_lock_pixels
       or one you allocate). One fixed format, the framebuffer's: a pixel is
       0xAARRGGBB. Every primitive clips to the framebuffer. */

    typedef enum
    {
        BLEND_NONE, /* overwrite */
        BLEND_ALPHA /* source-over onto an opaque destination */
    } BlendMode;

    typedef enum
    {
        SCALE_NEAREST,
        SCALE_BILINEAR
    } ScaleMode;

    /* Optional scissor rect. Pixels outside the clip are silently dropped.
       draw_set_clip enables it; draw_reset_clip disables (default: off). */
    PLATFORM_API void draw_set_clip(int x, int y, int w, int h);
    PLATFORM_API void draw_reset_clip(void);

    PLATFORM_API void draw_clear(Framebuffer *fb, uint32_t color);
    PLATFORM_API void draw_pixel(Framebuffer *fb, int x, int y, uint32_t color, BlendMode blend);
    PLATFORM_API uint32_t draw_get_pixel(const Framebuffer *fb, int x, int y); /* 0 if out of bounds */

    PLATFORM_API void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color, BlendMode blend);
    /* Rounded rectangle, radius clamped to half the shorter side. radius <= 0
       falls back to the square versions. */
    PLATFORM_API void draw_round_rect(Framebuffer *fb, int x, int y, int w, int h, int radius, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_fill_round_rect(Framebuffer *fb, int x, int y, int w, int h, int radius, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_fill_circle(Framebuffer *fb, int cx, int cy, int radius, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_fill_triangle(Framebuffer *fb, int x0, int y0, int x1, int y1, int x2, int y2,
                                         uint32_t color, BlendMode blend);
    /* Copy a src region into a dst region, scaling between them. */
    PLATFORM_API void draw_blit(Framebuffer *dst, const Framebuffer *src,
                                int sx, int sy, int sw, int sh,
                                int dx, int dy, int dw, int dh,
                                BlendMode blend, ScaleMode scale);

    /* Owned framebuffers: allocate one (zeroed), wrap a copy of external pixels
       (e.g. a decoded stb_image buffer, already 0xAARRGGBB), or load/save a BMP.
       Release any of these with framebuffer_free. Do NOT free the framebuffer that
       window_lock_pixels hands back - the backend owns that one. */
    PLATFORM_API bool framebuffer_alloc(Framebuffer *fb, int width, int height);
    PLATFORM_API bool framebuffer_from_pixels(Framebuffer *fb, const uint32_t *pixels, int width, int height);
    PLATFORM_API void framebuffer_free(Framebuffer *fb);

    PLATFORM_API bool framebuffer_load_bmp(Framebuffer *fb, const char *path); /* 24/32-bit BI_RGB */
    PLATFORM_API bool framebuffer_save_bmp(const Framebuffer *fb, const char *path);

    /* ========================================================================== */
    /*  Geometry                                                                  */
    /* ========================================================================== */

    PLATFORM_API void window_get_size(PlatformWindow *w, int *width, int *height); /* screen coords */
    PLATFORM_API void window_set_size(PlatformWindow *w, int width, int height);
    PLATFORM_API void window_get_framebuffer_size(PlatformWindow *w, int *width, int *height); /* pixels */
    PLATFORM_API void window_get_position(PlatformWindow *w, int *x, int *y);                  /* virtual space */
    PLATFORM_API void window_set_position(PlatformWindow *w, int x, int y);
    PLATFORM_API void window_set_title(PlatformWindow *w, const char *title);
    PLATFORM_API void window_set_size_limits(PlatformWindow *w, int minw, int minh, int maxw, int maxh);
    PLATFORM_API float window_content_scale(PlatformWindow *w); /* framebuffer / screen ratio */

    PLATFORM_API void window_center_on_monitor(PlatformWindow *w, int monitor);
    PLATFORM_API void window_set_monitor(PlatformWindow *w, int monitor); /* move, keep size */

    /* ========================================================================== */
    /*  PlatformWindow state                                                              */
    /* ========================================================================== */

    PLATFORM_API void window_minimize(PlatformWindow *w);
    PLATFORM_API void window_maximize(PlatformWindow *w);
    PLATFORM_API void window_restore(PlatformWindow *w);
    PLATFORM_API void window_show(PlatformWindow *w);
    PLATFORM_API void window_hide(PlatformWindow *w);
    PLATFORM_API void window_focus(PlatformWindow *w);
    PLATFORM_API void window_request_attention(PlatformWindow *w);

    PLATFORM_API bool window_is_focused(PlatformWindow *w);
    PLATFORM_API bool window_is_minimized(PlatformWindow *w);
    PLATFORM_API bool window_is_maximized(PlatformWindow *w);
    PLATFORM_API bool window_is_visible(PlatformWindow *w);
    PLATFORM_API bool window_is_hovered(PlatformWindow *w);

    PLATFORM_API void window_set_mode(PlatformWindow *w, WindowMode mode, int monitor);
    PLATFORM_API WindowMode window_get_mode(PlatformWindow *w);

    PLATFORM_API void window_set_icon(PlatformWindow *w, int width, int height, const uint8_t *rgba);
    PLATFORM_API void window_set_opacity(PlatformWindow *w, float alpha); /* 0..1 */
    PLATFORM_API void window_set_always_on_top(PlatformWindow *w, bool on);

    /* ========================================================================== */
    /*  Monitors (all share one virtual coordinate space)                         */
    /* ========================================================================== */

    PLATFORM_API int monitor_count(void);
    PLATFORM_API bool monitor_get_info(int index, MonitorInfo *out);
    PLATFORM_API int monitor_from_window(PlatformWindow *w);
    PLATFORM_API int monitor_from_point(int x, int y); /* -1 if none */

    /* ========================================================================== */
    /*  Keyboard                                                                  */
    /* ========================================================================== */

    PLATFORM_API bool key_down(PlatformWindow *w, int key);
    PLATFORM_API bool key_up(PlatformWindow *w, int key);
    PLATFORM_API bool key_pressed(PlatformWindow *w, int key);  /* went down this frame */
    PLATFORM_API bool key_released(PlatformWindow *w, int key); /* went up this frame */

    /* Next key from this frame's press queue, in order. 0 when empty. */
    PLATFORM_API int key_get_pressed(PlatformWindow *w);
    /* KEYMOD_* mask of the modifier keys held now. */
    PLATFORM_API int key_mods(PlatformWindow *w);
    /* Next codepoint from the text-input queue. 0 when empty. Independent of the
       key queue, so it carries layout and composed input correctly. */
    PLATFORM_API uint32_t char_get_pressed(PlatformWindow *w);

    PLATFORM_API void key_set_exit(PlatformWindow *w, int key); /* sets should_close on press */

    /* ========================================================================== */
    /*  Mouse                                                                     */
    /* ========================================================================== */

    PLATFORM_API bool mouse_button_down(PlatformWindow *w, int button);
    PLATFORM_API bool mouse_button_up(PlatformWindow *w, int button);
    PLATFORM_API bool mouse_button_pressed(PlatformWindow *w, int button);
    PLATFORM_API bool mouse_button_released(PlatformWindow *w, int button);

    PLATFORM_API int mouse_x(PlatformWindow *w);
    PLATFORM_API int mouse_y(PlatformWindow *w);
    PLATFORM_API void mouse_position(PlatformWindow *w, int *x, int *y);
    PLATFORM_API void mouse_delta(PlatformWindow *w, int *dx, int *dy); /* vs previous frame */
    PLATFORM_API float mouse_wheel(PlatformWindow *w);                  /* vertical, this frame */
    PLATFORM_API void mouse_wheel_v(PlatformWindow *w, float *x, float *y);

    PLATFORM_API void mouse_set_position(PlatformWindow *w, int x, int y);
    PLATFORM_API void mouse_set_cursor(PlatformWindow *w, int cursor); /* CURSOR_* shape */
    PLATFORM_API void mouse_set_mode(PlatformWindow *w, int mode);     /* MOUSE_MODE_* */

    /* ========================================================================== */
    /*  Touch (multitouch)                                                        */
    /* ========================================================================== */

    PLATFORM_API int touch_count(PlatformWindow *w);
    PLATFORM_API int touch_x(PlatformWindow *w, int index);
    PLATFORM_API int touch_y(PlatformWindow *w, int index);
    PLATFORM_API void touch_position(PlatformWindow *w, int index, float *x, float *y);
    PLATFORM_API int touch_id(PlatformWindow *w, int index); /* stable across down..up */

    /* Desktop backends report no touch. With emulation on, the left mouse button is
       one finger: it raises EVENT_TOUCH (id TOUCH_ID_MOUSE) and shows up in
       touch_count/touch_position and in the gestures, like a real touch. Off by
       default, so an app that handles both mouse and touch does not see the click
       twice. Real touches that are active take priority over the mouse. */
#define TOUCH_ID_MOUSE (-2)
    PLATFORM_API void touch_set_mouse_emulation(PlatformWindow *w, bool on);

    /* ========================================================================== */
    /*  Gestures (built on the touch points)                                      */
    /* ========================================================================== */

    /* Detection follows raylib's rgestures: window_begin_frame updates it, and
       gesture_detected returns the gesture of the moment, or GESTURE_NONE. TAP and
       DOUBLETAP turn into HOLD on the next frame; a SWIPE lasts a single frame. */
    typedef enum
    {
        GESTURE_NONE = 0,
        GESTURE_TAP = 1,
        GESTURE_DOUBLETAP = 2,
        GESTURE_HOLD = 4,
        GESTURE_DRAG = 8,
        GESTURE_SWIPE_RIGHT = 16,
        GESTURE_SWIPE_LEFT = 32,
        GESTURE_SWIPE_UP = 64,
        GESTURE_SWIPE_DOWN = 128,
        GESTURE_PINCH_IN = 256,
        GESTURE_PINCH_OUT = 512,
        GESTURE_ALL = 1023
    } Gesture;

    PLATFORM_API void gesture_set_enabled(PlatformWindow *w, unsigned flags); /* GESTURE_* mask, default all */
    PLATFORM_API unsigned gesture_detected(PlatformWindow *w);
    PLATFORM_API bool gesture_is_detected(PlatformWindow *w, unsigned gesture);
    PLATFORM_API float gesture_hold_duration(PlatformWindow *w); /* seconds, while HOLD */
    PLATFORM_API void gesture_drag_vector(PlatformWindow *w, float *x, float *y); /* pixels, from the touch-down point */
    PLATFORM_API float gesture_drag_angle(PlatformWindow *w);                      /* degrees, set on a swipe; 0 = right, counterclockwise */
    PLATFORM_API void gesture_pinch_vector(PlatformWindow *w, float *x, float *y); /* pixels, first to second finger */
    PLATFORM_API float gesture_pinch_angle(PlatformWindow *w);                     /* degrees, like the drag angle */

    /* ========================================================================== */
    /*  Gamepads                                                                  */
    /* ========================================================================== */

    /* Polled like the keyboard. Devices are detected by platform_init and while
       the application runs; window_begin_frame refreshes the state. The layout is
       SDL_GameController's: a pad that does not follow the standard layout is not
       listed. Index 0..GAMEPAD_MAX-1 is a slot that stays taken while the device
       is connected. */
    enum
    {
        GAMEPAD_MAX = 4
    };

    typedef enum
    {
        GAMEPAD_BUTTON_A,
        GAMEPAD_BUTTON_B,
        GAMEPAD_BUTTON_X,
        GAMEPAD_BUTTON_Y,
        GAMEPAD_BUTTON_BACK,
        GAMEPAD_BUTTON_GUIDE,
        GAMEPAD_BUTTON_START,
        GAMEPAD_BUTTON_LEFT_STICK,
        GAMEPAD_BUTTON_RIGHT_STICK,
        GAMEPAD_BUTTON_LEFT_SHOULDER,
        GAMEPAD_BUTTON_RIGHT_SHOULDER,
        GAMEPAD_BUTTON_DPAD_UP,
        GAMEPAD_BUTTON_DPAD_DOWN,
        GAMEPAD_BUTTON_DPAD_LEFT,
        GAMEPAD_BUTTON_DPAD_RIGHT,
        GAMEPAD_BUTTON_COUNT
    } GamepadButton;

    /* Sticks are -1..1 with Y growing downwards; triggers are 0..1. No deadzone. */
    typedef enum
    {
        GAMEPAD_AXIS_LEFT_X,
        GAMEPAD_AXIS_LEFT_Y,
        GAMEPAD_AXIS_RIGHT_X,
        GAMEPAD_AXIS_RIGHT_Y,
        GAMEPAD_AXIS_TRIGGER_LEFT,
        GAMEPAD_AXIS_TRIGGER_RIGHT,
        GAMEPAD_AXIS_COUNT
    } GamepadAxis;

    PLATFORM_API bool gamepad_connected(int index);
    PLATFORM_API const char *gamepad_name(int index); /* NULL when not connected */
    PLATFORM_API bool gamepad_button_down(int index, int button);
    PLATFORM_API float gamepad_axis(int index, int axis);

    /* ========================================================================== */
    /*  Time                                                                      */
    /* ========================================================================== */

    PLATFORM_API double time_seconds(void); /* since platform_init */
    PLATFORM_API uint64_t time_nanos(void);
    PLATFORM_API void time_sleep(uint32_t milliseconds);

    /* ========================================================================== */
    /*  Clipboard                                                                 */
    /* ========================================================================== */

    PLATFORM_API void clipboard_set(const char *text);
    PLATFORM_API const char *clipboard_get(void); /* owned by the platform */

    /* ========================================================================== */
    /*  OS / filesystem                                                           */
    /* ========================================================================== */

    /* Reads and writes are separate namespaces (see ARCHITECTURE section 9).
       asset_ is read-only and routed through the backend on platforms where
       shipped resources are not real files (Android = AAssetManager); file_ and
       dir_ work on real, writable paths. asset and file read results are
       heap-allocated; release them with fs_free. */

    typedef struct IoStream IoStream;

    typedef enum
    {
        IO_SEEK_SET,
        IO_SEEK_CUR,
        IO_SEEK_END
    } IoWhence;

    /* mode as fopen: "r" "w" "a", optional '+', 'b' ignored. On Android a relative
       path is tried under the internal data dir, then (read-only) in the APK assets. */
    PLATFORM_API IoStream *io_open_file(const char *path, const char *mode);
    PLATFORM_API IoStream *io_open_asset(const char *path);
    PLATFORM_API IoStream *io_open_memory(const void *mem, size_t size);
    PLATFORM_API size_t io_read(IoStream *s, void *dst, size_t n);
    PLATFORM_API size_t io_write(IoStream *s, const void *src, size_t n);
    PLATFORM_API int64_t io_seek(IoStream *s, int64_t offset, IoWhence whence);
    PLATFORM_API int64_t io_tell(IoStream *s);
    PLATFORM_API int64_t io_size(IoStream *s);
    PLATFORM_API bool io_eof(IoStream *s);
    PLATFORM_API bool io_flush(IoStream *s);
    PLATFORM_API bool io_close(IoStream *s);
    /* Whole-stream loads are NUL-terminated (size excludes it); free with fs_free. */
    PLATFORM_API void *io_load(IoStream *s, size_t *out_size, bool close);
    PLATFORM_API void *io_load_file(const char *path, size_t *out_size);
    PLATFORM_API bool io_save_file(const char *path, const void *data, size_t size);

    PLATFORM_API uint8_t *asset_read(const char *path, size_t *out_size);
    PLATFORM_API char *asset_read_text(const char *path); /* NUL-terminated */
    PLATFORM_API bool asset_exists(const char *path);
    /* Root that asset_* reads from until a backend routes them. Defaults to the
       application directory. */
    PLATFORM_API void asset_set_root(const char *path);

    PLATFORM_API uint8_t *file_read(const char *path, size_t *out_size);
    PLATFORM_API char *file_read_text(const char *path);
    PLATFORM_API bool file_write(const char *path, const void *data, size_t size);
    PLATFORM_API bool file_write_text(const char *path, const char *text);
    PLATFORM_API void fs_free(void *data);

    PLATFORM_API bool file_exists(const char *path);
    PLATFORM_API bool dir_exists(const char *path);
    PLATFORM_API int64_t file_size(const char *path);     /* -1 if unknown */
    PLATFORM_API int64_t file_mod_time(const char *path); /* unix seconds, -1 if unknown */

    /* Path string helpers, no I/O. Returned pointers into path are valid as long
       as path is. */
    PLATFORM_API const char *path_filename(const char *path);                  /* "a/b/c.txt" -> "c.txt" */
    PLATFORM_API const char *path_extension(const char *path);                 /* -> ".txt", "" if none */
    PLATFORM_API void path_directory(const char *path, char *out, size_t cap); /* -> "a/b" */
    PLATFORM_API bool path_has_extension(const char *path, const char *ext);   /* case-insensitive */

    PLATFORM_API bool path_is_absolute(const char *path);
    PLATFORM_API bool path_join(char *out, size_t cap, const char *a, const char *b);
    PLATFORM_API bool path_normalize(char *out, size_t cap, const char *path);
    PLATFORM_API bool path_absolute(char *out, size_t cap, const char *path);
    PLATFORM_API bool path_relative(char *out, size_t cap, const char *path, const char *base);

    typedef enum
    {
        PATH_TYPE_NONE,
        PATH_TYPE_FILE,
        PATH_TYPE_DIRECTORY,
        PATH_TYPE_OTHER
    } PathType;

    typedef struct
    {
        PathType type;
        int64_t size;
        int64_t modify_time_ns; /* unix epoch */
    } PathInfo;

    /* Return false and set platform_get_error() on failure. Symlinks are followed
       by fs_get_path_info and not by fs_remove_path. */
    PLATFORM_API bool fs_get_path_info(const char *path, PathInfo *out); /* out may be NULL */
    PLATFORM_API bool fs_create_directory(const char *path);             /* recursive, idempotent */
    PLATFORM_API bool fs_remove_path(const char *path);                  /* a file or an empty directory */
    PLATFORM_API bool fs_rename_path(const char *from, const char *to);  /* replaces an existing file */

    /* Calls cb with the full path of every entry; returning false from cb stops
       early and is not an error. Recursion does not follow symlinks. */
    typedef bool (*FsEnumCallback)(const char *path, PathType type, void *user);
    PLATFORM_API bool fs_enumerate_directory(const char *path, bool recursive, FsEnumCallback cb, void *user);

    /* Executable directory and per-user writable directory (created), both ending
       in '/'. The temp directory has no trailing '/'. */
    PLATFORM_API bool fs_get_base_path(char *out, size_t cap);
    PLATFORM_API bool fs_get_pref_path(char *out, size_t cap, const char *org, const char *app);
    PLATFORM_API bool fs_get_temp_path(char *out, size_t cap);

    /* Directories, raylib-style cursor over the process cwd. The dir_* string
       getters return a pointer to internal storage, valid until the next call. */
    PLATFORM_API const char *dir_current(void); /* working directory */
    PLATFORM_API const char *dir_app(void);     /* the executable's directory */
    PLATFORM_API const char *dir_data(void);    /* writable per-app dir */
    PLATFORM_API bool dir_change(const char *path);
    PLATFORM_API bool dir_make(const char *path); /* recursive */

    typedef struct
    {
        char **paths;
        int count;
    } DirList;

    PLATFORM_API bool dir_list(const char *path, DirList *out);
    PLATFORM_API void dir_list_free(DirList *list);

    /* ========================================================================== */
    /*  Constants                                                                 */
    /* ========================================================================== */

    enum
    {
        KEY_NULL = 0,
        KEY_SPACE = 32,
        KEY_APOSTROPHE = 39,
        KEY_COMMA = 44,
        KEY_MINUS,
        KEY_PERIOD,
        KEY_SLASH,
        KEY_ZERO = 48,
        KEY_ONE,
        KEY_TWO,
        KEY_THREE,
        KEY_FOUR,
        KEY_FIVE,
        KEY_SIX,
        KEY_SEVEN,
        KEY_EIGHT,
        KEY_NINE,
        KEY_SEMICOLON = 59,
        KEY_EQUAL = 61,
        KEY_A = 65,
        KEY_B,
        KEY_C,
        KEY_D,
        KEY_E,
        KEY_F,
        KEY_G,
        KEY_H,
        KEY_I,
        KEY_J,
        KEY_K,
        KEY_L,
        KEY_M,
        KEY_N,
        KEY_O,
        KEY_P,
        KEY_Q,
        KEY_R,
        KEY_S,
        KEY_T,
        KEY_U,
        KEY_V,
        KEY_W,
        KEY_X,
        KEY_Y,
        KEY_Z,
        KEY_LEFT_BRACKET = 91,
        KEY_BACKSLASH,
        KEY_RIGHT_BRACKET,
        KEY_GRAVE = 96,

        KEY_ESCAPE = 256,
        KEY_ENTER,
        KEY_TAB,
        KEY_BACKSPACE,
        KEY_INSERT,
        KEY_DELETE,
        KEY_RIGHT,
        KEY_LEFT,
        KEY_DOWN,
        KEY_UP,
        KEY_PAGE_UP,
        KEY_PAGE_DOWN,
        KEY_HOME,
        KEY_END,
        KEY_CAPS_LOCK = 280,
        KEY_NUM_LOCK,
        KEY_PRINT_SCREEN,
        KEY_PAUSE,
        KEY_F1 = 290,
        KEY_F2,
        KEY_F3,
        KEY_F4,
        KEY_F5,
        KEY_F6,
        KEY_F7,
        KEY_F8,
        KEY_F9,
        KEY_F10,
        KEY_F11,
        KEY_F12,
        KEY_KP_0 = 320,
        KEY_KP_1,
        KEY_KP_2,
        KEY_KP_3,
        KEY_KP_4,
        KEY_KP_5,
        KEY_KP_6,
        KEY_KP_7,
        KEY_KP_8,
        KEY_KP_9,
        KEY_KP_DECIMAL,
        KEY_KP_DIVIDE,
        KEY_KP_MULTIPLY,
        KEY_KP_SUBTRACT,
        KEY_KP_ADD,
        KEY_KP_ENTER,
        KEY_KP_EQUAL,
        KEY_LEFT_SHIFT = 340,
        KEY_LEFT_CONTROL,
        KEY_LEFT_ALT,
        KEY_LEFT_SUPER,
        KEY_RIGHT_SHIFT,
        KEY_RIGHT_CONTROL,
        KEY_RIGHT_ALT,
        KEY_RIGHT_SUPER,
        KEY_MENU,
        KEY_SCROLL_LOCK,
        KEY_MAX
    };

    enum
    {
        KEYMOD_SHIFT = 1,
        KEYMOD_CTRL = 2,
        KEYMOD_ALT = 4,
        KEYMOD_SUPER = 8
    };

    enum
    {
        MOUSE_LEFT = 0,
        MOUSE_RIGHT,
        MOUSE_MIDDLE,
        MOUSE_X1,
        MOUSE_X2,
        MOUSE_BUTTON_MAX = 8
    };

    enum
    {
        CURSOR_DEFAULT = 0,
        CURSOR_ARROW,
        CURSOR_IBEAM,
        CURSOR_CROSSHAIR,
        CURSOR_HAND,
        CURSOR_RESIZE_EW,
        CURSOR_RESIZE_NS,
        CURSOR_NOT_ALLOWED,
        CURSOR_RESIZE_NWSE,
        CURSOR_RESIZE_NESW,
        CURSOR_RESIZE_ALL,
        CURSOR_COUNT
    };

    enum
    {
        MOUSE_MODE_NORMAL = 0, /* visible, free */
        MOUSE_MODE_HIDDEN,     /* hidden, free */
        MOUSE_MODE_CAPTURED    /* hidden and locked, relative motion only */
    };

#define MAX_TOUCH_POINTS 10

#ifdef __cplusplus
}
#endif

#endif /* PLATFORM_H */
