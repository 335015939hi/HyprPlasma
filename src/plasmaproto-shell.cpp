
#include <hyprland/src/Compositor.hpp>
#include <wayland-server-core.h>

#include "plasma-shell-server-protocol.h"

#include "hyprplasma.hpp"

#include "plasmaproto-shell.hpp"

static struct wl_global *g_plasmaShell = nullptr;

static void handleGetSurface(wl_client *client, wl_resource *shellResource,
                             uint32_t id, wl_resource *surface);

static const struct org_kde_plasma_shell_interface s_shellImpl = {
    .get_surface = handleGetSurface,
};

static void bindPlasmaShell(wl_client *client, void *data, uint32_t version,
                            uint32_t id) {
  const auto supportedVersion = std::min(
      version, static_cast<uint32_t>(org_kde_plasma_shell_interface.version));

  auto *resource = wl_resource_create(client, &org_kde_plasma_shell_interface,
                                      static_cast<int>(supportedVersion), id);

  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &s_shellImpl, data, nullptr);
}

static void surfaceDestroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

static void surfaceSetOutput(wl_client *, wl_resource *, wl_resource *) {}

static void surfaceSetPosition(wl_client *, wl_resource *, int32_t, int32_t) {}

static void surfaceSetRole(wl_client *, wl_resource *, uint32_t) {}

static void surfaceSetPanelBehavior(wl_client *, wl_resource *, uint32_t) {}

static void surfaceSetSkipTaskbar(wl_client *, wl_resource *, uint32_t) {}

static void surfacePanelAutoHideHide(wl_client *, wl_resource *) {}

static void surfacePanelAutoHideShow(wl_client *, wl_resource *) {}

static void surfaceSetPanelTakesFocus(wl_client *, wl_resource *, uint32_t) {}

static void surfaceSetSkipSwitcher(wl_client *, wl_resource *, uint32_t) {}

static void surfaceOpenUnderCursor(wl_client *, wl_resource *) {}

static const struct org_kde_plasma_surface_interface s_surfaceImpl = {
    .destroy = surfaceDestroy,
    .set_output = surfaceSetOutput,
    .set_position = surfaceSetPosition,
    .set_role = surfaceSetRole,
    .set_panel_behavior = surfaceSetPanelBehavior,
    .set_skip_taskbar = surfaceSetSkipTaskbar,
    .panel_auto_hide_hide = surfacePanelAutoHideHide,
    .panel_auto_hide_show = surfacePanelAutoHideShow,
    .set_panel_takes_focus = surfaceSetPanelTakesFocus,
    .set_skip_switcher = surfaceSetSkipSwitcher,
    .open_under_cursor = surfaceOpenUnderCursor,
};

static void handleGetSurface(wl_client *client, wl_resource *shellResource,
                             uint32_t id, wl_resource *surface) {
  if (!surface) {
    return;
  }
  const auto version = std::min(wl_resource_get_version(shellResource),
                                org_kde_plasma_surface_interface.version);
  auto *resource = wl_resource_create(client, &org_kde_plasma_surface_interface,
                                      version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }
  wl_resource_set_implementation(resource, &s_surfaceImpl, nullptr, nullptr);
}

namespace hyprplasma {
namespace proto {
void plasma_shell_init(void) {
  g_plasmaShell = wl_global_create(g_pCompositor->m_wlDisplay,
                                   &org_kde_plasma_shell_interface, 8, nullptr,
                                   bindPlasmaShell);
  if (g_plasmaShell == nullptr) {
    HyprlandAPI::addNotification(g_pluginHandle,
                                 "org.kde.plasma.shell failed to register",
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
