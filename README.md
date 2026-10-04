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
| Linux / X11 | `backend_x11.c` | requires X11 + Xrandr + GL/GLX dev packages |
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
gesture_set_mouse_emulation(w, true);                    // left button acts as one finger, to test on a desktop
```

Gesture detection follows raylib's rgestures: tap, double tap, hold, drag, four
swipes and pinch in/out. Touch points come from Android, the web and the fake
backend. The X11 and Win32 backends do not report touch yet, so on those use
`gesture_set_mouse_emulation`.

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
  draw2d.c                software rasterizer over Framebuffer
  image.c                 BMP load/save
  os.c                    filesystem and path utilities
  os_backend.h            asset-hook interface for Android/web
tests/
  test_input_logic.c      input edge/state tests (backend_fake)
  test_app_run.c          frame cycle test
  test_gestures.c         gesture recognizer and touch through the core
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
