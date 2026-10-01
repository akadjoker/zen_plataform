# Tests compile core.c + backend_fake.c directly, independent of the backend the
# main lib chose. The X11 lib and the headless fake tests live in the same tree.
function(add_fake_test name)
    add_executable(${name} ${name}.c
        ${CMAKE_SOURCE_DIR}/src/core.c
        ${CMAKE_SOURCE_DIR}/src/error.c
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

set(FS_SRC src/os.c src/error.c src/io.c src/io_posix.c src/fs_posix.c)
add_unit_test(test_fs ${FS_SRC})
add_unit_test(test_path ${FS_SRC})
add_unit_test(test_io ${FS_SRC})
add_unit_test(test_fsys ${FS_SRC})
add_unit_test(test_draw2d src/draw2d.c)
add_unit_test(test_image src/draw2d.c src/image.c ${FS_SRC})

if(PLATFORM_BACKEND_X11)
    add_executable(test_gl_x11 test_gl_x11.c)
    target_link_libraries(test_gl_x11 PRIVATE platform)
    add_test(NAME test_gl_x11 COMMAND test_gl_x11)
    set_tests_properties(test_gl_x11 PROPERTIES SKIP_RETURN_CODE 77)

    add_executable(test_input_x11 test_input_x11.c)
    target_link_libraries(test_input_x11 PRIVATE platform)
    add_test(NAME test_input_x11 COMMAND test_input_x11)
    set_tests_properties(test_input_x11 PROPERTIES SKIP_RETURN_CODE 77)
endif()
