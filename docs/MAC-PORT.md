# macOS port: where it stands

Branch `mac-backend`. The backend is `src/backend_cocoa.m` (the only Objective-C file of the backend, manual retain/release, every
call that reaches AppKit in its own `@autoreleasepool`) and `src/dialog_cocoa.m` (the dialogs). There is no Mac among the development
machines: everything is compiled and run by `.github/workflows/mac.yml` on a GitHub `macos-latest` runner (macOS 26, Apple M1
virtual, one 1024x768 display of scale 1, an Aqua session where real windows work). The workflow starts only by hand
(`gh workflow run mac.yml --ref mac-backend`, then `gh run list --branch mac-backend` and `gh run view <id> --log`); a push makes no
run. A runner is sometimes not acquired for 15 minutes ("not acquired by Runner"): start it again.

**The owner stopped the use of runs after run 44160945847 (the last one made). Everything written after it was never compiled by
the real compiler.** The code was only checked for syntax against stand-in headers written from memory (a clang on Linux with
`-fsyntax-only`), which finds typos and C mistakes but proves nothing about Apple's actual signatures. The first thing to do with a
Mac, or with one allowed run, is to run the job once and fix what the compiler says.

## State of each phase

| Phase | What | State |
|---|---|---|
| 1, 2 | the library without a window; windows, events, monitors, live callback | compiled and run on macOS, reviewed |
| 3 | OpenGL (NSOpenGLContext, CGL worker context), the pixel surface, zen_2d and the examples | compiled and run at run 44160945847: `test_gl_cocoa` 153/153, `test_gl_worker_cocoa` 71/71, builds without warnings. `test_pixels_cocoa` and `test_ui_cocoa` failed only on a comparison that included the window's rounded bottom corners; fixed in commit 9d305f5, **not run again** |
| 4 | keyboard, mouse, cursors | compiled and run at run 44160945847. In `test_input_cocoa` the pass with events given to the view passed the whole key table (96 keys), buttons, drags, scroll (AppKit's deltas printed against the library's), warp, captured mode, cursors (all eleven) and 230 custom cursors; the 70 failures were test-side (typing keys sent with no character, the press queue not drained, two text edge cases) plus one backend fix (the text switch outside the pump, commit 129670e). Those fixes were **not run again** |
| 5 | text input (`NSTextInputClient`, preedit, caret rectangle) | **written, never compiled** (`test_text_cocoa`) |
| 6 | clipboard (`NSPasteboard`), file drop, native dialogs (`dialog_cocoa.m`) | **written, never compiled** (`test_clipboard_cocoa`, `test_drop_cocoa`, `test_dialog_cocoa`) |
| 7 | fullscreen and window modes, Dock icon, opacity, always on top, undecorated windows, window kinds, hit test, trackpad gestures | **written, never compiled** (`test_window_cocoa`) |
| 8 | Vulkan surface (`VK_EXT_metal_surface`, `CAMetalLayer`) | **written, never compiled** (`test_vulkan_cocoa`); gamepads stay the stub (`gamepad_stub.c`) |

Last macOS run: 44160945847 at commit b3ca21a (phases 1 to 4). Last fully green run: none (the Linux X11 and fake builds pass 34/34 and
23/23 at every commit).

## What the runner showed (run 44160945847)

* The GL stack on the runner is `Apple Software Renderer`, GL 4.1 core (`GL_VERSION 4.1 APPLE-23.1.1`), GLSL 4.10, depth 32, MSAA 4
  works. Core 3.2/3.3/4.1 requests give 4.1; compatibility 2.1 gives `2.1 APPLE-23.1.1`.
* `CGWindowListCreateImage` of the process's own windows works without a permission: what is presented (GL clear colours, the
  software surface, the toolkit's frames) was read back from the window server and compared. The window server's picture has the
  window's rounded bottom corners (macOS 26) in it, so the comparisons leave a 48 pixel square out at each bottom corner.
* The frames of `test_ui_cocoa` (artifact `ui-frames`, PNG files) are right side up, with the right colours (no red and blue swapped)
  and at the right size, for both renderers.
* AppKit does not deliver a key press posted to the application's queue for a typing key if the event has no characters (a real
  keyboard never sends one); the test now passes the character. Posting keys, mouse buttons and moves with `postEvent:atStart:`
  works, as does handing events straight to the view; `CGEventCreateScrollWheelEvent` gives scroll events for the view.
* `AppKit` reports scroll as lines (a notch is about 1.0) or points, the library maps points to a tenth.
* Windows made and destroyed with a GL context grow the heap by about 1.8 KB per round (plain windows 0.5 KB): under the test's
  limit, not looked into.

## Decisions that a later reader would ask about

* Core profile: a request from 3.2 to 4.1 gives a context made with `NSOpenGLProfileVersion3_2Core` (up to 3.2) or `4_1Core`;
  the version is checked afterwards with `glGetIntegerv`. Compatibility only up to 2.1. ES and `debug` contexts fail with the reason.
* The window's own GL context belongs to the main thread; `gl_worker_*` is a CGL context made with `CGLCreateContext(pixel
  format, share)`, with no AppKit in it, so any thread may use it.
* `RENDER_PIXELS`: the buffer is as big as the framebuffer (pixels), `0xAARRGGBB`, stride = width, drawn from the view's `drawRect:`
  as an sRGB `CGImage` from the top left corner without scaling; `window_present_pixels` does `[view display]`. Alpha is ignored.
* Key table built from `kVK_*` (`Carbon/Carbon.h`, header only; `EventType` is renamed around the include because Carbon has a typedef of
  that name). Command is the Super key; Insert is Help, Num Lock is Clear, Print Screen, Scroll Lock and Pause are F13 to F15. Menu key
  equivalents are AppKit's: Cmd+Q is the menu (a close request), every other Command shortcut arrives as a key. The release of a key
  pressed with Command held is handed to the key window by the pump; a release whose press never came is dropped.
* Text (phase 5): a key press (without Command or Control) goes to `interpretKeyEvents:`; the view is the `NSTextInputClient`. Commits
  are `EVENT_CHAR` (same filter as before), marked text is `EVENT_TEXT_EDIT` built with `preedit.c`, and a commit ends the composition with an
  empty edit first. `window_set_text_input_rect` is what `firstRectForCharacterRange:` answers.
* Clipboard (phase 6): the general pasteboard; text and PNG as their types, `text/uri-list` as file URLs (one pasteboard item each), other MIME
  types as pasteboard types of that name. Only PNG is read for images (a TIFF on the pasteboard is not). A drop is `EVENT_WINDOW_DROP`;
  the paths live in the window until the next drop. Dialogs are modal for the whole application; the filters of a file dialog become one list of
  extensions (the panel has no filter list); `dialog.c` leaves the dialogs to `dialog_cocoa.m` when the native backend is built
  (`ZEN_DIALOG_COCOA`) and `test_dialog` (stand-in tools) runs only for the fake backend.
* Window modes (phase 7): both fullscreen modes are a borderless window the size of the chosen monitor, above the menu bar, with the
  Dock and the menu bar hidden while there is one (no system Space, no display mode change). The icon is the application's (Dock). The hit test
  drags with `performWindowDragWithEvent:` and resizes by its own tracking of the drag events (AppKit has no call for an edge). Trackpad
  gestures are made-up touch points (two fingers around the pointer for pinch and rotate, one finger for a swipe) pushed as `EVENT_TOUCH`,
  not through `core_push_touch`, so the left button is never pressed.
* Vulkan (phase 8): `vulkan_instance_extensions` gives `VK_KHR_surface` and `VK_EXT_metal_surface`; the program adds
  `VK_KHR_portability_enumeration` and the flag for MoltenVK. The surface is a `CAMetalLayer` set as the layer of the (layer-hosting) view.
  `vulkan.c` also looks for the loader in the Homebrew folders.
* Wheel: a notch (line delta) is 1.0, a trackpad's points count ten to a notch, up and right are positive.
* Mouse modes: hidden is a blank cursor on the view's cursor rect; captured is `CGAssociateMouseAndMouseCursorPosition(false)` with the
  pointer warped to the middle, deltas from the events, a virtual position, given back on focus loss and on destroy. `mouse_capture` is a
  global monitor (mouse events that go to other applications).
* Cursors: public AppKit cursors for every `CURSOR_*`; the two diagonal resize ones are public only from macOS 15
  (`frameResizeCursorFromPosition:inDirections:`, before it the crosshair), move-all is the open hand.

## Not verified (no way to on the runner)

A scale 2 display, the scale-change event, a drag with a real mouse, the pointer held by the captured mode with a real mouse, a trackpad,
a real keyboard and input method, held keys at focus gain (`CGEventSourceKeyState` may need a permission), the global monitor of
`mouse_capture`, the look of the diagonal resize cursors, a real drag of files from the Finder, the system's drag of a window by a
hit-test title bar, MoltenVK (not installed on the runner).

## Next steps

1. Run the job once (`gh workflow run mac.yml --ref mac-backend`) and fix what the compiler says: all of phases 5 to 8, the new tests,
   `dialog_cocoa.m`, the `-no_warn_duplicate_libraries` link option, the `QuartzCore` link. Likely trouble spots: the `NSCursorFrameResize*`
   names (phase 4), `NSPasteboard*` constants, the `NSTextInputClient` signatures, `NSWindowStyleMask*`, the `-Wunguarded-availability` of anything
   newer than 11.0, a name that collides with Carbon, QuartzCore or Metal headers.
2. Then the failures of the new tests (written without ever running; the gesture tests hold only to the touch points, the module's own thresholds
   are printed as notes).
3. Look at the growth of the heap in the GL create/destroy rounds with `leaks` (add the GL test to the "leaks report" step).
