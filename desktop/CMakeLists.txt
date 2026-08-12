# fenriz-desktop standalone. The root fenriz CMakeLists deliberately does not add_subdirectory()
cmake_minimum_required(VERSION 3.19)
project(fenriz-desktop VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4        REQUIRED IMPORTED_TARGET gtk4)
pkg_check_modules(LAYER_SHELL REQUIRED IMPORTED_TARGET gtk4-layer-shell-0)
pkg_check_modules(GIO_UNIX   REQUIRED IMPORTED_TARGET gio-unix-2.0)
pkg_check_modules(WAYLAND    REQUIRED IMPORTED_TARGET wayland-client)
pkg_check_modules(POLKIT     REQUIRED IMPORTED_TARGET polkit-agent-1)

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
    ${WL_PROTO_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml ext-idle-notify-v1)
wayland_client_protocol(PROTO_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml
    wlr-output-power-management-unstable-v1)

add_executable(fenriz-desktop
    src/main.cpp
    src/config.cpp
    src/theme.cpp
    src/log.cpp
    src/background.cpp
    src/wallpaper.cpp
    src/wallpaper_picker.cpp
    src/brightness.cpp
    src/osd.cpp
    src/menu.cpp
    src/launcher.cpp
    src/frecency.cpp
    src/search.cpp
    src/spawn.cpp
    src/auth.cpp
    src/lock.cpp
    src/idle.cpp
    src/screensaver.cpp
    src/power.cpp
    src/polkit.cpp
    ${PROTO_SRC}
)
target_include_directories(fenriz-desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz-desktop PRIVATE PkgConfig::GTK4 PkgConfig::LAYER_SHELL PkgConfig::GIO_UNIX PkgConfig::WAYLAND PkgConfig::POLKIT pam)

include(GNUInstallDirs)
install(TARGETS fenriz-desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# The defaults fenriz-desktop falls back to when the user has no ~/.config/fenriz/fenriz-desktop.conf
set(FENRIZ_DESKTOP_DATADIR ${CMAKE_INSTALL_FULL_DATADIR}/fenriz-desktop)
configure_file(defaults/fenriz-desktop.conf.in fenriz-desktop.conf @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fenriz-desktop.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz-desktop)
install(FILES docs/img/fenriz-desktop-wallpaper.png
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz-desktop RENAME wallpaper.png)
target_compile_definitions(fenriz-desktop PRIVATE
        FENRIZ_DESKTOP_DATADIR="${FENRIZ_DESKTOP_DATADIR}"
        FENRIZ_DESKTOP_DEFAULT_CONFIG="${FENRIZ_DESKTOP_DATADIR}/fenriz-desktop.conf")

### IMPORTANT ###
# Only the password service is installed. Fingerprint and face hard-require pam_fprintd.so / pam_gaze.so, which may not exist
install(FILES pam/fenriz-desktop DESTINATION /etc/pam.d)
install(FILES pam/fenriz-desktop-fprint pam/fenriz-desktop-gaze
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz-desktop/pam)

add_executable(desktop_config_test src/config.cpp src/test_config.cpp)
target_include_directories(desktop_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(desktop_config_test PRIVATE -UNDEBUG)
target_compile_definitions(desktop_config_test PRIVATE
        FENRIZ_DESKTOP_DEFAULT_CONFIG="${CMAKE_CURRENT_BINARY_DIR}/fenriz-desktop.conf")

add_executable(desktop_menu_test src/menu.cpp src/config.cpp src/spawn.cpp src/test_menu.cpp)
target_include_directories(desktop_menu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_menu_test PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX)
target_compile_options(desktop_menu_test PRIVATE -UNDEBUG)

add_executable(desktop_frecency_test src/frecency.cpp src/test_frecency.cpp)
target_include_directories(desktop_frecency_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_frecency_test PRIVATE PkgConfig::GTK4)
target_compile_options(desktop_frecency_test PRIVATE -UNDEBUG)

add_executable(desktop_search_test src/search.cpp src/test_search.cpp)
target_include_directories(desktop_search_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_search_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(desktop_search_test PRIVATE -UNDEBUG)

add_executable(desktop_auth_test src/auth.cpp src/test_auth.cpp)
target_include_directories(desktop_auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_auth_test PRIVATE PkgConfig::GTK4 pam)
target_compile_options(desktop_auth_test PRIVATE -UNDEBUG)

add_executable(desktop_wallpaper_test src/wallpaper.cpp src/test_wallpaper.cpp)
target_include_directories(desktop_wallpaper_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_wallpaper_test PRIVATE PkgConfig::GTK4)
target_compile_options(desktop_wallpaper_test PRIVATE -UNDEBUG)

add_executable(desktop_brightness_test src/brightness.cpp src/test_brightness.cpp)
target_include_directories(desktop_brightness_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_brightness_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(desktop_brightness_test PRIVATE -UNDEBUG)

add_executable(desktop_screensaver_test src/screensaver.cpp src/test_screensaver.cpp)
target_include_directories(desktop_screensaver_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_screensaver_test PRIVATE PkgConfig::GIO_UNIX)
target_compile_options(desktop_screensaver_test PRIVATE -UNDEBUG)

enable_testing()
add_test(NAME config   COMMAND desktop_config_test)
add_test(NAME menu     COMMAND desktop_menu_test)
add_test(NAME frecency COMMAND desktop_frecency_test)
add_test(NAME search   COMMAND desktop_search_test)
add_test(NAME auth     COMMAND desktop_auth_test)
add_test(NAME brightness COMMAND desktop_brightness_test)
add_test(NAME wallpaper COMMAND desktop_wallpaper_test)
add_test(NAME screensaver COMMAND desktop_screensaver_test)
