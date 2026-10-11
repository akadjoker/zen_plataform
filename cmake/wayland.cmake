# Wayland backend: the client libraries through pkg-config, and the C side of the
# protocols, generated at build time by wayland-scanner from wayland-protocols.
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED IMPORTED_TARGET wayland-client wayland-egl egl xkbcommon)
pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

find_program(WAYLAND_SCANNER wayland-scanner)
if(NOT WAYLAND_SCANNER)
    message(FATAL_ERROR "wayland-scanner not found (package wayland-protocols or libwayland-bin)")
endif()

set(WAYLAND_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/wayland)
set(WAYLAND_PROTOCOL_SRC "")
set(WAYLAND_SERVER_HEADERS "")

# wayland_protocol(<xml> <name>): <name>-client-protocol.h and <name>-protocol.c
# in WAYLAND_GEN_DIR; the .c is appended to WAYLAND_PROTOCOL_SRC. The server
# header is for the test compositor only.
function(wayland_protocol xml name)
    set(header ${WAYLAND_GEN_DIR}/${name}-client-protocol.h)
    set(code   ${WAYLAND_GEN_DIR}/${name}-protocol.c)
    set(server ${WAYLAND_GEN_DIR}/${name}-server-protocol.h)
    add_custom_command(
        OUTPUT ${server}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WAYLAND_GEN_DIR}
        COMMAND ${WAYLAND_SCANNER} server-header ${xml} ${server}
        DEPENDS ${xml}
        VERBATIM)
    set(WAYLAND_SERVER_HEADERS ${WAYLAND_SERVER_HEADERS} ${server} PARENT_SCOPE)
    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${WAYLAND_GEN_DIR}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${header}
        DEPENDS ${xml}
        VERBATIM)
    add_custom_command(
        OUTPUT ${code}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${code}
        DEPENDS ${xml} ${header}
        VERBATIM)
    set(WAYLAND_PROTOCOL_SRC ${WAYLAND_PROTOCOL_SRC} ${code} PARENT_SCOPE)
endfunction()

wayland_protocol(${WAYLAND_PROTOCOLS_DIR}/stable/xdg-shell/xdg-shell.xml xdg-shell)
add_custom_target(wayland_server_headers DEPENDS ${WAYLAND_SERVER_HEADERS})
