
#include <algorithm>
#include <cstdint>
#include <debug/log/Logger.hpp>
#include <desktop/DesktopTypes.hpp>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/Window.hpp>
#include <hyprland/src/desktop/view/types/GeometricMovable.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/layout/LayoutManager.hpp>
#include <hyprland/src/layout/target/Target.hpp>
#include <hyprland/src/protocols/XDGShell.hpp>
#include <hyprland/src/protocols/core/Compositor.hpp>
#include <hyprland/src/protocols/core/Output.hpp>
#include <hyprland/src/render/Renderer.hpp>
#include <hyprutils/math/Vector2D.hpp>
#include <new>
#include <unordered_map>
#include <wayland-server-core.h>

#include "plasma-shell-server-protocol.h"

#include "../hyprplasma.hpp"
#include "windowmanagement.hpp"

#include "shell.hpp"

struct PlasmaSurfaceState {
  wl_resource *resource = nullptr;
  wl_resource *surface = nullptr;
  wl_listener surfaceDestroyListener{};
  bool roleSet = false;
  uint32_t role = 0;
  int32_t x = 0;
  int32_t y = 0;
  bool set_position_pending = false;
  bool positionSet = false;
  bool no_initial_focus = false;
  bool no_decorations = false;
  bool skip_taskbar = false;
  bool hide_when_showdesktop = true;
  bool will_cancel_showdesktop = true;
  bool should_float = false;
  SP<CWLSurfaceResource> hyprSurface;
  PHLMONITORREF monitor;
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
static CHyprSignalListener g_windowOpenListener;

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

static bool updateGeometry(PlasmaSurfaceState *state) {
  if (!state || !state->hyprSurface)
    return false;

  auto surface = Desktop::View::CWLSurface::fromResource(state->hyprSurface);
  if (!surface)
    return false;

  auto window = Desktop::View::CWindow::fromView(surface->view());
  if (!window || window->m_isX11)
    return false;

  auto xdg = window->m_xdgSurface.lock();
  auto target = window->layoutTarget();
  if (!xdg || !target || !target->space())
    return false;

  // Use committed XDG window geometry, not the raw buffer dimensions.
  auto size = xdg->m_current.geometry.size();

  if (size.x <= 0 || size.y <= 0)
    return false;

  auto box = target->position();

  // Preserve the existing position unless Plasma supplied one.
  auto pos = box.pos();
  if (state->set_position_pending) {
    pos = Vector2D{double(state->x), double(state->y)};
    // all paths currently succeed, that's why this can go here.
    state->set_position_pending = false;
  }

  CBox newBox{pos, size};

  if (box.pos() == newBox.pos() && box.size() == newBox.size())
    return true;

  target->setPositionGlobal(newBox, Layout::TARGET_UPDATE_NO_CLIENT_CONFIGURE);

  return true;
}

// returns whether to unshow the desktop
static bool openWindowListener_real(PHLWINDOW window) {
  bool cancel_showdesktop = true;
  if (!window || !window->wlSurface())
    return cancel_showdesktop;

  auto windowSurface = window->wlSurface()->resource();

  if (!windowSurface)
    return cancel_showdesktop;

  for (const auto &[_, state] : g_surfaces) {
    if (!state || !state->hyprSurface ||
        state->hyprSurface.get() != windowSurface.get())
      continue;

    // The normal layout placement may have overwritten our earlier move.
    if (state->positionSet) {
      state->set_position_pending = true;

      if (!updateGeometry(state)) {
        hyprplasma::log(Log::WARN,
                        "Could not apply initial position to Plasma surface");
        // set floating directly. going through the layout manager breaks things
        if (state->should_float) {
          window->m_isFloating = true;
        }
      } else {
        hyprplasma::log(Log::DEBUG, "Applied initial Plasma position {}, {}",
                        state->x, state->y);
      }
    }

    // remove window decorations
    if (state->no_decorations) {
      window->m_ruleApplicator->decorateOverride(
          Desktop::Types::COverridableVar(false,
                                          Desktop::Types::PRIORITY_SET_PROP));
    }
    // set no initial focus
    if (state->no_initial_focus) {
      window->m_noInitialFocus = true;
    }

    cancel_showdesktop = state->will_cancel_showdesktop;

    break;
  }
  return cancel_showdesktop;
}

static void openWindowListener(PHLWINDOW window) {
  bool cancel_showdesktop = openWindowListener_real(window);
  if (cancel_showdesktop) {
    hyprplasma::log(Log::INFO, "canceling showdesktop");
    hyprplasma::proto::clear_show_desktop();
  }
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

static void surfaceSetOutput(wl_client *, wl_resource *resource,
                             wl_resource *output) {
  hyprplasma::log(Log::INFO, "surfaceSetOutput {}", (void *)output);
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));

  if (!state)
    return;

  auto outputResource = CWLOutputResource::fromResource(output);

  if (!outputResource) {
    hyprplasma::log(Log::WARN,
                    "set_output: failed to resolve wl_output resource");
    return;
  }

  auto monitor = outputResource->m_monitor.lock();
  if (!monitor) {
    hyprplasma::log(Log::WARN, "set_output: monitor is unavailable");
    return;
  }

  state->monitor = outputResource->m_monitor;

  hyprplasma::log(Log::INFO, "set_output: assigned monitor");

  // Retry an earlier position request if one is pending.
  if (state->set_position_pending)
    updateGeometry(state);
}

static void surfaceSetPosition(wl_client *, wl_resource *resource, int32_t x,
                               int32_t y) {
  hyprplasma::log(Log::INFO, "surfaceSetPosition {} {}", x, y);
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));
  if (!state)
    return;

  state->x = x;
  state->y = y;

  state->set_position_pending = true;
  state->positionSet = true;
  updateGeometry(state);
}

static void surfaceSetRole(wl_client *, wl_resource *resource, uint32_t role) {
  hyprplasma::log(Log::INFO, "surfaceSetRole {}", role);
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));
  if (!state)
    return;
  if (state->roleSet) {
    return;
  }
  switch (role) {
  case ORG_KDE_PLASMA_SURFACE_ROLE_NORMAL: //=0
    state->hide_when_showdesktop = false;
    state->should_float = true;
    state->will_cancel_showdesktop = false;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_DESKTOP: //=1
    state->no_decorations = true;
    state->skip_taskbar = true;
    state->hide_when_showdesktop = false;
    state->will_cancel_showdesktop = false;
    state->no_initial_focus = true;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_PANEL: //=2
    state->no_decorations = true;
    state->hide_when_showdesktop = false;
    state->skip_taskbar = true;
    state->will_cancel_showdesktop = false;
    state->should_float = true;
    state->no_initial_focus = true;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_ONSCREENDISPLAY: //=3
    state->hide_when_showdesktop = false;
    state->skip_taskbar = true;
    state->should_float = true;
    state->will_cancel_showdesktop = false;
    state->no_initial_focus = true;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_NOTIFICATION: //=4
    state->skip_taskbar = true;
    state->no_initial_focus = true;
    state->should_float = true;
    state->will_cancel_showdesktop = false;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_TOOLTIP: //=5
    state->hide_when_showdesktop = false;
    state->skip_taskbar = true;
    state->should_float = true;
    state->no_decorations = true;
    state->will_cancel_showdesktop = false;
    state->no_initial_focus = true;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_CRITICALNOTIFICATION: //=6
    state->hide_when_showdesktop = false;
    state->should_float = true;
    state->skip_taskbar = true;
    state->will_cancel_showdesktop = false;
    state->no_initial_focus = true;
    break;
  case ORG_KDE_PLASMA_SURFACE_ROLE_APPLETPOPUP: //=7
    state->hide_when_showdesktop = false;
    state->should_float = true;
    state->no_initial_focus = true;
    state->will_cancel_showdesktop = false;
    break;
  default:
    hyprplasma::log(Log::ERR, "surfaceSetRole: invalid role {}", role);
    return;
  }
  state->role = role;
  state->roleSet = true;
  hyprplasma::log(Log::INFO, "found role {}", role);
}

static void surfaceSetPanelBehavior(wl_client *, wl_resource *,
                                    uint32_t behaviour) {
  hyprplasma::log(Log::INFO, "surfaceSetPanelBehavior {}", behaviour);
}

static void surfaceSetSkipTaskbar(wl_client *, wl_resource *resource,
                                  uint32_t skip) {
  hyprplasma::log(Log::INFO, "surfaceSetSkipTaskbar {}", skip);
  auto *state =
      static_cast<PlasmaSurfaceState *>(wl_resource_get_user_data(resource));

  if (!state)
    return;

  const bool next = skip != 0;

  if (state->skip_taskbar == next)
    return;

  state->skip_taskbar = next;

  // The surface may not belong to a mapped window yet.
  auto surface = Desktop::View::CWLSurface::fromResource(state->hyprSurface);
  if (!surface)
    return;

  auto window = Desktop::View::CWindow::fromView(surface->view());
  if (window)
    hyprplasma::proto::window_management_update_state(window.get());
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
    hyprplasma::log(Log::ERR, "Could not resolve Hyprland wl_surface");
    delete state;
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

    updateGeometry(state);

    auto hlSurface =
        Desktop::View::CWLSurface::fromResource(state->hyprSurface);
    if (hlSurface) {
      auto view = hlSurface->view();
      if (view) {
        auto window = Desktop::View::CWindow::fromView(view);
        if (window) {
          // set floating directly. going through the layout manager
          // breaks things
          if (state->should_float) {
            window->m_isFloating = true;
          }
          auto xdg = window->m_xdgSurface.lock();
          auto toplevel = xdg ? xdg->m_toplevel.lock() : nullptr;

          if (xdg && toplevel) {
            const auto geometry = xdg->m_current.geometry;
            const auto &limits = toplevel->m_current;
            hyprplasma::log(Log::DEBUG,
                            "Plasma geometry: size={}, origin={}, geometry={}, "
                            "min={}, max={}, floating={} role={}",
                            current.size, geometry.pos(), geometry.size(),
                            limits.minSize, limits.maxSize,
                            window->m_isFloating, state->role);
          }
        }
      }
    }
  });
}

namespace hyprplasma {
namespace proto {

bool plasma_surface_skips_taskbar(Desktop::View::CWindow *window) {
  if (!window || !window->wlSurface())
    return false;

  auto windowSurface = window->wlSurface()->resource();
  if (!windowSurface)
    return false;

  for (const auto &[_, state] : g_surfaces) {
    if (!state || !state->hyprSurface)
      continue;

    if (state->hyprSurface.get() == windowSurface.get())
      return state->skip_taskbar;
  }

  return false;
}
bool hide_when_show_desktop(Desktop::View::CWindow *window) {
  if (!window || !window->wlSurface())
    return true;
  auto windowSurface = window->wlSurface()->resource();
  if (!windowSurface)
    return true;
  for (const auto &[_, state] : g_surfaces) {
    if (!state || !state->hyprSurface)
      continue;
    if (state->hyprSurface.get() == windowSurface.get())
      return state->hide_when_showdesktop;
  }
  return true;
}

void plasma_shell_init(void) {
  g_plasmaShell = wl_global_create(g_pCompositor->m_wlDisplay,
                                   &org_kde_plasma_shell_interface, 8, nullptr,
                                   bindPlasmaShell);
  g_windowOpenListener =
      Event::bus()->m_events.window.open.listen(openWindowListener);

  if (g_plasmaShell == nullptr || g_windowOpenListener == nullptr) {
    HyprlandAPI::addNotification(g_pluginHandle,
                                 "org.kde.plasma.shell failed to register",
                                 CHyprColor(0.0F, 1.0F, 0.0F, 1.0F), 3000.0F);
    plasma_shell_exit();
  }
}
void plasma_shell_exit(void) {
  if (g_windowOpenListener) {
    g_windowOpenListener.reset();
  }

  std::vector<wl_resource *> resources;
  resources.reserve(g_surfaces.size());

  for (const auto &[_, state] : g_surfaces) {
    if (state && state->resource)
      resources.push_back(state->resource);
  }

  for (auto *resource : resources)
    wl_resource_destroy(resource);

  g_surfaces.clear();

  if (g_plasmaShell) {
    wl_global_destroy(g_plasmaShell);
    g_plasmaShell = nullptr;
  }
}
} // namespace proto
} // namespace hyprplasma
