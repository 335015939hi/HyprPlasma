#include "wayland.hpp"

#include "../hyprplasma.hpp"
#include "../protocols/windowmanagement.hpp"

#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/cli/Logger.hpp>

#include <wayland-server-core.h>

#include "hyprplasma-control-v1-server-protocol.h"

#include <algorithm>
#include <unordered_set>
#include <vector>

namespace hyprplasma::ipc {

namespace {

wl_global *g_global = nullptr;
std::unordered_set<wl_resource *> g_resources;

void resourceDestroyed(wl_resource *resource) { g_resources.erase(resource); }

void handleToggleShowDesktop(wl_client *, wl_resource *, uint32_t) {
  hyprplasma::proto::toggle_show_desktop();
}

const struct hyprplasma_control_v1_interface s_implementation = {
    .toggle_show_desktop = handleToggleShowDesktop,
};

void bindControl(wl_client *client, void *, uint32_t version, uint32_t id) {
  auto *resource =
      wl_resource_create(client, &hyprplasma_control_v1_interface,
                         static_cast<int>(std::min(version, 1u)), id);

  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &s_implementation, nullptr,
                                 resourceDestroyed);

  g_resources.insert(resource);
}

} // namespace

void init() {
  if (g_global)
    return;

  g_global = wl_global_create(g_pCompositor->m_wlDisplay,
                              &hyprplasma_control_v1_interface, 1, nullptr,
                              bindControl);

  if (!g_global) {
    hyprplasma::log(Log::ERR, "Failed to register hyprplasma_control_v1");
    return;
  }

  hyprplasma::log(Log::INFO, "Registered hyprplasma_control_v1");
}

void exit() {
  // Resource callbacks point into this plugin. Destroy all bound objects
  // before the shared library is unloaded.
  std::vector<wl_resource *> resources(g_resources.begin(), g_resources.end());

  for (auto *resource : resources)
    wl_resource_destroy(resource);

  g_resources.clear();

  if (g_global) {
    wl_global_destroy(g_global);
    g_global = nullptr;
  }
}

} // namespace hyprplasma::ipc
