cmake_minimum_required(VERSION 3.16)

project(
    hyprplasma
    LANGUAGES CXX
    VERSION 0.1.0
    DESCRIPTION "Hyprland plugin template built with CMake"
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
pkg_check_modules(HYPRAND REQUIRED IMPORTED_TARGET hyprland)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
add_library(${PROJECT_NAME} SHARED
    src/main.cpp
    src/hyprplasma.cpp
)

target_compile_options(${PROJECT_NAME} PRIVATE
    -Wall -Wextra -Wpedantic
)

# Headers and helper libraries (hyprlang, hyprutils, wayland-server, xkbcommon, …)
target_link_libraries(${PROJECT_NAME} PRIVATE
    PkgConfig::HYPRAND
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
