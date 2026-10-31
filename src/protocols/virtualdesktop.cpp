
#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <Compositor.hpp>
#include <desktop/Workspace.hpp>
#include <event/EventBus.hpp>
#include <output/Monitor.hpp>
#include <state/WorkspaceState.hpp>
#include <wayland-server-core.h>

#include "plasma-virtual-desktop-server-protocol.h"

#include "../hyprplasma.hpp"
#include "virtualdesktop.hpp"

struct WorkspaceWatch {
  PHLWORKSPACEREF workspace;
  std::string id;
  CHyprSignalListener destroy;
};

struct VirtualDesktopState {
  wl_resource *resource = nullptr;
  PHLWORKSPACEREF workspace;
  std::string id;
  uint32_t lastPosition = UINT32_MAX;

  struct {
    CHyprSignalListener active;
    CHyprSignalListener renamed;
    CHyprSignalListener monitorChanged;
  } listeners;
};

static wl_global *g_plasmaVirtualDesktopManagement = nullptr;

static uint64_t g_nextDesktopId = 0;

static std::unordered_map<CWorkspace *, std::unique_ptr<WorkspaceWatch>>
    g_workspaces;

static std::unordered_set<wl_resource *> g_managers;

static std::unordered_map<wl_resource *, VirtualDesktopState *> g_desktops;

static CHyprSignalListener g_workspaceCreatedListener;

// Forward declarations
static void bindPlasmaVirtualDesktop(wl_client *client, void *data,
                                     uint32_t version, uint32_t id);

static void onWorkspaceDestroyed(CWorkspace *workspace);
static void registerWorkspace(PHLWORKSPACE workspace, bool announce);
static void updateDesktopPosition(VirtualDesktopState *state);
static void sendDesktopState(VirtualDesktopState *state);

// -----------------------------------------------------------------------------
// Workspace helpers
// -----------------------------------------------------------------------------

static std::string makeDesktopId() {
  return "hyprland-" + std::to_string(++g_nextDesktopId);
}

static WorkspaceWatch *findWorkspace(const std::string &id) {
  for (auto &[_, watch] : g_workspaces) {
    if (watch->id == id)
      return watch.get();
  }

  return nullptr;
}

static std::vector<WorkspaceWatch *> orderedWorkspaces() {
  std::vector<WorkspaceWatch *> result;

  for (auto &[_, watch] : g_workspaces) {
    auto workspace = watch->workspace.lock();

    if (!workspace || workspace->m_isSpecialWorkspace)
      continue;

    result.push_back(watch.get());
  }

  std::sort(result.begin(), result.end(),
            [](const WorkspaceWatch *a, const WorkspaceWatch *b) {
              return a->workspace.lock()->m_id < b->workspace.lock()->m_id;
            });

  return result;
}

static uint32_t workspacePosition(const WorkspaceWatch *watch) {
  const auto ordered = orderedWorkspaces();

  const auto it = std::find(ordered.begin(), ordered.end(), watch);

  if (it == ordered.end())
    return 0;

  return static_cast<uint32_t>(std::distance(ordered.begin(), it));
}

// -----------------------------------------------------------------------------
// Management events
// -----------------------------------------------------------------------------

static void sendManagerDone(wl_resource *resource) {
  org_kde_plasma_virtual_desktop_management_send_done(resource);
}

static void notifyWorkspaceCreated(WorkspaceWatch *watch) {
  const auto position = workspacePosition(watch);

  for (auto *manager : g_managers) {
    org_kde_plasma_virtual_desktop_management_send_desktop_created(
        manager, watch->id.c_str(), position);

    sendManagerDone(manager);
  }
}

static void notifyWorkspaceRemoved(const std::string &id) {
  for (auto *manager : g_managers) {
    org_kde_plasma_virtual_desktop_management_send_desktop_removed(manager,
                                                                   id.c_str());

    sendManagerDone(manager);
  }

  // Destroy child resources associated with this desktop.
  std::vector<wl_resource *> toDestroy;

  for (const auto &[resource, state] : g_desktops) {
    if (state && state->id == id) {
      org_kde_plasma_virtual_desktop_send_removed(resource);
      toDestroy.push_back(resource);
    }
  }

  for (auto *resource : toDestroy)
    wl_resource_destroy(resource);
}

static void updateAllDesktopPositions() {
  for (auto &[_, state] : g_desktops)
    updateDesktopPosition(state);
}

// -----------------------------------------------------------------------------
// Workspace lifecycle
// -----------------------------------------------------------------------------

static void onWorkspaceDestroyed(CWorkspace *workspace) {
  auto it = g_workspaces.find(workspace);

  if (it == g_workspaces.end())
    return;

  // Keep the record alive until this callback has finished.
  auto watch = std::move(it->second);
  g_workspaces.erase(it);

  const auto id = watch->id;

  notifyWorkspaceRemoved(id);
  updateAllDesktopPositions();

  watch->destroy.reset();
}

static void registerWorkspace(PHLWORKSPACE workspace, bool announce) {
  if (!workspace || workspace->m_isSpecialWorkspace)
    return;

  if (g_workspaces.contains(workspace.get()))
    return;

  auto watch = std::make_unique<WorkspaceWatch>();

  watch->workspace = workspace;
  watch->id = makeDesktopId();

  auto *raw = workspace.get();

  watch->destroy =
      workspace->m_events.destroy.listen([raw] { onWorkspaceDestroyed(raw); });

  auto *watchPtr = watch.get();

  g_workspaces.emplace(raw, std::move(watch));

  if (announce) {
    notifyWorkspaceCreated(watchPtr);
    updateAllDesktopPositions();
  }
}

// -----------------------------------------------------------------------------
// Desktop resource events
// -----------------------------------------------------------------------------

static void sendDesktopPosition(VirtualDesktopState *state) {
  if (!state || !state->resource)
    return;

  if (wl_resource_get_version(state->resource) < 3)
    return;

  auto *watch = findWorkspace(state->id);

  if (!watch)
    return;

  const auto position = workspacePosition(watch);

  if (position == state->lastPosition)
    return;

  state->lastPosition = position;

  org_kde_plasma_virtual_desktop_send_position(state->resource, position);
}

static void updateDesktopPosition(VirtualDesktopState *state) {
  if (!state)
    return;

  sendDesktopPosition(state);

  if (state->resource)
    org_kde_plasma_virtual_desktop_send_done(state->resource);
}

static void sendDesktopState(VirtualDesktopState *state) {
  if (!state || !state->resource)
    return;

  auto workspace = state->workspace.lock();

  if (!workspace)
    return;

  org_kde_plasma_virtual_desktop_send_desktop_id(state->resource,
                                                 state->id.c_str());

  org_kde_plasma_virtual_desktop_send_name(state->resource,
                                           workspace->m_name.c_str());

  sendDesktopPosition(state);

  if (workspace->m_visible) {
    org_kde_plasma_virtual_desktop_send_activated(state->resource);

    if (wl_resource_get_version(state->resource) >= 4) {
      auto monitor = workspace->m_monitor.lock();

      if (monitor) {
        org_kde_plasma_virtual_desktop_send_output_entered(
            state->resource, monitor->m_name.c_str());
      }
    }
  } else {
    org_kde_plasma_virtual_desktop_send_deactivated(state->resource);
  }

  org_kde_plasma_virtual_desktop_send_done(state->resource);
}

// -----------------------------------------------------------------------------
// Desktop resource requests
// -----------------------------------------------------------------------------

static void desktopRequestActivate(wl_client *, wl_resource *resource) {
  auto *state =
      static_cast<VirtualDesktopState *>(wl_resource_get_user_data(resource));

  if (!state)
    return;

  auto workspace = state->workspace.lock();

  if (!workspace)
    return;

  auto monitor = workspace->m_monitor.lock();

  if (!monitor)
    return;

  monitor->changeWorkspace(workspace);
}

static void desktopRequestEnterOutput(wl_client *, wl_resource *resource,
                                      const char *outputName) {
  auto *state =
      static_cast<VirtualDesktopState *>(wl_resource_get_user_data(resource));

  if (!state || !outputName)
    return;

  auto workspace = state->workspace.lock();

  if (!workspace)
    return;

  auto monitor = workspace->m_monitor.lock();

  if (!monitor)
    return;

  // This implementation currently supports activation on the
  // workspace's assigned monitor, not transferring workspaces.
  if (monitor->m_name != outputName) {
    hyprplasma::log(Log::WARN,
                    "Virtual desktop {} requested on unsupported output {}",
                    state->id, outputName);
    return;
  }

  monitor->changeWorkspace(workspace);
}

static const struct org_kde_plasma_virtual_desktop_interface
    s_virtualDesktopImpl = {
        .request_activate = desktopRequestActivate,
        .request_enter_output = desktopRequestEnterOutput,
};

// -----------------------------------------------------------------------------
// Desktop resource lifecycle
// -----------------------------------------------------------------------------

static void onVirtualDesktopDestroyed(wl_resource *resource) {
  auto *state =
      static_cast<VirtualDesktopState *>(wl_resource_get_user_data(resource));

  if (!state)
    return;

  g_desktops.erase(resource);

  state->listeners.active.reset();
  state->listeners.renamed.reset();
  state->listeners.monitorChanged.reset();

  delete state;
}

static void attachDesktopListeners(VirtualDesktopState *state) {
  auto workspace = state->workspace.lock();

  if (!workspace)
    return;

  state->listeners.active = workspace->m_events.activeChanged.listen([state] {
    auto workspace = state->workspace.lock();

    if (!workspace || !state->resource)
      return;

    if (workspace->m_visible) {
      org_kde_plasma_virtual_desktop_send_activated(state->resource);

      if (wl_resource_get_version(state->resource) >= 4) {
        auto monitor = workspace->m_monitor.lock();

        if (monitor) {
          org_kde_plasma_virtual_desktop_send_output_entered(
              state->resource, monitor->m_name.c_str());
        }
      }
    } else {
      org_kde_plasma_virtual_desktop_send_deactivated(state->resource);
    }

    org_kde_plasma_virtual_desktop_send_done(state->resource);
  });

  state->listeners.renamed = workspace->m_events.renamed.listen([state] {
    auto workspace = state->workspace.lock();

    if (!workspace || !state->resource)
      return;

    org_kde_plasma_virtual_desktop_send_name(state->resource,
                                             workspace->m_name.c_str());

    org_kde_plasma_virtual_desktop_send_done(state->resource);
  });

  state->listeners.monitorChanged =
      workspace->m_events.monitorChanged.listen([state] {
        auto workspace = state->workspace.lock();

        if (!workspace || !state->resource || !workspace->m_visible ||
            wl_resource_get_version(state->resource) < 4)
          return;

        auto monitor = workspace->m_monitor.lock();

        if (!monitor)
          return;

        org_kde_plasma_virtual_desktop_send_output_entered(
            state->resource, monitor->m_name.c_str());

        org_kde_plasma_virtual_desktop_send_done(state->resource);
      });
}

// -----------------------------------------------------------------------------
// Management resource requests
// -----------------------------------------------------------------------------

static void managerGetVirtualDesktop(wl_client *client, wl_resource *resource,
                                     uint32_t id, const char *desktopId) {
  if (!desktopId)
    return;

  auto *watch = findWorkspace(desktopId);

  if (!watch || !watch->workspace.lock()) {
    wl_resource_post_error(resource, 0, "Unknown virtual desktop: %s",
                           desktopId);
    return;
  }

  const auto version = std::min(
      static_cast<uint32_t>(wl_resource_get_version(resource)),
      static_cast<uint32_t>(org_kde_plasma_virtual_desktop_interface.version));

  auto *child =
      wl_resource_create(client, &org_kde_plasma_virtual_desktop_interface,
                         static_cast<int>(version), id);

  if (!child) {
    wl_client_post_no_memory(client);
    return;
  }

  auto *state = new (std::nothrow) VirtualDesktopState{};

  if (!state) {
    wl_resource_destroy(child);
    wl_client_post_no_memory(client);
    return;
  }

  state->resource = child;
  state->workspace = watch->workspace;
  state->id = watch->id;

  wl_resource_set_implementation(child, &s_virtualDesktopImpl, state,
                                 onVirtualDesktopDestroyed);

  g_desktops.emplace(child, state);

  attachDesktopListeners(state);
  sendDesktopState(state);
}

static void managerRequestCreateVirtualDesktop(wl_client *, wl_resource *,
                                               const char *name,
                                               uint32_t position) {
  hyprplasma::log(
      Log::WARN,
      "request_create_virtual_desktop: not implemented (name={}, position={})",
      name ? name : "", position);
}

static void managerRequestRemoveVirtualDesktop(wl_client *, wl_resource *,
                                               const char *desktopId) {
  hyprplasma::log(Log::WARN,
                  "request_remove_virtual_desktop: not implemented ({})",
                  desktopId ? desktopId : "");
}

static const struct org_kde_plasma_virtual_desktop_management_interface
    s_virtualDesktopManagementImpl = {
        .get_virtual_desktop = managerGetVirtualDesktop,
        .request_create_virtual_desktop = managerRequestCreateVirtualDesktop,
        .request_remove_virtual_desktop = managerRequestRemoveVirtualDesktop,
};

// -----------------------------------------------------------------------------
// Management resource lifecycle
// -----------------------------------------------------------------------------

static void onManagerDestroyed(wl_resource *resource) {
  g_managers.erase(resource);
}

static void bindPlasmaVirtualDesktop(wl_client *client, void *,
                                     uint32_t version, uint32_t id) {
  const auto supportedVersion = std::min(
      version,
      static_cast<uint32_t>(
          org_kde_plasma_virtual_desktop_management_interface.version));

  auto *resource = wl_resource_create(
      client, &org_kde_plasma_virtual_desktop_management_interface,
      static_cast<int>(supportedVersion), id);

  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &s_virtualDesktopManagementImpl,
                                 nullptr, onManagerDestroyed);

  g_managers.emplace(resource);

  // Initial desktop list.
  const auto workspaces = orderedWorkspaces();

  for (size_t i = 0; i < workspaces.size(); ++i) {
    const auto *watch = workspaces[i];

    org_kde_plasma_virtual_desktop_management_send_desktop_created(
        resource, watch->id.c_str(), static_cast<uint32_t>(i));
  }

  // One row of virtual desktops.
  if (supportedVersion >= 2) {
    org_kde_plasma_virtual_desktop_management_send_rows(resource, 1);
  }

  sendManagerDone(resource);
}

// -----------------------------------------------------------------------------
// Initialization / teardown
// -----------------------------------------------------------------------------

namespace hyprplasma {
namespace proto {

void plasma_virtualdesktop_init(void) {
  // Register existing workspaces.
  for (const auto &workspace : State::workspaceState()->workspacesCopy()) {
    registerWorkspace(workspace, false);
  }

  // Track future workspaces.
  g_workspaceCreatedListener = Event::bus()->m_events.workspace.created.listen(
      [](PHLWORKSPACEREF workspaceRef) {
        auto workspace = workspaceRef.lock();

        if (workspace)
          registerWorkspace(workspace, true);
      });

  g_plasmaVirtualDesktopManagement =
      wl_global_create(g_pCompositor->m_wlDisplay,
                       &org_kde_plasma_virtual_desktop_management_interface, 4,
                       nullptr, bindPlasmaVirtualDesktop);

  if (!g_plasmaVirtualDesktopManagement) {
    hyprplasma::log(Log::ERR,
                    "Failed to register Plasma virtual desktop management");

    plasma_virtualdesktop_exit();
  }
}

void plasma_virtualdesktop_exit(void) {
  if (g_plasmaVirtualDesktopManagement) {
    wl_global_destroy(g_plasmaVirtualDesktopManagement);
    g_plasmaVirtualDesktopManagement = nullptr;
  }

  g_workspaceCreatedListener.reset();

  // Destroy child resources before manager resources.
  std::vector<wl_resource *> desktops;
  for (const auto &[resource, _] : g_desktops)
    desktops.push_back(resource);

  for (auto *resource : desktops)
    wl_resource_destroy(resource);

  std::vector<wl_resource *> managers(g_managers.begin(), g_managers.end());

  for (auto *resource : managers)
    wl_resource_destroy(resource);

  g_workspaces.clear();
}

} // namespace proto
} // namespace hyprplasma
