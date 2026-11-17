# Tests compile core.c + backend_fake.c directly, independent of the backend the
# main lib chose. The X11 lib and the headless fake tests live in the same tree.
function(add_fake_test name)
    add_executable(${name} ${name}.c
        ${CMAKE_SOURCE_DIR}/src/core.c
        ${CMAKE_SOURCE_DIR}/src/gesture.c
        ${CMAKE_SOURCE_DIR}/src/error.c
        ${CMAKE_SOURCE_DIR}/src/log.c
        ${CMAKE_SOURCE_DIR}/src/gamepad.c
        ${CMAKE_SOURCE_DIR}/src/gamepad_stub.c
        ${CMAKE_SOURCE_DIR}/src/backend_fake.c)
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${name} PRIVATE c_std_11)
    target_compile_definitions(${name} PRIVATE PLATFORM_STATIC)
    target_link_libraries(${name} PRIVATE platform_sanitize)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_fake_test(test_input_logic)
add_fake_test(test_app_run)
add_fake_test(test_gestures)
add_fake_test(test_log)
add_fake_test(test_events)
add_fake_test(test_time)

# Portable modules with no window backend: compile just their sources (in ARGN).
function(add_unit_test name)
    set(srcs "")
    foreach(s ${ARGN})
        list(APPEND srcs ${CMAKE_SOURCE_DIR}/${s})
    endforeach()
    add_executable(${name} ${name}.c ${srcs})
    target_include_directories(${name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
    target_compile_features(${name} PRIVATE c_std_11)
    target_compile_definitions(${name} PRIVATE PLATFORM_STATIC)
    target_link_libraries(${name} PRIVATE platform_sanitize)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

if(WIN32)
    set(FS_SRC src/os.c src/error.c src/log.c src/io.c src/io_win32.c src/fs_win32.c src/win32_util.c)
    set(PROCESS_SRC src/process_win32.c src/win32_util.c src/error.c src/log.c)
else()
    set(FS_SRC src/os.c src/error.c src/log.c src/io.c src/io_posix.c src/fs_posix.c)
    set(PROCESS_SRC src/process_posix.c src/error.c src/log.c)
endif()
add_unit_test(test_fs ${FS_SRC})
add_unit_test(test_process ${PROCESS_SRC})
if(NOT WIN32)
    # includes src/process_posix.c itself, so only its dependencies are compiled in
    add_unit_test(test_process_slots src/error.c src/log.c)
    target_include_directories(test_process_slots PRIVATE ${CMAKE_SOURCE_DIR}/src)
    find_package(Threads REQUIRED)
    target_link_libraries(test_process_slots PRIVATE Threads::Threads)
endif()
add_unit_test(test_path ${FS_SRC})
add_unit_test(test_io ${FS_SRC})
target_include_directories(test_io PRIVATE ${CMAKE_SOURCE_DIR}/src)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_io PRIVATE Threads::Threads)
endif()
add_unit_test(test_fsys ${FS_SRC})
add_unit_test(test_xinput_map src/gamepad.c src/gamepad_stub.c)
add_unit_test(test_library src/library.c ${FS_SRC})
add_unit_test(test_preedit src/preedit.c)
add_unit_test(test_crash src/crash.c src/thread.c src/error.c src/log.c)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_crash PRIVATE Threads::Threads)
endif()
target_link_libraries(test_library PRIVATE ${CMAKE_DL_LIBS})
add_unit_test(test_draw2d src/draw2d.c)
add_unit_test(test_image src/draw2d.c src/image.c ${FS_SRC})

if(UNIX AND NOT APPLE)
    add_unit_test(test_gamepad src/gamepad.c ${FS_SRC})
endif()

if(WIN32 AND NOT PLATFORM_USE_FAKE)
    add_executable(test_win32 test_win32.c)
    target_link_libraries(test_win32 PRIVATE platform)
    add_test(NAME test_win32 COMMAND test_win32)
    add_executable(test_win32_input test_win32_input.c)
    target_link_libraries(test_win32_input PRIVATE platform)
    add_test(NAME test_win32_input COMMAND test_win32_input)
    set_tests_properties(test_win32_input PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(PLATFORM_BACKEND_X11 OR (WIN32 AND NOT PLATFORM_USE_FAKE))
    add_executable(test_vulkan test_vulkan.c)
    target_link_libraries(test_vulkan PRIVATE platform)
    add_test(NAME test_vulkan COMMAND test_vulkan)
    set_tests_properties(test_vulkan PROPERTIES SKIP_RETURN_CODE 77)
endif()

if(PLATFORM_BACKEND_WAYLAND)
    add_executable(test_gl_wayland test_gl_wayland.c)
    target_link_libraries(test_gl_wayland PRIVATE platform)
    add_test(NAME test_gl_wayland COMMAND test_gl_wayland)
    set_tests_properties(test_gl_wayland PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_wayland_window test_wayland_window.c)
    target_link_libraries(test_wayland_window PRIVATE platform)
    add_test(NAME test_wayland_window COMMAND test_wayland_window)
    set_tests_properties(test_wayland_window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    # Input against a compositor of its own, which needs no display. The test uses
    # the library's protocol tables, so a shared library is left out.
    pkg_check_modules(WAYLAND_SERVER IMPORTED_TARGET wayland-server)
    if(WAYLAND_SERVER_FOUND AND NOT BUILD_SHARED_LIBS)
        add_executable(test_input_wayland test_input_wayland.c)
        add_dependencies(test_input_wayland wayland_server_headers)
        target_include_directories(test_input_wayland PRIVATE ${WAYLAND_GEN_DIR})
        if(WAYLAND_POINTER_LOCK)
            target_compile_definitions(test_input_wayland PRIVATE ZEN_WL_POINTER_LOCK)
        endif()
        target_link_libraries(test_input_wayland PRIVATE platform PkgConfig::WAYLAND_SERVER)
        add_test(NAME test_input_wayland COMMAND test_input_wayland)
        set_tests_properties(test_input_wayland PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
        add_executable(test_wayland_shutdown test_wayland_shutdown.c)
        add_dependencies(test_wayland_shutdown wayland_server_headers)
        target_include_directories(test_wayland_shutdown PRIVATE ${WAYLAND_GEN_DIR})
        if(WAYLAND_XDG_DECORATION)
            target_compile_definitions(test_wayland_shutdown PRIVATE ZEN_WL_XDG_DECORATION)
        endif()
        target_link_libraries(test_wayland_shutdown PRIVATE platform PkgConfig::WAYLAND_SERVER)
        add_test(NAME test_wayland_shutdown COMMAND test_wayland_shutdown)
        set_tests_properties(test_wayland_shutdown PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    endif()
endif()

# The dialog plumbing is the same on macOS (the fake backend and the native one alike): stand-in programs on PATH.
if(APPLE)
    add_executable(test_dialog test_dialog.c)
    target_link_libraries(test_dialog PRIVATE platform)
    add_test(NAME test_dialog COMMAND test_dialog)
endif()

if(PLATFORM_BACKEND_X11)
    add_executable(test_gl_x11 test_gl_x11.c)
    target_link_libraries(test_gl_x11 PRIVATE platform)
    add_test(NAME test_gl_x11 COMMAND test_gl_x11)
    set_tests_properties(test_gl_x11 PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_clipboard_x11 test_clipboard_x11.c)
    target_link_libraries(test_clipboard_x11 PRIVATE platform)
    add_test(NAME test_clipboard_x11 COMMAND test_clipboard_x11)
    set_tests_properties(test_clipboard_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

    add_executable(test_drop_x11 test_drop_x11.c)
    target_link_libraries(test_drop_x11 PRIVATE platform)
    add_test(NAME test_drop_x11 COMMAND test_drop_x11)
    set_tests_properties(test_drop_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)

    add_executable(test_multiwindow_x11 test_multiwindow_x11.c)
    target_link_libraries(test_multiwindow_x11 PRIVATE platform)
    add_test(NAME test_multiwindow_x11 COMMAND test_multiwindow_x11)
    set_tests_properties(test_multiwindow_x11 PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_dialog test_dialog.c)
    target_link_libraries(test_dialog PRIVATE platform)
    add_test(NAME test_dialog COMMAND test_dialog)

    add_executable(test_x11_window test_x11_window.c)
    target_link_libraries(test_x11_window PRIVATE platform)
    add_test(NAME test_x11_window COMMAND test_x11_window)
    set_tests_properties(test_x11_window PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_gl_worker_x11 test_gl_worker_x11.c)
    target_link_libraries(test_gl_worker_x11 PRIVATE platform)
    add_test(NAME test_gl_worker_x11 COMMAND test_gl_worker_x11)
    set_tests_properties(test_gl_worker_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_executable(test_x11_lifecycle test_x11_lifecycle.c)
    target_link_libraries(test_x11_lifecycle PRIVATE platform)
    add_test(NAME test_x11_lifecycle COMMAND test_x11_lifecycle)
    set_tests_properties(test_x11_lifecycle PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)

    add_executable(test_input_x11 test_input_x11.c)
    target_link_libraries(test_input_x11 PRIVATE platform)
    # Real key events through XTest, when the development files are there.
    find_library(XTEST_LIB Xtst)
    find_path(XTEST_INCLUDE X11/extensions/XTest.h)
    if(XTEST_LIB AND XTEST_INCLUDE)
        target_compile_definitions(test_input_x11 PRIVATE HAVE_XTEST=1)
        target_link_libraries(test_input_x11 PRIVATE ${XTEST_LIB})
    endif()
    add_test(NAME test_input_x11 COMMAND test_input_x11)
    set_tests_properties(test_input_x11 PROPERTIES SKIP_RETURN_CODE 77)

    # Buttons and drags with the pointer moved by XTest (the server's own pointer).
    add_executable(test_mouse_x11 test_mouse_x11.c)
    target_link_libraries(test_mouse_x11 PRIVATE platform)
    if(XTEST_LIB AND XTEST_INCLUDE)
        target_compile_definitions(test_mouse_x11 PRIVATE HAVE_XTEST=1)
        target_link_libraries(test_mouse_x11 PRIVATE ${XTEST_LIB})
    endif()
    add_test(NAME test_mouse_x11 COMMAND test_mouse_x11)
    set_tests_properties(test_mouse_x11 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
endif()


# The macOS window, events and monitors, on the window server of a graphical session.
if(PLATFORM_BACKEND_COCOA AND NOT PLATFORM_USE_FAKE)
    add_executable(test_cocoa_window test_cocoa_window.c)
    target_link_libraries(test_cocoa_window PRIVATE platform "-framework CoreGraphics" "-framework CoreFoundation")
    add_test(NAME test_cocoa_window COMMAND test_cocoa_window)
    # 77: no graphical session (the CI job sets ZP_REQUIRE_DISPLAY, which turns that into a failure)
    set_tests_properties(test_cocoa_window PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120)
    if(PLATFORM_SANITIZE)
        # AppKit keeps objects alive for the life of the process, which a leak check at exit takes for leaks; the test
        # looks for the leaks of this library itself (the windows AppKit still holds, the heap that grows)
        set_property(TEST test_cocoa_window APPEND PROPERTY ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
    endif()

    # OpenGL, the pixel surface, the keyboard and the mouse, and the toolkit's frames: each on a window of its own.
    # What the tests write for a person to look at (the frames of test_ui_cocoa) goes to test-images/ of the build folder.
    file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/test-images)
    set(COCOA_TESTS test_gl_cocoa test_pixels_cocoa test_input_cocoa test_gl_worker_cocoa test_ui_cocoa)
    foreach(t ${COCOA_TESTS})
        add_executable(${t} ${t}.c)
        if(t STREQUAL "test_ui_cocoa")
            target_link_libraries(${t} PRIVATE zen_2d zen_ui platform m "-framework CoreGraphics" "-framework CoreFoundation")
        elseif(t STREQUAL "test_gl_worker_cocoa")
            target_link_libraries(${t} PRIVATE platform) # no Apple headers in it: the library's thread_create and Mach's cannot meet
        else()
            target_link_libraries(${t} PRIVATE platform "-framework CoreGraphics" "-framework CoreFoundation")
        endif()
        add_test(NAME ${t} COMMAND ${t})
        set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120
                             ENVIRONMENT "ZP_TEST_OUT=${CMAKE_BINARY_DIR}/test-images")
        if(PLATFORM_SANITIZE)
            set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT "ASAN_OPTIONS=detect_leaks=0")
        endif()
    endforeach()
endif()

# Fake test with extra source files (widgets + UI + software renderer).
function(add_fake_ext_test name)
    set(srcs "")
    foreach(s ${ARGN})
        list(APPEND srcs ${CMAKE_SOURCE_DIR}/${s})
    endforeach()
    add_executable(${name} ${name}.c
        ${CMAKE_SOURCE_DIR}/src/core.c
        ${CMAKE_SOURCE_DIR}/src/gesture.c
        ${CMAKE_SOURCE_DIR}/src/backend_fake.c
        ${srcs})
    target_include_directories(${name} PRIVATE
        ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(${name} PRIVATE c_std_11)
    target_compile_definitions(${name} PRIVATE PLATFORM_STATIC)
    target_link_libraries(${name} PRIVATE platform_sanitize)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

# zen_ui compiled into a test directly: the toolkit, the software renderer and the fonts, over the fake backend.
set(ZEN_UI_TEST_SRC
    ${FS_SRC}
    src/gamepad.c
    src/gamepad_stub.c
    src/draw2d.c
    src/ui/ui_core.c
    src/ui/ui_widgets.c
    src/ui/ui_layout.c
    src/ui/ui_state.c
    src/ui/ui_theme.c
    src/ui/ui_render_software.c
    src/ui/ui_font.c
    src/ui/stb_impl.c
    src/ui/ui_utf8.c
    src/ui/ui_dialog.c)

# Full widget test in both PIXEL and GL mode
add_fake_ext_test(test_widgets ${ZEN_UI_TEST_SRC})

# zen_ui below the widgets: renderer against a reference, fonts, the draw list's text arena, the state store
add_fake_ext_test(test_ui ${ZEN_UI_TEST_SRC})
target_compile_definitions(test_ui PRIVATE ZEN_TEST_FONT="${CMAKE_SOURCE_DIR}/src/ui/fonts/DejaVuSans.ttf")

# Clipboard API over the fake backend's in-memory store, PNG encoder and decoder.
add_fake_ext_test(test_clipboard
    ${FS_SRC}
    src/gamepad.c
    src/gamepad_stub.c
    src/clipboard.c
    src/png.c
    src/draw2d.c)

# Threads: the fake core only supplies time_sleep and time_seconds to the test.
add_fake_ext_test(test_thread
    ${FS_SRC}
    src/gamepad.c
    src/gamepad_stub.c
    src/crash.c
    src/thread.c)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(test_thread PRIVATE Threads::Threads)
endif()

# The test also needs libm for stb_truetype math functions (sqrt, floor, etc.)
if(NOT WIN32)
    target_link_libraries(test_widgets PRIVATE m)
    target_link_libraries(test_ui PRIVATE m)
endif()

# A test that hangs must not hold a runner for the default 25 minutes.
get_property(ZEN_ALL_TESTS DIRECTORY PROPERTY TESTS)
foreach(t ${ZEN_ALL_TESTS})
    get_test_property(${t} TIMEOUT existing)
    if(NOT existing)
        set_tests_properties(${t} PROPERTIES TIMEOUT 120)
    endif()
    # UBSan only prints and carries on by default, and the test then passes with undefined behaviour in it. With the
    # sanitizers on, a report ends the test with a failure. test_crash is left out: its children dereference a null
    # pointer on purpose, to die of SIGSEGV.
    if(PLATFORM_SANITIZE AND NOT t STREQUAL "test_crash")
        set_property(TEST ${t} APPEND PROPERTY ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
    endif()
endforeach()
