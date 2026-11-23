cmake_minimum_required(VERSION 3.16)

# macOS: the oldest system a build runs on. 11.0 is the first one on Apple Silicon, so one target holds for both
# architectures (below it the compiler raises an arm64 build to it anyway). It has to be set before project().
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
endif()

project(platform LANGUAGES C)

# macOS: the native backend is Objective-C, and so is the test that checks the library's names against Apple's headers
# (tests/test_cocoa_names.m, which every macOS build has, the fake backend's too).
if(APPLE AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    enable_language(OBJC)
endif()

# Static CRT: executables and the library depend on no runtime DLL (vcruntime, ...).
# MSVC is only defined once the compiler is known, so this goes after project().
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

option(PLATFORM_BUILD_TESTS "Build unit tests" ON)
option(PLATFORM_USE_FAKE    "Force the fake backend" OFF)
option(PLATFORM_SANITIZE    "Build with ASan + UBSan (debug)" OFF)
# Window system on Linux and the other Unix systems: x11 or wayland. One per build.
set(PLATFORM_LINUX_BACKEND "x11" CACHE STRING "Window system backend on Unix: x11 or wayland")
set_property(CACHE PLATFORM_LINUX_BACKEND PROPERTY STRINGS x11 wayland)
# BUILD_SHARED_LIBS (CMake builtin): ON -> .so/.dll, OFF -> .a/.lib

set(CORE_SRC src/core.c src/gesture.c src/log.c src/library.c src/thread.c src/dialog.c src/preedit.c src/vulkan.c src/clipboard.c src/png.c src/error.c src/crash.c src/gamepad.c src/io.c src/os.c src/draw2d.c src/image.c src/glad.c)
if(UNIX AND NOT APPLE AND NOT ANDROID AND NOT EMSCRIPTEN AND NOT PLATFORM_USE_FAKE)
    list(APPEND CORE_SRC src/gamepad_evdev.c)
elseif(WIN32 AND NOT PLATFORM_USE_FAKE)
    list(APPEND CORE_SRC src/gamepad_xinput.c)
else()
    list(APPEND CORE_SRC src/gamepad_stub.c)
endif()
if(WIN32)
    list(APPEND CORE_SRC src/io_win32.c src/fs_win32.c src/process_win32.c src/win32_util.c)
else()
    list(APPEND CORE_SRC src/io_posix.c src/fs_posix.c src/process_posix.c)
endif()

if(PLATFORM_USE_FAKE)
    set(BACKEND_SRC src/backend_fake.c)
elseif(EMSCRIPTEN)
    set(BACKEND_SRC src/backend_web.c)
elseif(ANDROID)
    set(BACKEND_SRC src/backend_android.c src/io_android.c
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
    # The glue's entry renamed: backend_android.c's ANativeActivity_onCreate calls it
    # and wraps the focus callback there, on the UI thread, before any focus event.
    set_source_files_properties(${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c
        PROPERTIES COMPILE_DEFINITIONS ANativeActivity_onCreate=zen_glue_on_create)
elseif(WIN32)
    set(BACKEND_SRC src/backend_win32.c)
elseif(APPLE)
    # macOS: the one Objective-C file of the library, over AppKit. iOS has no backend (a fake build still works).
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        message(FATAL_ERROR "No native iOS backend; configure with -DPLATFORM_USE_FAKE=ON for a headless build")
    endif()
    # dialog_cocoa.m: the message boxes, the file dialogs and open_url with the system's own (dialog.c leaves them to it)
    set(BACKEND_SRC src/backend_cocoa.m src/dialog_cocoa.m)
    set(PLATFORM_BACKEND_COCOA ON)
    # Manual retain/release, not ARC: a BackendWindow is a calloc'ed C struct holding the window, the view and the delegate,
    # and ARC would need a bridging cast at every use of them (see the comment at the top of the file).
    # -Wunguarded-availability: an AppKit call newer than the deployment target needs an @available check, or it warns.
    set_source_files_properties(src/backend_cocoa.m src/dialog_cocoa.m PROPERTIES COMPILE_OPTIONS "-fno-objc-arc;-Wunguarded-availability")
    set_source_files_properties(src/dialog.c PROPERTIES COMPILE_DEFINITIONS ZEN_DIALOG_COCOA)
elseif(UNIX)
    if(PLATFORM_LINUX_BACKEND STREQUAL "wayland")
        include(cmake/wayland.cmake)
        set(BACKEND_SRC src/backend_wayland.c ${WAYLAND_PROTOCOL_SRC})
        set(PLATFORM_BACKEND_WAYLAND ON)
    elseif(PLATFORM_LINUX_BACKEND STREQUAL "x11")
        set(BACKEND_SRC src/backend_x11.c)
        set(PLATFORM_BACKEND_X11 ON)
    else()
        message(FATAL_ERROR "PLATFORM_LINUX_BACKEND must be x11 or wayland")
    endif()
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

add_library(platform ${CORE_SRC} ${BACKEND_SRC})
target_include_directories(platform PUBLIC include PRIVATE src)
target_compile_features(platform PUBLIC c_std_11)
if(EMSCRIPTEN)
    # drop and fetch live in a JS library; it is merged at the final link
    target_link_options(platform INTERFACE --js-library ${CMAKE_CURRENT_SOURCE_DIR}/web/pf_web.js)
    set_property(TARGET platform APPEND PROPERTY INTERFACE_LINK_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/web/pf_web.js)
endif()
set_target_properties(platform PROPERTIES
    C_VISIBILITY_PRESET hidden OBJC_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(platform PUBLIC PLATFORM_STATIC)
endif()

# zen_ui: immediate-mode widget toolkit. Backend-agnostic core; links only the
# platform, never OpenGL. Ships the GL-free software renderer; the GL renderer
# lives in the separate zen_2d library below.
# The embedded default font: a vendored TTF turned into a C array at build time
# by cmake/embed_font.cmake (cmake -P, so it works under cross toolchains too).
set(ZEN_UI_FONT_TTF ${CMAKE_CURRENT_SOURCE_DIR}/src/ui/fonts/DejaVuSans.ttf)
set(ZEN_UI_FONT_C   ${CMAKE_CURRENT_BINARY_DIR}/ui_font_default.c)
add_custom_command(
    OUTPUT ${ZEN_UI_FONT_C}
    COMMAND ${CMAKE_COMMAND} -DSRC=${ZEN_UI_FONT_TTF} -DDST=${ZEN_UI_FONT_C}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake
    DEPENDS ${ZEN_UI_FONT_TTF} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake
    COMMENT "Embedding default font ${ZEN_UI_FONT_TTF}"
    VERBATIM)

set(ZEN_UI_SRC
    src/ui/ui_core.c
    src/ui/ui_utf8.c
    src/ui/ui_font.c
    src/ui/ui_layout.c
    src/ui/ui_theme.c
    src/ui/ui_state.c
    src/ui/stb_impl.c
    src/ui/ui_widgets.c
    src/ui/ui_dialog.c
    src/ui/ui_render_software.c
    ${ZEN_UI_FONT_C})
add_library(zen_ui ${ZEN_UI_SRC})
target_include_directories(zen_ui PUBLIC include PRIVATE src src/ui)
target_compile_features(zen_ui PUBLIC c_std_11)
set_target_properties(zen_ui PROPERTIES
    C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
target_link_libraries(zen_ui PUBLIC platform)

# zen_2d (r2d): batched 2D engine on GL, and the zen_ui GL renderer. Separate
# library so the toolkit core never depends on OpenGL. Desktop uses GL 3.3 core
# via glad (Linux, Windows and macOS, where it is a 4.1 core context); web (WebGL2) and Android (GLES3) use the GLES3
# path in r2d.c.
if(NOT PLATFORM_USE_FAKE AND (EMSCRIPTEN OR ANDROID OR UNIX OR WIN32))
    add_library(zen_2d src/r2d/r2d.c src/r2d/r2d_ui.c)
    target_include_directories(zen_2d PUBLIC include PRIVATE src/r2d)
    target_compile_features(zen_2d PUBLIC c_std_11)
    set_target_properties(zen_2d PROPERTIES
        C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
    if(EMSCRIPTEN)
        target_link_libraries(zen_2d PUBLIC zen_ui platform) # GL resolved at app link (FULL_ES3)
    elseif(ANDROID)
        target_link_libraries(zen_2d PUBLIC zen_ui platform GLESv3 m)
    else()
        find_package(OpenGL REQUIRED)
        target_link_libraries(zen_2d PUBLIC zen_ui platform OpenGL::GL)
        if(NOT WIN32)
            target_link_libraries(zen_2d PUBLIC m)
        endif()
    endif()
endif()

# Sanitizers wire into every target through these interface flags. ASan + UBSan
# catch the lifetime and signed-overflow bugs this kind of code attracts.
add_library(platform_sanitize INTERFACE)
if(PLATFORM_SANITIZE)
    if(MSVC)
        target_compile_options(platform_sanitize INTERFACE /fsanitize=address)
    else()
        target_compile_options(platform_sanitize INTERFACE
            -fsanitize=address,undefined -fno-omit-frame-pointer -g)
        target_link_options(platform_sanitize INTERFACE
            -fsanitize=address,undefined)
    endif()
endif()
target_link_libraries(platform PUBLIC platform_sanitize)
# Every function and every datum in a section of its own, and the linker drops the sections nothing reaches: the shared
# library goes from 356 kB to 192 kB (what a minimal window program pulls out of the static one, from 130 kB to 78 kB) with the
# same exported API. PUBLIC so that a program linking the static library is linked with --gc-sections too (without it the
# sections stay). Not for Apple (-dead_strip) or MSVC (/OPT:REF is the release default).
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang" AND NOT APPLE)
    target_compile_options(platform PRIVATE -ffunction-sections -fdata-sections)
    if(NOT EMSCRIPTEN)
        target_link_options(platform PUBLIC -Wl,--gc-sections)
    endif()
endif()
if(MSVC)
    target_compile_options(platform PUBLIC /utf-8)
    target_compile_definitions(platform PUBLIC _CRT_SECURE_NO_WARNINGS)
endif()

# native libs per platform
if(EMSCRIPTEN)
    # GL/EGL emulated; nothing to link
elseif(ANDROID)
    target_include_directories(platform PUBLIC
        ${ANDROID_NDK}/sources/android/native_app_glue)
    target_link_libraries(platform PUBLIC android EGL GLESv3 log)
    # Keep the activity entry point the glue exports from being stripped from the .so.
    target_link_options(platform PUBLIC "-u" "ANativeActivity_onCreate")
elseif(WIN32)
    target_link_libraries(platform PUBLIC opengl32 gdi32 user32 shell32)
    if(MINGW)
        target_link_options(platform PUBLIC -static)
    endif()
elseif(PLATFORM_BACKEND_WAYLAND AND NOT PLATFORM_USE_FAKE)
    find_package(Threads REQUIRED)
    target_include_directories(platform PRIVATE ${WAYLAND_GEN_DIR})
    target_compile_definitions(platform PRIVATE ZEN_BACKEND_WAYLAND)
    if(WAYLAND_POINTER_LOCK)
        target_compile_definitions(platform PRIVATE ZEN_WL_POINTER_LOCK)
    endif()
    if(WAYLAND_XDG_DECORATION)
        target_compile_definitions(platform PRIVATE ZEN_WL_XDG_DECORATION)
    endif()
    if(WAYLAND_PRIMARY)
        target_compile_definitions(platform PRIVATE ZEN_WL_PRIMARY)
    endif()
    target_link_libraries(platform PUBLIC PkgConfig::WAYLAND ${CMAKE_DL_LIBS} Threads::Threads)
elseif(PLATFORM_BACKEND_COCOA AND NOT PLATFORM_USE_FAKE)
    # Cocoa is AppKit and Foundation; CoreGraphics has the display and session calls (the monitors, the window server check,
    # the pointer's warp and its association with the mouse, the software surface's image); OpenGL has CGL, which the
    # NSOpenGLContext of a window sits on and the shared context of gl_worker_* is made with; QuartzCore has the CAMetalLayer
    # of a Vulkan surface (MoltenVK is found at run time, not linked). Carbon is included for the
    # virtual key codes (kVK_*), which are constants in a header: nothing of it is linked.
    target_link_libraries(platform PUBLIC "-framework Cocoa" "-framework CoreGraphics" "-framework OpenGL" "-framework QuartzCore")
    # A program that links zen_2d, zen_ui and platform names libplatform.a more than once (each of them needs it), which the
    # Apple linker warns about; it is as harmless as it is on the other linkers, which say nothing.
    target_link_options(platform INTERFACE "LINKER:-no_warn_duplicate_libraries")
elseif(UNIX AND NOT PLATFORM_USE_FAKE AND NOT APPLE)
    find_package(X11 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC X11 Xrandr GL ${CMAKE_DL_LIBS} Threads::Threads)
endif()

if(PLATFORM_BUILD_TESTS AND NOT EMSCRIPTEN AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()

option(PLATFORM_BUILD_EXAMPLES "Build examples" ON)
# The programs that compare the core with GLFW and SDL2 (bench/compare); they need those libraries, so off by default.
option(PLATFORM_BUILD_COMPARE "Build the GLFW / SDL2 comparison programs" OFF)
if(PLATFORM_BUILD_COMPARE AND UNIX AND NOT APPLE AND NOT PLATFORM_USE_FAKE)
    add_subdirectory(bench/compare)
endif()
if(PLATFORM_BUILD_EXAMPLES AND NOT PLATFORM_USE_FAKE AND (EMSCRIPTEN OR ANDROID OR UNIX OR WIN32))
    add_subdirectory(examples)
endif()

install(TARGETS platform ARCHIVE DESTINATION lib LIBRARY DESTINATION lib RUNTIME DESTINATION bin)
install(FILES include/platform.h include/platform_export.h DESTINATION include)
