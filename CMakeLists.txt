cmake_minimum_required(VERSION 3.20)

project(
    hyprplasma
    LANGUAGES CXX C
    VERSION 0.1.0
    DESCRIPTION "Hyprland plugin implementing KDE Plasma compatibility"
)

# Hyprland is built with a modern C++ standard. Plugins share C++ objects with
# Hyprland, so pick a standard compatible with the installed Hyprland headers.
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# Plugins are shared objects loaded into the Hyprland process.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/hyprplasma.cpp
    src/protocols/shell.cpp
    src/protocols/virtualdesktop.cpp
    src/protocols/windowmanagement.cpp
)

# ---------------------------------------------------------------------------
# Hyprland (from the system)
# ---------------------------------------------------------------------------
# Hyprland provides headers and helper libraries via pkg-config. There is no
# libhyprland.so (Hyprland is a monolithic executable); the HyprlandAPI
# symbols used by the plugin are resolved from the running Hyprland process
# at load time. We therefore only need the headers + helper libs from pkg-config.
#
# See: https://wiki.hypr.land/hyprland-plugins/development/getting-started
#      https://github.com/hyprwm/hyprland-plugins
find_package(PkgConfig REQUIRED)
pkg_check_modules(HYPRLAND REQUIRED IMPORTED_TARGET hyprland)

# ---------------------------------------------------------------------------
# wayland protocols
# ---------------------------------------------------------------------------
find_package(PlasmaWaylandProtocols CONFIG REQUIRED)
pkg_check_modules(WAYLAND_SERVER REQUIRED
    IMPORTED_TARGET wayland-server
)
find_program(WAYLAND_SCANNER
    NAMES wayland-scanner
    REQUIRED
)
# Select the protocol XML to implement.
set(
  PLASMA_PROTOCOLS
  plasma-window-management
  plasma-shell
  plasma-virtual-desktop
  # org-kde-plasma-virtual-desktop seems to be the same as plasma-virtual-desktop
)


set(PLASMA_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${PLASMA_GENERATED_DIR}")

foreach(protocol IN LISTS PLASMA_PROTOCOLS)
    set(xml
        "${PLASMA_WAYLAND_PROTOCOLS_DIR}/${protocol}.xml"
    )
    set(header
        "${PLASMA_GENERATED_DIR}/${protocol}-server-protocol.h"
    )
    set(code
        "${PLASMA_GENERATED_DIR}/${protocol}-protocol.c"
    )

    if(NOT EXISTS "${xml}")
        message(FATAL_ERROR "Protocol XML not found: ${xml}")
    endif()

    add_custom_command(
        OUTPUT "${header}" "${code}"

        COMMAND "${WAYLAND_SCANNER}"
            server-header "${xml}" "${header}"

        COMMAND "${WAYLAND_SCANNER}"
            private-code "${xml}" "${code}"

        DEPENDS "${xml}"
        VERBATIM
    )

    target_sources(${PROJECT_NAME} PRIVATE
        "${header}"
        "${code}"
    )
endforeach()

target_include_directories(${PROJECT_NAME} PRIVATE
    "${PLASMA_GENERATED_DIR}"
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::WAYLAND_SERVER
)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
)

# Headers and helper libraries (hyprlang, hyprutils, wayland-server, xkbcommon, …)
target_link_libraries(${PROJECT_NAME} PRIVATE
  PkgConfig::HYPRLAND
)

# Export the plugin entry-point symbols so Hyprland can dlsym() them.
# APICALL / EXPORT in PluginAPI.hpp already mark these default-visibility,
# but we make sure no visibility flags hide them.
set_target_properties(${PROJECT_NAME} PROPERTIES
    CXX_VISIBILITY_PRESET default
    VISIBILITY_INLINES_HIDDEN OFF
)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
# Install into a plugins directory. The plugin can be loaded from any path with
# `hyprctl plugin load /absolute/path/to/${PROJECT_NAME}.so`.
install(TARGETS ${PROJECT_NAME}
    ARCHIVE DESTINATION lib/hyprland/plugins
    LIBRARY DESTINATION lib/hyprland/plugins
    RUNTIME DESTINATION bin
)
