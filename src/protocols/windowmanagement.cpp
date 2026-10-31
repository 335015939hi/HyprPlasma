
#include <Compositor.hpp>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <desktop/Workspace.hpp>
#include <desktop/state/FocusState.hpp>
#include <desktop/state/WindowState.hpp>
#include <desktop/view/Window.hpp>
#include <event/EventBus.hpp>
#include <layout/space/Space.hpp>
#include <layout/target/Target.hpp>
#include <memory>
#include <output/Monitor.hpp>
#include <string>
#include <sys/types.h>
#include <unistd.h>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wayland-server-core.h>

#include "plasma-window-management-server-protocol.h"

#include "../hyprplasma.hpp"
#include "virtualdesktop.hpp"
#include "windowmanagement.hpp"

using Desktop::View::CWindow;

// -----------------------------------------------------------------------------
// Protocol constants
// -----------------------------------------------------------------------------

static constexpr uint32_t MANAGEMENT_VERSION = 21;

namespace WindowState {
constexpr uint32_t ACTIVE = 0x000001;
constexpr uint32_t MINIMIZED = 0x000002;
constexpr uint32_t MAXIMIZED = 0x000004;
constexpr uint32_t FULLSCREEN = 0x000008;
constexpr uint32_t KEEP_ABOVE = 0x000010;
constexpr uint32_t KEEP_BELOW = 0x000020;
constexpr uint32_t ON_ALL_DESKTOPS = 0x000040;
constexpr uint32_t DEMANDS_ATTENTION = 0x000080;
constexpr uint32_t CLOSEABLE = 0x000100;
constexpr uint32_t MINIMIZABLE = 0x000200;
constexpr uint32_t MAXIMIZABLE = 0x000400;
constexpr uint32_t FULLSCREENABLE = 0x000800;
constexpr uint32_t SKIP_TASKBAR = 0x001000;
constexpr uint32_t MOVABLE = 0x008000;
constexpr uint32_t RESIZABLE = 0x010000;
constexpr uint32_t VIRTUAL_DESKTOP_CHANGEABLE = 0x020000;
} // namespace WindowState

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

struct WindowGeometry {
  int32_t x = 0;
  int32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  bool operator==(const WindowGeometry &) const = default;
};

struct WindowWatch {
  PHLWINDOWREF window;

  uint32_t legacyId = 0;
  std::string uuid;
  bool announced = false;

  std::unordered_set<wl_resource *> resources;

  std::string title;
  std::string appId;
  std::string desktopId;

  WindowGeometry geometry;
  uint32_t state = 0;

  bool geometryValid = false;
  bool stateValid = false;
  bool desktopValid = false;

  struct {
    CHyprSignalListener destroy;
    CHyprSignalListener unmap;
    CHyprSignalListener resize;
    CHyprSignalListener monitorChanged;
  } listeners;
};

struct WindowResourceData {
  // The UUID is stored rather than a WindowWatch pointer because the
  // window can unmap while the client still owns this resource.
  std::string uuid;
};

static wl_global *g_windowManagementGlobal = nullptr;
static uint32_t g_nextWindowId = 0;

static std::unordered_map<CWindow *, std::unique_ptr<WindowWatch>> g_windows;
static std::unordered_map<std::string, WindowWatch *> g_windowsByUuid;
static std::unordered_map<uint32_t, WindowWatch *> g_windowsByLegacyId;

static std::unordered_map<wl_resource *, std::unique_ptr<WindowResourceData>>
    g_windowResources;

static std::unordered_set<wl_resource *> g_managers;
static std::vector<std::string> g_lastStackingOrder;

static struct {
  CHyprSignalListener open;
  CHyprSignalListener active;
  CHyprSignalListener title;
  CHyprSignalListener appId;
  CHyprSignalListener moveToWorkspace;
  CHyprSignalListener pin;
  CHyprSignalListener urgent;
  CHyprSignalListener tick;
} g_listeners;

// -----------------------------------------------------------------------------
// Lookup helpers
// -----------------------------------------------------------------------------

static WindowWatch *findWindow(CWindow *window) {
  auto it = g_windows.find(window);
  return it == g_windows.end() ? nullptr : it->second.get();
}

static WindowWatch *findWindowByUuid(const std::string &uuid) {
  auto it = g_windowsByUuid.find(uuid);
  return it == g_windowsByUuid.end() ? nullptr : it->second;
}

static WindowWatch *findWindowByLegacyId(uint32_t id) {
  auto it = g_windowsByLegacyId.find(id);
  return it == g_windowsByLegacyId.end() ? nullptr : it->second;
}

// -----------------------------------------------------------------------------
// Window properties
// -----------------------------------------------------------------------------

static std::string desktopIdForWindow(CWindow *window) {
  if (!window)
    return {};

  auto workspace = window->m_workspace;

  // A special workspace is associated with the monitor's currently active
  // regular workspace for Pager purposes.
  if (workspace && workspace->m_isSpecialWorkspace) {
    auto monitor = window->m_monitor.lock();
    if (monitor)
      workspace = monitor->m_activeWorkspace;
  }

  return workspace ? hyprplasma::proto::virtualdesktop_id_for_workspace(
                         workspace.get())
                   : std::string{};
}

static WindowGeometry geometryForWindow(CWindow *window) {
  if (!window)
    return {};

  // Includes the window's decoration extents and uses compositor coordinates.
  const auto box = window->getFullWindowBoundingBox();

  return {
      .x = static_cast<int32_t>(std::lround(box.x)),
      .y = static_cast<int32_t>(std::lround(box.y)),
      .width = static_cast<uint32_t>(std::max(0L, std::lround(box.w))),
      .height = static_cast<uint32_t>(std::max(0L, std::lround(box.h))),
  };
}

static uint32_t stateForWindow(CWindow *window) {
  if (!window)
    return 0;

  uint32_t state = WindowState::CLOSEABLE | WindowState::MOVABLE |
                   WindowState::RESIZABLE |
                   WindowState::VIRTUAL_DESKTOP_CHANGEABLE;

  if (Desktop::focusState()->window() == window->m_self.lock())
    state |= WindowState::ACTIVE;

  if (window->m_pinned)
    state |= WindowState::ON_ALL_DESKTOPS;

  if (window->m_isUrgent)
    state |= WindowState::DEMANDS_ATTENTION;

  return state;
}

// -----------------------------------------------------------------------------
// Resource event helpers
// -----------------------------------------------------------------------------

static void sendTitle(WindowWatch *watch, CWindow *window) {
  if (!watch || !window)
    return;

  if (watch->title == window->m_title)
    return;

  watch->title = window->m_title;

  for (auto *resource : watch->resources)
    org_kde_plasma_window_send_title_changed(resource, watch->title.c_str());
}

static void sendAppId(WindowWatch *watch, CWindow *window) {
  if (!watch || !window)
    return;

  if (watch->appId == window->m_class)
    return;

  watch->appId = window->m_class;

  for (auto *resource : watch->resources)
    org_kde_plasma_window_send_app_id_changed(resource, watch->appId.c_str());
}

static void sendState(WindowWatch *watch, CWindow *window) {
  if (!watch || !window)
    return;

  const auto state = stateForWindow(window);

  if (watch->stateValid && watch->state == state)
    return;

  watch->state = state;
  watch->stateValid = true;

  for (auto *resource : watch->resources)
    org_kde_plasma_window_send_state_changed(resource, state);
}

static void sendGeometry(WindowWatch *watch, CWindow *window) {
  if (!watch || !window)
    return;

  const auto geometry = geometryForWindow(window);

  if (watch->geometryValid && watch->geometry == geometry)
    return;

  watch->geometry = geometry;
  watch->geometryValid = true;

  for (auto *resource : watch->resources) {
    if (wl_resource_get_version(resource) < 6)
      continue;

    org_kde_plasma_window_send_geometry(resource, geometry.x, geometry.y,
                                        geometry.width, geometry.height);
  }
}

static void sendDesktopMembership(WindowWatch *watch, CWindow *window) {
  if (!watch || !window)
    return;

  // An empty desktop list means "on all desktops" in this protocol.
  const std::string nextDesktop =
      window->m_pinned ? std::string{} : desktopIdForWindow(window);

  // Do not replace a valid membership with an accidental empty list if the
  // workspace has not been registered by the virtual-desktop module.
  if (!window->m_pinned && nextDesktop.empty())
    return;

  if (watch->desktopValid && watch->desktopId == nextDesktop)
    return;

  if (!watch->desktopId.empty()) {
    for (auto *resource : watch->resources) {
      if (wl_resource_get_version(resource) >= 8) {
        org_kde_plasma_window_send_virtual_desktop_left(
            resource, watch->desktopId.c_str());
      }
    }
  }

  if (!nextDesktop.empty()) {
    for (auto *resource : watch->resources) {
      if (wl_resource_get_version(resource) >= 8) {
        org_kde_plasma_window_send_virtual_desktop_entered(resource,
                                                           nextDesktop.c_str());
      }
    }
  }

  watch->desktopId = nextDesktop;
  watch->desktopValid = true;
}

static void updateWindow(WindowWatch *watch) {
  if (!watch || !watch->announced)
    return;

  auto window = watch->window.lock();
  if (!window || !window->m_isMapped)
    return;

  sendTitle(watch, window.get());
  sendAppId(watch, window.get());
  sendState(watch, window.get());
  sendGeometry(watch, window.get());
  sendDesktopMembership(watch, window.get());
}

static void sendInitialWindowState(WindowWatch *watch, wl_resource *resource) {
  if (!watch || !resource)
    return;

  auto window = watch->window.lock();
  if (!window || !window->m_isMapped)
    return;

  const auto version = wl_resource_get_version(resource);
  const auto geometry = geometryForWindow(window.get());
  const auto state = stateForWindow(window.get());
  const auto desktopId =
      window->m_pinned ? std::string{} : desktopIdForWindow(window.get());

  org_kde_plasma_window_send_title_changed(resource, window->m_title.c_str());

  org_kde_plasma_window_send_app_id_changed(resource, window->m_class.c_str());

  org_kde_plasma_window_send_state_changed(resource, state);

  if (version >= 6) {
    org_kde_plasma_window_send_geometry(resource, geometry.x, geometry.y,
                                        geometry.width, geometry.height);
  }

  if (version >= 8) {
    org_kde_plasma_window_send_pid_changed(
        resource, static_cast<uint32_t>(std::max<pid_t>(0, window->getPID())));

    if (!desktopId.empty()) {
      org_kde_plasma_window_send_virtual_desktop_entered(resource,
                                                         desktopId.c_str());
    }
  }

  // "mapped" is available in window-management protocol version 21.
  // Send it before initial_state so the client has the mapped flag when it
  // consumes the initial state.
  if (version >= 21)
    org_kde_plasma_window_send_mapped(resource);

  if (version >= 4)
    org_kde_plasma_window_send_initial_state(resource);

  watch->title = window->m_title;
  watch->appId = window->m_class;
  watch->state = state;
  watch->stateValid = true;
  watch->geometry = geometry;
  watch->geometryValid = true;
  watch->desktopId = desktopId;
  watch->desktopValid = true;
}

// -----------------------------------------------------------------------------
// Window lifecycle
// -----------------------------------------------------------------------------

static void notifyStackingOrderChanged();

static void unmapWindow(WindowWatch *watch) {
  if (!watch || !watch->announced)
    return;

  for (auto *resource : watch->resources)
    org_kde_plasma_window_send_unmapped(resource);

  g_windowsByUuid.erase(watch->uuid);
  g_windowsByLegacyId.erase(watch->legacyId);

  watch->resources.clear();
  watch->announced = false;

  watch->uuid.clear();
  watch->legacyId = 0;

  watch->title.clear();
  watch->appId.clear();
  watch->desktopId.clear();

  watch->geometryValid = false;
  watch->stateValid = false;
  watch->desktopValid = false;

  notifyStackingOrderChanged();
}

static void unmapWindow(CWindow *window) { unmapWindow(findWindow(window)); }

static void forgetWindow(CWindow *window) {
  auto it = g_windows.find(window);
  if (it == g_windows.end())
    return;

  unmapWindow(it->second.get());

  it->second->listeners.destroy.reset();
  it->second->listeners.unmap.reset();
  it->second->listeners.resize.reset();
  it->second->listeners.monitorChanged.reset();

  g_windows.erase(it);
  notifyStackingOrderChanged();
}

static void announceWindow(WindowWatch *watch) {
  if (!watch || watch->announced)
    return;

  auto window = watch->window.lock();
  if (!window || !window->m_isMapped)
    return;

  const auto id = ++g_nextWindowId;

  // The UUID is opaque to Plasma. It only needs to be unique and stable for
  // this mapping lifetime.
  watch->uuid = "hyprplasma-window-" + std::to_string(id);
  watch->legacyId = id;
  watch->announced = true;

  watch->title.clear();
  watch->appId.clear();
  watch->desktopId.clear();

  watch->geometryValid = false;
  watch->stateValid = false;
  watch->desktopValid = false;

  g_windowsByUuid.emplace(watch->uuid, watch);
  g_windowsByLegacyId.emplace(watch->legacyId, watch);

  for (auto *manager : g_managers) {
    org_kde_plasma_window_management_send_window_with_uuid(
        manager, watch->legacyId, watch->uuid.c_str());
  }

  notifyStackingOrderChanged();
}

static void registerWindow(PHLWINDOW window) {
  if (!window || !window->m_isMapped)
    return;

  auto *raw = window.get();

  if (auto *existing = findWindow(raw)) {
    // A CWindow can be unmapped and mapped again. Give the new mapping a
    // fresh UUID because Plasma removes its old window object on unmapped.
    announceWindow(existing);
    return;
  }

  auto watch = std::make_unique<WindowWatch>();
  watch->window = window;

  auto *watchPtr = watch.get();

  watch->listeners.destroy =
      window->m_events.destroy.listen([raw] { forgetWindow(raw); });

  watch->listeners.unmap =
      window->m_events.unmap.listen([raw] { unmapWindow(raw); });

  watch->listeners.resize = window->m_events.resize.listen([raw] {
    if (auto *watch = findWindow(raw))
      sendGeometry(watch, raw);
  });

  watch->listeners.monitorChanged =
      window->m_events.monitorChanged.listen([raw] {
        if (auto *watch = findWindow(raw)) {
          sendGeometry(watch, raw);
          sendDesktopMembership(watch, raw);
        }
      });

  g_windows.emplace(raw, std::move(watch));
  announceWindow(watchPtr);
}

// -----------------------------------------------------------------------------
// Stacking order
// -----------------------------------------------------------------------------

static std::vector<std::string> currentStackingOrder() {
  std::vector<std::string> result;

  for (const auto &window : Desktop::windowState()->windows()) {
    if (!window || !window->m_isMapped)
      continue;

    auto *watch = findWindow(window.get());
    if (!watch || !watch->announced)
      continue;

    result.push_back(watch->uuid);
  }

  // CWindowState stores windows from bottom to top. That is also the order
  // Pager needs for its rectangle z values.
  return result;
}

static void notifyStackingOrderChanged() {
  const auto order = currentStackingOrder();

  if (order == g_lastStackingOrder)
    return;

  g_lastStackingOrder = order;

  for (auto *manager : g_managers) {
    if (wl_resource_get_version(manager) >= 17)
      org_kde_plasma_window_management_send_stacking_order_changed_2(manager);
  }
}

static void sendStackingOrder(wl_client *client, wl_resource *manager,
                              uint32_t id) {
  const auto version = std::min(
      static_cast<uint32_t>(wl_resource_get_version(manager)),
      static_cast<uint32_t>(org_kde_plasma_stacking_order_interface.version));

  auto *resource =
      wl_resource_create(client, &org_kde_plasma_stacking_order_interface,
                         static_cast<int>(version), id);

  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  // This interface only sends events.
  wl_resource_set_implementation(resource, nullptr, nullptr, nullptr);

  for (const auto &uuid : currentStackingOrder()) {
    org_kde_plasma_stacking_order_send_window(resource, uuid.c_str());
  }

  org_kde_plasma_stacking_order_send_done(resource);
  wl_resource_destroy(resource);
}

// -----------------------------------------------------------------------------
// Window resource lifecycle
// -----------------------------------------------------------------------------

static void onWindowResourceDestroyed(wl_resource *resource) {
  auto it = g_windowResources.find(resource);
  if (it == g_windowResources.end())
    return;

  if (auto *watch = findWindowByUuid(it->second->uuid))
    watch->resources.erase(resource);

  g_windowResources.erase(it);
}

static void onManagerDestroyed(wl_resource *resource) {
  g_managers.erase(resource);
}

// -----------------------------------------------------------------------------
// Window requests
// -----------------------------------------------------------------------------

static WindowWatch *watchForResource(wl_resource *resource) {
  auto *data =
      static_cast<WindowResourceData *>(wl_resource_get_user_data(resource));

  return data ? findWindowByUuid(data->uuid) : nullptr;
}

static void windowRequestSetState(wl_client *, wl_resource *resource,
                                  uint32_t flags, uint32_t state) {
  auto *watch = watchForResource(resource);
  if (!watch)
    return;

  auto window = watch->window.lock();
  if (!window)
    return;

  if ((flags & WindowState::ACTIVE) && (state & WindowState::ACTIVE))
    window->activate();
}

static void windowRequestSetVirtualDesktop(wl_client *, wl_resource *,
                                           uint32_t) {
  // Deprecated numeric desktop API. Modern Plasma clients use desktop IDs.
}

static void windowRequestSetMinimizedGeometry(wl_client *, wl_resource *,
                                              wl_resource *, uint32_t, uint32_t,
                                              uint32_t, uint32_t) {}

static void windowRequestUnsetMinimizedGeometry(wl_client *, wl_resource *,
                                                wl_resource *) {}

static void windowRequestClose(wl_client *, wl_resource *resource) {
  auto *watch = watchForResource(resource);
  if (!watch)
    return;

  if (auto window = watch->window.lock())
    window->sendClose();
}

static void windowRequestMove(wl_client *, wl_resource *) {}

static void windowRequestResize(wl_client *, wl_resource *) {}

static void windowRequestDestroy(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

static void windowRequestGetIcon(wl_client *, wl_resource *, int32_t fd) {
  // We do not provide serialized QIcon data yet. Do not leak the received fd.
  if (fd >= 0)
    close(fd);
}

static void windowRequestEnterVirtualDesktop(wl_client *, wl_resource *resource,
                                             const char *desktopId) {
  if (!desktopId)
    return;

  auto *watch = watchForResource(resource);
  if (!watch)
    return;

  auto window = watch->window.lock();
  auto workspace =
      hyprplasma::proto::virtualdesktop_workspace_for_id(desktopId);

  if (!window || !workspace || workspace->m_isSpecialWorkspace ||
      !workspace->m_space)
    return;

  // Hyprland has one regular workspace per window. The Pager's drag operation
  // asks to enter the target desktop, then leave the source desktop.
  window->m_pinned = false;

  if (auto target = window->layoutTarget())
    target->assignToSpace(workspace->m_space);
}

static void windowRequestEnterNewVirtualDesktop(wl_client *, wl_resource *) {}

static void windowRequestLeaveVirtualDesktop(wl_client *, wl_resource *,
                                             const char *) {
  // The enter request above already moves the window to the target workspace.
  // Leaving the previous desktop is therefore implicit.
}

static void windowRequestEnterActivity(wl_client *, wl_resource *,
                                       const char *) {}

static void windowRequestLeaveActivity(wl_client *, wl_resource *,
                                       const char *) {}

static void windowRequestSendToOutput(wl_client *, wl_resource *,
                                      wl_resource *) {}

static const struct org_kde_plasma_window_interface s_windowImpl = {
    .set_state = windowRequestSetState,
    .set_virtual_desktop = windowRequestSetVirtualDesktop,
    .set_minimized_geometry = windowRequestSetMinimizedGeometry,
    .unset_minimized_geometry = windowRequestUnsetMinimizedGeometry,
    .close = windowRequestClose,
    .request_move = windowRequestMove,
    .request_resize = windowRequestResize,
    .destroy = windowRequestDestroy,
    .get_icon = windowRequestGetIcon,
    .request_enter_virtual_desktop = windowRequestEnterVirtualDesktop,
    .request_enter_new_virtual_desktop = windowRequestEnterNewVirtualDesktop,
    .request_leave_virtual_desktop = windowRequestLeaveVirtualDesktop,
    .request_enter_activity = windowRequestEnterActivity,
    .request_leave_activity = windowRequestLeaveActivity,
    .send_to_output = windowRequestSendToOutput,
};

static void createWindowResource(wl_client *client, wl_resource *manager,
                                 uint32_t id, WindowWatch *watch,
                                 const std::string &requestedUuid) {
  const auto version =
      std::min(static_cast<uint32_t>(wl_resource_get_version(manager)),
               static_cast<uint32_t>(org_kde_plasma_window_interface.version));

  auto *resource = wl_resource_create(client, &org_kde_plasma_window_interface,
                                      static_cast<int>(version), id);

  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  auto data = std::make_unique<WindowResourceData>();
  data->uuid = watch ? watch->uuid : requestedUuid;

  auto *rawData = data.get();

  g_windowResources.emplace(resource, std::move(data));

  wl_resource_set_implementation(resource, &s_windowImpl, rawData,
                                 onWindowResourceDestroyed);

  if (watch && watch->announced) {
    watch->resources.insert(resource);
    sendInitialWindowState(watch, resource);
  } else {
    // The window may have unmapped between window_with_uuid and
    // get_window_by_uuid. Give the client a valid, already-unmapped object.
    if (version >= 4)
      org_kde_plasma_window_send_initial_state(resource);

    org_kde_plasma_window_send_unmapped(resource);
  }
}

// -----------------------------------------------------------------------------
// Management requests
// -----------------------------------------------------------------------------

static void managerShowDesktop(wl_client *, wl_resource *, uint32_t) {
  // Show-desktop is not implemented yet.
}

static void managerGetWindow(wl_client *client, wl_resource *manager,
                             uint32_t id, uint32_t legacyId) {
  createWindowResource(client, manager, id, findWindowByLegacyId(legacyId), {});
}

static void managerGetWindowByUuid(wl_client *client, wl_resource *manager,
                                   uint32_t id, const char *uuid) {
  const std::string requestedUuid = uuid ? uuid : "";

  createWindowResource(client, manager, id, findWindowByUuid(requestedUuid),
                       requestedUuid);
}

static void managerGetStackingOrder(wl_client *client, wl_resource *manager,
                                    uint32_t id) {
  sendStackingOrder(client, manager, id);
}

static const struct org_kde_plasma_window_management_interface
    s_managementImpl = {
        .show_desktop = managerShowDesktop,
        .get_window = managerGetWindow,
        .get_window_by_uuid = managerGetWindowByUuid,
        .get_stacking_order = managerGetStackingOrder,
};

// -----------------------------------------------------------------------------
// Management binding
// -----------------------------------------------------------------------------

static void bindWindowManagement(wl_client *client, void *,
                                 uint32_t requestedVersion, uint32_t id) {
  const auto version = std::min(requestedVersion, MANAGEMENT_VERSION);

  auto *resource =
      wl_resource_create(client, &org_kde_plasma_window_management_interface,
                         static_cast<int>(version), id);

  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &s_managementImpl, nullptr,
                                 onManagerDestroyed);

  // Plasma's current task model requires version 17 for UUID-based windows
  // and the newer stacking-order mechanism.
  if (version < 17) {
    wl_resource_post_error(
        resource, 0,
        "org_kde_plasma_window_management requires version 17 or newer");
    return;
  }

  g_managers.insert(resource);

  org_kde_plasma_window_management_send_show_desktop_changed(resource, 0);

  for (const auto &window : Desktop::windowState()->windows()) {
    if (!window || !window->m_isMapped)
      continue;

    auto *watch = findWindow(window.get());
    if (!watch || !watch->announced)
      continue;

    org_kde_plasma_window_management_send_window_with_uuid(
        resource, watch->legacyId, watch->uuid.c_str());
  }

  // Prompt the client to request a fresh stacking-order object.
  org_kde_plasma_window_management_send_stacking_order_changed_2(resource);
}

// -----------------------------------------------------------------------------
// Periodic updates and event listeners
// -----------------------------------------------------------------------------

static void updateAllWindows() {
  for (auto &[_, watch] : g_windows)
    updateWindow(watch.get());

  notifyStackingOrderChanged();
}

namespace hyprplasma {
namespace proto {

void window_management_init(void) {
  // Register windows that already exist before the plugin was loaded.
  for (const auto &window : Desktop::windowState()->windows()) {
    if (window && window->m_isMapped)
      registerWindow(window);
  }

  // Track newly mapped windows.
  g_listeners.open = Event::bus()->m_events.window.open.listen(
      [](PHLWINDOW window) { registerWindow(window); });

  // Update properties which can change without a geometry/workspace change.
  g_listeners.active = Event::bus()->m_events.window.active.listen(
      [](PHLWINDOW, Desktop::eFocusReason) { updateAllWindows(); });

  g_listeners.title =
      Event::bus()->m_events.window.title.listen([](PHLWINDOW window) {
        if (auto *watch = findWindow(window.get()))
          sendTitle(watch, window.get());
      });

  g_listeners.appId =
      Event::bus()->m_events.window.class_.listen([](PHLWINDOW window) {
        if (auto *watch = findWindow(window.get()))
          sendAppId(watch, window.get());
      });

  g_listeners.moveToWorkspace =
      Event::bus()->m_events.window.moveToWorkspace.listen(
          [](PHLWINDOW window, PHLWORKSPACE) {
            if (auto *watch = findWindow(window.get()))
              updateWindow(watch);
          });

  g_listeners.pin =
      Event::bus()->m_events.window.pin.listen([](PHLWINDOW window) {
        if (auto *watch = findWindow(window.get()))
          updateWindow(watch);
      });

  g_listeners.urgent =
      Event::bus()->m_events.window.urgent.listen([](PHLWINDOW window) {
        if (auto *watch = findWindow(window.get()))
          sendState(watch, window.get());
      });

  // Hyprland does not expose a general position-changed signal on CWindow.
  // Compare geometry on ticks, but send Wayland events only when it changed.
  g_listeners.tick =
      Event::bus()->m_events.tick.listen([] { updateAllWindows(); });

  g_windowManagementGlobal = wl_global_create(
      g_pCompositor->m_wlDisplay, &org_kde_plasma_window_management_interface,
      MANAGEMENT_VERSION, nullptr, bindWindowManagement);

  if (!g_windowManagementGlobal) {
    hyprplasma::log(Log::ERR, "Failed to register Plasma window management");

    window_management_exit();
    return;
  }

  hyprplasma::log(Log::INFO, "Registered Plasma window management");
}

void window_management_exit(void) {
  if (g_windowManagementGlobal) {
    wl_global_destroy(g_windowManagementGlobal);
    g_windowManagementGlobal = nullptr;
  }

  g_listeners.open.reset();
  g_listeners.active.reset();
  g_listeners.title.reset();
  g_listeners.appId.reset();
  g_listeners.moveToWorkspace.reset();
  g_listeners.pin.reset();
  g_listeners.urgent.reset();
  g_listeners.tick.reset();

  // Destroy resources before the state objects they refer to.
  std::vector<wl_resource *> windowResources;
  windowResources.reserve(g_windowResources.size());

  for (const auto &[resource, _] : g_windowResources)
    windowResources.push_back(resource);

  for (auto *resource : windowResources)
    wl_resource_destroy(resource);

  std::vector<wl_resource *> managers(g_managers.begin(), g_managers.end());

  for (auto *resource : managers)
    wl_resource_destroy(resource);

  g_windows.clear();
  g_windowsByUuid.clear();
  g_windowsByLegacyId.clear();
  g_windowResources.clear();
  g_managers.clear();
  g_lastStackingOrder.clear();
}

} // namespace proto
} // namespace hyprplasma
