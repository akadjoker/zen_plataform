# Comparison programs (zen_platform against GLFW 3 and SDL2): built only with -DPLATFORM_BUILD_COMPARE=ON, so the
# normal build needs neither. See tools/bench_compare.sh and docs/COMPARACAO_CORE.md.
#   -DZP_COMPARE_GLFW_ROOT=<dir>  a prefix holding include/GLFW and lib/ (default: what pkg-config / the system has)
find_package(PkgConfig REQUIRED)
find_package(X11 REQUIRED)

add_executable(bench_zp bench_main.c bench_zp.c)
target_include_directories(bench_zp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(bench_zp PRIVATE platform X11)

add_executable(bench_run bench_run.c)
target_link_libraries(bench_run PRIVATE X11 Xtst)

add_library(mcount SHARED mcount.c)
set_target_properties(mcount PROPERTIES PREFIX "lib")
target_link_libraries(mcount PRIVATE ${CMAKE_DL_LIBS})

# GLFW: the distribution's shared library, found by path so that a copy unpacked in a directory of its own works too
if(ZP_COMPARE_GLFW_ROOT)
    find_library(ZP_GLFW_LIB glfw PATHS ${ZP_COMPARE_GLFW_ROOT}/lib ${ZP_COMPARE_GLFW_ROOT}/usr/lib/x86_64-linux-gnu NO_DEFAULT_PATH)
    find_path(ZP_GLFW_INC GLFW/glfw3.h PATHS ${ZP_COMPARE_GLFW_ROOT}/include ${ZP_COMPARE_GLFW_ROOT}/usr/include NO_DEFAULT_PATH)
else()
    find_library(ZP_GLFW_LIB glfw)
    find_path(ZP_GLFW_INC GLFW/glfw3.h)
endif()
if(ZP_GLFW_LIB AND ZP_GLFW_INC)
    add_executable(bench_glfw bench_main.c bench_glfw.c)
    target_include_directories(bench_glfw PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ZP_GLFW_INC})
    target_link_libraries(bench_glfw PRIVATE ${ZP_GLFW_LIB} X11)
else()
    message(STATUS "compare: GLFW not found (set ZP_COMPARE_GLFW_ROOT), bench_glfw is not built")
endif()

pkg_check_modules(ZP_SDL2 sdl2)
if(ZP_SDL2_FOUND)
    add_executable(bench_sdl2 bench_main.c bench_sdl2.c)
    target_include_directories(bench_sdl2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${ZP_SDL2_INCLUDE_DIRS})
    target_link_libraries(bench_sdl2 PRIVATE ${ZP_SDL2_LINK_LIBRARIES} X11)
else()
    message(STATUS "compare: SDL2 not found, bench_sdl2 is not built")
endif()
