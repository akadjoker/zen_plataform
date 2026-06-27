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
| Windows | `backend_win32.c` | MSVC or MinGW, links opengl32+gdi32+user32 |
| Web | `backend_web.c` | `emcmake cmake ..` for Emscripten |
| Android | `backend_android.c` | NativeActivity + EGL + NDK glue |
| macOS/iOS | `backend_cocoa.mm` | Obj-C++, in progress |

## API Overview

### Lifecycle

```c
if (!platform_init()) return 1;

WindowConfig cfg = {
    .title = "Hello", .width = 640, .height = 480,
    .render = RENDER_GL
};
PlatformWindow *w = window_create(&cfg);

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
```

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
uint8_t *data = file_read("file.bin", &size);   // heap-allocated, free with fs_free
char *text = file_read_text("doc.txt");
file_write("out.bin", data, size);
bool exists = file_exists("path");
const char *cwd = dir_current();
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
  backend_cocoa.mm        macOS/iOS backend (future)
  draw2d.c                software rasterizer over Framebuffer
  image.c                 BMP load/save
  os.c                    filesystem and path utilities
  os_backend.h            asset-hook interface for Android/web
tests/
  test_input_logic.c      input edge/state tests (backend_fake)
  test_app_run.c          frame cycle test
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
