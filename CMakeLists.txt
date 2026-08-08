cmake_minimum_required(VERSION 3.19)
project(fenriz VERSION 0.1.5 LANGUAGES CXX C)

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
    src/xwayland.cpp
    ${LAYER_SHELL_HEADER}
    ${OUTPUT_POWER_HEADER}
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

# A starting config for packaged installs; users copy it to ~/.config/fenriz/fenriz.conf.
install(FILES fenriz.conf.example
        DESTINATION ${CMAKE_INSTALL_DATADIR}/fenriz)

add_executable(fenriz_config_test src/config.cpp src/test_config.cpp)
target_include_directories(fenriz_config_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(fenriz_config_test PRIVATE PkgConfig::XKBCOMMON)

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

foreach(t fenriz_config_test fenriz_tiling_test fenriz_keybind_test fenriz_output_test fenrizctl_test)
    target_compile_options(${t} PRIVATE -UNDEBUG)
endforeach()

enable_testing()
add_test(NAME config  COMMAND fenriz_config_test)
add_test(NAME tiling  COMMAND fenriz_tiling_test)
add_test(NAME keybind COMMAND fenriz_keybind_test)
add_test(NAME output  COMMAND fenriz_output_test)
add_test(NAME ctl     COMMAND fenrizctl_test)

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
