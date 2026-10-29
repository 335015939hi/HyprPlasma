// HyprPlasma - a Hyprland plugin template (CMake build system)
//
// Build:
//   cmake -B build -S . && cmake --build build
//
// Load (running Hyprland):
//   hyprctl plugin load ./build/libhyprplasma.so
//   hyprctl plugin unload hyprplasma
//
// The Hyprland plugin API passes C++ objects across the ABI boundary, so there
// is no ABI compatibility guarantee between Hyprland versions. Compile your
// plugin with the same compiler as Hyprland, ideally on the same machine, and
// rebuild it whenever you update Hyprland.
//
// Reference:
// https://wiki.hypr.land/hyprland-plugins/development/getting-started
//            /usr/include/x86_64-linux-gnu/hyprland/src/plugins/PluginAPI.hpp

#include <hyprland/src/helpers/Color.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>

#include <string>

#include "hyprplasma.hpp"

using namespace hyprplasma;

namespace hyprplasma {

// The handle Hyprland gives us at init; keep it for every API call.
HANDLE g_pluginHandle = nullptr;

} // namespace hyprplasma

/* -----------------------------------------------------------------------
 * REQUIRED entry points
 * --------------------------------------------------------------------- */

// Tell Hyprland which API version this plugin was compiled against.
// On mismatch Hyprland ejects the plugin. Do not change the signature.
APICALL EXPORT std::string pluginAPIVersion() { return HYPRLAND_API_VERSION; }

// Called when the plugin is loaded. Returns the plugin description.
APICALL EXPORT PLUGIN_DESCRIPTION_INFO pluginInit(HANDLE handle) {
  g_pluginHandle = handle;

  hyprplasma::log(Log::INFO, "loaded");

  hyprplasma::init();

  return PLUGIN_DESCRIPTION_INFO{
      .name = "hyprplasma",
      .description = "KDE Plasma compatibility for Hyprland",
      .author = "You",
      .version = "0.1.0",
  };
}

/* -----------------------------------------------------------------------
 * OPTIONAL entry points
 * --------------------------------------------------------------------- */

// Called when the user unloads the plugin. Not called if Hyprland ejects it
// due to a crash. All hooks dispatched by Hyprland are removed automatically.
APICALL EXPORT void pluginExit() {
  // TODO: release your resources here.
  hyprplasma::exit();
  g_pluginHandle = nullptr;
  hyprplasma::log(Log::INFO, "unloaded");
}
