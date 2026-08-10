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

# ext-idle-notify-v1 has no library; generate the client stubs from the protocol XML.
find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
pkg_get_variable(WL_PROTO_DIR wayland-protocols pkgdatadir)
set(IDLE_XML ${WL_PROTO_DIR}/staging/ext-idle-notify/ext-idle-notify-v1.xml)
set(IDLE_HDR ${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-client-protocol.h)
set(IDLE_SRC ${CMAKE_CURRENT_BINARY_DIR}/ext-idle-notify-v1-protocol.c)
add_custom_command(OUTPUT ${IDLE_HDR}
    COMMAND ${WAYLAND_SCANNER} client-header ${IDLE_XML} ${IDLE_HDR}
    DEPENDS ${IDLE_XML} VERBATIM)
add_custom_command(OUTPUT ${IDLE_SRC}
    COMMAND ${WAYLAND_SCANNER} private-code ${IDLE_XML} ${IDLE_SRC}
    DEPENDS ${IDLE_XML} VERBATIM)

add_executable(fenriz-desktop
    src/main.cpp
    src/config.cpp
    src/background.cpp
    src/menu.cpp
    src/launcher.cpp
    src/frecency.cpp
    src/spawn.cpp
    src/auth.cpp
    src/lock.cpp
    src/idle.cpp
    src/polkit.cpp
    ${IDLE_HDR}
    ${IDLE_SRC}
)
target_include_directories(fenriz-desktop PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz-desktop PRIVATE PkgConfig::GTK4 PkgConfig::LAYER_SHELL PkgConfig::GIO_UNIX PkgConfig::WAYLAND PkgConfig::POLKIT pam)

include(GNUInstallDirs)
install(TARGETS fenriz-desktop RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES fenriz-desktop.conf.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz-desktop)

### IMPORTANT ###
# Not installed into /etc/pam.d automatically: a PAM file is a security surface and its
# contents are distro-specific. Packagers and users copy it deliberately.
install(FILES pam/fenriz-desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz-desktop/pam)

add_executable(desktop_config_test src/config.cpp src/test_config.cpp)
target_include_directories(desktop_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(desktop_config_test PRIVATE -UNDEBUG)

add_executable(desktop_menu_test src/menu.cpp src/config.cpp src/spawn.cpp src/test_menu.cpp)
target_include_directories(desktop_menu_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_menu_test PRIVATE PkgConfig::GTK4 PkgConfig::GIO_UNIX)
target_compile_options(desktop_menu_test PRIVATE -UNDEBUG)

add_executable(desktop_frecency_test src/frecency.cpp src/test_frecency.cpp)
target_include_directories(desktop_frecency_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_frecency_test PRIVATE PkgConfig::GTK4)
target_compile_options(desktop_frecency_test PRIVATE -UNDEBUG)

add_executable(desktop_auth_test src/auth.cpp src/test_auth.cpp)
target_include_directories(desktop_auth_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(desktop_auth_test PRIVATE PkgConfig::GTK4 pam)
target_compile_options(desktop_auth_test PRIVATE -UNDEBUG)

enable_testing()
add_test(NAME config   COMMAND desktop_config_test)
add_test(NAME menu     COMMAND desktop_menu_test)
add_test(NAME frecency COMMAND desktop_frecency_test)
add_test(NAME auth     COMMAND desktop_auth_test)
