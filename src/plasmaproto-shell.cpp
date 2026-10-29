
#include <algorithm>
#include <cstdint>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/types/GeometricMovable.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <new>
#include <unordered_map>
#include <wayland-server-core.h>

#include "plasma-shell-server-protocol.h"

#include "hyprplasma.hpp"

#include "plasmaproto-shell.hpp"

struct PlasmaSurfaceState {
  wl_resource *resource = nullptr;
  wl_resource *surface = nullptr;
  wl_listener surfaceDestroyListener{};
  bool roleSet = false;
  uint32_t role = 0;
  int32_t x = 0;
  bool set_position_pending = false;
  int32_t y = 0;
  SP<CWLSurfaceResource> hyprSurface;
  struct {
    CHyprSignalListener commit;
  } listeners;
};

static void handleGetSurface(wl_client *client, wl_resource *shellResource,
                             uint32_t id, wl_resource *surface);

static const struct org_kde_plasma_shell_interface s_shellImpl = {
    .get_surface = handleGetSurface,
};

static std::unordered_map<wl_resource *, PlasmaSurfaceState *> g_surfaces;

static struct wl_global *g_plasmaShell = nullptr;

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

static bool applyPosition(PlasmaSurfaceState *state) {
  if (!state) {
    return false;
  }

  state->set_position_pending = true;

  if (!state->hyprSurface) {
    return false;
  }

  // Resolve Hyprland's higher-level surface wrapper.
  auto hlSurface = Desktop::View::CWLSurface::fromResource(state->hyprSurface);
  if (!hlSurface)
    return false;

  // Find the compositor view associated with this surface.
  auto view = hlSurface->view();

  if (!view) {
    hyprplasma::log(Log::DEBUG, "Plasma surface has no compositor view yet");
    return false;
  }

  // Not every view supports arbitrary movement.
  auto movable = dynamicPointerCast<Desktop::View::IGeometricMovable>(view);
  if (!movable) {
    hyprplasma::log(Log::DEBUG, "Plasma surface view type is not movable");
    return false;
  }

  movable->move(
      Vector2D{static_cast<double>(state->x), static_cast<double>(state->y)});

  state->set_position_pending = false;
  return true;
}

static void onPlasmaSurfaceDestroyed(wl_resource *resource) {
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));
  if (!state)
    return;

  if (state->surface) {
    wl_list_remove(&state->surfaceDestroyListener.link);
    g_surfaces.erase(state->surface);
  }

  state->listeners.commit.reset();
  state->hyprSurface.reset();

  delete state;
}

static void onWlSurfaceDestroyed(wl_listener *, void *data) {
  auto *surface = static_cast<wl_resource *>(data);

  auto it = g_surfaces.find(surface);
  if (it == g_surfaces.end())
    return;

  auto *state = it->second;
  g_surfaces.erase(it);

  wl_list_remove(&state->surfaceDestroyListener.link);
  state->surface = nullptr;

  wl_resource_destroy(state->resource);
}

static void surfaceDestroy(wl_client *, wl_resource *resource) {
  hyprplasma::log(Log::INFO, "surfaceDestroy");
  wl_resource_destroy(resource);
}

static void surfaceSetOutput(wl_client *, wl_resource *, wl_resource *output) {
  hyprplasma::log(Log::INFO, "surfaceSetOutput {}", (void *)output);
}

static void surfaceSetPosition(wl_client *client, wl_resource *resource,
                               int32_t x, int32_t y) {
  hyprplasma::log(Log::INFO, "surfaceSetPosition {} {}", x, y);
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));
  if (!state)
    return;

  state->x = x;
  state->y = y;

  applyPosition(state);
}

static void surfaceSetRole(wl_client *, wl_resource *resource, uint32_t role) {
  hyprplasma::log(Log::INFO, "surfaceSetRole {}", role);
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));
  if (state->roleSet) {
    return;
  }
  switch (role) {
  case ORG_KDE_PLASMA_SURFACE_ROLE_NORMAL:
  case ORG_KDE_PLASMA_SURFACE_ROLE_DESKTOP:
  case ORG_KDE_PLASMA_SURFACE_ROLE_PANEL:
  case ORG_KDE_PLASMA_SURFACE_ROLE_ONSCREENDISPLAY:
  case ORG_KDE_PLASMA_SURFACE_ROLE_NOTIFICATION:
  case ORG_KDE_PLASMA_SURFACE_ROLE_TOOLTIP:
  case ORG_KDE_PLASMA_SURFACE_ROLE_CRITICALNOTIFICATION:
  case ORG_KDE_PLASMA_SURFACE_ROLE_APPLETPOPUP:
    state->role = role;
    state->roleSet = true;
    break;
  default:
    return;
  }
  hyprplasma::log(Log::INFO, "found role {}", role);
}

static void surfaceSetPanelBehavior(wl_client *, wl_resource *,
                                    uint32_t behaviour) {
  hyprplasma::log(Log::INFO, "surfaceSetPanelBehavior {}", behaviour);
}

static void surfaceSetSkipTaskbar(wl_client *, wl_resource *, uint32_t skip) {
  hyprplasma::log(Log::INFO, "surfaceSetSkipTaskbar {}", skip);
}

static void surfacePanelAutoHideHide(wl_client *, wl_resource *) {
  hyprplasma::log(Log::INFO, "surfacePanelAutoHideHide");
}

static void surfacePanelAutoHideShow(wl_client *, wl_resource *) {
  hyprplasma::log(Log::INFO, "surfacePanelAutoHideShow");
}

static void surfaceSetPanelTakesFocus(wl_client *, wl_resource *, uint32_t) {
  hyprplasma::log(Log::INFO, "surfaceSetPanelTakesFocus");
}

static void surfaceSetSkipSwitcher(wl_client *, wl_resource *, uint32_t skip) {
  hyprplasma::log(Log::INFO, "surfaceSetSkipSwitcher {}", skip);
}

static void surfaceOpenUnderCursor(wl_client *, wl_resource *) {
  hyprplasma::log(Log::INFO, "surfaceOpenUnderCursor");
}

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
  if (g_surfaces.contains(surface)) {
    wl_resource_post_error(shellResource, 0,
                           "wl_surface already has a Plasma surface");
    return;
  }

  const auto version =
      std::min(static_cast<uint32_t>(wl_resource_get_version(shellResource)),
               static_cast<uint32_t>(org_kde_plasma_surface_interface.version));

  auto *state = new (std::nothrow) PlasmaSurfaceState{};
  if (!state) {
    wl_client_post_no_memory(client);
    return;
  }

  auto *resource = wl_resource_create(client, &org_kde_plasma_surface_interface,
                                      static_cast<int>(version), id);
  if (!resource) {
    delete state;
    wl_client_post_no_memory(client);
    return;
  }

  auto hyprSurface = CWLSurfaceResource::fromResource(surface);
  if (!hyprSurface) {
    wl_resource_post_error(shellResource, 0,
                           "Could not resolve Hyprland wl_surface");
    return;
  }

  state->resource = resource;
  state->surface = surface;
  state->surfaceDestroyListener.notify = onWlSurfaceDestroyed;

  wl_resource_set_implementation(resource, &s_surfaceImpl, state,
                                 onPlasmaSurfaceDestroyed);

  g_surfaces.emplace(surface, state);
  wl_resource_add_destroy_listener(surface, &state->surfaceDestroyListener);

  state->hyprSurface = std::move(hyprSurface);

  state->listeners.commit = state->hyprSurface->m_events.commit.listen([state] {
    const auto &current = state->hyprSurface->m_current;

    // try move to correct position, if needed
    // BUG: adding the guard stops initial position working properly , don't
    // know why
    // if (state->set_position_pending) {
    applyPosition(state);
    // }
    // resize the window
    auto hlSurface =
        Desktop::View::CWLSurface::fromResource(state->hyprSurface);
    if (hlSurface) {
      auto view = hlSurface->view();
      if (view) {
        auto movable =
            dynamicPointerCast<Desktop::View::IGeometricMovable>(view);
        if (movable) {
          movable->resize(current.size);
        }
      }
    }

    hyprplasma::log(
        Log::INFO, "Plasma surface: role={}, mapped={}, size={}x{}, texture={}",
        state->role, state->hyprSurface->m_mapped, current.size.x,
        current.size.y, static_cast<bool>(current.texture));
  });
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
