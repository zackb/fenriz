cmake_minimum_required(VERSION 3.19)
project(fenriz VERSION 0.1.10 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Memory audits: cmake --preset sanitize
option(FENRIZ_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
if(FENRIZ_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g -UNDEBUG)
    add_link_options(-fsanitize=address,undefined)
endif()

find_package(PkgConfig REQUIRED)

pkg_check_modules(SCENEFX   REQUIRED IMPORTED_TARGET scenefx-0.5)
pkg_check_modules(WLROOTS   REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND   REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN    REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBINPUT  REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(XCB       REQUIRED IMPORTED_TARGET xcb xcb-icccm xcb-ewmh)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
set(LAYER_SHELL_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wlr-layer-shell-unstable-v1-protocol.h)
add_custom_command(
    OUTPUT ${LAYER_SHELL_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
            ${LAYER_SHELL_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml
    VERBATIM
)

set(OUTPUT_POWER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wlr-output-power-management-unstable-v1-protocol.h)
add_custom_command(
    OUTPUT ${OUTPUT_POWER_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml
            ${OUTPUT_POWER_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml
    VERBATIM
)

pkg_get_variable(WL_PROTO_DIR wayland-protocols pkgdatadir)

set(TOPLEVEL_DRAG_HEADER ${CMAKE_CURRENT_BINARY_DIR}/xdg-toplevel-drag-v1-server-protocol.h)
add_custom_command(
    OUTPUT ${TOPLEVEL_DRAG_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${WL_PROTO_DIR}/staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml
            ${TOPLEVEL_DRAG_HEADER}
    DEPENDS ${WL_PROTO_DIR}/staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml
    VERBATIM
)

# ext-background-effect-v1
set(BACKGROUND_EFFECT_HEADER ${CMAKE_CURRENT_BINARY_DIR}/ext-background-effect-v1-server-protocol.h)
add_custom_command(
    OUTPUT ${BACKGROUND_EFFECT_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${WL_PROTO_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml
            ${BACKGROUND_EFFECT_HEADER}
    DEPENDS ${WL_PROTO_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml
    VERBATIM
)

set(KDE_BLUR_HEADER ${CMAKE_CURRENT_BINARY_DIR}/kde-blur-protocol.h)
add_custom_command(
    OUTPUT ${KDE_BLUR_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/kde-blur.xml ${KDE_BLUR_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/kde-blur.xml
    VERBATIM
)

pkg_check_modules(WAYLAND_CLIENT REQUIRED IMPORTED_TARGET wayland-client)
set(WL_CLIENT_PROTO_SRC "")
function(wl_client_proto name xml)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/${name}-client-protocol.h)
    set(src ${CMAKE_CURRENT_BINARY_DIR}/${name}-protocol.c)
    add_custom_command(OUTPUT ${hdr}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${hdr}
        DEPENDS ${xml} VERBATIM)
    add_custom_command(OUTPUT ${src}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${src}
        DEPENDS ${xml} VERBATIM)
    set(WL_CLIENT_PROTO_SRC ${WL_CLIENT_PROTO_SRC} ${hdr} ${src} PARENT_SCOPE)
endfunction()

wl_client_proto(xdg-shell            ${WL_PROTO_DIR}/stable/xdg-shell/xdg-shell.xml)
wl_client_proto(viewporter           ${WL_PROTO_DIR}/stable/viewporter/viewporter.xml)
wl_client_proto(xdg-decoration-unstable-v1
                                     ${WL_PROTO_DIR}/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml)
wl_client_proto(fractional-scale-v1  ${WL_PROTO_DIR}/staging/fractional-scale/fractional-scale-v1.xml)
wl_client_proto(ext-workspace-v1     ${WL_PROTO_DIR}/staging/ext-workspace/ext-workspace-v1.xml)
wl_client_proto(xdg-dialog-v1        ${WL_PROTO_DIR}/staging/xdg-dialog/xdg-dialog-v1.xml)
wl_client_proto(xdg-toplevel-icon-v1 ${WL_PROTO_DIR}/staging/xdg-toplevel-icon/xdg-toplevel-icon-v1.xml)
wl_client_proto(xdg-foreign-unstable-v1
                                     ${WL_PROTO_DIR}/unstable/xdg-foreign/xdg-foreign-unstable-v1.xml)
wl_client_proto(xdg-foreign-unstable-v2
                                     ${WL_PROTO_DIR}/unstable/xdg-foreign/xdg-foreign-unstable-v2.xml)
wl_client_proto(xdg-toplevel-drag-v1 ${WL_PROTO_DIR}/staging/xdg-toplevel-drag/xdg-toplevel-drag-v1.xml)
wl_client_proto(xdg-toplevel-tag-v1  ${WL_PROTO_DIR}/staging/xdg-toplevel-tag/xdg-toplevel-tag-v1.xml)
wl_client_proto(alpha-modifier-v1    ${WL_PROTO_DIR}/staging/alpha-modifier/alpha-modifier-v1.xml)
wl_client_proto(xdg-system-bell-v1   ${WL_PROTO_DIR}/staging/xdg-system-bell/xdg-system-bell-v1.xml)
wl_client_proto(ext-background-effect-v1
                                     ${WL_PROTO_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml)
wl_client_proto(kde-blur             ${CMAKE_CURRENT_SOURCE_DIR}/protocols/kde-blur.xml)
wl_client_proto(wlr-layer-shell-unstable-v1
                                     ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-layer-shell-unstable-v1.xml)
# test client only
wl_client_proto(wlr-screencopy-unstable-v1
                                     ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-screencopy-unstable-v1.xml)
wl_client_proto(wlr-virtual-pointer-unstable-v1
                                     ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-virtual-pointer-unstable-v1.xml)

add_executable(fenriz
    src/main.cpp
    src/log.cpp
    src/server.cpp
    src/output.cpp
    src/output_policy.cpp
    src/view.cpp
    src/keyboard.cpp
    src/cursor.cpp
    src/tiling.cpp
    src/config.cpp
    src/layer.cpp
    src/lock.cpp
    src/decoration.cpp
    src/ipc.cpp
    src/workspace_protocol.cpp
    src/toplevel_drag.cpp
    src/toplevel_props.cpp
    src/background_blur.cpp
    src/xwayland.cpp
    ${LAYER_SHELL_HEADER}
    ${OUTPUT_POWER_HEADER}
    ${TOPLEVEL_DRAG_HEADER}
    ${BACKGROUND_EFFECT_HEADER}
    ${KDE_BLUR_HEADER}
    ${CMAKE_CURRENT_BINARY_DIR}/ext-background-effect-v1-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/kde-blur-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/xdg-toplevel-drag-v1-protocol.c
    ${CMAKE_CURRENT_BINARY_DIR}/xdg-shell-protocol.c
)

# wlroots headers require this before inclusion
target_compile_definitions(fenriz PRIVATE WLR_USE_UNSTABLE)
target_include_directories(fenriz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz PRIVATE
    PkgConfig::SCENEFX
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND
    PkgConfig::XKBCOMMON
    PkgConfig::PIXMAN
    PkgConfig::LIBINPUT
    PkgConfig::XCB
)

add_executable(fenrizctl src/fenrizctl.cpp)
target_compile_definitions(fenrizctl PRIVATE FENRIZ_VERSION="${PROJECT_VERSION}")

include(GNUInstallDirs)
install(TARGETS fenriz fenrizctl RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Wayland session entry so greeters
configure_file(fenriz.desktop.in fenriz.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fenriz.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)

# Screen-sharing: route ScreenCast/RemoteDesktop to the wlr portal backend.
install(FILES fenriz-portals.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)

# The defaults fenriz falls back to when the user has no ~/.config/fenriz/fenriz.conf.
install(FILES defaults/fenriz.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz)
target_compile_definitions(fenriz PRIVATE
        FENRIZ_DEFAULT_CONFIG="${CMAKE_INSTALL_FULL_DATADIR}/fenriz/fenriz.conf")

add_executable(fenriz_config_test src/config.cpp src/test_config.cpp)
target_include_directories(fenriz_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_config_test PRIVATE PkgConfig::XKBCOMMON)
target_compile_definitions(fenriz_config_test PRIVATE
        FENRIZ_DEFAULT_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/defaults/fenriz.conf")

add_executable(fenriz_tiling_test src/test_tiling.cpp)
target_include_directories(fenriz_tiling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(fenriz_output_test src/output_policy.cpp src/test_output.cpp)
target_include_directories(fenriz_output_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_output_test PRIVATE PkgConfig::WAYLAND)

add_executable(fenriz_keybind_test src/config.cpp src/test_keybind.cpp)
target_include_directories(fenriz_keybind_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_keybind_test PRIVATE PkgConfig::XKBCOMMON)

add_executable(fenrizctl_test src/test_fenrizctl.cpp)
target_include_directories(fenrizctl_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Wayland client tests for integration testing
add_executable(fenriz-test
    tests/client/main.c
    tests/client/wlclient.c
    tests/client/scenarios.c
    ${WL_CLIENT_PROTO_SRC}
)
target_include_directories(fenriz-test PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz-test PRIVATE PkgConfig::WAYLAND_CLIENT rt)

foreach(t fenriz_config_test fenriz_tiling_test fenriz_keybind_test fenriz_output_test fenrizctl_test fenriz-test)
    target_compile_options(${t} PRIVATE -UNDEBUG)
endforeach()

enable_testing()
add_test(NAME config  COMMAND fenriz_config_test)
add_test(NAME tiling  COMMAND fenriz_tiling_test)
add_test(NAME keybind COMMAND fenriz_keybind_test)
add_test(NAME output  COMMAND fenriz_output_test)
add_test(NAME ctl     COMMAND fenrizctl_test)

# fails when wlroots or wayland-protocols offers a protocol that is in neither src/ nor docs/PROTOCOLS.md.
add_test(NAME protocols COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/protocol-audit.sh)

foreach(s popup layer-popup resize fullscreen dnd grab subsurface scale destroy-parent hotplug workspace foreign dialog icon toplevel-drag toplevel-tag bell alpha fixes background-effect kde-blur pixels layer-blur blur-off evil)
    add_test(NAME wl-${s}
             COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/scripts/wltest.sh
                     --build-dir ${CMAKE_CURRENT_BINARY_DIR} ${s})
    set_tests_properties(wl-${s} PROPERTIES LABELS integration TIMEOUT 120)
endforeach()

set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE FALSE)
set(CPACK_PACKAGE_NAME "fenriz")
set(CPACK_PACKAGE_VENDOR "zackb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "A minimal tiling Wayland compositor")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "zack@bartel.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
# drop the -Linux suffix so the name matches the release tag
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")

# wlroots0.20 and scenefx0.5 are not packaged on Debian/Fedora, so they are absent
# here. Arch is the supported path; deb/rpm are convenience.
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "zack@bartel.com")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libwayland-server0, libxkbcommon0, libpixman-1-0, libinput10, libxcb1")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/zackb/fenriz")

set(CPACK_RPM_PACKAGE_REQUIRES "libwayland-server, libxkbcommon, pixman, libinput, libxcb")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_RELOCATABLE FALSE)
# owned by other packages; don't claim them
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/share;/usr/share/wayland-sessions;/usr/share/xdg-desktop-portal")

set(CPACK_GENERATOR "TGZ;DEB;RPM")
include(CPack)
