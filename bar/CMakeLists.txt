# fenriz-bar standalone. Shares fenriz-desktop's config, theme, blur and log by compiling its sources directly (HACK, TODO).
cmake_minimum_required(VERSION 3.19)
project(fenriz-bar VERSION 0.1.14 LANGUAGES CXX C)

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

add_executable(fenriz-bar
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

add_executable(bar_activity_test src/test_activity.cpp)
target_include_directories(bar_activity_test PRIVATE src)
target_compile_options(bar_activity_test PRIVATE -UNDEBUG)

add_executable(bar_spring_test src/test_spring.cpp)
target_include_directories(bar_spring_test PRIVATE src)
target_compile_options(bar_spring_test PRIVATE -UNDEBUG)

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
