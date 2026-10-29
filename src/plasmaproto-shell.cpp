
#include <hyprland/src/Compositor.hpp>
#include <wayland-server-core.h>

#include "plasma-shell-server-protocol.h"

#include "hyprplasma.hpp"

#include "plasmaproto-shell.hpp"

static struct wl_global *g_plasmaShell = nullptr;

static void bindPlasmaShell(wl_client *client, void *data, uint32_t version,
                            uint32_t id) {
  // Create the protocol resource and attach its request handlers here.
}

namespace hyprplasma {
namespace proto {
void plasma_shell_init(void) {
  g_plasmaShell = wl_global_create(g_pCompositor->m_wlDisplay,
                                   &org_kde_plasma_shell_interface, 8, nullptr,
                                   bindPlasmaShell);
  HyprlandAPI::addNotification(g_pluginHandle, "g_plasmaShell",
                               CHyprColor(0.0F, 1.0F, 0.0F, 1.0F), 3000.0F);
  if (g_plasmaShell == nullptr) {
    HyprlandAPI::addNotification(g_pluginHandle, "g_plasmaShell is NULL",
                                 CHyprColor(0.0F, 1.0F, 0.0F, 1.0F), 3000.0F);
  }
}
void plasma_shell_exit(void) {
  if (g_plasmaShell) {
    wl_global_destroy(g_plasmaShell);
    g_plasmaShell = nullptr;
  }
}
} // namespace proto
} // namespace hyprplasma
