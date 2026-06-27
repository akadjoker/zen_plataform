cmake_minimum_required(VERSION 3.16)
project(platform LANGUAGES C)

option(PLATFORM_BUILD_TESTS "Build unit tests" ON)
option(PLATFORM_USE_FAKE    "Force the fake backend" OFF)
option(PLATFORM_SANITIZE    "Build with ASan + UBSan (debug)" OFF)
# BUILD_SHARED_LIBS (CMake builtin): ON -> .so/.dll, OFF -> .a/.lib

set(CORE_SRC src/core.c src/os.c src/draw2d.c src/image.c)

if(PLATFORM_USE_FAKE)
    set(BACKEND_SRC src/backend_fake.c)
elseif(EMSCRIPTEN)
    set(BACKEND_SRC src/backend_web.c)
elseif(ANDROID)
    set(BACKEND_SRC src/backend_android.c
        ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
elseif(WIN32)
    set(BACKEND_SRC src/backend_win32.c)
elseif(UNIX)
    set(BACKEND_SRC src/backend_x11.c)
else()
    message(FATAL_ERROR "Unsupported platform")
endif()

add_library(platform ${CORE_SRC} ${BACKEND_SRC})
target_include_directories(platform PUBLIC include PRIVATE src)
target_compile_features(platform PUBLIC c_std_11)
set_target_properties(platform PROPERTIES
    C_VISIBILITY_PRESET hidden POSITION_INDEPENDENT_CODE ON)
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(platform PUBLIC PLATFORM_STATIC)
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
    target_link_libraries(platform PUBLIC opengl32 gdi32 user32)
elseif(UNIX AND NOT PLATFORM_USE_FAKE)
    find_package(X11 REQUIRED)
    target_link_libraries(platform PUBLIC X11 Xrandr GL ${CMAKE_DL_LIBS})
endif()

if(PLATFORM_BUILD_TESTS AND NOT EMSCRIPTEN AND NOT ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()

option(PLATFORM_BUILD_EXAMPLES "Build examples" ON)
if(PLATFORM_BUILD_EXAMPLES AND NOT PLATFORM_USE_FAKE AND (EMSCRIPTEN OR ANDROID OR UNIX))
    add_subdirectory(examples)
endif()
