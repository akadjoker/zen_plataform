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

The X11 tests open windows and type with XTest, so on a desktop they belong on a
display of their own. `tests/run_x11_isolated.sh` starts a headless gnome-shell,
a rootful Xwayland and an ibus-x11 for it, and runs each test with ibus and with no
input method, the session in use untouched:

```sh
tests/run_x11_isolated.sh build/tests/test_input_x11 build/tests/test_x11_window
```

`tools/check.sh` is the one command that says whether the tree is good: the tests in a Release build and in an ASan + UBSan
build, the threaded tests under ThreadSanitizer, and a MinGW cross-compile with `-Wall -Wextra` (compile only), one line per
stage and a non-zero exit when one fails. Every test it starts goes through `tools/xvfb_run.sh` (an invisible Xvfb, no Wayland
and no session of yours), and so should anything else you run from here: `tools/xvfb_run.sh build/tests/test_io`.
`docs/SUSPEITAS.md` lists what looks wrong and is not proved.

On Windows, from a Visual Studio developer prompt:

```sh
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release
```

The CRT is linked statically (`/MT`; `-static` with MinGW), so the executables
import only DLLs that ship with Windows: no vcruntime, no SDL, no GLFW.

On macOS (Xcode or the command line tools, CMake 3.16 or later), the same three commands build the native backend, which is
Objective-C over AppKit (`src/backend_cocoa.m`, the only file of the library that is not C) and runs on macOS 11.0 or later
(`-DCMAKE_OSX_DEPLOYMENT_TARGET=...` changes that). `-DPLATFORM_USE_FAKE=ON` builds the headless library there too. The tests of the
window, OpenGL, the pixel surface, the keyboard and mouse, text input, the clipboard, drops, dialogs, window modes and looks, Vulkan and
the toolkit's frames (`test_cocoa_window`, `test_gl_cocoa`, `test_gl_worker_cocoa`, `test_pixels_cocoa`, `test_input_cocoa`,
`test_text_cocoa`, `test_clipboard_cocoa`, `test_drop_cocoa`, `test_dialog_cocoa`, `test_window_cocoa`, `test_vulkan_cocoa`,
`test_ui_cocoa`, `test_wait_cocoa`) need a graphical session and skip without one; `test_cocoa_names` (the library's names against the headers of Cocoa, Carbon,
OpenGL, QuartzCore and Mach, with a thread, a mutex and a condition variable) needs none and is built by the fake backend on macOS too;
`test_ui_cocoa` writes the toolkit's software and GL frames as PNG files into `test-images/` of the build folder. There is no Mac among the machines it is developed
on, so `.github/workflows/mac.yml` builds and tests it on a GitHub macOS runner. It runs only when it is started by hand (the Actions tab or `gh workflow run mac.yml --ref <branch>`; a push makes no run). `docs/MAC-PORT.md` says where the port stands (the
phases, what the runner showed, what could not be verified, the next steps).

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
| `-DPLATFORM_USE_FAKE=ON` | OFF | Use fake/headless backend (also the way to build for macOS without a window, and for iOS) |
| `-DCMAKE_OSX_DEPLOYMENT_TARGET=` | 11.0 | Oldest macOS the build runs on (macOS only) |
| `-DPLATFORM_LINUX_BACKEND=wayland` | x11 | Window system on Linux: `x11` or `wayland` |
| `-DPLATFORM_SANITIZE=ON` | OFF | Enable ASan + UBSan |
| `-DPLATFORM_BUILD_COMPARE=ON` | OFF | Build the GLFW / SDL2 comparison programs of `bench/compare` (X11 only; they need those libraries); see `docs/COMPARACAO_CORE.md` and `tools/bench_compare.sh` |

### Platforms

| Platform | Backend | Notes |
|---|---|---|
| Linux / X11 | `backend_x11.c` | requires X11 + Xrandr + GL/GLX dev packages; Vulkan loaded at run time |
| Linux / Wayland | `backend_wayland.c` | in progress: windows, window state, keyboard, pointer, cursors and the GL context (EGL) so far; needs wayland-client, wayland-cursor, wayland-egl, EGL, xkbcommon and wayland-protocols |
| Windows | `backend_win32.c` | MSVC or MinGW, links opengl32+gdi32+user32+shell32; XInput is loaded at run time |
| Web | `backend_web.c` | `emcmake cmake ..` for Emscripten |
| Android | `backend_android.c` | NativeActivity + EGL + NDK glue |
| macOS | `backend_cocoa.m`, `dialog_cocoa.m` | in progress (macOS 11.0 or later; links Cocoa, CoreGraphics, OpenGL and QuartzCore). Compiled and run on a macOS runner (run 44160945847): windows (create, destroy, show, hide, title, size, position, limits, minimize, maximize, focus), the window events (close, resize, move, focus, minimize, maximize, restore, pointer enter and leave), high-DPI (size in points, framebuffer in pixels, content scale), monitors, the menu bar with Quit (Cmd+Q, which raises a close request), the live callback; OpenGL (a core 3.2 to 4.1 context, what any core request from 3.2 to 4.1 gives is a 4.1 one, or the legacy 2.1 one; depth, stencil, samples, vsync, one context for several windows, a shared context for a worker thread, the `zen_2d` renderer; a request macOS cannot meet fails with the reason), the pixel surface, keyboard (the key table from Apple's virtual key codes, repeat, left and right modifiers, Command as Super, Caps Lock), mouse (buttons, drags, wheel and trackpad scroll, warp, capture, hidden and captured modes) and cursors. Menu key equivalents are AppKit's first: Cmd+Q quits through the menu and every other Command shortcut arrives as a key. Compiled and run at run 44285580324 (Xcode 26.6, SDK 26.5; no error, no warning in four configurations; see `docs/MAC-PORT.md` for each check): text input through `NSTextInputClient` (dead keys, composition, the candidate window), the clipboard (`NSPasteboard`: text, PNG, file URIs, custom types), file drop, the native dialogs (`NSAlert`, `NSOpenPanel`, `NSSavePanel`, `NSWorkspace`), fullscreen and borderless fullscreen (a borderless window the size of the monitor), the Dock icon, opacity, always on top, undecorated windows and the window kinds, the hit test (drag and resize regions), trackpad pinch, rotate and swipe as touch points, and the Vulkan surface (`VK_EXT_metal_surface` over a `CAMetalLayer`, MoltenVK found at run time). Of these the run passed text input, the dialogs (opened and ended by a timer, not answered by a person), the window modes and looks, the hit test, the gestures (made-up events) and a real Vulkan surface (MoltenVK from Homebrew); it failed the clipboard's custom types, the drop of a name with an accent and the level of a popup or tooltip, which were fixed afterwards and not run again. Gamepads: not implemented (a stub). Not verifiable on the CI machine, which has one display of scale 1 and no keyboard, mouse or trackpad of its own: the scale-change event, a scale 2 display, a drag with a real mouse, the pointer held by the captured mode with a real mouse, a trackpad |
| iOS | none | not supported; only the headless fake backend builds (`-DPLATFORM_USE_FAKE=ON`) |

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

### Sleeping until something happens (0 % CPU when idle)

`app_run` and a loop of `window_begin_frame` calls run flat out: a window with no vsync spins a core. A program that draws
only when something changed sleeps in `platform_wait_events` between frames:

```c
while (!window_should_close(w))
{
    platform_wait_events(-1);   // sleeps until an event, a platform_wake, a gamepad, or the timeout (seconds; -1: none)
    window_begin_frame(w);      // the events the wait woke for are here: the wait takes nothing
    handle_input(w);
    if (dirty) draw(w);
}

// a worker thread, a timer or a signal handler finishes something: set the data, then
platform_wake();                // any thread, any time; a wake that finds no wait ends the next one at once, once
```

`platform_wait_events` returns true when something is waiting for the next frame (events, a wake, a gamepad) and false when
the time ran out, a signal interrupted it (EINTR: look at your flag) or the backend has a deadline of its own that only
`window_begin_frame` serves (a key held, the repeats the Wayland backend makes itself, a drop whose source never answers):
call `window_begin_frame` after every return and loop, that is the whole contract. A wait with events already pending
returns at once. The click that was released in the frame of its press and the tap that turns into a hold count as pending
(the state that only the next `window_begin_frame` changes). Gamepads wake the wait on Linux (their descriptors are watched);
on Windows XInput cannot be waited on, so a plugged pad limits the sleep to 8 ms. On the web it returns at once (the browser
calls the frame); on Android it sleeps in the looper. `platform_wake` costs one `write(2)` and is safe in a signal handler
once the platform is up.

For programs that draw on demand `app_run_events` is `app_run` with this built in: the frame callback runs when events
arrived, `platform_wake` was called, the frame asked for another with `app_request_frame()` (an animation), or the time given
to `app_request_frame_at(time_seconds() + 0.5)` (a blinking caret) has come; the first frame always runs.

```c
static void frame(PlatformWindow *w, void *user)
{
    draw(w, user);
    if (animating(user))        app_request_frame();                              // again, now
    else if (caret_blinks(user)) app_request_frame_at(time_seconds() + 0.5);       // or at a deadline
}
app_run_events(w, frame, &state);   // idle: the process sleeps in poll(2)
```

Measured against GLFW and SDL2 in `docs/COMPARACAO_CORE.md` (instructions per wake and wait cycle, wake latency, idle CPU).

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
swipes and pinch in/out.

Touch points come from touch screens on Windows (WM_TOUCH), X11 (XInput 2.2, with
libXi opened at run time), Android and the web. On a desktop touch screen the first
finger also drives the mouse (left button, position), and the system's own mouse events
for it are dropped while a finger is down, so a touch is never counted twice. Without a
touch screen, `touch_set_mouse_emulation` turns the left button into one finger with a
real `EVENT_TOUCH`.

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
permission on them, normally the `input` group (and write permission for
vibration). Pads are detected at startup and on hot-plug. Only pads that follow the
kernel gamepad layout are gamepads; there is no mapping database.

```c
gamepad_rumble(0, 0.8f, 0.3f, 250);      // strong (low) motor, weak (high) motor, milliseconds; 0 ms stops

// Hot-plug as events, per window, including pads already connected at the first frame
case EVENT_GAMEPAD_CONNECTED:    /* e.data.device.index */ break;
case EVENT_GAMEPAD_DISCONNECTED: break;

// Everything that is not a standard gamepad: sticks, wheels, arcade encoders. Raw order, no mapping.
for (int i = 0; i < JOYSTICK_MAX; i++)
    if (joystick_connected(i))
    {
        float x = joystick_axis(i, 0);                  // -1..1
        bool fire = joystick_button(i, 0);
        int hat = joystick_hat(i, 0);                   // JOYHAT_UP | JOYHAT_RIGHT ...
        int pad = joystick_gamepad_index(i);            // its gamepad slot, or -1
    }
```

Windows reads pads through XInput (with vibration) and other joysticks through winmm.

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
  preedit.c               the text an input method is composing
  thread.c                threads, recursive mutexes, condition variables, cpu_count
  dialog.c                message boxes, file dialogs, open_url (native, zenity/kdialog, web)
  clipboard.c             clipboard API (text, MIME data, PNG images)
  png.c                   minimal PNG encoder and decoder for the clipboard
  clipboard_mem.h         in-process clipboard store for fake, Android and web
  web_key_rules.h         what the web backend decides about a key: clipboard command, text or not (no Emscripten in it)
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
  test_events.c           event hook, text and IME events, lock keys, capture, device and touch events (fake backend)
  test_preedit.c          the composition buffer an input method edits
  test_x11_window.c       capture, lock keys, window kinds, hit test, image cursors on a real X server
  test_win32_input.c      keys and mouse through SendInput: every key, both shifts, AltGr, Print Screen,
                          held keys across focus changes, buttons, wheel, drags, captured motion
  test_input_x11.c        keys and mouse on a real X server: every key and button in the frame the
                          server sends it, keys held together and through keyboard grabs, an input
                          method's copies and commits, dead keys, wheel, enter/leave, captured motion
  test_library.c          shared library loading
  test_thread.c           threads, mutex, condition, timeout, detach
  test_dialog.c           zenity/kdialog/xdg-open arguments and answers, with stand-in programs
  test_clipboard.c        clipboard API, the primary selection and the PNG round trip (fake backend)
  test_clipboard_x11.c    real X11 selections (CLIPBOARD and PRIMARY) against forked clients, INCR included
  test_web_keys.c         the web backend's key rules (clipboard keys, text with Ctrl or Meta) on the host
  tests/web/              the web clipboard and text keys in a real headless Chrome (run_clip_web.js, shell.html)
  test_multiwindow_x11.c  two windows on one X connection: events reach their own window
  test_vulkan.c           RENDER_VULKAN window, handles, a real VkSurfaceKHR (skips without a driver)
  test_draw2d.c           rasterizer tests (nearest + bilinear blit)
  test_fs.c               filesystem round-trip tests
  test_image.c            BMP read/write round-trip
examples/
  hello_x11.c             minimal GL example
  clipboard_web.c         the web backend's paste, copy and cut and its text keys, in a page (tests/web/run_clip_web.js)
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
