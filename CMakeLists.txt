cmake_minimum_required(VERSION 3.19)
project(fenriz VERSION 0.1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(SCENEFX   REQUIRED IMPORTED_TARGET scenefx-0.5)
pkg_check_modules(WLROOTS   REQUIRED IMPORTED_TARGET wlroots-0.20)
pkg_check_modules(WAYLAND   REQUIRED IMPORTED_TARGET wayland-server)
pkg_check_modules(XKBCOMMON REQUIRED IMPORTED_TARGET xkbcommon)
pkg_check_modules(PIXMAN    REQUIRED IMPORTED_TARGET pixman-1)
pkg_check_modules(LIBINPUT  REQUIRED IMPORTED_TARGET libinput)
pkg_check_modules(XCB       REQUIRED IMPORTED_TARGET xcb xcb-icccm xcb-ewmh)

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

# Same story for wlr_output_power_management_v1.h (DPMS control).
set(OUTPUT_POWER_HEADER ${CMAKE_CURRENT_BINARY_DIR}/wlr-output-power-management-unstable-v1-protocol.h)
add_custom_command(
    OUTPUT ${OUTPUT_POWER_HEADER}
    COMMAND ${WAYLAND_SCANNER} server-header
            ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml
            ${OUTPUT_POWER_HEADER}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/protocols/wlr-output-power-management-unstable-v1.xml
    VERBATIM
)

add_executable(fenriz
    src/main.cpp
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
    src/xwayland.cpp
    ${LAYER_SHELL_HEADER}
    ${OUTPUT_POWER_HEADER}
)

# wlroots headers require this before inclusion
target_compile_definitions(fenriz PRIVATE WLR_USE_UNSTABLE)
target_include_directories(fenriz PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(fenriz PRIVATE
    # SceneFX is a superset fork of wlr_scene; both libs export wlr_scene_* symbols,
    # so scenefx must precede wlroots in link order to win symbol resolution.
    PkgConfig::SCENEFX
    PkgConfig::WLROOTS
    PkgConfig::WAYLAND
    PkgConfig::XKBCOMMON
    PkgConfig::PIXMAN
    PkgConfig::LIBINPUT
    PkgConfig::XCB
)

include(GNUInstallDirs)
install(TARGETS fenriz RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# Wayland session entry so greeters (greetd/tuigreet, gtkgreet, SDDM, GDM) can list fenriz.
# Generated so Exec is an absolute path to the installed binary (no reliance on PATH).
configure_file(fenriz.desktop.in fenriz.desktop @ONLY)
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/fenriz.desktop
        DESTINATION ${CMAKE_INSTALL_DATADIR}/wayland-sessions)

# Screen-sharing: route ScreenCast/RemoteDesktop to the wlr portal backend. Matched
# by the "fenriz" component of XDG_CURRENT_DESKTOP (set in Server::start).
install(FILES fenriz-portals.conf
        DESTINATION ${CMAKE_INSTALL_DATADIR}/xdg-desktop-portal)

# A starting config for packaged installs; users copy it to ~/.config/fenriz/fenriz.conf.
install(FILES fenriz.conf.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz)

# Self-checks (built, not installed). Pure logic, deliberately decoupled from the
# wlroots compositor stack so they build and run without a display.
#
# These are assert()-based, and a release build defines NDEBUG
add_executable(fenriz_config_test src/config.cpp src/test_config.cpp)
target_include_directories(fenriz_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_config_test PRIVATE PkgConfig::XKBCOMMON)

add_executable(fenriz_tiling_test src/test_tiling.cpp)
target_include_directories(fenriz_tiling_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The workspace-assignment policy (evacuate/restore) is pure by design; it links only
# output_policy.cpp, so the clamshell guarantee is checked without a compositor or a display.
add_executable(fenriz_output_test src/output_policy.cpp src/test_output.cpp)
target_include_directories(fenriz_output_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_output_test PRIVATE PkgConfig::WAYLAND)

add_executable(fenriz_keybind_test src/config.cpp src/test_keybind.cpp)
target_include_directories(fenriz_keybind_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_keybind_test PRIVATE PkgConfig::XKBCOMMON)

foreach(t fenriz_config_test fenriz_tiling_test fenriz_keybind_test fenriz_output_test)
    target_compile_options(${t} PRIVATE -UNDEBUG)
endforeach()

# Register the self-checks with CTest so `ctest` runs them and reports pass/fail.
enable_testing()
add_test(NAME config  COMMAND fenriz_config_test)
add_test(NAME tiling  COMMAND fenriz_tiling_test)
add_test(NAME keybind COMMAND fenriz_keybind_test)
add_test(NAME output  COMMAND fenriz_output_test)

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
set(CPACK_GENERATOR "TGZ")
include(CPack)
