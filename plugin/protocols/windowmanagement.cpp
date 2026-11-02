
#include <Compositor.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <config/ConfigManager.hpp>
#include <config/shared/animation/AnimationTree.hpp>
#include <cstdint>
#include <cstdlib>
#include <debug/log/Logger.hpp>
#include <desktop/Workspace.hpp>
#include <desktop/state/FocusState.hpp>
#include <desktop/state/WindowState.hpp>
#include <desktop/view/Window.hpp>
#include <event/EventBus.hpp>
#include <filesystem>
#include <fstream>
#include <layout/space/Space.hpp>
#include <layout/target/Target.hpp>
#include <memory>
#include <optional>
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
#include "shell.hpp"
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

  std::string iconName;
  bool iconValid = false;
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

static std::unordered_map<std::string, std::optional<std::string>>
    g_desktopIconCache;

// Hyprland 0.56.2 currently uses bits 0-2 for its own input-block reasons.
// Keep show-desktop blocking independent so clearing it does not clear other
// reasons owned by the compositor.
static constexpr auto INPUT_BLOCK_SHOW_DESKTOP =
    static_cast<Desktop::View::eWindowInputBlockReason>(1u << 3);

static bool g_showDesktop = false;
static PHLWINDOWREF g_showDesktopFocusedWindow;
static std::vector<PHLWINDOWREF> g_showDesktopHiddenWindows;

// Geometry has no general position-changed signal in this Hyprland version,
// so poll it separately at a bounded rate. Stacking is event-driven where
// possible, with a slower poll as a fallback for direct raise()/lower() calls.
static PHLWINDOWREF g_lastActiveWindow;
static std::chrono::steady_clock::time_point g_lastGeometryPoll{};
static std::chrono::steady_clock::time_point g_lastStackingPoll{};
static constexpr auto GEOMETRY_POLL_INTERVAL = std::chrono::milliseconds{33};
static constexpr auto STACKING_POLL_INTERVAL = std::chrono::milliseconds{250};

static struct {
  CHyprSignalListener open;
  CHyprSignalListener active;
  CHyprSignalListener title;
  CHyprSignalListener appId;
  CHyprSignalListener moveToWorkspace;
  CHyprSignalListener pin;
  CHyprSignalListener urgent;
  CHyprSignalListener floating;
  CHyprSignalListener fullscreen;
  CHyprSignalListener tick;
  CHyprSignalListener openLate;
} g_listeners;

// declarations
static void setShowDesktop(bool enabled);

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

namespace fs = std::filesystem;

static std::string trim(std::string value) {
  const auto first =
      std::find_if_not(value.begin(), value.end(),
                       [](unsigned char c) { return std::isspace(c); });

  const auto last =
      std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
      }).base();

  if (first >= last)
    return {};

  return std::string(first, last);
}

static std::string lower(std::string value) {
  std::transform(
      value.begin(), value.end(), value.begin(),
      [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return value;
}

// Read a key exclusively from the [Desktop Entry] group.
static std::optional<std::string> desktopEntryValue(const fs::path &path,
                                                    const std::string &key) {
  std::ifstream file(path);
  if (!file)
    return std::nullopt;

  bool inDesktopEntry = false;
  std::string line;

  while (std::getline(file, line)) {
    line = trim(std::move(line));

    if (line.empty() || line.starts_with('#'))
      continue;

    if (line.front() == '[') {
      inDesktopEntry = line == "[Desktop Entry]";
      continue;
    }

    if (!inDesktopEntry)
      continue;

    const auto separator = line.find('=');
    if (separator == std::string::npos)
      continue;

    if (trim(line.substr(0, separator)) != key)
      continue;

    auto value = trim(line.substr(separator + 1));
    if (value.empty())
      return std::nullopt;

    return value;
  }

  return std::nullopt;
}

static std::vector<fs::path> applicationDirectories() {
  std::vector<fs::path> result;

  auto add = [&](fs::path path) {
    if (path.empty())
      return;

    path /= "applications";

    if (std::find(result.begin(), result.end(), path) == result.end())
      result.push_back(std::move(path));
  };

  if (const char *dataHome = std::getenv("XDG_DATA_HOME");
      dataHome && *dataHome) {
    add(dataHome);
  } else if (const char *home = std::getenv("HOME"); home && *home) {
    add(fs::path(home) / ".local/share");
  }

  const char *dataDirs = std::getenv("XDG_DATA_DIRS");
  std::string dirs =
      dataDirs && *dataDirs ? dataDirs : "/usr/local/share:/usr/share";

  size_t start = 0;
  while (start <= dirs.size()) {
    const auto end = dirs.find(':', start);
    const auto dir = dirs.substr(
        start, end == std::string::npos ? std::string::npos : end - start);

    if (!dir.empty())
      add(dir);

    if (end == std::string::npos)
      break;

    start = end + 1;
  }

  // Flatpak exports are not necessarily included in XDG_DATA_DIRS.
  if (const char *home = std::getenv("HOME"); home && *home)
    add(fs::path(home) / ".local/share/flatpak/exports/share");

  add("/var/lib/flatpak/exports/share");

  return result;
}

static std::optional<std::string>
iconNameFromDesktopFile(const fs::path &path) {
  auto icon = desktopEntryValue(path, "Icon");
  if (!icon)
    return std::nullopt;

  // themed_icon_name_changed expects a theme icon name, not an arbitrary
  // absolute path. QIcon::fromTheme() is used by Plasma's client.
  if (icon->find('/') != std::string::npos)
    return std::nullopt;

  // Most desktop files use the extensionless theme name. Normalize the
  // occasional filename-style Icon= entry.
  const fs::path iconPath(*icon);
  if (iconPath.has_extension())
    *icon = iconPath.stem().string();

  return icon->empty() ? std::nullopt : icon;
}

static std::optional<std::string>
desktopIconForAppId(const std::string &appId) {
  if (appId.empty() || appId.find('/') != std::string::npos ||
      appId.find('\\') != std::string::npos)
    return std::nullopt;

  if (const auto it = g_desktopIconCache.find(appId);
      it != g_desktopIconCache.end())
    return it->second;

  const auto dirs = applicationDirectories();
  const auto desktopFile = appId + ".desktop";

  // First try the exact desktop-file ID. This is the common Wayland case.
  for (const auto &dir : dirs) {
    std::error_code ec;
    const auto path = dir / desktopFile;

    if (!fs::is_regular_file(path, ec))
      continue;

    if (auto icon = iconNameFromDesktopFile(path)) {
      g_desktopIconCache.emplace(appId, icon);
      return icon;
    }
  }

  // XWayland WM_CLASS values often differ from the desktop-file ID.
  // Search StartupWMClass as a fallback.
  const auto wantedClass = lower(appId);

  for (const auto &dir : dirs) {
    std::error_code ec;

    if (!fs::is_directory(dir, ec))
      continue;

    fs::recursive_directory_iterator it(
        dir, fs::directory_options::skip_permission_denied, ec);
    const fs::recursive_directory_iterator end;

    while (it != end) {
      if (ec) {
        ec.clear();
        it.increment(ec);
        continue;
      }

      const auto path = it->path();

      if (it->is_regular_file(ec) && path.extension() == ".desktop") {
        auto startupClass = desktopEntryValue(path, "StartupWMClass");

        if (startupClass && lower(*startupClass) == wantedClass) {
          if (auto icon = iconNameFromDesktopFile(path)) {
            g_desktopIconCache.emplace(appId, icon);
            return icon;
          }
        }
      }

      it.increment(ec);
    }
  }

  g_desktopIconCache.emplace(appId, std::nullopt);
  return std::nullopt;
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

  uint32_t state =
      WindowState::CLOSEABLE | WindowState::MOVABLE | WindowState::RESIZABLE |
      WindowState::VIRTUAL_DESKTOP_CHANGEABLE | WindowState::FULLSCREENABLE;

  if (Desktop::focusState()->window() == window->m_self.lock())
    state |= WindowState::ACTIVE;

  if (window->m_pinned)
    state |= WindowState::ON_ALL_DESKTOPS;

  if (hyprplasma::proto::plasma_surface_skips_taskbar(window))
    state |= WindowState::SKIP_TASKBAR;

  if (window->m_isUrgent)
    state |= WindowState::DEMANDS_ATTENTION;

  return state;
}

// -----------------------------------------------------------------------------
// Resource event helpers
// -----------------------------------------------------------------------------

static void sendTitle(WindowWatch *watch, CWindow *window) {
  if (!watch || !window || watch->resources.empty())
    return;

  if (watch->title == window->m_title)
    return;

  watch->title = window->m_title;

  for (auto *resource : watch->resources)
    org_kde_plasma_window_send_title_changed(resource, watch->title.c_str());
}

static void sendIconName(WindowWatch *watch, CWindow *window) {
  if (!watch || !window || watch->resources.empty())
    return;

  const auto resolved = desktopIconForAppId(window->m_class);
  const std::string nextIcon = resolved.value_or("");

  if (watch->iconValid && watch->iconName == nextIcon)
    return;

  const bool hadPreviousIcon = watch->iconValid;

  watch->iconName = nextIcon;
  watch->iconValid = true;

  // Don't send an unnecessary empty icon on the first lookup.
  // A null name clears a previously supplied themed icon.
  if (nextIcon.empty() && !hadPreviousIcon)
    return;

  const char *iconName = resolved ? resolved->c_str() : nullptr;
  for (auto *resource : watch->resources) {
    org_kde_plasma_window_send_themed_icon_name_changed(resource, iconName);
  }
}

static void sendAppId(WindowWatch *watch, CWindow *window) {
  if (!watch || !window || watch->resources.empty())
    return;

  if (watch->appId == window->m_class)
    return;

  watch->appId = window->m_class;

  for (auto *resource : watch->resources)
    org_kde_plasma_window_send_app_id_changed(resource, watch->appId.c_str());

  sendIconName(watch, window);
}

static void sendState(WindowWatch *watch, CWindow *window) {
  if (!watch || !window || watch->resources.empty())
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
  if (!watch || !window || watch->resources.empty())
    return;

  const bool hasGeometryResource =
      std::any_of(watch->resources.begin(), watch->resources.end(),
                  [](wl_resource *resource) {
                    return wl_resource_get_version(resource) >= 6;
                  });
  if (!hasGeometryResource)
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
  if (!watch || !window || watch->resources.empty())
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

  const auto iconName = desktopIconForAppId(window->m_class);
  if (iconName) {
    org_kde_plasma_window_send_themed_icon_name_changed(resource,
                                                        iconName->c_str());
  }
  watch->iconName = iconName.value_or("");
  watch->iconValid = true;

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

  watch->iconName.clear();
  watch->iconValid = false;

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
  // No client can consume this notification yet. The cache is initialized
  // when the first manager binds.
  if (g_managers.empty())
    return;

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

  if ((flags & WindowState::ACTIVE) && (state & WindowState::ACTIVE)) {
    if (g_showDesktop)
      setShowDesktop(false);

    window->activate();
  }
}

static void windowRequestSetVirtualDesktop(wl_client *, wl_resource *,
                                           uint32_t) {
  hyprplasma::log(Log::ERR,
                  "windowRequestSetVirtualDesktop deprecated, not implemented");
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

static void windowRequestMove(wl_client *, wl_resource *) {
  hyprplasma::log(Log::ERR, "windowRequestMove not implemented");
}

static void windowRequestResize(wl_client *, wl_resource *) {
  hyprplasma::log(Log::ERR, "windowRequestResize not implemented");
}

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

// Fade a window in or out using Hyprland's existing animation variable
// and the user's configured fadeIn/fadeOut animations. Unlike an immediate
// hidden-state toggle, this does not suspend the Wayland client.
static void setWindowHiddenWithAnimation(PHLWINDOW window, bool hidden) {
  if (!window || !window->m_isMapped)
    return;

  window->setInputBlocked(INPUT_BLOCK_SHOW_DESKTOP, hidden);

  auto &fade = window->alpha(Desktop::View::WINDOW_ALPHA_FADE);
  fade->setConfig(Config::animationTree()->getAnimationPropertyConfig(
      hidden ? "fadeOut" : "fadeIn"));
  *fade = hidden ? 0.F : 1.F;
}

static void hideWindowForShowDesktop(PHLWINDOW window) {
  if (!g_showDesktop || !window || !window->m_isMapped || window->isHidden())
    return;
  if (!hyprplasma::proto::hide_when_show_desktop(window.get())) {
    hyprplasma::log(Log::INFO, "not hiding a window");
    return;
  }

  // Avoid recording the same window twice.
  for (const auto &weak : g_showDesktopHiddenWindows) {
    if (weak.lock() == window)
      return;
  }

  // Preserve the desktop-state focus reason instead of relying on the
  // generic focus transition performed by setInputBlocked().
  if (Desktop::focusState()->window() == window) {
    Desktop::focusState()->fullWindowFocus(
        nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
  }

  g_showDesktopHiddenWindows.emplace_back(window);
  setWindowHiddenWithAnimation(window, true);
}

static void notifyShowDesktopChanged() {
  const uint32_t state = g_showDesktop ? 1 : 0;

  for (auto *manager : g_managers) {
    org_kde_plasma_window_management_send_show_desktop_changed(manager, state);
  }
}

static void setShowDesktop(bool enabled) {
  if (enabled == g_showDesktop)
    return;

  if (enabled) {
    hyprplasma::log(Log::INFO, "Showing desktop");
    // Save the focused window before clearing focus.
    g_showDesktopFocusedWindow = Desktop::focusState()->window();

    if (g_showDesktopFocusedWindow.lock()) {
      Desktop::focusState()->fullWindowFocus(
          nullptr, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    }

    g_showDesktop = true;

    // Hide every mapped window, including windows on inactive workspaces
    // and other monitors.
    for (const auto &window : Desktop::windowState()->windows()) {
      hideWindowForShowDesktop(window);
    }
  } else {
    hyprplasma::log(Log::INFO, "stopping Showing desktop");
    g_showDesktop = false;

    // Restore only the windows this implementation hid. Windows which were
    // already hidden before show-desktop was enabled remain hidden.
    auto hiddenWindows = std::move(g_showDesktopHiddenWindows);
    g_showDesktopHiddenWindows.clear();

    for (const auto &weak : hiddenWindows) {
      auto window = weak.lock();

      if (window && window->m_isMapped)
        setWindowHiddenWithAnimation(window, false);
    }

    // Restore focus only if the old window still exists and its workspace
    // is currently visible. Don't unexpectedly switch workspaces here.
    auto focused = g_showDesktopFocusedWindow.lock();
    g_showDesktopFocusedWindow.reset();

    if (focused && focused->m_isMapped && !focused->isHidden() &&
        focused->m_workspace && focused->m_workspace->m_visible) {
      Desktop::focusState()->fullWindowFocus(
          focused, Desktop::FOCUS_REASON_DESKTOP_STATE_CHANGE);
    }
  }

  notifyShowDesktopChanged();
}

// -----------------------------------------------------------------------------
// Management requests
// -----------------------------------------------------------------------------

static void managerShowDesktop(wl_client *, wl_resource *, uint32_t state) {
  setShowDesktop(state != 0);
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
  g_lastStackingOrder = currentStackingOrder();

  org_kde_plasma_window_management_send_show_desktop_changed(
      resource, g_showDesktop ? 1 : 0);

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

static void updateActiveWindowState() {
  // The active event is emitted for focus transitions. Only the previously
  // focused and newly focused windows can have their ACTIVE bit changed.
  auto previous = g_lastActiveWindow.lock();
  auto current = Desktop::focusState()->window();

  if (previous) {
    if (auto *watch = findWindow(previous.get()))
      sendState(watch, previous.get());
  }

  if (current && current != previous) {
    if (auto *watch = findWindow(current.get()))
      sendState(watch, current.get());
  }

  g_lastActiveWindow = current;
  notifyStackingOrderChanged();
}

static void pollWindowGeometries() {
  if (g_managers.empty())
    return;

  for (auto &[_, watch] : g_windows) {
    if (!watch || !watch->announced || watch->resources.empty())
      continue;

    auto window = watch->window.lock();
    if (!window || !window->m_isMapped)
      continue;

    sendGeometry(watch.get(), window.get());
  }
}

static void handleCompositorTick() {
  if (g_managers.empty())
    return;

  const auto now = std::chrono::steady_clock::now();

  // Position changes do not have a general CWindow signal in this Hyprland
  // version. Keep geometry fresh, but do not do it at render/tick frequency.
  if (g_lastGeometryPoll == std::chrono::steady_clock::time_point{} ||
      now - g_lastGeometryPoll >= GEOMETRY_POLL_INTERVAL) {
    g_lastGeometryPoll = now;
    pollWindowGeometries();
  }

  // CWindowState::raise()/lower() has no corresponding EventBus event.
  // Known stacking changes notify immediately; this slower poll catches
  // changes made through those unsignalled paths.
  if (g_lastStackingPoll == std::chrono::steady_clock::time_point{} ||
      now - g_lastStackingPoll >= STACKING_POLL_INTERVAL) {
    g_lastStackingPoll = now;
    notifyStackingOrderChanged();
  }
}

namespace hyprplasma {
namespace proto {

void clear_show_desktop(void) { setShowDesktop(false); }
void toggle_show_desktop(void) { setShowDesktop(!g_showDesktop); }

void window_management_update_state(Desktop::View::CWindow *window) {
  if (!window)
    return;

  if (auto *watch = findWindow(window))
    sendState(watch, window);
}

void window_management_init(void) {
  g_lastActiveWindow = Desktop::focusState()->window();
  g_lastGeometryPoll = {};
  g_lastStackingPoll = {};

  // Register windows that already exist before the plugin was loaded.
  for (const auto &window : Desktop::windowState()->windows()) {
    if (window && window->m_isMapped)
      registerWindow(window);
  }

  // Track newly mapped windows.
  g_listeners.open = Event::bus()->m_events.window.open.listen(
      [](PHLWINDOW window) { registerWindow(window); });

  // Focus only changes the ACTIVE state of the old and new focused windows.
  g_listeners.active = Event::bus()->m_events.window.active.listen(
      [](PHLWINDOW, Desktop::eFocusReason) { updateActiveWindowState(); });

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
              sendDesktopMembership(watch, window.get());
            notifyStackingOrderChanged();
          });

  g_listeners.pin =
      Event::bus()->m_events.window.pin.listen([](PHLWINDOW window) {
        if (auto *watch = findWindow(window.get())) {
          sendState(watch, window.get());
          sendDesktopMembership(watch, window.get());
        }
        notifyStackingOrderChanged();
      });

  g_listeners.urgent =
      Event::bus()->m_events.window.urgent.listen([](PHLWINDOW window) {
        if (auto *watch = findWindow(window.get()))
          sendState(watch, window.get());
      });

  // These transitions can affect the compositor's effective stacking order.
  g_listeners.floating = Event::bus()->m_events.window.floating.listen(
      [](PHLWINDOW) { notifyStackingOrderChanged(); });

  g_listeners.fullscreen = Event::bus()->m_events.window.fullscreen.listen(
      [](PHLWINDOW) { notifyStackingOrderChanged(); });

  // Only geometry and a low-frequency stacking fallback are polled. Metadata,
  // focus state, and desktop membership are updated by their own events.
  g_listeners.tick =
      Event::bus()->m_events.tick.listen([] { handleCompositorTick(); });

  g_windowManagementGlobal = wl_global_create(
      g_pCompositor->m_wlDisplay, &org_kde_plasma_window_management_interface,
      MANAGEMENT_VERSION, nullptr, bindWindowManagement);

  g_listeners.openLate = Event::bus()->m_events.window.openLate.listen(
      [](PHLWINDOW window) { hideWindowForShowDesktop(window); });

  if (!g_windowManagementGlobal) {
    hyprplasma::log(Log::ERR, "Failed to register Plasma window management");

    window_management_exit();
    return;
  }

  hyprplasma::log(Log::INFO, "Registered Plasma window management");
}

void window_management_exit(void) {
  if (g_showDesktop)
    setShowDesktop(false);
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
  g_listeners.floating.reset();
  g_listeners.fullscreen.reset();
  g_listeners.tick.reset();
  g_listeners.openLate.reset();

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
  g_lastActiveWindow.reset();
  g_lastGeometryPoll = {};
  g_lastStackingPoll = {};
  g_desktopIconCache.clear();
}

} // namespace proto
} // namespace hyprplasma
