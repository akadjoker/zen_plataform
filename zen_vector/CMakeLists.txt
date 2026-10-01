cmake_minimum_required(VERSION 3.16)
project(zen_vector LANGUAGES C)

# zen_vector only needs the Framebuffer and a BMP reader/writer from zen_platform,
# so by default it builds against the headless backend: no X11, no GL.
option(ZV_WINDOW "Build zen_platform with a real window backend" OFF)
if(NOT ZV_WINDOW)
    set(PLATFORM_USE_FAKE ON CACHE BOOL "" FORCE)
endif()
set(PLATFORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(PLATFORM_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/.. platform_build)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(zv_harness STATIC tools/bmpcmp.c tools/scene.c)
target_include_directories(zv_harness PUBLIC tools)
target_link_libraries(zv_harness PUBLIC platform)

add_executable(vcmp tools/vcmp.c)
target_link_libraries(vcmp PRIVATE zv_harness)

add_executable(scene_run tools/scene_run.c)
target_link_libraries(scene_run PRIVATE zv_harness)

add_executable(bench_draw2d tools/bench_draw2d.c)
target_link_libraries(bench_draw2d PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(bench_draw2d PRIVATE m)
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(CAIRO QUIET cairo)
endif()
if(CAIRO_FOUND)
    add_executable(cairo_ref tools/cairo_ref.c)
    target_include_directories(cairo_ref PRIVATE ${CAIRO_INCLUDE_DIRS})
    target_link_libraries(cairo_ref PRIVATE zv_harness ${CAIRO_LIBRARIES} m)
endif()

enable_testing()
add_executable(test_harness tests/test_harness.c)
target_link_libraries(test_harness PRIVATE zv_harness)
target_compile_definitions(test_harness PRIVATE ZV_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
add_test(NAME test_harness COMMAND test_harness)
