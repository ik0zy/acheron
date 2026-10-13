# What sharing a screen and its sound needs on Linux beyond ffmpeg, normalized into a single
# INTERFACE target named `linuxcapture`. Each part is optional: a build without one compiles
# and offers that much less.
#   libpipewire-0.3  the portal's pictures, and what programs are playing
#   Qt DBus          asking the desktop portal for a screen or a window
#   xcb              listing an X11 display's screens and windows

add_library(linuxcapture INTERFACE)
if(NOT UNIX OR APPLE)
    return()
endif()

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PIPEWIRE QUIET IMPORTED_TARGET libpipewire-0.3)
    if(TARGET PkgConfig::PIPEWIRE)
        target_link_libraries(linuxcapture INTERFACE PkgConfig::PIPEWIRE)
        target_compile_definitions(linuxcapture INTERFACE ACHERON_HAVE_PIPEWIRE)
    endif()

    pkg_check_modules(XCB QUIET IMPORTED_TARGET xcb xcb-randr xcb-composite)
    if(TARGET PkgConfig::XCB)
        target_link_libraries(linuxcapture INTERFACE PkgConfig::XCB)
        target_compile_definitions(linuxcapture INTERFACE ACHERON_HAVE_XCB)
    endif()
endif()

find_package(Qt${QT_VERSION_MAJOR} QUIET COMPONENTS DBus)
if(TARGET Qt::DBus)
    target_link_libraries(linuxcapture INTERFACE Qt::DBus)
    target_compile_definitions(linuxcapture INTERFACE ACHERON_HAVE_DBUS)
endif()

message(STATUS "Linux capture: pipewire=${PIPEWIRE_FOUND} xcb=${XCB_FOUND} dbus=${Qt${QT_VERSION_MAJOR}DBus_FOUND}")
