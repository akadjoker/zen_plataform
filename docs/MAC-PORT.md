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

On 2026-11-18 the branch was prepared for exactly one run: `main` (beb1db0) merged in, everything never compiled desk-checked, the
workflow made to report everything at once. "Preparation for the one run" below says what was done and with what proof, and "Reading
the run" is the checklist for its logs.

## State of each phase

| Phase | What | State |
|---|---|---|
| 1, 2 | the library without a window; windows, events, monitors, live callback | compiled and run on macOS, reviewed |
| 3 | OpenGL (NSOpenGLContext, CGL worker context), the pixel surface, zen_2d and the examples | compiled and run at run 44160945847: `test_gl_cocoa` 153/153, `test_gl_worker_cocoa` 71/71, builds without warnings. `test_pixels_cocoa` and `test_ui_cocoa` failed only on a comparison that included the window's rounded bottom corners; fixed in commit 9d305f5, **not run again** |
| 4 | keyboard, mouse, cursors | compiled and run at run 44160945847. In `test_input_cocoa` the pass with events given to the view passed the whole key table (96 keys), buttons, drags, scroll (AppKit's deltas printed against the library's), warp, captured mode, cursors (all eleven) and 230 custom cursors; the 70 failures were test-side (typing keys sent with no character, the press queue not drained, two text edge cases) plus one backend fix (the text switch outside the pump, commit 129670e). Those fixes were **not run again** |
| 5 | text input (`NSTextInputClient`, preedit, caret rectangle) | **written, never compiled**, desk-checked (`test_text_cocoa`) |
| 6 | clipboard (`NSPasteboard`), file drop, native dialogs (`dialog_cocoa.m`) | **written, never compiled**, desk-checked (`test_clipboard_cocoa`, `test_drop_cocoa`, `test_dialog_cocoa`) |
| 7 | fullscreen and window modes, Dock icon, opacity, always on top, undecorated windows, window kinds, hit test, trackpad gestures | **written, never compiled**, desk-checked (`test_window_cocoa`) |
| 8 | Vulkan surface (`VK_EXT_metal_surface`, `CAMetalLayer`) | **written, never compiled**, desk-checked (`test_vulkan_cocoa`); gamepads stay the stub (`gamepad_stub.c`) |

Last macOS run: 44160945847 at commit b3ca21a (phases 1 to 4). Last fully green run: none (the Linux X11 and fake builds pass 34/34 and
23/23 at every commit, merge of main included).

## Preparation for the one run (2026-11-18)

### The merge of main (beb1db0)

A merge commit (the branch was pushed). No text conflicts: `include/platform.h`, `src/thread.c`, `src/crash.c`, `src/crash_internal.h`,
`tests/test_crash.c`, `tests/test_gl_worker_x11.c` and `tests/test_thread.c` came from main. The one conflict of meaning: main renamed the
thread, mutex and condition variable functions verb first with no aliases (`thread_create` is now `create_thread`, and so on, 15 functions;
the old name collided with Mach's `thread_create` in `<mach/task.h>`, which Cocoa includes), and `tests/test_gl_worker_cocoa.c` still
called `thread_create` and `thread_join` six times: on a Mac that file (no Apple header in it) would have failed with "call to undeclared
function", and a file that did include Cocoa would have been given Mach's function. Renamed. The `#define thread_create
zen_platform_thread_create` workaround is gone from `backend_cocoa.m`, `dialog_cocoa.m`, `test_cocoa_window.c` and `cocoa_test_util.h`
(and with it `COCOA_TEST_USES_THREADS`); `platform.h` is now included after the Apple headers in all of them. Nothing named `thread_create`,
`mutex_*`, `cond_*` is left in the tree (a grep of the whole tree is the check; `pthread_create` is the system's).

### The compile test for names (`tests/test_cocoa_names.m`)

`platform.h` and the library's internal headers are included **after** Foundation, Cocoa, ApplicationServices, Carbon (with `EventType`
renamed around it, as `backend_cocoa.m` does), OpenGL, QuartzCore, Metal, CoreVideo, IOKit, GameController, UniformTypeIdentifiers and the
Mach headers, in an Objective-C file, and the program creates a thread, takes a mutex and waits on a condition variable of the library. A
name that a system header already has is a compile error in this one small file, which says the header and the name. It is built on every
macOS (the native backend and the fake one; `enable_language(OBJC)` moved to the top `CMakeLists.txt` for that) and is ctest `test_cocoa_names`.
Metal, CoreVideo, IOKit, GameController and UniformTypeIdentifiers are included only to catch the names of a future gamepad or Metal backend
early: if one of them fails for a reason that has nothing to do with the library, drop that line.

### The desk check: how, and what it found

How: every line of `backend_cocoa.m`, `dialog_cocoa.m` and the cocoa tests was read as the compiler and as an AppKit reviewer. Then, since
no Apple SDK is on the development machine and no GNUstep is installed, stand-in headers were written for the AppKit, Foundation,
CoreFoundation, CoreGraphics, QuartzCore, OpenGL, Carbon and Objective-C runtime declarations the code uses (the selectors and constants as
Apple spells them, from memory and from the documentation), and `clang -fsyntax-only -Wall -Wextra -Wshadow -Wmissing-prototypes` (the
Objective-C files as `-fobjc-runtime=macosx -fno-objc-arc -fblocks`) was run on `backend_cocoa.m`, `dialog_cocoa.m`, `test_cocoa_names.m` and
all twelve cocoa tests, against the **real** `platform.h` and internal headers: no error, no warning. A planted wrong selector and a planted
wrong constant were both reported, so the check does see. That proves the consistency of the code with the library's own headers (field
names, enum names, prototypes, arities, `static` and `extern`, format strings, missing returns, the 11 required methods of `NSTextInputClient`),
**not** the spelling of Apple's API: that was read against Apple's documentation (below). The object files of the library as the Mac build
lists them (`CORE_SRC`, the POSIX files, `backend_cocoa.m`, `dialog_cocoa.m` with `ZEN_DIALOG_COCOA`) were also linked into one relocatable
object on Linux: every undefined symbol left is libc or an Apple API of Cocoa, CoreGraphics, OpenGL (CGL) or QuartzCore, which are all in the
link list; every function of `backend.h` is defined in `backend_cocoa.m`. The deployment target is 11.0 and nothing newer is called
unguarded except `frameResizeCursorFromPosition:` (guarded by `@available(macOS 15.0, *)`); `-Werror` is not used.

What it found (none was a compile error: the code was consistent with the headers it was written against; what follows is what AppKit
would have done):

| where | what | fix |
|---|---|---|
| `tests/test_gl_worker_cocoa.c:171,185,230,239,268,279` | the merge: calls of `thread_create` and `thread_join`, names that no longer exist | `create_thread`, `join_thread` |
| `src/thread.c:318` | the merge: a comment that sent the reader to `cond_wait_timeout` | `wait_cond_timeout` |
| `src/backend_cocoa.m` `backend_create`, `ZenPanel` windows | an `NSPanel` has `hidesOnDeactivate` true by default (Apple: false for `NSWindow`, true for `NSPanel`): a utility, popup or tooltip window left the screen whenever the application was not the active one, with no event to the program, and `window_is_visible` was false | `setHidesOnDeactivate:NO` |
| `src/backend_cocoa.m` `backend_destroy` | the parent of a window was cleared (`w->parent = NULL`) but the window stayed an AppKit child of the destroyed window, and `orderOut:` of the parent takes its children off the screen: a dialog, utility, popup or tooltip the program still had vanished with its parent (not so on X11) | `removeChildWindow:` first, `attached` false |
| `src/backend_cocoa.m` `ZenView` | a Vulkan surface makes a `CAMetalLayer` the view's hosted layer (`setLayer:` then `setWantsLayer:`); nothing resized it with the window (a hosted layer's geometry is its owner's), so the swapchain and `test_vulkan_cocoa` would have seen the first size forever | `setFrameSize:` sets the layer's frame from the view's bounds, with implicit actions off |
| `src/backend_cocoa.m` `ZenView` | declared only `NSTextInputClient` while implementing three `NSDraggingDestination` methods | declares both (`NSView` adopts the second already) |
| `src/dialog_cocoa.m` `bring_up` | a dialog before `platform_init` (a program saying why it could not start): the process is a background one until its activation policy is set (the alert is behind everything and takes no keys), and `NSApplication` ends the process when there is no window server, which `backend_init` guards against and the dialogs did not | asks `CGSessionCopyCurrentDictionary` first (error "no graphical session"), sets the Regular policy, activates |
| `tests/test_dialog_cocoa.c` | the timer that ends the modal session (`abortModal`, which Apple documents for a timer on the modal run loop) fired once at 0.6 s; a dialog not yet in its modal loop (the open and save panels are made by a service of their own) was left waiting for a person until the test's 90 s alarm | repeats every 0.5 s until the dialog returns |
| `tests/` (all cocoa tests) | stdout is a pipe under ctest, so it is block buffered: a test stopped by its alarm, ctest's timeout or a crash left none of what it had printed | `tests/test_boot.h`: line buffered stdout, and a SIGALRM handler that says "TIMEOUT" and exits 124 |

### Looked up in Apple's documentation, and where it left doubt

Confirmed (existence, spelling, availability): `abortModal` (stops `runModal`; "use when responding to an NSTimer added to the modal panel run
loop mode"; the loop returns `NSModalResponseAbort`), the 11 required methods of `NSTextInputClient` (all implemented), `NSPasteboardTypeFileURL`
(10.13), `NSPasteboardURLReadingFileURLsOnlyKey` and `readObjectsForClasses:options:` (10.6), `performWindowDragWithEvent:` (10.11, "returns right
away, and a mouse-up event may not get sent": handled), `NSWindowStyleMaskUtilityWindow` and `NSWindowStyleMaskNonactivatingPanel` (panel styles; the
panel classes carry them), `CAMetalLayer` (10.11), `setAllowedFileTypes:` (deprecated in 12.0, present; an empty array raises, the code never
passes one), `discardMarkedText` ("the client should clear its marked range when sending this message": it does), `hidesOnDeactivate`.

Doubt left by the documentation: (1) whether `HideDock | HideMenuBar` is a valid combination is not stated on the constants' pages (setting an
invalid one raises `NSInvalidArgumentException`); it is the combination of Apple's own kiosk examples, and the test reads it back. (2) Whether
a plain `NSWindow` with `NonactivatingPanel` raises is not stated; no `NSWindow` gets that mask here (popups are panels). (3) The sign of
`-[NSEvent rotation]` for a trackpad rotation: the page of the property documents only the tablet use; the code takes degrees,
counterclockwise positive (the convention of `rotateWithEvent:` in other code), and `test_window_cocoa` fails if that is wrong. (4) Whether
AppKit sizes the layer of a layer-hosting view (the page of `NSView.layer` says nothing): the code does not rely on it. (5) What happens to child
windows when the parent is ordered out is not documented in the pages read; the fix above relies on the usual behaviour (they go with it).
(6) `-[NSURL fileSystemRepresentation]` returns the decomposed (NFD) form of a name; `drop_files` and the open panel use it on purpose (it is
what the file system calls take), and `test_drop_cocoa` compares a path with a composed e-acute (U+00E9) byte for byte, so that check may fail on the
accent alone (see below).

### What could not be verified here

Everything that needs the real SDK and the window server: that the stand-in declarations are what Apple declares (the documentation
lookups above are the only evidence), the `-Wunguarded-availability` and deprecation warnings (the check was by reading: only
`frameResizeCursorFromPosition:` is newer than 11.0, and `activateIgnoringOtherApps:`, `NSWorkspace openURL:` and `setAllowedFileTypes:`
are the deprecated ones, the last two silenced), the link with ld (the `-no_warn_duplicate_libraries` option exists from Xcode 15's linker),
how AppKit behaves for any of phases 5 to 8, and the new workflow (YAML read with PyYAML, each shell block through `bash -n`, `tools/mac_ci.sh`
run on Linux on a project with files that do not compile, a warning, a failing test and a hung one; the GitHub-only parts, such as
`actions/upload-artifact`, `$GITHUB_STEP_SUMMARY`, `brew install molten-vk vulkan-loader`, `screencapture`, were not).

Linux proof that nothing else broke (merged branch, private Xvfb, redirected HOME and XDG): X11 Release 34/34, X11 ASan + UBSan 34/34, fake
(ASan + UBSan, `-Wall -Wextra -Wpedantic`) 23/23, MinGW cross-compile with `-Wall -Wextra` no warnings (nothing run). The warnings of the
fake build are the ones main has (stb headers, `vulkan.c` pedantic function pointer casts, `test_crash.c` infinite recursion).

## Reading the run

Start the job by hand (`gh workflow run mac.yml --ref mac-backend`). The job summary (Actions page) lists, per configuration, the distinct
error lines of the build and the failed tests. The artifact `logs` holds everything: `machine.txt` (image, Xcode, SDK, session),
`<config>.configure.log`, `<config>.build.log` (all the compile commands, `VERBOSE=1`: look for `-mmacosx-version-min=11.0`, the SDK path and
`-fno-objc-arc` on the two `.m` files), `<config>.build-errors.log` (the errors again, one file at a time), `<config>.warnings.txt`,
`<config>.ctest.log` (every test's whole output), `<config>.tests.txt` (one line per test), `<config>.LastTest.log`, `leaks.log`,
`crash-reports/`, `desktop-at-the-end.png`, `test-images/`. The configurations are `fake`, `native`, `native-asan`, `fake-asan`. In this
order:

1. **`machine.txt`**: the runner image, Xcode and SDK versions (a SDK older than 15 means `frameResizeCursorFromPosition:` does not exist
   and `native.build.log` will say so), `GUI session: Aqua`, a `WindowServer` process.
2. **`fake.build.log`, `fake.ctest.log`**: the merge. Errors here are the thread renames or the new `test_cocoa_names` target (the
   `tests/CMakeLists.txt` block, `enable_language(OBJC)` at the top). `test_cocoa_names` also tells whether the fake build links Cocoa.
3. **`native.build.log` and `native.build-errors.log`**, in this order of likelihood: `backend_cocoa.m` phases 5 to 8, `dialog_cocoa.m`, the
   new tests; the link of any test (`Undefined symbols`, `-no_warn_duplicate_libraries`); the warnings in `native.warnings.txt` (what to
   look for: `-Wunguarded-availability`, `-Wdeprecated-declarations`, `-Wincompatible-pointer-types` on a protocol method).
4. **`test_cocoa_names`** (the step "names against Apple's headers"): an error names the header and the library's identifier; the
   workaround, if one is unavoidable, is a `#define` around that one header as for `EventType`.
5. **Phase 3 and 4 tests** (`test_gl_cocoa`, `test_gl_worker_cocoa`, `test_pixels_cocoa`, `test_ui_cocoa`, `test_input_cocoa`,
   `test_cocoa_window`): they passed or failed only on test-side points at run 44160945847; the fixes are the rounded corner of the window
   in the image comparison (`image_diff_corners`, `WINDOW_CORNER` 48), the typing keys with a character, the press queue drained, two text edge cases.
   A failure here is a fix that was wrong. `test-images/` and the artifact `ui-frames` are the frames to look at.
6. **Phase 5** `test_text_cocoa`: the `commit`, `composition`, `text input off`, `focus loss`, `caret rectangle`, `destroyed while composing`
   headings print as they go. `caret rectangle` prints the rectangle as the screen has it against the window position: compare by eye if a
   check fails.
7. **Phase 6** `test_clipboard_cocoa` (a failure naming "public.png" or a custom type: the type strings of the pasteboard; "/tmp" in the URI
   list: `/tmp` is a link to `/private/tmp`), `test_drop_cocoa` (the accent: the composed e-acute (U+00E9) of the test path against `fileSystemRepresentation`'s
   decomposed form, and the same in the open panel), `test_dialog_cocoa`: each dialog is opened and aborted by a repeating timer; look for
   `confirm_box ended by the abort: 0`, a `TIMEOUT` line (the panels' service did not come up) and `desktop-at-the-end.png` for a dialog
   left open. A dialog that appears and is never ended is the one thing that can hold a test until its alarm (90 s).
8. **Phase 7** `test_window_cocoa` (the heading before each failure says `modes`, `looks`, `kinds`, `hit test`, `gestures`): `mode N: window WxH at X,Y (monitor ...)`
   against the monitor; the readings of `presentationOptions` (10) and the `level` (25 above the menu, 3 floating, 101 popup); the
   popup/tooltip `canBecomeKeyWindow`; after a hit-test drag the window size against `+40,+30`; `pinch out: ... the gesture module saw ...`
   and `rotation by 30 degrees: the line of the fingers is at -30` (the sign of `rotation` is the doubt above); the swipe notes (the gesture
   module's own thresholds, a note and not a failure).
9. **Phase 8** `test_vulkan_cocoa`: `extensions` and `refused` need no loader. If the Homebrew step installed MoltenVK, `a real surface`
   prints `vulkan_create_surface: made` and the layer's class, bounds and scale (the runner may have no GPU: then `vkCreateInstance failed`
   or "no Vulkan loader" is a note and not a failure).
10. **`native-asan`**: the same tests under ASan + UBSan (`detect_leaks=0`): a report here is a real memory error. `leaks.log` is information
    only. `crash-reports/` holds the system's report of any test that crashed.
11. **The verdict**: a red step names the configuration; the summary has the first 40 errors and the failed tests. `Native backend, Release,
    every test` runs all tests even when some did not build (they show as "Not Run").

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

1. Start the job once (`gh workflow run mac.yml --ref mac-backend`), read it as "Reading the run" says, and fix what the compiler says: all of
   phases 5 to 8, the new tests, `dialog_cocoa.m`, the `-no_warn_duplicate_libraries` link option, the `QuartzCore` link. Likely trouble spots
   not already named: the `NSCursorFrameResize*` names (phase 4, compiled), `NSPasteboard*` constants, the `NSTextInputClient` signatures,
   `NSWindowStyleMask*`, the `-Wunguarded-availability` of anything newer than 11.0, a name that collides with Carbon, QuartzCore or Metal
   headers (`test_cocoa_names` says it first).
2. Then the failures of the new tests (written without ever running; the gesture tests hold only to the touch points, the module's own thresholds
   are printed as notes).
3. Look at the growth of the heap in the GL create/destroy rounds with `leaks` (the job already runs it on `test_cocoa_window`; add the GL test).
