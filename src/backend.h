/*
 * backend.h - the interface every platform implements. The core never touches
 * the OS; it goes through here. A backend pushes normalized events into the core
 * via core_push_event / core_push_char (core_internal.h); it owns no queue.
 */
#ifndef BACKEND_H
#define BACKEND_H

#include "platform.h"

typedef struct Core Core;
typedef struct BackendWindow BackendWindow;

/* Global platform bring-up, before any window or monitor query (X11 opens the
   Display here). Returns false if the platform cannot be brought up. */
bool backend_init(void);
void backend_shutdown(void);

/* create window + GL context; returns native handle or NULL */
BackendWindow *backend_create(const WindowConfig *cfg);
void backend_destroy(BackendWindow *b);

/* Drain all pending OS events. For each, normalize and call core_push_event /
   core_push_char. Called once per window_begin_frame. The core pointer is valid
   only for the duration of the call. */
void backend_pump_events(BackendWindow *b, Core *core);

/* Sleep until the OS has events for the windows (backend_pump_events would find some), platform_wake was called, something
   the core polls has input (gamepads), or timeout_ns have passed (negative: no limit); returns at once if any of that is
   already so. The events stay where they are: nothing is handed to the core here. Returns true when there is something for
   the next backend_pump_events to deliver or a wake was taken; false when the time ran out, a signal interrupted the sleep,
   or only a deadline of the backend's own came due (a key held that must be looked at, a repeat, a drop that gave up: the
   next backend_pump_events does that work, so the caller runs one in any case). Main thread only. backend_wake is the
   backend's own half of platform_wake, for a backend whose sleep is not on core_wake_fd / core_wake_handle (Android's
   looper); any thread, and it must not touch a window: empty elsewhere. */
bool backend_wait_events(int64_t timeout_ns);
void backend_wake(void);

void backend_swap(BackendWindow *b);

/* geometry - two distinct sizes (see hi-DPI gotcha) */
void backend_get_size(BackendWindow *b, int *w, int *h); /* screen coords */
void backend_set_size(BackendWindow *b, int w, int h);
void backend_get_fb_size(BackendWindow *b, int *w, int *h); /* pixels */
void backend_get_pos(BackendWindow *b, int *x, int *y);
void backend_set_pos(BackendWindow *b, int x, int y);
float backend_content_scale(BackendWindow *b);
void backend_set_title(BackendWindow *b, const char *title);
void backend_set_size_limits(BackendWindow *b, int minw, int minh, int maxw, int maxh);

/* state */
void backend_minimize(BackendWindow *b);
void backend_maximize(BackendWindow *b);
void backend_restore(BackendWindow *b);
void backend_show(BackendWindow *b);
void backend_hide(BackendWindow *b);
void backend_focus(BackendWindow *b);
void backend_request_attention(BackendWindow *b);
bool backend_get_flag(BackendWindow *b, int flag); /* WIN_FLAG_* */
void backend_set_mode(BackendWindow *b, WindowMode mode, int monitor);
WindowMode backend_get_mode(BackendWindow *b);
void backend_set_icon(BackendWindow *b, int w, int h, const uint8_t *rgba);
void backend_set_opacity(BackendWindow *b, float a);
void backend_set_always_on_top(BackendWindow *b, bool on);
void backend_set_decorated(BackendWindow *b, bool on);
void backend_set_hit_test(BackendWindow *b, PlatformWindow *w, HitTestFunc fn, void *user);

/* GL context */
void backend_make_current(BackendWindow *b);
void backend_make_current_on(BackendWindow *target, BackendWindow *context);
void backend_set_vsync(BackendWindow *b, bool on);
void *backend_gl_proc_address(const char *name);
/* shared context for a worker thread; create returns NULL when unsupported */
typedef struct BackendGLWorker BackendGLWorker;
BackendGLWorker *backend_gl_worker_create(BackendWindow *b);
bool backend_gl_worker_make_current(BackendGLWorker *worker, bool current);
void backend_gl_worker_destroy(BackendGLWorker *worker);

/* native handles (NativeHandleType); NULL when there is no such object */
void *backend_native_handle(BackendWindow *b, NativeHandleType type);

/* Vulkan: the instance extensions the window system needs (a static array; NULL
   where Vulkan is not supported), and surface creation for a RENDER_VULKAN
   window. The loader and the vkGetInstanceProcAddr plumbing are in vulkan.c; a
   backend fills its create info and calls vulkan_call_create_surface. */
const char *const *backend_vulkan_extensions(uint32_t *count);
bool backend_vulkan_create_surface(BackendWindow *b, void *instance, const void *allocator, uint64_t *out_surface);

/* pixel surface (RENDER_PIXELS windows); lock returns false for RENDER_GL */
bool backend_lock_pixels(BackendWindow *b, Framebuffer *out);
void backend_present_pixels(BackendWindow *b);

/* modal-loop redraw callback (Windows) and the Caps/Num Lock state (KEYMOD_*_LOCK) */
void backend_set_live_callback(BackendWindow *b, PlatformWindow *w, FrameCallback cb, void *user);
int backend_lock_state(void);

/* input method: engage or release it, and where the caret is (window coordinates) */
void backend_set_text_input(BackendWindow *b, bool on);
void backend_set_text_input_rect(BackendWindow *b, int x, int y, int w, int h);

/* mouse */
bool backend_mouse_capture(BackendWindow *b, bool on);
PlatformCursor *backend_cursor_create(const uint32_t *argb, int w, int h, int hot_x, int hot_y);
void backend_cursor_destroy(PlatformCursor *c);
void backend_set_cursor_image(BackendWindow *b, PlatformCursor *c); /* NULL: back to the shape */
void backend_set_mouse_pos(BackendWindow *b, int x, int y);
void backend_set_cursor(BackendWindow *b, int cursor);   /* CURSOR_* */
void backend_set_mouse_mode(BackendWindow *b, int mode); /* MOUSE_MODE_* */

/* clipboard - one content in several representations, by MIME type. set copies
   what it is given (count 0 clears). get returns a malloc'd copy followed by a NUL
   byte that *size does not count, or NULL; the caller frees it. Text is exactly
   CLIPBOARD_TEXT, UTF-8; a backend maps it to the native text format. */
bool backend_clipboard_set(const ClipboardItem *items, int count);
bool backend_clipboard_has(const char *mime);
void *backend_clipboard_get(const char *mime, size_t *size);

/* the primary selection (the text selected in a program, pasted by the middle button), text only. supported says whether
   there is one (X11; Wayland when the compositor has the protocol; the fake): where it is false set returns false without an
   error and get returns NULL. set copies the UTF-8 text; NULL or length 0 gives the selection up. get returns a malloc'd
   copy followed by a NUL byte that *size does not count, or NULL when nobody owns the selection or it holds no text. */
bool backend_primary_supported(void);
bool backend_primary_set(const char *utf8, size_t len);
void *backend_primary_get(size_t *size);

/* monitors - one shared virtual coordinate space */
int backend_monitor_count(void);
bool backend_monitor_info(int index, MonitorInfo *out);
bool backend_mouse_global_position(int *x, int *y);

/* Loop. Desktop: while(!should_close){ frame(); }. Web: emscripten_set_main_loop. */
void backend_run(BackendWindow *b, PlatformWindow *w, FrameCallback frame, void *user);

#endif /* BACKEND_H */
