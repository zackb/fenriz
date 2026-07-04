cmake_minimum_required(VERSION 3.19)
project(fenriz VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(WLROOTS   REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND   REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN    REQUIRED IMPORTED_TARGET pixman-1)

# wlroots' wlr_layer_shell_v1.h includes the generated wlr-layer-shell protocol
# server header, which this wlroots package does not ship. Generate it ourselves
# from the vendored XML with wayland-scanner.
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

add_executable(fenriz
    src/main.cpp
    src/server.cpp
    src/output.cpp
    src/view.cpp
    src/keyboard.cpp
    src/cursor.cpp
    src/tiling.cpp
    src/config.cpp
    src/layer.cpp
    src/decoration.cpp
    src/ipc.cpp
    ${LAYER_SHELL_HEADER}
)

# wlroots headers require this before inclusion
target_compile_definitions(fenriz PRIVATE WLR_USE_UNSTABLE)
target_include_directories(fenriz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz PRIVATE
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND
    PkgConfig::XKBCOMMON
    PkgConfig::PIXMAN
)

include(GNUInstallDirs)
install(TARGETS fenriz RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Wayland session entry so greeters (greetd/tuigreet, gtkgreet, SDDM, GDM) can list fenriz.
# Generated so Exec is an absolute path to the installed binary (no reliance on PATH).
configure_file(fenriz.desktop.in fenriz.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fenriz.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)

# Self-checks (built, not installed). Pure logic, deliberately decoupled from the
# wlroots compositor stack so they build and run without a display.
add_executable(fenriz_config_test src/config.cpp src/test_config.cpp)
target_include_directories(fenriz_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_config_test PRIVATE PkgConfig::XKBCOMMON)

add_executable(fenriz_tiling_test src/test_tiling.cpp)
target_include_directories(fenriz_tiling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(fenriz_keybind_test src/config.cpp src/test_keybind.cpp)
target_include_directories(fenriz_keybind_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_keybind_test PRIVATE PkgConfig::XKBCOMMON)
