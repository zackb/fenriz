# Compiles every SVG here into a GResource under /dev/fenriz/icons, which theme::install adds to the icon theme.
# Sets FENRIZ_ICONS_SRC to the generated source; shared by fenriz-desktop and fenriz-bar.
find_program(GLIB_COMPILE_RESOURCES glib-compile-resources REQUIRED)

set(FENRIZ_ICONS_DIR ${CMAKE_CURRENT_LIST_DIR})
file(GLOB FENRIZ_ICON_SVGS CONFIGURE_DEPENDS ${FENRIZ_ICONS_DIR}/*.svg)

set(xml "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<gresources>\n  <gresource prefix=\"/dev/fenriz/icons/scalable/actions\">\n")
foreach(svg ${FENRIZ_ICON_SVGS})
    get_filename_component(name ${svg} NAME)
    string(APPEND xml "    <file>${name}</file>\n")
endforeach()
string(APPEND xml "  </gresource>\n</gresources>\n")
set(FENRIZ_ICONS_XML ${CMAKE_CURRENT_BINARY_DIR}/icons.gresource.xml)
file(CONFIGURE OUTPUT ${FENRIZ_ICONS_XML} CONTENT "${xml}")

set(FENRIZ_ICONS_SRC ${CMAKE_CURRENT_BINARY_DIR}/icons-resources.c)
add_custom_command(OUTPUT ${FENRIZ_ICONS_SRC}
    COMMAND ${GLIB_COMPILE_RESOURCES} --generate-source --sourcedir=${FENRIZ_ICONS_DIR} --target=${FENRIZ_ICONS_SRC}
            ${FENRIZ_ICONS_XML}
    DEPENDS ${FENRIZ_ICONS_XML} ${FENRIZ_ICON_SVGS} VERBATIM)
