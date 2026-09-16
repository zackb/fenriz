# fenriz-bar standalone. Shares fenriz-desktop's config, theme, blur and log by compiling its sources directly (HACK, TODO).
cmake_minimum_required(VERSION 3.19)
project(fenriz-bar VERSION 0.1.16 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(DESKTOP_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../desktop/src)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4        REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
pkg_check_modules(WAYLAND     REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(JSON_GLIB   REQUIRED IMPORTED_TARGET json-glib-1.0)
pkg_check_modules(GIO_UNIX    REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(WIREPLUMBER REQUIRED IMPORTED_TARGET wireplumber-0.5)
pkg_check_modules(LIBNM       REQUIRED IMPORTED_TARGET libnm)

find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WL_PROTO_DIR wayland-protocols pkgdatadir)

function(wayland_client_protocol out_var xml basename)
    set(hdr ${CMAKE_CURRENT_BINARY_DIR}/${basename}-client-protocol.h)
    set(src ${CMAKE_CURRENT_BINARY_DIR}/${basename}-protocol.c)
    add_custom_command(OUTPUT ${hdr}
        COMMAND ${WAYLAND_SCANNER} client-header ${xml} ${hdr}
        DEPENDS ${xml} VERBATIM)
    add_custom_command(OUTPUT ${src}
        COMMAND ${WAYLAND_SCANNER} private-code ${xml} ${src}
        DEPENDS ${xml} VERBATIM)
    set(${out_var} ${${out_var}} ${hdr} ${src} PARENT_SCOPE)
endfunction()

wayland_client_protocol(PROTO_SRC
    ${WL_PROTO_DIR}/staging/ext-background-effect/ext-background-effect-v1.xml ext-background-effect-v1)
wayland_client_protocol(PROTO_SRC
    ${WL_PROTO_DIR}/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml idle-inhibit-unstable-v1)

include(${CMAKE_CURRENT_SOURCE_DIR}/../desktop/icons/icons.cmake)

add_executable(fenriz-bar
    ${FENRIZ_ICONS_SRC}
    src/main.cpp
    src/bar.cpp
    src/island.cpp
    src/compositor.cpp
    src/audio.cpp
    src/audio_ui.cpp
    src/mpris.cpp
    src/media_ui.cpp
    src/upower.cpp
    src/power_ui.cpp
    src/sysstat.cpp
    src/system_ui.cpp
    src/inhibit.cpp
    src/bluetooth.cpp
    src/bluetooth_ui.cpp
    src/network.cpp
    src/wifi_ui.cpp
    src/plugin.cpp
    src/plugin_ui.cpp
    src/recorder.cpp
    src/recorder_ui.cpp
    src/tray.cpp
    src/tray_ui.cpp
    src/dbusmenu.cpp
    ${DESKTOP_SRC}/brightness.cpp
    ${DESKTOP_SRC}/config.cpp
    ${DESKTOP_SRC}/theme.cpp
    ${DESKTOP_SRC}/blur.cpp
    ${DESKTOP_SRC}/log.cpp
    ${PROTO_SRC}
)
target_include_directories(fenriz-bar PRIVATE src ${DESKTOP_SRC} ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz-bar PRIVATE PkgConfig::GTK4 PkgConfig::LAYER_SHELL PkgConfig::WAYLAND PkgConfig::JSON_GLIB PkgConfig::GIO_UNIX
                      PkgConfig::WIREPLUMBER PkgConfig::LIBNM)

include(GNUInstallDirs)
install(TARGETS fenriz-bar RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

add_executable(bar_compositor_test src/compositor.cpp src/test_compositor.cpp)
target_include_directories(bar_compositor_test PRIVATE src)
target_link_libraries(bar_compositor_test PRIVATE PkgConfig::JSON_GLIB PkgConfig::GIO_UNIX)
target_compile_options(bar_compositor_test PRIVATE -UNDEBUG)

add_executable(bar_mpris_test src/mpris.cpp src/test_mpris.cpp)
target_include_directories(bar_mpris_test PRIVATE src)
target_link_libraries(bar_mpris_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(bar_mpris_test PRIVATE -UNDEBUG)

add_executable(bar_sysstat_test src/sysstat.cpp src/upower.cpp src/test_sysstat.cpp)
target_include_directories(bar_sysstat_test PRIVATE src)
target_link_libraries(bar_sysstat_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(bar_sysstat_test PRIVATE -UNDEBUG)
target_compile_definitions(bar_sysstat_test PRIVATE FENRIZ_ICONS_DIR="${FENRIZ_ICONS_DIR}")

add_executable(bar_bluetooth_test src/bluetooth.cpp src/test_bluetooth.cpp)
target_include_directories(bar_bluetooth_test PRIVATE src)
target_link_libraries(bar_bluetooth_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(bar_bluetooth_test PRIVATE -UNDEBUG)

add_executable(bar_network_test src/network.cpp src/test_network.cpp)
target_include_directories(bar_network_test PRIVATE src)
target_link_libraries(bar_network_test PRIVATE PkgConfig::LIBNM)
target_compile_options(bar_network_test PRIVATE -UNDEBUG)

add_executable(bar_tray_test src/tray.cpp src/dbusmenu.cpp src/test_tray.cpp)
target_include_directories(bar_tray_test PRIVATE src)
target_link_libraries(bar_tray_test PRIVATE PkgConfig::GTK4)
target_compile_options(bar_tray_test PRIVATE -UNDEBUG)

add_executable(bar_plugin_test src/plugin.cpp src/test_plugin.cpp)
target_include_directories(bar_plugin_test PRIVATE src)
target_link_libraries(bar_plugin_test PRIVATE PkgConfig::JSON_GLIB PkgConfig::GIO_UNIX)
target_compile_options(bar_plugin_test PRIVATE -UNDEBUG)

add_executable(bar_activity_test src/test_activity.cpp)
target_include_directories(bar_activity_test PRIVATE src)
target_compile_options(bar_activity_test PRIVATE -UNDEBUG)

add_executable(bar_spring_test src/test_spring.cpp)
target_include_directories(bar_spring_test PRIVATE src)
target_compile_options(bar_spring_test PRIVATE -UNDEBUG)

add_executable(bar_recorder_test src/recorder.cpp src/test_recorder.cpp)
target_include_directories(bar_recorder_test PRIVATE src)
target_link_libraries(bar_recorder_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(bar_recorder_test PRIVATE -UNDEBUG)

add_executable(bar_share_test src/test_share.cpp)
target_include_directories(bar_share_test PRIVATE src)
target_compile_options(bar_share_test PRIVATE -UNDEBUG)

enable_testing()
add_test(NAME compositor COMMAND bar_compositor_test)
add_test(NAME spring     COMMAND bar_spring_test)
add_test(NAME mpris      COMMAND bar_mpris_test)
add_test(NAME sysstat    COMMAND bar_sysstat_test)
add_test(NAME activity   COMMAND bar_activity_test)
add_test(NAME bluetooth  COMMAND bar_bluetooth_test)
add_test(NAME network    COMMAND bar_network_test)
add_test(NAME tray       COMMAND bar_tray_test)
add_test(NAME share      COMMAND bar_share_test)
add_test(NAME plugin     COMMAND bar_plugin_test)
add_test(NAME recorder   COMMAND bar_recorder_test)

set(CPACK_SET_DESTDIR ON)
set(CPACK_PACKAGE_RELOCATABLE FALSE)
set(CPACK_PACKAGE_NAME "fenriz-bar")
set(CPACK_PACKAGE_VENDOR "zackb")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Status bar for the fenriz Wayland compositor")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT "zack@bartel.com")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/../LICENSE")
set(CPACK_RESOURCE_FILE_README "${CMAKE_CURRENT_SOURCE_DIR}/README.md")
# drop the -Linux suffix so the name matches the release tag
set(CPACK_PACKAGE_FILE_NAME "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}")

# gtk4-layer-shell and wireplumber have no stable package name across Debian/Fedora
# releases, so they are absent here. Arch is the supported path; deb/rpm are convenience.
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "zack@bartel.com")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "libgtk-4-1, libglib2.0-0, libwayland-client0, libjson-glib-1.0-0, libnm0")
set(CPACK_DEBIAN_PACKAGE_SECTION "x11")
set(CPACK_DEBIAN_PACKAGE_HOMEPAGE "https://github.com/zackb/fenriz")

set(CPACK_RPM_PACKAGE_REQUIRES "gtk4, glib2, libwayland-client, json-glib, NetworkManager-libnm")
set(CPACK_RPM_PACKAGE_LICENSE "MIT")
set(CPACK_RPM_PACKAGE_RELOCATABLE FALSE)

set(CPACK_GENERATOR "TGZ;DEB;RPM")
include(CPack)
