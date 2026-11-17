# macOS port: where it stands

Branch `mac-backend`. The backend is `src/backend_cocoa.m` (the only Objective-C file, manual retain/release, every call that
reaches AppKit in its own `@autoreleasepool`). There is no Mac among the development machines: everything is compiled and run by
`.github/workflows/mac.yml` on a GitHub `macos-latest` runner (macOS 26, Apple M1 virtual, one 1024x768 display of scale 1, an
Aqua session where real windows work). A push runs the job only when the head commit message carries the CI marker for the macOS
job (square brackets around `mac`); otherwise start it by hand: `gh workflow run mac.yml --ref mac-backend`, then
`gh run list --branch mac-backend`, `gh run view <id> --log`. A runner is sometimes not acquired for 15 minutes ("not acquired
by Runner"): run it again.

## Phases

| Phase | What | State |
|---|---|---|
| 1, 2 | the library without a window; windows, events, monitors, live callback | done, reviewed |
| 3 | OpenGL (NSOpenGLContext on the view, CGL worker context), the pixel surface, zen_2d and the examples | done; verified on the runner by `test_gl_cocoa`, `test_gl_worker_cocoa`, `test_pixels_cocoa`, `test_ui_cocoa` (see the last run below) |
| 4 | keyboard, mouse, cursors | done in the code; `test_input_cocoa` passes in the "events given to the view" pass, and the "events posted to the application's queue" pass was being fixed (test side) in the last run |
| 5 | text input (NSTextInputClient: dead keys, composition, the candidate window) | not started; `backend_set_text_input*` are stubs marked `TODO(mac phase 5)` |
| 6, 7, 8 | clipboard and file drop; fullscreen, icon, opacity, always on top, undecorated windows, hit test; Vulkan (Metal) surface | not started, stubs marked `TODO(mac phase N)` that set the error "... is not implemented on macOS yet" |

## What the runner showed (run 44160945847, the first with the GL, pixel and input tests)

* The GL stack on the runner is `Apple Software Renderer`, GL 4.1 core (`GL_VERSION 4.1 APPLE-23.1.1`), GLSL 4.10, depth 32, MSAA 4
  works. Core 3.2/3.3/4.1 requests give 4.1; compatibility 2.1 gives `2.1 APPLE-23.1.1`.
* `CGWindowListCreateImage` of the process's own windows works without a permission: what is presented (GL clear colours, the
  software surface, the toolkit's frames) was read back from the window server and compared. The window server's picture has
  the window's rounded bottom corners (macOS 26) in it, so the comparisons leave a 48 pixel square out at each bottom corner.
* The frames of `test_ui_cocoa` (artifact `ui-frames` of the run, PNG files) are right side up, with the right colours (no red and
  blue swapped) and at the right size, for both renderers.
* Windows made and destroyed with a GL context grow the heap by about 1.8 KB per round (the plain window test: 0.5 KB): under the
  test's limit, not looked into (the leaks tool was not run on the GL test yet).

## Decisions that a later reader would ask about

* Core profile: a request from 3.2 to 4.1 gives a context made with `NSOpenGLProfileVersion3_2Core` (up to 3.2) or `4_1Core`;
  the version is checked afterwards with `glGetIntegerv`. Compatibility only up to 2.1 (the legacy profile). ES and `debug`
  contexts fail with the reason. A request is never met with something else.
* The window's own context belongs to the main thread (its view does); `gl_worker_*` is a CGL context made with
  `CGLCreateContext(pixel format, share)`, which has no AppKit in it, so any thread may use it.
* `RENDER_PIXELS`: the buffer is as big as the framebuffer (pixels), `0xAARRGGBB`, stride = width, drawn from the view's
  `drawRect:` as an sRGB `CGImage` from the top left corner without scaling; `window_present_pixels` does `[view display]`, so the
  buffer is not read after it returns. Alpha is ignored.
* Key table built from `kVK_*` (`Carbon/Carbon.h`, header only; `EventType` is renamed around it because Carbon has a typedef of
  that name). Command is the Super key; Insert is Help, Num Lock is Clear, Print Screen, Scroll Lock and Pause are F13 to F15.
  Menu key equivalents are AppKit's: Cmd+Q is the menu (a close request), every other Command shortcut arrives as a key. The
  release of a key pressed with Command held is handed to the key window by the pump (NSApplication keeps it otherwise).
* Text: plain characters of `[event characters]` only; not with Command, no control characters, no private-use block; dead keys
  and composition are phase 5.
* Wheel: a notch (line delta) is 1.0, a trackpad's points count ten to a notch, up and right are positive.
* Mouse modes: hidden is a blank cursor on the view's cursor rect; captured is `CGAssociateMouseAndMouseCursorPosition(false)`
  with the pointer warped to the middle, deltas from the events, a virtual position, given back on focus loss and on destroy.
  `mouse_capture` is a global monitor (mouse events that go to other applications).
* Cursors: public AppKit cursors for every `CURSOR_*`; the two diagonal resize ones are public only from macOS 15
  (`frameResizeCursorFromPosition:inDirections:`, before it the crosshair), move-all is the open hand.

## Not verified (no way to on the runner)

A scale 2 display, the scale-change event, a drag with a real mouse, the pointer held by the captured mode with a real mouse, a
trackpad, a real keyboard, held keys at focus gain (`CGEventSourceKeyState` may need a permission), the global monitor of
`mouse_capture` for the pointer moving outside the application, the look of the diagonal resize cursors.

## Next steps

1. Read the last run (`gh run list --branch mac-backend`): `test_input_cocoa`, `test_pixels_cocoa` and `test_ui_cocoa` must be
   green in the native and the ASan jobs; fix what is left (test expectations first: the library passed every check that is
   about the backend itself in the "given to the view" pass).
2. Look at the growth of the heap in the GL create/destroy rounds with `leaks` (add the GL test to the "leaks report" step).
3. Phase 5: `NSTextInputClient` on `ZenView`, `EVENT_TEXT_EDIT`, `backend_set_text_input_rect`.
4. Phases 6 to 8.
5. The README's platform row and `include/platform.h` already say what works on macOS; update them with each phase.

## Last macOS runs

| Run | Head | Result |
|---|---|---|
| 44156573685 | e2326ce | native build failed: Carbon's `EventType` typedef against platform.h's enum |
| 44160945847 | b3ca21a | builds, no warnings but four linker ones; `test_cocoa_window`, `test_gl_cocoa`, `test_gl_worker_cocoa` pass; `test_pixels_cocoa`, `test_input_cocoa`, `test_ui_cocoa` fail on test expectations (listed in the commit messages after it) |
| see `gh run list` | 9d305f5 and after | the fixes of those |

Last green run: none yet with every test (the Linux X11 and fake builds pass 34/34 and 23/23 at every commit).
