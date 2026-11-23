# macOS port: where it stands

Branch `mac-backend`. The backend is `src/backend_cocoa.m` (the only Objective-C file of the backend, manual retain/release, every
call that reaches AppKit in its own `@autoreleasepool`) and `src/dialog_cocoa.m` (the dialogs). There is no Mac among the development
machines: everything is compiled and run by `.github/workflows/mac.yml` on a GitHub `macos-latest` runner (macOS 26, Apple M1
virtual, one 1024x768 display of scale 1, an Aqua session where real windows work). The workflow starts only by hand
(`gh workflow run mac.yml --ref mac-backend`, then `gh run list --branch mac-backend` and `gh run view <id> --log`); a push makes no
run. A runner is sometimes not acquired for 15 minutes ("not acquired by Runner"): start it again.

**The owner allowed few runs.** Run 44160945847 (commit b3ca21a) compiled and ran phases 1 to 4. Run 44285580324 (commit 3fca0cd, 2026-11-18,
macOS 26.7.2, Xcode 26.6, SDK 26.5) was the single run for everything written after it, phases 5 to 8, and the merge of `main`: **everything
compiled, with no error and no warning of our own, in the four configurations** (the code had been desk-checked against stand-in
headers before it, see "Preparation for the one run"), and 4 of the 35 tests failed in each native configuration, three of them in both.
"What run 44285580324 showed" below has the facts, the cause and the fix of each failure, and what the next run has to confirm. **The
fixes made after that run (commits 52506da, 1525b9e, 6568c9b, badd284 and the CI changes) were not run, and the next run may be the only one:
where a check gave too little information it now prints the values.**

## The first run on a real Mac (2026-11-23)

A borrowed MacBook Pro (Apple M3 Pro, macOS 15.7.3, command line tools with Apple clang 17, CMake 4.4.5), one built-in display of
1512x982 points at scale 2 (3024x1964 pixels), wide gamut (Display P3), 120 Hz: the first display of scale 2 and the first that is not
sRGB. `tools/mac_ci.sh` was run by hand, with a person at the trackpad for the parts no test can make (the pointer, the keys, files
dragged from the Finder). Everything compiled with no error and no warning of our own. What it showed, and what was done:

| what | seen | done |
|---|---|---|
| colours of an OpenGL window | a window with no colour space is taken to be in the display's: 0,122,204 drawn was 0,124,210 on the screen (brought back to sRGB); `test_gl_cocoa` "presented FF30A050" within 48 of a channel, `test_ui_cocoa` gl 15385 pixels | the window's colour space is sRGB (commit 417329f): within 0, 0 pixels |
| the first row of the content | AppKit draws the title bar's separator over it: a white frame had its first pixel row D9D9D9 and the second F2F2F2 (macOS 15, scale 2) | `setTitlebarSeparatorStyle:None` (417329f): `test_ui_cocoa` 0 pixels differ for both renderers |
| an image copied by another program | a pasteboard with TIFF or JPEG and no PNG (what Preview and Safari put there) read as "the clipboard has no image" | `image/png` stands for the other image types too, converted when read (417329f) |
| Vulkan with no loader | `vulkan_create_surface` failed with an empty error | "the Vulkan loader was not found" (417329f): `test_vulkan_cocoa` 10/10 |
| files dragged from the Finder by hand | the paths were right (accents, an emoji, a folder with a space), but `mouse_position()` at `EVENT_WINDOW_DROP` was the pointer's place before the drag in four drops of five | the drag's place is pointer movement (603baf3); seven drops by hand afterwards all inside the window; `test_drop_cocoa` checks it |
| the merge of `main` | `backend_wait_events`, `backend_wake` and the primary selection did not exist here: no link | the wait is AppKit's own (`nextEventMatchingMask` with `dequeue:NO`), a wake is a write to the core's descriptor that the main run loop watches, there is no primary selection; `test_wait_cocoa` (595 checks: timeouts, idle CPU 0.2 ms in 2 s, 10000 wakes from 4 threads, an event posted from another thread, a signal whose handler wakes) |
| a child whose program cannot be run | `fprintf(stderr)` in the forked child never came back when a thread of the parent held stderr's lock (glibc gives the child fresh locks, macOS does not): `test_process_hostile.c:153` | `write(2)` in the child |

By hand, with the clipboard of the system (`pbcopy`, `pbpaste`, AppleScript): text both ways (accents, CJK, an emoji; 20 MB with the
same hash), PNG both ways pixel for pixel (alpha, 1x1, 4000x3000), copied files, HTML and a MIME type of our own. Real input: 558 pointer
moves, 22 button events, 48 keys and 16 characters, the first click on a window without the focus, a drag that leaves the window.

**All green on this machine afterwards**: fake 29/29 and native 41/41, Release and under ASan + UBSan. The three tests that still
failed were the tests' own and were changed: the comparison with the window server's picture takes its tolerance from the display
(`server_tolerance` in `tests/cocoa_test_util.h`: 2 on an sRGB display, 10 on any other; measured here over 1.4 million pixels of
every hue, 93% within 1 and 9 at the worst, FF04EBFF back as FF0DEAFF), and the count of heap blocks is printed and no longer checked
in `test_cocoa_window` and `test_gl_cocoa` (`blocks_in_use` grew 60 to 90 a round while the bytes did not, and the system's heap tool
counted 29856 nodes after 130 rounds of windows and 27138 after 630, no class of ours growing). The leaks tool, on the twelve
windowed tests signed so that it may look: 0 leaks in each, once two objects the tests themselves forgot were released.

**The loops, timed** (Release, this machine): a frame of a pixel window of 1600x1200 pixels is 0.5 ms of `draw_clear` and 0.5 ms of
`window_present_pixels`; `window_swap` without vsync 0.3 ms, with it 8.3 ms (120 Hz); `window_begin_frame` with nothing waiting 3 to
4 us; no heap growth over 500 frames in any of them; `window_get_size`, `mouse_position`, `key_down` and the like 0.001 to 0.16 us a
call. A mouse event costs 22 to 97 us in `window_begin_frame` (24 us each in a stream of 48400): the profile has it in AppKit's own
pass of the run loop for every event taken (`updateWindows`, the Core Animation flush) and in its tracking areas, none of it in the
backend's handlers.

**Known and left as they are.** A program
that copies several files and ends at once leaves only the first on the pasteboard (alive for 3 s, all of them stay). Text that is not
UTF-8 leaves the clipboard empty. **Not tried**: the trackpad's scroll and gestures by hand (no event came in the minute the window
was open), a real Vulkan surface (no MoltenVK), a change of scale or a second display, gamepads (the stub).

## State of each phase

| Phase | What | State |
|---|---|---|
| 1, 2 | the library without a window; windows, events, monitors, live callback | compiled and run on macOS, reviewed |
| 3 | OpenGL (NSOpenGLContext, CGL worker context), the pixel surface, zen_2d and the examples | compiled and run at run 44160945847: `test_gl_cocoa` 153/153, `test_gl_worker_cocoa` 71/71, builds without warnings. `test_pixels_cocoa` and `test_ui_cocoa` failed only on a comparison that included the window's rounded bottom corners; fixed in commit 9d305f5, **not run again** |
| 4 | keyboard, mouse, cursors | compiled and run at run 44160945847. In `test_input_cocoa` the pass with events given to the view passed the whole key table (96 keys), buttons, drags, scroll (AppKit's deltas printed against the library's), warp, captured mode, cursors (all eleven) and 230 custom cursors; the 70 failures were test-side (typing keys sent with no character, the press queue not drained, two text edge cases) plus one backend fix (the text switch outside the pump, commit 129670e). Those fixes were **not run again** |
| 5 | text input (`NSTextInputClient`, preedit, caret rectangle) | **compiled and run at run 44285580324**: `test_text_cocoa` 81/81 (Release and ASan): commits of text, composition (marked text) with the caret in bytes, the 63-byte cut, the switch off, focus loss, the caret rectangle in screen coordinates, 20 windows destroyed while composing. The input context is driven the way AppKit drives it; a real input method was not started |
| 6 | clipboard (`NSPasteboard`), file drop, native dialogs (`dialog_cocoa.m`) | **compiled and run**: dialogs `test_dialog_cocoa` 20/20 (each dialog opens and is ended by the abort timer; a cancel is no error; a person's choice was not tried). Clipboard: text, PNG round trip and the URI list passed (36 checks); the custom MIME type **failed** (NSPasteboard takes only UTIs, 5 checks), fixed in 1525b9e, **not run again**. Drop: 108 of 109; a name with a composed accent **failed**, fixed in 6568c9b, **not run again** |
| 7 | fullscreen and window modes, Dock icon, opacity, always on top, undecorated windows, window kinds, hit test, trackpad gestures | **compiled and run**: `test_window_cocoa` 100/102. Proven: both fullscreen modes (1024x768 at 0,0 on a 1024x768 monitor, style, level 25, presentation options 10, the way back), opacity, always on top, decorations on and off, the icon, dialog/utility parents, the hit test (an edge drag of +40,+30 gives 340x230 from 300x200, limits hold), gestures from made-up events (pinch 160 to 594 points, rotation -30.0 degrees, four swipes). **Failed**: the level of a popup and of a tooltip (AppKit resets a child window's level to its parent's), fixed in 52506da, **not run again** |
| 8 | Vulkan surface (`VK_EXT_metal_surface`, `CAMetalLayer`) | **compiled and run**: `test_vulkan_cocoa` 18/18 (Release and ASan); the job installed the Vulkan loader and MoltenVK with Homebrew, a real instance and surface were made, the view's layer is a `CAMetalLayer` with `wantsLayer`, its size and scale checks and the resize passed; gamepads stay the stub (`gamepad_stub.c`) |

Last macOS run: 44285580324 at commit 3fca0cd (all phases). Last fully green run: none (fake 24/24 in both configurations, native Release
31/35, native ASan 32/35). The Linux X11 and fake builds pass 34/34 and 23/23 at every commit, the fixes after the run included.

## What run 44285580324 showed

Machine: runner image `macos26` 20261112.0504.1, macOS 26.7.2 (25G332), Apple M1 (Virtual) with 3 CPUs and 7 GB, Xcode 26.6 (SDK 26.5, Apple
clang 21.0.0), CMake 4.4.5, GNU Make 3.81, an Aqua session with a WindowServer, one display "Apple Virtual" of 1024x768 at scale 1 and 60 Hz,
12 display modes (none with the same points and another scale, so the scale-change checks were skipped). The tests took 67 s in the native Release
configuration, 113 s under ASan, 10 s and 39 s for the fake backend.

Build: **0 errors in all four configurations; the only warnings are the two of the vendored `stb_image.h`**
(`native.warnings.txt` and the others are empty). The compile commands carry `-mmacosx-version-min=11.0`, `-fno-objc-arc` and
`-Wunguarded-availability` on the two `.m` files, and no warning of availability or deprecation came out (so the stand-in headers did not
hide one). The link accepted `-Xlinker -no_warn_duplicate_libraries`, the frameworks Cocoa, CoreGraphics, OpenGL and QuartzCore were enough
(Metal, CoreVideo, IOKit, GameController and UniformTypeIdentifiers headers were included by `test_cocoa_names` without a collision, and
it passed: 10 of 10, the library's names do not meet Apple's).

Tests: fake 24/24 (Release and ASan). Native Release 31/35, native ASan 32/35. Passed in both: `test_cocoa_names`, `test_cocoa_window`
(166/166, 167/167 under the leaks tool), `test_gl_cocoa` (153), `test_pixels_cocoa` (162; **0 pixels differ** from the window server's
picture, the rounded-corner fix of 9d305f5 works), `test_text_cocoa`, `test_dialog_cocoa`, `test_vulkan_cocoa`, `test_gl_worker_cocoa` (71), `test_ui_cocoa`
(12; 0 of 268800 pixels differ for both renderers, and the two renderers agree in 100% of the blocks), `test_crash`, `test_thread` and the
other portable ones. `test_input_cocoa` passed 695/695 under ASan and failed 4 of 697 in Release.

### The four failures

| test | what the log had | cause | fixed in |
|---|---|---|---|
| `test_input_cocoa.c:516, 517, 522, 523`, Release only | the left or right button down at 33,44 in the pass with events posted to the application's queue did not come as one event with its position, the polled state was not down/pressed; then the release was not one either. The values were not printed. The pass given straight to the view and the whole ASan run were fine | **the test, not the library.** The desktop picture at the end of the run shows the system's own "Do you want to enable Dictation?" dialog (its text: "press the Control key twice"). The test's `test_modifiers` posts the left Control and, milliseconds later, the right Control through `NSApplication`, which looks at the flags of the events that pass through it for a double tap of Control (the default dictation shortcut). The dialog is another application's window; it took the key window from the test's (the core then lets go of the held buttons in the stream before the focus loss, and ignores a release of a button that is not held), and all four failures are what that gives for one press and release. The ASan build ran after the dialog was already on the screen. Not timing, not event order, not a click that activates a window. Sure about the dialog and the mechanism of the failures; **not sure that the posted Control events are what raised it (the F5 key of the key table is the other candidate)**, and the log has no time stamps | badd284: the two Control keys' events go to the view in both passes; the button checks print the button, every event seen (with `focus LOST`), the polled state, key window and active state; `need_key_window` before the moves, buttons and drags of the posted pass notes and restores the key window; the job takes a picture of the desktop every 4 s during the tests (575d481) |
| `test_clipboard_cocoa.c:83, 86, 88, 94, 97` | AppKit's own line: `'application/x-zen-test' is not a valid UTI string.  Cannot set data for an invalid UTI.` Text, PNG and the URI list passed | **the library.** `NSPasteboard` names what it holds by UTI and `setData:forType:` says NO to anything else; the backend gave it the MIME string, ignored the NO, and `clipboard_set_data` said true with nothing written. The test's expectation (the data is there and reads back) was right, except that its oracle looked in AppKit under the MIME string | 1525b9e: `src/pasteboard_type.h` turns a MIME type into a UTI (the types the system knows are its UTI: `text/html` is `public.html`, `image/jpeg` is `public.jpeg`, ...; any other is `com.zen-platform.mime.` plus the type spelled with letters, digits, hyphen and period: `application/x-zen-test` is `com.zen-platform.mime.application-sx-dzen-dtest`); the refusal of `setString:forType:` and `setData:forType:` is now an error; the test's oracle reads that type, writes `public.html` and `public.jpeg` as other programs would, and prints the pasteboard's types on failure. Sure the library was wrong; fairly sure (85%) that a valid-looking UTI is accepted (Qt does the same with its own prefix) |
| `test_drop_cocoa.c:90` | the path of the dropped file with an odd name did not match byte for byte; the two-file drop with spaces, the refused non-file and the 50 drops passed. The bytes were not printed | **the library, if the cause is what it appears to be.** The odd name has a composed e-acute and an ideograph. The backend read paths with `-[NSURL fileSystemRepresentation]`, the file system's form of the name, which is known (Apple's page does not say) to be the decomposed (NFD) one; the program got bytes it never saw and that do not compare equal to the name it knows. The other backends hand over the URI's bytes decoded and no more | 6568c9b: `file_url_path` gives `[[url filePathURL] path]` (the name as the URL says it) for dropped files and for the paths of the file dialogs; the test makes its URL from the bytes (percent-escaped `URLWithString:`), so the pasteboard certainly holds the composed name, and prints wanted and got as hex and the pasteboard URL's `absoluteString`, `path` and `fileSystemRepresentation`. About 70% sure: the log has no bytes |
| `test_window_cocoa.c:208, 209` | the popup and the tooltip: "no title bar bit AND level == 101" false for both; the two windows that have a level check once attached | **the library.** The style cannot have the title bit (borderless plus non-activating panel). `apply_level` ran before `addChildWindow:ordered:`, and AppKit gives a child window its parent's level when it is attached (not in Apple's pages; OpenJDK's macOS port and Electron both found it and set the level again after attaching). `setHidesOnDeactivate:NO` (a8d81a7) and the panel class do not touch the level | 52506da: `attach_to_parent` re-applies the level, `apply_level` re-applies the levels of the windows attached to it (setting a window's level sets its children's too); the test prints style and level numbers for each kind. About 85% sure |

### What else the logs showed

* **The Dictation dialog stayed on the screen** from the Release `test_input_cocoa` to the end of the job (the picture at the end): the
  runner has nobody to answer it, and it is another application's window, so the tests that ran after it (`test_dialog_cocoa`, the
  ASan configuration) were run with it there, and passed.
* Dialogs: `test_dialog_cocoa` took 6.8 s for eight dialogs (message box twice, confirm, open, save, folder, several files, open with a
  filter that allows everything), 0.85 s each on average: each was ended by the first or second tick of the abort timer, and none
  waited for the 90 s alarm; `confirm_box` ended by the abort returns 0.
* `leaks`: 96 leaks of 6272 bytes, **all of one root**, an `NSXPCConnection` to `com.apple.linkd.autoShortcut` made by Apple's `AppIntents` and
  `LinkServices`; none names this library, AppKit windows or `Zen*` classes. Heap growth in 300 create/destroy rounds: 594 to 738 bytes a
  round for plain windows (blocks go down or up by about 4 a round, windows on screen 1 -> 1, NSApp windows 2 -> 2), 2016 bytes a round with GL
  contexts (220 rounds): not leaks as the tool counts them, not looked into further.
* Crash reports: 25, **all of `test_crash`**, which crashes on purpose (17 `EXC_BAD_ACCESS` SIGSEGV, 4 SIGBUS, 4 SIGABRT) and passes
  22/22; no other process crashed.
* The frames of `test_ui_cocoa` (artifact `ui-frames`) are right: the toolkit's "Controls" and "Dial" panels, right side up, the right colours,
  the same in the software and the GL renderer. In both the list box inside "Controls" draws its last line ("zen_ui.c") below the panel's
  edge: the same in both renderers, so it is the toolkit's scene and clipping, not the macOS backend.
* The shared library (`BUILD_SHARED_LIBS=ON`) built and linked with no warning; the count of its exports was printed by a step whose
  output is not in the artifacts (the next run writes `shared.exports.txt`). The MoltenVK step's output is not in the artifacts either
  (`moltenvk.log` from now on); what the run proves is that the loader and MoltenVK were found and a surface was made.
* Notes that are facts about this runner: the display offers no second scale; `mouse_capture` outside the application cannot be tried;
  the horizontal scroll sign (`scroll 2 lines to the right: AppKit says 2.00, the library reports -2.00`) passes the test, whose
  expectation and the backend's `-dx` come from the same reading of AppKit's convention: nothing here proves that sign against a real device.
* `CGWindowListCreateImage` kept working from a process without any permission on macOS 26.7.

### What the next run has to confirm

1. `test_window_cocoa`: the popup and the tooltip are at level 101 (`check_look` prints style and level otherwise), dialog and utility levels
   (printed as information), nothing else regressed.
2. `test_clipboard_cocoa`: the custom type is written, found, read and held by AppKit under `com.zen-platform.mime.application-sx-dzen-dtest`;
   `public.html` and `public.jpeg` hold the data; a too long MIME type is an error. If AppKit still logs "not a valid UTI string", the
   rule of `pasteboard_type.h` is what to change, and the error text now says it.
3. `test_drop_cocoa`: the odd name arrives byte for byte; if not, the hex lines say whether `absoluteString`, `path` or
   `fileSystemRepresentation` changed it (and so whether NSURL normalises on its way and the test must compare by file identity).
4. `test_input_cocoa`, Release: 697 checks, no `note: the test window was not the key window`; **open the `native.shots/` pictures**: no
   Dictation dialog. If one appears, the time stamp of its first picture against the cumulative test times in `native.ctest.log` says
   which test raised it, and the F5 key (`0x60`, in the key table) is the next candidate.
5. Everything that passed still passes (the library changes are in the backend, `dialog_cocoa.m` and the new header), and the build has
   no warning of our own.

## Preparation for the one run (2026-11-18)

What was done before run 44285580324, kept as it was written. The run then showed what this desk check could not see: it found nothing to
fix at compile level (there was nothing: 0 errors, 0 warnings) and four of what AppKit would do, but it missed the four failures above
(a pasteboard that takes only UTIs, the Unicode form of a path, the level of an attached child window, a system dialog taking the focus).

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

## Reading a run

Start the job by hand (`gh workflow run mac.yml --ref mac-backend`). The job summary (Actions page) lists, per configuration, the distinct
error lines of the build and the failed tests. The artifact `logs` holds everything: `machine.txt` (image, Xcode, SDK, session),
`<config>.configure.log`, `<config>.build.log` (all the compile commands, `VERBOSE=1`: look for `-mmacosx-version-min=11.0`, the SDK path and
`-fno-objc-arc` on the two `.m` files), `<config>.build-errors.log` (the errors again, one file at a time), `<config>.warnings.txt`,
`<config>.ctest.log` (every test's whole output), `<config>.tests.txt` (one line per test), `<config>.LastTest.log`, `leaks.log`,
`crash-reports/`, `desktop-at-the-end.png`, `test-images/`, and from the next run `<config>.shots/` (a picture of the desktop every 4 seconds
of the tests, named by the seconds since they began), `moltenvk.log` and `shared.exports.txt`. The configurations are `fake`, `native`,
`native-asan`, `fake-asan`. In this order:

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
7. **Phase 6** `test_clipboard_cocoa` (a failure prints the library's error and the types the pasteboard holds; the `/tmp` of the URI list
   passed, the link to `/private/tmp` did not matter), `test_drop_cocoa` (a failure prints the wanted and the received path as hex and what the
   pasteboard's URL says by `absoluteString`, `path` and `fileSystemRepresentation`), `test_dialog_cocoa`: each dialog is opened and aborted by a repeating timer; look for
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
  types as UTIs (`src/pasteboard_type.h`: NSPasteboard takes only valid UTIs, run 44285580324; the types the system knows keep its UTI, any other
  is `com.zen-platform.mime.` plus the type with `/` as `-s`, `-` as `-d`; a refusal is an error). Only PNG is read for images (a TIFF on the pasteboard is not). A drop is `EVENT_WINDOW_DROP`;
  the paths live in the window until the next drop, and are the URL's path as it says it (`[[url filePathURL] path]`), not `fileSystemRepresentation` (the decomposed form), as the file dialogs return them too. Dialogs are modal for the whole application; the filters of a file dialog become one list of
  extensions (the panel has no filter list); `dialog.c` leaves the dialogs to `dialog_cocoa.m` when the native backend is built
  (`ZEN_DIALOG_COCOA`) and `test_dialog` (stand-in tools) runs only for the fake backend.
* Window levels (phase 7): a popup or tooltip is at the pop-up menu level, but `addChildWindow:ordered:` sets a child's level to its parent's and
  `setLevel:` sets the level of the window's children, so `attach_to_parent` and `apply_level` put each window's own level back.
* Tests that post events to the application's queue never post the Control keys: `NSApplication` takes two taps of Control for the
  dictation shortcut and the system puts up its own dialog, which takes the key window.
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

A scale 2 display, the scale-change event, a drag with a real mouse, the pointer held by the captured mode with a real mouse, a trackpad
(the gesture tests use made-up events, so the real signs of `rotation` and of the horizontal scroll are assumptions), a real keyboard and
input method, held keys at focus gain (`CGEventSourceKeyState` may need a permission), the global monitor of `mouse_capture`, the look of
the diagonal resize cursors, a real drag of files from the Finder, the system's drag of a window by a hit-test title bar
(`performWindowDragWithEvent:` is never called by a test), the answer of a person in a dialog, the Dock icon as the Dock shows it.
The Vulkan surface was proven only on a runner with MoltenVK from Homebrew and no GPU of its own.

## Next steps

1. Start the job once (`gh workflow run mac.yml --ref mac-backend`) and read it as "What the next run has to confirm" says: the four fixes of
   the failures, and that nothing else changed. The fixes were made without a Mac and without a run.
2. If `test_input_cocoa` fails again in Release, look at the pictures in `native.shots/` and at the values it prints before anything else.
3. Then the unverified list above, with a Mac or a person: a real trackpad (the sign of `rotation`), the Finder drag, the input method.
4. Look at the growth of the heap in the GL create/destroy rounds (about 2 KB a round, not a leak for the `leaks` tool) if it matters.
