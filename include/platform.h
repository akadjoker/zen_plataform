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
       (window_lock_pixels / window_present_pixels). Chosen once, at creation. */
    typedef enum
    {
        RENDER_GL,
        RENDER_PIXELS
    } RenderMode;

#define WINDOW_POS_CENTERED (-1)  /* place centered on the chosen monitor */
#define WINDOW_POS_UNDEFINED (-2) /* let the system choose the position   */
#define MONITOR_CURRENT (-1)      /* the monitor the window is mostly on  */

    typedef struct
    {
        const char *title;
        int width, height; /* screen coordinates, not pixels */
        int x, y;          /* WINDOW_POS_CENTERED / _UNDEFINED, or virtual coord */
        int monitor;       /* index, or MONITOR_CURRENT */
        WindowMode mode;
        RenderMode render; /* RENDER_GL (default) or RENDER_PIXELS */
        int gl_major, gl_minor;
        bool resizable;
        bool vsync;
        int msaa; /* sample count, 0 to disable */
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

    PLATFORM_API void draw_clear(Framebuffer *fb, uint32_t color);
    PLATFORM_API void draw_pixel(Framebuffer *fb, int x, int y, uint32_t color, BlendMode blend);
    PLATFORM_API uint32_t draw_get_pixel(const Framebuffer *fb, int x, int y); /* 0 if out of bounds */

    PLATFORM_API void draw_line(Framebuffer *fb, int x0, int y0, int x1, int y1, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color, BlendMode blend);
    PLATFORM_API void draw_fill_rect(Framebuffer *fb, int x, int y, int w, int h, uint32_t color, BlendMode blend);
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

    /* ========================================================================== */
    /*  Time                                                                      */
    /* ========================================================================== */

    PLATFORM_API double time_seconds(void); /* since platform_init */
    PLATFORM_API uint64_t time_nanos(void);

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
        KEY_LEFT_SHIFT = 340,
        KEY_LEFT_CONTROL,
        KEY_LEFT_ALT,
        KEY_LEFT_SUPER,
        KEY_RIGHT_SHIFT,
        KEY_RIGHT_CONTROL,
        KEY_RIGHT_ALT,
        KEY_RIGHT_SUPER,
        KEY_MAX
    };

    enum
    {
        MOD_SHIFT = 1,
        MOD_CTRL = 2,
        MOD_ALT = 4,
        MOD_SUPER = 8
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
        CURSOR_NOT_ALLOWED
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
