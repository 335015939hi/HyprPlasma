
#include <Compositor.hpp>
#include <algorithm>
#include <cstdint>
#include <desktop/Workspace.hpp>
#include <desktop/state/FocusState.hpp>
#include <event/EventBus.hpp>
#include <layout/space/Space.hpp>
#include <memory>
#include <output/Monitor.hpp>
#include <state/MonitorState.hpp>
#include <state/WorkspaceState.hpp>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
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
static std::vector<CWorkspace *> g_workspaceOrder;
static bool g_suppressWorkspaceCreated = false;
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
  result.reserve(g_workspaceOrder.size());

  for (auto *workspace : g_workspaceOrder) {
    auto it = g_workspaces.find(workspace);
    if (it == g_workspaces.end())
      continue;

    auto ws = it->second->workspace.lock();
    if (!ws || ws->m_isSpecialWorkspace)
      continue;

    result.push_back(it->second.get());
  }
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
  for (auto &[_, state] : g_desktops) {
    updateDesktopPosition(state);
  }
}

static void forgetWorkspace(CWorkspace *workspace) {
  auto it = g_workspaces.find(workspace);
  if (it == g_workspaces.end())
    return;

  const auto id = it->second->id;

  // Disconnect before erasing the listener's owner.
  it->second->destroy.reset();

  g_workspaces.erase(it);
  std::erase(g_workspaceOrder, workspace);

  notifyWorkspaceRemoved(id);
  updateAllDesktopPositions();
}

// -----------------------------------------------------------------------------
// Workspace lifecycle
// -----------------------------------------------------------------------------

static void onWorkspaceDestroyed(CWorkspace *workspace) {
  forgetWorkspace(workspace);
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
  g_workspaceOrder.push_back(raw);

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

  auto monitor = Desktop::focusState()->monitor();

  // Fall back to the first available monitor.
  if (!monitor) {
    const auto monitors = State::monitorState()->monitors();
    if (!monitors.empty())
      monitor = monitors.front();
  }

  if (!monitor) {
    hyprplasma::log(Log::ERR,
                    "Cannot create Plasma desktop: no available monitor");
    return;
  }

  const std::string desktopName = name && *name ? name : "New Desktop";

  // Hyprland allocates unique IDs for named workspaces.
  const auto workspaceID =
      State::workspaceState()->nextAvailableNamedWorkspace();

  // CWorkspace::create emits workspace.created synchronously.
  // We'll register this one ourselves so we can insert it at the
  // requested Plasma position.
  g_suppressWorkspaceCreated = true;

  auto workspace = State::workspaceState()->create(workspaceID, monitor->m_id,
                                                   desktopName, false);

  g_suppressWorkspaceCreated = false;

  if (!workspace) {
    hyprplasma::log(Log::ERR, "Failed to create Plasma virtual desktop '{}'",
                    desktopName);
    return;
  }

  // Keep an otherwise-empty desktop alive.
  workspace->setPersistent(true);

  registerWorkspace(workspace, false);

  auto *watch = g_workspaces.at(workspace.get()).get();

  // The new workspace was appended by registerWorkspace().
  // Move it to the requested position, clamped to [0, count].
  std::erase(g_workspaceOrder, workspace.get());

  const auto insertPosition =
      std::min<std::size_t>(position, g_workspaceOrder.size());

  g_workspaceOrder.insert(g_workspaceOrder.begin() + insertPosition,
                          workspace.get());

  hyprplasma::log(Log::INFO,
                  "Created Plasma virtual desktop '{}' (Hyprland workspace {}) "
                  "at position {}",
                  desktopName, workspaceID, insertPosition);

  // Announce the new desktop to every manager.
  notifyWorkspaceCreated(watch);

  // Existing desktop resources may have shifted positions.
  updateAllDesktopPositions();
}

static void managerRequestRemoveVirtualDesktop(wl_client *, wl_resource *,
                                               const char *desktopId) {

  if (!desktopId)
    return;

  auto *watch = findWorkspace(desktopId);
  if (!watch) {
    hyprplasma::log(Log::WARN,
                    "Unknown Plasma desktop requested for removal: {}",
                    desktopId);
    return;
  }

  auto workspace = watch->workspace.lock();
  if (!workspace || workspace->m_isSpecialWorkspace)
    return;

  const auto desktops = orderedWorkspaces();

  // Never remove the final desktop.
  if (desktops.size() <= 1) {
    hyprplasma::log(Log::WARN, "Refusing to remove the last virtual desktop");
    return;
  }

  auto monitor = workspace->m_monitor.lock();
  if (!monitor) {
    hyprplasma::log(Log::WARN,
                    "Cannot remove desktop {}: its monitor is unavailable",
                    desktopId);
    return;
  }

  const auto it = std::find(desktops.begin(), desktops.end(), watch);
  if (it == desktops.end())
    return;

  const auto index =
      static_cast<std::size_t>(std::distance(desktops.begin(), it));

  // Find the nearest remaining desktop on the same monitor.
  // Prefer the next desktop, then the previous one.
  PHLWORKSPACE replacement;

  for (std::size_t offset = 1; offset < desktops.size(); ++offset) {
    const auto nextIndex = (index + offset) % desktops.size();

    auto candidate = desktops[nextIndex]->workspace.lock();

    if (candidate && candidate != workspace &&
        candidate->m_monitor.lock() == monitor) {
      replacement = candidate;
      break;
    }
  }

  // Hyprland requires an active workspace on each monitor.
  // Refuse removal rather than leave this monitor without one.
  if (!replacement) {
    hyprplasma::log(
        Log::WARN,
        "Refusing to remove desktop {}: it is the only desktop on monitor {}",
        desktopId, monitor->m_name);
    return;
  }

  hyprplasma::log(
      Log::INFO,
      "Removing Plasma desktop {} and moving its windows to workspace {}",
      desktopId, replacement->m_id);

  // Switch away before removing an active workspace.
  if (monitor->m_activeWorkspace == workspace) {
    monitor->changeWorkspace(replacement, false, true, false);
  }

  // Take a copy because assignToSpace() modifies the old space's target list.
  std::vector<SP<Layout::ITarget>> targets;

  if (workspace->m_space) {
    for (const auto &targetRef : workspace->m_space->targets()) {
      if (auto target = targetRef.lock())
        targets.push_back(target);
    }
  }

  // Move all windows/layout targets to the replacement workspace.
  for (const auto &target : targets)
    target->assignToSpace(replacement->m_space);

  // Remove the protocol desktop and notify clients.
  // The workspace object itself will be destroyed when its remaining
  // references are released.
  forgetWorkspace(workspace.get());

  // Release the self-reference used to keep empty workspaces alive.
  workspace->setPersistent(false);
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
  auto workspaces = State::workspaceState()->workspacesCopy();
  std::sort(workspaces.begin(), workspaces.end(),
            [](const auto &a, const auto &b) { return a->m_id < b->m_id; });

  for (const auto &workspace : workspaces)
    registerWorkspace(workspace, false);

  // Track future workspaces.
  g_workspaceCreatedListener = Event::bus()->m_events.workspace.created.listen(
      [](PHLWORKSPACEREF workspaceRef) {
        if (g_suppressWorkspaceCreated)
          return;
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
  g_workspaceOrder.clear();
}

std::string virtualdesktop_id_for_workspace(CWorkspace *workspace) {
  if (!workspace)
    return {};

  auto it = g_workspaces.find(workspace);
  if (it == g_workspaces.end())
    return {};

  return it->second->id;
}

PHLWORKSPACE virtualdesktop_workspace_for_id(const std::string &id) {
  auto *watch = findWorkspace(id);
  return watch ? watch->workspace.lock() : nullptr;
}

} // namespace proto
} // namespace hyprplasma
