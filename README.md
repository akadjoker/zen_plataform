# zen_platform

A minimal, cross-platform window, input, and software-rendering library for C.
Zero third-party dependencies: only native OS SDKs (X11, Win32, Cocoa, EGL,
Emscripten, Android NDK). Inspired by GLFW and raylib.

## Features

- **Single public header** -- `#include "platform.h"`
- **Window creation** with OpenGL context or CPU pixel buffer
- **Input state** -- keyboard, mouse, touch, gamepad, clipboard
- **Event queue** -- per-frame event list for both polled and event-driven code
- **Software rasterizer (draw2d)** -- lines, rects, circles, triangles, blit with
  bilinear or nearest-neighbour scaling, alpha blending
- **Framebuffer** -- ownable pixel buffers with BMP load/save
- **Filesystem** -- read/write files, asset packaging, directory listing
- **Time** -- monotonic seconds and nanoseconds
- **Cross-platform loop** -- `app_run()` works identically on desktop, web, and
  mobile (loop inversion)
- **No malloc/free surprises** -- the library does not allocate memory without
  your explicit call

## Building

```sh
mkdir build && cd build
cmake ..
make
```

On Windows, from a Visual Studio developer prompt:

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The CRT is linked statically (`/MT`; `-static` with MinGW), so the executables
import only DLLs that ship with Windows: no vcruntime, no SDL, no GLFW.

### Releases

CI builds Linux, Windows (MSVC and MinGW), Web and Android on every push. Pushing a
tag that starts with `v` (for example `v0.1.0`) publishes a GitHub release with one
zip per platform: the static library, `platform.h`, and the examples.
A tag with a hyphen (`v0.1.0-rc1`) is marked as a pre-release.

Without a local tag, run the workflow by hand (Actions, CI, Run workflow) and type the
version in `release_version`; the release is created on the chosen commit.

### Options

| Flag | Default | Description |
|---|---|---|
| `-DBUILD_SHARED_LIBS=ON` | OFF | Build as a shared library |
| `-DPLATFORM_BUILD_TESTS=ON` | ON | Build unit tests |
| `-DPLATFORM_BUILD_EXAMPLES=ON` | ON | Build examples |
| `-DPLATFORM_USE_FAKE=ON` | OFF | Use fake/headless backend |
| `-DPLATFORM_SANITIZE=ON` | OFF | Enable ASan + UBSan |

### Platforms

| Platform | Backend | Notes |
|---|---|---|
| Linux / X11 | `backend_x11.c` | requires X11 + Xrandr + GL/GLX dev packages; Vulkan loaded at run time |
| Windows | `backend_win32.c` | MSVC or MinGW, links opengl32+gdi32+user32+shell32; XInput is loaded at run time |
| Web | `backend_web.c` | `emcmake cmake ..` for Emscripten |
| Android | `backend_android.c` | NativeActivity + EGL + NDK glue |
| macOS/iOS | none | not supported yet; only the headless fake backend builds (`-DPLATFORM_USE_FAKE=ON`) |

## API Overview

### Lifecycle

```c
if (!platform_init()) return 1;

WindowConfig cfg = {
    .title = "Hello", .width = 640, .height = 480,
    .render = RENDER_GL,
    .gl = { .profile = GL_PROFILE_CORE, .major = 4, .minor = 5, .msaa = 4, .debug = true }
};
PlatformWindow *w = window_create(&cfg);
if (!w) fprintf(stderr, "%s\n", platform_get_error());

app_run(w, frame_callback, user_data);

window_destroy(w);
platform_shutdown();
```

### Input state (polled)

```c
bool down = key_down(w, KEY_W);
bool pressed = key_pressed(w, KEY_R);   // single-frame edge
int mx = mouse_x(w), my = mouse_y(w);
int dx, dy;  mouse_delta(w, &dx, &dy);
bool left_click = mouse_button_released(w, MOUSE_LEFT);
int mods = key_mods(w);                 // KEYMOD_SHIFT | KEYMOD_CTRL | KEYMOD_ALT | KEYMOD_SUPER held now
bool numpad7 = key_down(w, KEY_KP_7);
mouse_set_cursor(w, CURSOR_RESIZE_NWSE);
time_sleep(16);                         // milliseconds
```

### Event hook, live resize, capture, lock keys

```c
window_set_event_hook(w, my_hook, ui);       // sees every event as it is queued (SDL_AddEventWatch)
window_set_live_callback(w, redraw, app);    // Windows: called while the OS drags/resizes the window
mouse_capture(w, true);                      // keep getting the mouse outside the window, off when the drag ends
if (key_mods(w) & KEYMOD_CAPS_LOCK) { /* ... */ }   // also KEYMOD_NUM_LOCK
```

The hook runs before the event reaches the polled state, even while Windows runs its
own loop for a resize, so a UI layer (Dear ImGui) keeps being fed. Text also arrives
as `EVENT_CHAR` events now, next to `char_get_pressed`.

### Window kinds, custom title bars, cursor images

```c
WindowConfig tip = {.title = "tip", .width = 160, .height = 40, .kind = WINDOW_KIND_TOOLTIP, .parent = main_window};
WindowConfig dlg = {.title = "Settings", .width = 400, .height = 300, .kind = WINDOW_KIND_DIALOG, .parent = main_window};
WindowConfig bar = {.title = "Mine", .width = 800, .height = 600, .undecorated = true, .resizable = true};

HitTestResult hit(PlatformWindow *w, int x, int y, void *user)
{
    if (y < 32) return HIT_DRAG;                    // my title bar
    if (x > 790) return HIT_RESIZE_RIGHT;           // my right edge
    return HIT_NORMAL;
}
window_set_hit_test(w, hit, NULL);
window_set_decorated(w, true);                      // show or hide the real frame at run time

PlatformCursor *c = cursor_create(argb, 32, 32, 4, 4);   // 0xAARRGGBB pixels, hot spot
mouse_set_cursor_image(w, c);                       // NULL goes back to mouse_set_cursor's shape
```

A `POPUP` or `TOOLTIP` has no decorations and stays out of the taskbar; a `TOOLTIP`
never takes the focus. Presses on a hit-test title bar or edge go to the window
manager, which drags or resizes the window. On X11 the image cursors need
libXcursor at run time.

### Touch and gestures

```c
for (int i = 0; i < touch_count(w); i++)
{
    float x, y;  touch_position(w, i, &x, &y);   // touch_id(w, i) is stable down..up
}

if (gesture_is_detected(w, GESTURE_TAP)) { /* ... */ }
if (gesture_is_detected(w, GESTURE_SWIPE_LEFT)) { /* one frame only */ }
if (gesture_detected(w) == GESTURE_PINCH_OUT)
{
    float px, py;  gesture_pinch_vector(w, &px, &py);   // pixels, first to second finger
}
gesture_set_enabled(w, GESTURE_TAP | GESTURE_DRAG);      // report only these
touch_set_mouse_emulation(w, true);                      // desktop: the left button becomes one finger (TOUCH_ID_MOUSE)
```

Gesture detection follows raylib's rgestures: tap, double tap, hold, drag, four
swipes and pinch in/out. Touch points come from Android, the web and the fake
backend. The X11 and Win32 backends do not report touch yet, so on those use
`touch_set_mouse_emulation`, which turns the left button into one finger with a
real `EVENT_TOUCH`.

### Logging

```c
log_set_level(LOGLEVEL_DEBUG);              // default LOGLEVEL_INFO
log_info("loaded %d assets", n);            // log_debug / log_warn / log_error too
log_set_callback(my_sink, my_data);         // void my_sink(LogLevel, const char *msg, void *user); NULL restores stderr
```

The default sink prints `[LEVEL] text` to stderr (the browser console on the web),
to logcat on Android, and to the debugger output on Windows. The platform logs
the errors behind `platform_get_error()` at `LOGLEVEL_DEBUG`.

### Dialogs and URLs

```c
message_box(w, MESSAGE_ERROR, "Save failed", "The disk is full.");
if (confirm_box(w, "Quit", "Discard changes?") == 1) { /* yes */ }

FileFilter images[] = {{"Images", "*.png;*.jpg"}, {"All files", "*"}};
char path[1024];
if (dialog_open_file(w, "Open", NULL, images, 2, path, sizeof path)) { /* ... */ }
dialog_save_file(w, "Save as", "untitled.png", images, 1, path, sizeof path);
dialog_pick_folder(w, "Choose a folder", NULL, path, sizeof path);
int n = dialog_open_files(w, "Open several", NULL, NULL, 0, many, sizeof many);   // paths one per line

open_url("https://example.com");                 // the user's browser
```

Native dialogs on Windows; `zenity` or `kdialog` on Linux (an error if neither is
installed); `alert`/`confirm` on the web; none on Android yet. A cancelled dialog
returns false and leaves `platform_get_error()` empty, so you can tell it from one
that could not be shown.

### Threads

```c
static int work(void *user) { /* ... */ return 0; }

PlatformThread *t = thread_create(work, &data, "worker");
PlatformMutex *m = mutex_create();               // recursive
PlatformCond *c = cond_create();

mutex_lock(m);
while (!ready) cond_wait(c, m);                  // or cond_wait_timeout(c, m, 100)
mutex_unlock(m);

int result = thread_join(t);                     // or thread_detach(t)
int n = cpu_count();
```

pthreads and the Win32 API underneath; not available on the web. The platform's window,
input and clipboard state belong to one thread; `log_message` and the filesystem calls
are safe from any.

### Vulkan, native handles, shared libraries

```c
if (vulkan_supported())                         // loader present + the surface extensions
{
    WindowConfig cfg = {.title = "vk", .width = 1280, .height = 720, .render = RENDER_VULKAN};
    PlatformWindow *w = window_create(&cfg);    // a bare window, no GL context

    uint32_t n;  const char *const *ext = vulkan_instance_extensions(&n);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)vulkan_get_proc_addr();
    // vkCreateInstance with ext/n in VkInstanceCreateInfo, then:
    uint64_t surface;
    vulkan_create_surface(w, instance, NULL, &surface);   // a VkSurfaceKHR
}

void *hwnd = window_native_handle(w, NATIVE_WINDOW);      // HWND / X11 Window / ANativeWindow*
SharedLibrary *lib = library_open("libfoo.so");           // dlopen / LoadLibrary
void *fn = library_symbol(lib, "foo");
```

zen_platform does not link Vulkan and does not include its headers: the loader
(`vulkan-1.dll`, `libvulkan.so.1`) is opened at run time, and handles are opaque
(`VkInstance` is a `void*`, `VkSurfaceKHR` a `uint64_t`). Use the Vulkan headers,
or volk, in your own code. Supported on X11, Windows and Android; not on the web
or in the fake backend. `NATIVE_DISPLAY`, `NATIVE_WINDOW` and `NATIVE_GL_CONTEXT`
return the X11 `Display*`/`Window`/`GLXContext`, the Win32
`HINSTANCE`/`HWND`/`HGLRC`, and the Android `EGLDisplay`/`ANativeWindow*`/`EGLContext`.

### Clipboard

```c
clipboard_set("text");                          // text, as before
const char *t = clipboard_get();                // never NULL, "" when there is no text

clipboard_set_image(&fb);                       // a Framebuffer, as PNG
Framebuffer img;
if (clipboard_get_image(&img)) { /* ... */ framebuffer_free(&img); }

ClipboardItem items[] = {{CLIPBOARD_TEXT, "hi", 2}, {"text/html", html, html_len}};
clipboard_set_items(items, 2);                  // one content, several representations
if (clipboard_has_data(CLIPBOARD_URIS))         // copied files, as file:// lines
{
    size_t n;  char *uris = clipboard_get_data(CLIPBOARD_URIS, &n);  /* ... */  fs_free(uris);
}
```

Types are MIME strings. `CLIPBOARD_TEXT`, `CLIPBOARD_PNG` and `CLIPBOARD_URIS` map to
the native formats (`CF_UNICODETEXT`, `PNG` plus `CF_DIBV5`, `CF_HDROP` on Windows;
selection targets on X11); any other string is a custom type. On X11 large data goes
by INCR in both directions, and the application that copied must keep running its
event loop for others to paste. Android and the web keep the data inside the app,
and only text reaches the system clipboard on the web.

### Gamepads

```c
for (int i = 0; i < GAMEPAD_MAX; i++)
{
    if (!gamepad_connected(i)) continue;
    bool jump = gamepad_button_down(i, GAMEPAD_BUTTON_A);
    float x = gamepad_axis(i, GAMEPAD_AXIS_LEFT_X);   // -1..1, no deadzone
    float rt = gamepad_axis(i, GAMEPAD_AXIS_TRIGGER_RIGHT); // 0..1
}
```

Polled; `window_begin_frame` refreshes the state. Layout and order follow
SDL_GameController. Linux reads `/dev/input/event*` (evdev) and needs read
permission on them, normally the `input` group. Pads are detected at startup and
on hot-plug. Only pads that follow the kernel gamepad layout are listed; there is
no mapping database.

### Event queue

```c
Event e;
while (poll_event(w, &e)) {
    if (e.type == EVENT_KEY && e.data.key.down)
        printf("key: %d\n", e.data.key.key);
}
```

### Draw primitives (RENDER_PIXELS mode)

```c
Framebuffer fb;
if (window_lock_pixels(w, &fb)) {
    draw_clear(&fb, 0xFF181818);
    draw_fill_rect(&fb, 10, 10, 100, 50, 0xFF44CC44, BLEND_NONE);
    draw_rect(&fb, 10, 10, 100, 50, 0xFFFFFFFF, BLEND_NONE);
    draw_line(&fb, 0, 0, 200, 200, 0xFFFF4444, BLEND_NONE);
    draw_circle(&fb, 100, 100, 30, 0xFF4444FF, BLEND_NONE);
    draw_fill_circle(&fb, 100, 100, 30, 0xFF442244, BLEND_ALPHA);
    draw_fill_triangle(&fb, 10, 10, 100, 10, 50, 80, 0xFFFFCC44, BLEND_NONE);
    draw_blit(&fb, &src, 0, 0, src.w, src.h, 10, 10, 200, 200,
              BLEND_NONE, SCALE_BILINEAR);
    window_present_pixels(w);
}
```

### Owned framebuffers

```c
Framebuffer rt;
framebuffer_alloc(&rt, 320, 200);            // own pixel memory
framebuffer_load_bmp(&rt, "image.bmp");      // read BMP from disk
framebuffer_save_bmp(&rt, "out.bmp");        // write BMP to disk
framebuffer_free(&rt);
```

### Filesystem

```c
IoStream *s = io_open_file("save.dat", "rb");        // no stdio: POSIX fd / AAsset
io_read(s, buf, n); io_seek(s, 0, IO_SEEK_END); io_close(s);
void *all = io_load_file("level.bin", &size);        // NUL-terminated, fs_free
io_save_file("save.dat", data, size);                // atomic: .tmp + fsync + rename
IoStream *a = io_open_asset("tex/a.png");            // APK on Android, asset root elsewhere
uint8_t *data = file_read("file.bin", &size);   // heap-allocated, free with fs_free
char *text = file_read_text("doc.txt");
file_write("out.bin", data, size);
bool exists = file_exists("path");
const char *cwd = dir_current();

char save[512];
fs_get_pref_path(save, sizeof save, "MyOrg", "MyGame");  // created, ends with '/'
fs_get_base_path(base, sizeof base);                      // executable dir, ends with '/'
PathInfo info;
if (fs_get_path_info("save.dat", &info) && info.type == PATH_TYPE_FILE)
    printf("%lld bytes\n", (long long)info.size);
fs_create_directory("a/b/c");                             // recursive
fs_rename_path("a.tmp", "a.dat");
fs_enumerate_directory("assets", true, on_entry, NULL);   // bool on_entry(path, type, user)

char path[512];
path_join(path, sizeof path, "assets", "tex/a.png");     // "assets/tex/a.png"
path_normalize(path, sizeof path, "a/./b/../c");         // "a/c"
path_relative(path, sizeof path, "/p/assets/a.png", "/p"); // "assets/a.png"
if (!path_absolute(path, sizeof path, "save.dat"))
    printf("%s\n", platform_get_error());
```

## Examples

| Example | Description | Key APIs shown |
|---|---|---|
| **hello_x11** | Minimal GL window, prints input events, displays version | `RENDER_GL`, `poll_event`, `clipboard_*`, `monitor_*`, `glViewport` |
| **raytrace** | Interactive 3D ray tracer (spheres, reflections, shadows) with WASD + mouse look | `RENDER_PIXELS`, `key_down/pressed`, `mouse_delta`, `mouse_set_mode`, `framebuffer_save_bmp`, `window_set_title`, `draw_blit`, `draw_fill_rect/line/circle`, `file_exists` |
| **ddemo** | DOOM-style ray caster with DDA walls, floor/ceiling, animated sprite, minimap, z-buffer | pixel-level DDA ray casting, z-buffer occlusion, direct framebuffer writes, minimap with bresenham line |
| **galaxy** | 80s space shooter (Galaxian-like) with starfield, enemy formation, bullets, scoring | full game loop, direct pixel rendering, collision detection, game state management |
| **uitest** | Immediate-mode GUI demo: buttons, checkboxes, sliders | `mouse_button_pressed/released`, `mouse_x/y`, draw primitives with hover/active states |
| **clipboard_demo** | Copy an image in any program, press Ctrl+V and see it drawn (alpha over a checkerboard); copy it back, copy a test pattern or text, see which formats the clipboard offers (text, PNG, files), save as BMP. `--paste` pastes on start | `clipboard_get_image/set_image`, `clipboard_has_data`, `clipboard_get_data`, `EVENT_WINDOW_FOCUS`, `draw_blit` with alpha |
| **events_demo** | Every event live: a colour-coded stream, the pointer trail, touch points, recognised gestures, polled window/keyboard/mouse state, platform log lines; touch emulation with the mouse | `poll_event` (all `EVENT_*`), `touch_*`, `gesture_*`, `touch_set_mouse_emulation`, `log_set_callback`, `window_set_mode` |
| **mwidgets** | Desktop widget demo with moveable/resizable windows, tabs, sliders | advanced IMGUI with window management, custom bitmap 5x7 font |

## Project Structure

```
include/
  platform.h              public API header
  platform_export.h       PLATFORM_API export/import macro
src/
  core.c                  portable input state, queue, edge detection
  core_internal.h         core-backend shared structs
  backend.h               backend interface (one function table per OS)
  backend_x11.c           X11/GLX backend
  backend_win32.c         Win32/WGL backend
  backend_web.c           Emscripten backend
  backend_android.c       Android NativeActivity backend
  backend_fake.c          headless backend for tests
  gesture.c               touch gesture recognizer (after raylib's rgestures)
  log.c                   leveled logging with a replaceable sink
  library.c               shared library loading (dlopen / LoadLibrary)
  thread.c                threads, recursive mutexes, condition variables, cpu_count
  dialog.c                message boxes, file dialogs, open_url (native, zenity/kdialog, web)
  clipboard.c             clipboard API (text, MIME data, PNG images)
  png.c                   minimal PNG encoder and decoder for the clipboard
  clipboard_mem.h         in-process clipboard store for fake, Android and web
  vulkan.c                Vulkan loader and surface creation (no Vulkan SDK needed)
  vulkan_internal.h       the few Vulkan declarations the backends use
  draw2d.c                software rasterizer over Framebuffer
  image.c                 BMP load/save
  os.c                    filesystem and path utilities
  os_backend.h            asset-hook interface for Android/web
tests/
  test_input_logic.c      input edge/state tests (backend_fake)
  test_app_run.c          frame cycle test
  test_gestures.c         gesture recognizer and touch through the core
  test_log.c              log levels, sink, truncation
  test_events.c           event hook, text events, lock keys, capture (fake backend)
  test_x11_window.c       capture, lock keys, window kinds, hit test, image cursors on a real X server
  test_library.c          shared library loading
  test_thread.c           threads, mutex, condition, timeout, detach
  test_dialog.c           zenity/kdialog/xdg-open arguments and answers, with stand-in programs
  test_clipboard.c        clipboard API and PNG round trip (fake backend)
  test_clipboard_x11.c    real X11 selections against forked clients, INCR included
  test_multiwindow_x11.c  two windows on one X connection: events reach their own window
  test_vulkan.c           RENDER_VULKAN window, handles, a real VkSurfaceKHR (skips without a driver)
  test_draw2d.c           rasterizer tests (nearest + bilinear blit)
  test_fs.c               filesystem round-trip tests
  test_image.c            BMP read/write round-trip
examples/
  hello_x11.c             minimal GL example
  raytrace.c              interactive software ray tracer
  ddemo.c                 DOOM-style ray caster
  galaxy.c                80s space shooter
  uitest.c                IMGUI test
  mwidgets.c              desktop widget demo
  CMakeLists.txt
CMakeLists.txt            root CMake config
ARCHITECTURE.md           architectural decisions (read first)
CLAUDE.md                 coding conventions
```

## License

MIT
