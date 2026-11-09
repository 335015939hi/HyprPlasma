#include "idle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <hyprland/src/Compositor.hpp>
#include <hyprland/src/desktop/state/WindowState.hpp>
#include <hyprland/src/desktop/view/LayerSurface.hpp>
#include <hyprland/src/desktop/view/WLSurface.hpp>
#include <hyprland/src/desktop/view/types/AlphaModifiable.hpp>
#include <hyprland/src/desktop/view/window/Window.hpp>
#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>
#include <hyprland/src/managers/input/InputManager.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprland/src/protocols/IdleInhibit.hpp>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <wayland-server-core.h>

#include "../hyprplasma.hpp"
#include "idle-server-protocol.h"

namespace {

using Clock = std::chrono::steady_clock;

class CIdleTimeout;
class CIdleAction;

static std::unordered_map<wl_resource *, std::unique_ptr<CIdleTimeout>>
    g_timeouts;
static std::unordered_map<uint64_t, std::unique_ptr<CIdleAction>> g_actions;
static std::unordered_set<wl_resource *> g_managerResources;
static wl_global *g_idleGlobal = nullptr;
static SP<Event::CEventBus::CCustomEvent> g_idleEvent;
static SP<Event::CEventBus::CCustomEvent> g_idleActionEvent;

static Clock::time_point g_lastActivity = Clock::now();
static uint64_t g_nextActionId = 1;
static bool g_manualInhibited = false;
static bool g_effectiveInhibited = false;
static bool g_lastReportedIdle = false;
static bool g_lastReportedInhibited = false;
static uint32_t g_nativeInhibitorCount = 0;
static bool g_initialized = false;

uint32_t elapsedIdleMs();
void emitState(bool force = false);
void notifyActivity();
void recalculateInhibition();

class CIdleTimeout {
public:
  CIdleTimeout(wl_resource *resource, uint32_t timeoutMs, uint32_t elapsedMs,
               bool inhibited)
      : m_resource(resource), m_timeoutMs(timeoutMs), m_inhibited(inhibited) {
    m_timer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](SP<CEventLoopTimer>, void *) {
          if (!m_resource || m_idle || m_inhibited)
            return;

          m_idle = true;
          hyprplasma::log(Log::DEBUG, "idle: timeout {} ms fired", m_timeoutMs);
          org_kde_kwin_idle_timeout_send_idle(m_resource);
          emitState();
        },
        nullptr);

    g_pEventLoopManager->addTimer(m_timer);

    if (!m_inhibited)
      update(elapsedMs);
  }

  ~CIdleTimeout() {
    if (m_timer && g_pEventLoopManager)
      g_pEventLoopManager->removeTimer(m_timer);
  }

  void reset() {
    if (m_inhibited)
      return;
    update(0);
  }

  void setInhibited(bool inhibited) {
    if (m_inhibited == inhibited)
      return;

    m_inhibited = inhibited;
    if (m_inhibited) {
      m_timer->updateTimeout(std::nullopt);
      hyprplasma::log(Log::DEBUG, "idle: paused timeout {} ms", m_timeoutMs);
    } else {
      // KWin's FollowsInhibitors mode restarts the timer but does not emit
      // resumed merely because inhibition ended. Real input emits resumed.
      m_timer->updateTimeout(std::chrono::milliseconds(m_timeoutMs));
      hyprplasma::log(Log::DEBUG, "idle: restarted timeout {} ms", m_timeoutMs);
    }
  }

  bool isIdle() const { return m_idle; }
  bool isPending() const { return !m_idle && !m_inhibited; }
  bool isInhibited() const { return m_inhibited; }
  uint32_t timeoutMs() const { return m_timeoutMs; }
  uint32_t objectId() const {
    return m_resource ? wl_resource_get_id(m_resource) : 0;
  }

private:
  void update(uint32_t elapsedMs) {
    if (!m_resource || !m_timer || m_inhibited)
      return;

    m_timer->updateTimeout(std::nullopt);

    if (elapsedMs < m_timeoutMs) {
      if (m_idle) {
        m_idle = false;
        hyprplasma::log(Log::DEBUG, "idle: resumed timeout {} ms", m_timeoutMs);
        org_kde_kwin_idle_timeout_send_resumed(m_resource);
        emitState();
      }

      m_timer->updateTimeout(
          std::chrono::milliseconds(m_timeoutMs - elapsedMs));
    } else if (!m_idle) {
      m_idle = true;
      hyprplasma::log(Log::DEBUG, "idle: timeout {} ms fired", m_timeoutMs);
      org_kde_kwin_idle_timeout_send_idle(m_resource);
      emitState();
    }
  }

  wl_resource *m_resource = nullptr;
  uint32_t m_timeoutMs = 0;
  bool m_idle = false;
  bool m_inhibited = false;
  SP<CEventLoopTimer> m_timer;
};

class CIdleAction {
public:
  CIdleAction(uint64_t id, uint32_t timeoutMs, std::string command)
      : m_id(id), m_timeoutMs(timeoutMs), m_command(std::move(command)),
        m_inhibited(g_effectiveInhibited) {
    m_timer = makeShared<CEventLoopTimer>(
        std::nullopt, [this](SP<CEventLoopTimer>, void *) { fire(); }, nullptr);
    g_pEventLoopManager->addTimer(m_timer);
  }

  ~CIdleAction() {
    if (m_timer && g_pEventLoopManager)
      g_pEventLoopManager->removeTimer(m_timer);
  }

  void arm(uint32_t elapsedMs) {
    if (m_inhibited || m_fired)
      return;

    const auto remaining =
        elapsedMs < m_timeoutMs ? m_timeoutMs - elapsedMs : 0;
    m_timer->updateTimeout(std::chrono::milliseconds(remaining));
  }

  void activity() {
    m_fired = false;
    if (!m_inhibited)
      arm(0);
  }

  void setInhibited(bool inhibited) {
    if (m_inhibited == inhibited)
      return;

    m_inhibited = inhibited;
    if (m_inhibited) {
      m_timer->updateTimeout(std::nullopt);
    } else if (!m_fired) {
      // Start a complete idle interval after the last inhibitor disappears.
      arm(0);
    }
  }

  bool hasFired() const { return m_fired; }
  bool isPending() const { return !m_fired && !m_inhibited; }
  bool isInhibited() const { return m_inhibited; }
  uint32_t timeoutMs() const { return m_timeoutMs; }

private:
  void fire() {
    if (m_fired || m_inhibited)
      return;

    m_fired = true;
    hyprplasma::log(Log::INFO, "idle: running action {} after {} ms", m_id,
                    m_timeoutMs);

    // Emit an action event instead of calling invokeHyprctlCommand here:
    // that API is synchronous IPC and calling it from the compositor's own
    // event loop can deadlock. Lua can safely hand the command to hl.exec_cmd.
    if (g_idleActionEvent) {
      using Event::CEventBus;
      const auto actionId = static_cast<int>(std::min<uint64_t>(
          m_id, static_cast<uint64_t>(std::numeric_limits<int>::max())));
      const auto result = g_idleActionEvent->emit({actionId, m_command});
      if (!result)
        hyprplasma::log(Log::ERR, "idle: action event failed: {}",
                        result.error());
    } else {
      hyprplasma::log(
          Log::ERR,
          "idle: action {} fired but no Lua action event is registered", m_id);
    }

    emitState(true);
  }

  uint64_t m_id = 0;
  uint32_t m_timeoutMs = 0;
  std::string m_command;
  bool m_fired = false;
  bool m_inhibited = false;
  SP<CEventLoopTimer> m_timer;
};

struct SNativeInhibitor {
  SP<CIdleInhibitor> inhibitor;
  CHyprSignalListener destroy;
  bool nonDesktop = false;
};

static std::unordered_map<const CIdleInhibitor *, SNativeInhibitor>
    g_nativeInhibitors;

struct {
  CHyprSignalListener mouseMove;
  CHyprSignalListener mouseButton;
  CHyprSignalListener mouseAxis;
  CHyprSignalListener keyboardKey;
  CHyprSignalListener tabletAxis;
  CHyprSignalListener tabletButton;
  CHyprSignalListener tabletProximity;
  CHyprSignalListener tabletTip;
  CHyprSignalListener touchCancel;
  CHyprSignalListener touchDown;
  CHyprSignalListener touchUp;
  CHyprSignalListener touchMotion;
  CHyprSignalListener swipeBegin;
  CHyprSignalListener swipeEnd;
  CHyprSignalListener swipeUpdate;
  CHyprSignalListener pinchBegin;
  CHyprSignalListener pinchEnd;
  CHyprSignalListener pinchUpdate;

  CHyprSignalListener newIdleInhibitor;
  CHyprSignalListener windowOpen;
  CHyprSignalListener windowClose;
  CHyprSignalListener windowDestroy;
  CHyprSignalListener windowActive;
  CHyprSignalListener windowFullscreen;
  CHyprSignalListener windowUpdateRules;
  CHyprSignalListener workspaceActive;
  CHyprSignalListener layerOpened;
  CHyprSignalListener layerClosed;
} g_listeners;

static SP<CEventLoopTimer> g_inhibitorPollTimer;

uint32_t elapsedIdleMs() {
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           Clock::now() - g_lastActivity)
                           .count();

  return static_cast<uint32_t>(std::min<int64_t>(
      std::max<int64_t>(elapsed, 0), std::numeric_limits<uint32_t>::max()));
}

uint32_t activeManualWindowInhibitors() {
  if (!g_pInputManager)
    return 0;

  uint32_t count = 0;
  for (const auto &window : Desktop::windowState()->windows()) {
    if (window && g_pInputManager->isWindowInhibiting(window, true))
      ++count;
  }
  return count;
}

uint32_t activeNativeSurfaceInhibitors() {
  uint32_t count = 0;
  for (const auto &[_, entry] : g_nativeInhibitors) {
    if (!entry.inhibitor)
      continue;

    const auto surfaceResource = entry.inhibitor->m_surface.lock();
    if (!surfaceResource)
      continue;

    if (entry.nonDesktop) {
      // Hyprland treats non-desktop surfaces (e.g. layer surfaces) as visible.
      ++count;
      continue;
    }

    const auto surface =
        Desktop::View::CWLSurface::fromResource(surfaceResource);
    if (!surface)
      continue;

    const auto view = surface->view();
    if (!view)
      continue;

    bool mapped = false;

    if (const auto window = dynamicPointerCast<Desktop::View::CWindow>(view)) {
      mapped = window->mapped();
    } else if (const auto layer =
                   dynamicPointerCast<Desktop::View::CLayerSurface>(view)) {
      mapped = layer->mapped();
    }

    if (mapped)
      ++count;
  }

  return count;
}

uint32_t currentIdleInhibitorCount() {
  return activeManualWindowInhibitors() + activeNativeSurfaceInhibitors();
}

bool currentIdleState() {
  for (const auto &[_, timeout] : g_timeouts) {
    if (timeout && timeout->isIdle())
      return true;
  }
  for (const auto &[_, action] : g_actions) {
    if (action && action->hasFired())
      return true;
  }
  return false;
}

void emitState(bool force) {
  const bool idle = currentIdleState();

  if (!force && idle == g_lastReportedIdle &&
      g_effectiveInhibited == g_lastReportedInhibited)
    return;

  g_lastReportedIdle = idle;
  g_lastReportedInhibited = g_effectiveInhibited;

  if (!g_idleEvent)
    return;

  const auto clampInt = [](uint64_t value) {
    return static_cast<int>(std::min<uint64_t>(
        value, static_cast<uint64_t>(std::numeric_limits<int>::max())));
  };

  const std::vector<Event::CEventBus::CCustomEvent::ValidVariant> args = {
      idle,
      g_effectiveInhibited,
      clampInt(elapsedIdleMs()),
      static_cast<int>(std::min<uint32_t>(
          currentIdleInhibitorCount(),
          static_cast<uint32_t>(std::numeric_limits<int>::max()))),
      clampInt(g_timeouts.size()),
  };

  const auto result = g_idleEvent->emit(args);
  if (!result)
    hyprplasma::log(Log::WARN, "idle: Lua state event failed: {}",
                    result.error());
}

void notifyActivity() {
  const bool wasIdle = currentIdleState();
  g_lastActivity = Clock::now();

  if (!g_effectiveInhibited) {
    for (const auto &[_, timeout] : g_timeouts) {
      if (timeout)
        timeout->reset();
    }
  }

  // Activity during inhibition still starts a new action cycle. The action
  // remains paused until inhibition is lifted.
  for (const auto &[_, action] : g_actions) {
    if (action)
      action->activity();
  }

  if (wasIdle)
    hyprplasma::log(Log::DEBUG, "idle: user activity resumed the session");
  emitState();
}

void recalculateInhibition() {
  const auto inhibitorCount = currentIdleInhibitorCount();
  const bool inhibited = g_manualInhibited || inhibitorCount > 0;
  const bool countChanged = inhibitorCount != g_nativeInhibitorCount;

  g_nativeInhibitorCount = inhibitorCount;

  if (inhibited != g_effectiveInhibited) {
    g_effectiveInhibited = inhibited;

    if (!g_effectiveInhibited)
      g_lastActivity = Clock::now();

    for (const auto &[_, timeout] : g_timeouts) {
      if (timeout)
        timeout->setInhibited(g_effectiveInhibited);
    }

    for (const auto &[_, action] : g_actions) {
      if (action)
        action->setInhibited(g_effectiveInhibited);
    }

    hyprplasma::log(Log::INFO, "idle: inhibition {} ({} native, manual={})",
                    g_effectiveInhibited ? "enabled" : "disabled",
                    g_nativeInhibitorCount, g_manualInhibited);
    emitState(true);
  } else if (countChanged) {
    hyprplasma::log(Log::DEBUG, "idle: {} active native inhibitor(s)",
                    g_nativeInhibitorCount);
    emitState(true);
  }
}

void onNewNativeInhibitor(SP<CIdleInhibitor> inhibitor) {
  if (!inhibitor)
    return;

  auto resource = inhibitor->m_resource.lock();
  if (!resource)
    return;

  const auto surface = inhibitor->m_surface.lock();
  const bool nonDesktop =
      surface && !Desktop::View::CWLSurface::fromResource(surface);

  const auto *key = inhibitor.get();
  auto [it, inserted] = g_nativeInhibitors.try_emplace(key);
  if (!inserted)
    return;

  it->second.inhibitor = inhibitor;
  it->second.nonDesktop = nonDesktop;
  it->second.destroy = resource->m_events.destroy.listen([key] {
    g_nativeInhibitors.erase(key);
    recalculateInhibition();
  });

  hyprplasma::log(Log::DEBUG, "idle: native inhibitor registered ({})",
                  nonDesktop ? "non-desktop" : "surface-backed");
  recalculateInhibition();
}

void timeoutRelease(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

void timeoutSimulateUserActivity(wl_client *, wl_resource *) {
  notifyActivity();
}

const struct org_kde_kwin_idle_timeout_interface s_timeoutImplementation = {
    .release = timeoutRelease,
    .simulate_user_activity = timeoutSimulateUserActivity,
};

void timeoutResourceDestroyed(wl_resource *resource) {
  const auto it = g_timeouts.find(resource);
  if (it == g_timeouts.end())
    return;

  g_timeouts.erase(it);
  emitState(true);
  hyprplasma::log(Log::DEBUG, "idle: timeout object destroyed");
}

void managerResourceDestroyed(wl_resource *resource) {
  g_managerResources.erase(resource);
}

void getIdleTimeout(wl_client *client, wl_resource *managerResource,
                    uint32_t id, wl_resource *seat, uint32_t timeoutMs) {
  if (!seat || wl_resource_get_client(seat) != client ||
      std::string_view(wl_resource_get_class(seat)) != "wl_seat") {
    wl_resource_post_error(
        managerResource, 0,
        "get_idle_timeout requires a wl_seat from this client");
    return;
  }

  auto *resource =
      wl_resource_create(client, &org_kde_kwin_idle_timeout_interface,
                         wl_resource_get_version(managerResource), id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  try {
    auto timeout = std::make_unique<CIdleTimeout>(
        resource, timeoutMs, elapsedIdleMs(), g_effectiveInhibited);
    g_timeouts.emplace(resource, std::move(timeout));

    wl_resource_set_implementation(resource, &s_timeoutImplementation, nullptr,
                                   timeoutResourceDestroyed);

    hyprplasma::log(Log::DEBUG, "idle: created timeout {} ms ({} total)",
                    timeoutMs, g_timeouts.size());
    emitState(true);
  } catch (const std::bad_alloc &) {
    wl_resource_destroy(resource);
    wl_client_post_no_memory(client);
  }
}

const struct org_kde_kwin_idle_interface s_idleImplementation = {
    .get_idle_timeout = getIdleTimeout,
};

void bindIdleManager(wl_client *client, void *, uint32_t version, uint32_t id) {
  const auto supportedVersion = std::min(
      version, static_cast<uint32_t>(org_kde_kwin_idle_interface.version));

  auto *resource = wl_resource_create(client, &org_kde_kwin_idle_interface,
                                      static_cast<int>(supportedVersion), id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &s_idleImplementation, nullptr,
                                 managerResourceDestroyed);
  g_managerResources.emplace(resource);
  hyprplasma::log(Log::DEBUG, "idle: client bound manager v{}",
                  supportedVersion);
}

void clearListeners() {
  g_listeners.mouseMove.reset();
  g_listeners.mouseButton.reset();
  g_listeners.mouseAxis.reset();
  g_listeners.keyboardKey.reset();
  g_listeners.tabletAxis.reset();
  g_listeners.tabletButton.reset();
  g_listeners.tabletProximity.reset();
  g_listeners.tabletTip.reset();
  g_listeners.touchCancel.reset();
  g_listeners.touchDown.reset();
  g_listeners.touchUp.reset();
  g_listeners.touchMotion.reset();
  g_listeners.swipeBegin.reset();
  g_listeners.swipeEnd.reset();
  g_listeners.swipeUpdate.reset();
  g_listeners.pinchBegin.reset();
  g_listeners.pinchEnd.reset();
  g_listeners.pinchUpdate.reset();

  g_listeners.newIdleInhibitor.reset();
  g_listeners.windowOpen.reset();
  g_listeners.windowClose.reset();
  g_listeners.windowDestroy.reset();
  g_listeners.windowActive.reset();
  g_listeners.windowFullscreen.reset();
  g_listeners.windowUpdateRules.reset();
  g_listeners.workspaceActive.reset();
  g_listeners.layerOpened.reset();
  g_listeners.layerClosed.reset();
}

void updateInhibitorsFromTimer(SP<CEventLoopTimer> timer, void *) {
  recalculateInhibition();
  timer->updateTimeout(std::chrono::milliseconds(500));
}

void registerIdleLuaEvent() {
  using Event::CEventBus;
  g_idleEvent = makeShared<CEventBus::CCustomEvent>(
      "plugin.hyprplasma.idle",
      std::vector<CEventBus::CCustomEvent::eType>{
          CEventBus::CCustomEvent::TYPE_BOOL, // idle
          CEventBus::CCustomEvent::TYPE_BOOL, // inhibited
          CEventBus::CCustomEvent::TYPE_INT,  // idle_for_ms
          CEventBus::CCustomEvent::TYPE_INT,  // active_inhibitor_count
          CEventBus::CCustomEvent::TYPE_INT,  // timeout_count
      });

  if (!HyprlandAPI::addEvent(hyprplasma::g_pluginHandle, g_idleEvent)) {
    hyprplasma::log(Log::WARN,
                    "idle: could not register plugin.hyprplasma.idle event");
    g_idleEvent.reset();
  }

  g_idleActionEvent = makeShared<CEventBus::CCustomEvent>(
      "plugin.hyprplasma.idle_action",
      std::vector<CEventBus::CCustomEvent::eType>{
          CEventBus::CCustomEvent::TYPE_INT,    // action id
          CEventBus::CCustomEvent::TYPE_STRING, // command
      });

  if (!HyprlandAPI::addEvent(hyprplasma::g_pluginHandle, g_idleActionEvent)) {
    hyprplasma::log(
        Log::WARN,
        "idle: could not register plugin.hyprplasma.idle_action event");
    g_idleActionEvent.reset();
  }
}

} // namespace

namespace hyprplasma {
namespace proto {

void plasma_idle_init(void) {
  if (g_initialized)
    return;

  if (!g_pCompositor || !g_pCompositor->m_wlDisplay) {
    hyprplasma::log(Log::ERR, "idle: compositor Wayland display unavailable");
    return;
  }

  g_idleGlobal = wl_global_create(
      g_pCompositor->m_wlDisplay, &org_kde_kwin_idle_interface,
      org_kde_kwin_idle_interface.version, nullptr, bindIdleManager);

  if (!g_idleGlobal) {
    hyprplasma::log(Log::ERR, "idle: failed to register org_kde_kwin_idle");
    return;
  }

  g_initialized = true;
  g_lastActivity = Clock::now();
  g_manualInhibited = false;
  g_effectiveInhibited = false;
  g_nativeInhibitorCount = 0;
  g_lastReportedIdle = false;
  g_lastReportedInhibited = false;

  registerIdleLuaEvent();

  // Reuse Hyprland's existing zwp_idle_inhibit_manager_v1 implementation.
  // Do not register a second global with the same protocol interface.
  if (PROTO::idleInhibit) {
    g_listeners.newIdleInhibitor =
        PROTO::idleInhibit->m_events.newIdleInhibitor.listen(
            [](SP<CIdleInhibitor> inhibitor) {
              onNewNativeInhibitor(std::move(inhibitor));
            });
  } else {
    hyprplasma::log(Log::WARN,
                    "idle: Hyprland idle-inhibit protocol unavailable");
  }

  // Input signals drive the protocol. The no-argument overload intentionally
  // discards event payloads and avoids Hyprutils' generic-lambda ambiguity.
  const std::function<void()> activity = [] { notifyActivity(); };
  const std::function<void()> refreshInhibitors = [] {
    recalculateInhibition();
  };
  auto &events = Event::bus();

  g_listeners.mouseMove = events->m_events.input.mouse.move.listen(activity);
  g_listeners.mouseButton =
      events->m_events.input.mouse.button.listen(activity);
  g_listeners.mouseAxis = events->m_events.input.mouse.axis.listen(activity);
  g_listeners.keyboardKey =
      events->m_events.input.keyboard.key.listen(activity);
  g_listeners.tabletAxis = events->m_events.input.tablet.axis.listen(activity);
  g_listeners.tabletButton =
      events->m_events.input.tablet.button.listen(activity);
  g_listeners.tabletProximity =
      events->m_events.input.tablet.proximity.listen(activity);
  g_listeners.tabletTip = events->m_events.input.tablet.tip.listen(activity);
  g_listeners.touchCancel =
      events->m_events.input.touch.cancel.listen(activity);
  g_listeners.touchDown = events->m_events.input.touch.down.listen(activity);
  g_listeners.touchUp = events->m_events.input.touch.up.listen(activity);
  g_listeners.touchMotion =
      events->m_events.input.touch.motion.listen(activity);
  g_listeners.swipeBegin =
      events->m_events.gesture.swipe.begin.listen(activity);
  g_listeners.swipeEnd = events->m_events.gesture.swipe.end.listen(activity);
  g_listeners.swipeUpdate =
      events->m_events.gesture.swipe.update.listen(activity);
  g_listeners.pinchBegin =
      events->m_events.gesture.pinch.begin.listen(activity);
  g_listeners.pinchEnd = events->m_events.gesture.pinch.end.listen(activity);
  g_listeners.pinchUpdate =
      events->m_events.gesture.pinch.update.listen(activity);

  g_listeners.windowOpen =
      events->m_events.window.openLate.listen(refreshInhibitors);
  g_listeners.windowClose =
      events->m_events.window.close.listen(refreshInhibitors);
  g_listeners.windowDestroy =
      events->m_events.window.destroy.listen(refreshInhibitors);
  g_listeners.windowActive =
      events->m_events.window.active.listen(refreshInhibitors);
  g_listeners.windowFullscreen =
      events->m_events.window.fullscreen.listen(refreshInhibitors);
  g_listeners.windowUpdateRules =
      events->m_events.window.updateRules.listen(refreshInhibitors);
  g_listeners.workspaceActive =
      events->m_events.workspace.active.listen(refreshInhibitors);
  g_listeners.layerOpened =
      events->m_events.layer.opened.listen(refreshInhibitors);
  g_listeners.layerClosed =
      events->m_events.layer.closed.listen(refreshInhibitors);

  g_inhibitorPollTimer = makeShared<CEventLoopTimer>(
      std::chrono::milliseconds(500), updateInhibitorsFromTimer, nullptr);
  g_pEventLoopManager->addTimer(g_inhibitorPollTimer);

  recalculateInhibition();
  hyprplasma::log(Log::INFO,
                  "idle: registered org_kde_kwin_idle and activity tracking");
}

void plasma_idle_exit(void) {
  if (!g_initialized && !g_idleGlobal)
    return;

  clearListeners();

  if (g_inhibitorPollTimer && g_pEventLoopManager)
    g_pEventLoopManager->removeTimer(g_inhibitorPollTimer);
  g_inhibitorPollTimer.reset();

  if (g_idleGlobal) {
    wl_global_destroy(g_idleGlobal);
    g_idleGlobal = nullptr;
  }

  // Destroy client-owned Wayland resources before unloading this plugin's code.
  std::vector<wl_resource *> timeoutResources;
  timeoutResources.reserve(g_timeouts.size());
  for (const auto &[resource, _] : g_timeouts)
    timeoutResources.push_back(resource);
  for (auto *resource : timeoutResources)
    wl_resource_destroy(resource);
  g_timeouts.clear();

  std::vector<wl_resource *> managerResources(g_managerResources.begin(),
                                              g_managerResources.end());
  for (auto *resource : managerResources)
    wl_resource_destroy(resource);
  g_managerResources.clear();

  g_actions.clear();
  g_nativeInhibitors.clear();

  if (g_idleActionEvent) {
    HyprlandAPI::removeEvent(hyprplasma::g_pluginHandle,
                             "plugin.hyprplasma.idle_action");
    g_idleActionEvent.reset();
  }

  if (g_idleEvent) {
    HyprlandAPI::removeEvent(hyprplasma::g_pluginHandle,
                             "plugin.hyprplasma.idle");
    g_idleEvent.reset();
  }

  g_manualInhibited = false;
  g_effectiveInhibited = false;
  g_nativeInhibitorCount = 0;
  g_lastActivity = Clock::now();
  g_initialized = false;

  hyprplasma::log(Log::INFO, "idle: subsystem stopped");
}

SIdleStatus plasma_idle_status() {
  SIdleStatus status{};
  status.available = g_initialized;
  status.idle = currentIdleState();
  status.inhibited = g_effectiveInhibited;
  status.manualInhibited = g_manualInhibited;
  status.activeInhibitors = g_nativeInhibitorCount;
  status.timeoutCount = static_cast<uint32_t>(g_timeouts.size());
  status.actionCount = static_cast<uint32_t>(g_actions.size());
  status.idleForMs = elapsedIdleMs();

  uint32_t idleTimeoutCount = 0;
  uint32_t firedActionCount = 0;
  std::optional<uint64_t> nextTimeout;

  for (const auto &[_, timeout] : g_timeouts) {
    if (!timeout)
      continue;
    if (timeout->isIdle())
      ++idleTimeoutCount;
    status.timeouts.push_back(SIdleTimeoutStatus{
        .objectId = timeout->objectId(),
        .timeoutMs = timeout->timeoutMs(),
        .idle = timeout->isIdle(),
        .inhibited = timeout->isInhibited(),
    });
    if (timeout->isPending() && !g_effectiveInhibited) {
      const auto remaining = timeout->timeoutMs() > status.idleForMs
                                 ? timeout->timeoutMs() - status.idleForMs
                                 : 0;
      nextTimeout = nextTimeout ? std::min(*nextTimeout, remaining) : remaining;
    }
  }

  for (const auto &[_, action] : g_actions) {
    if (!action)
      continue;
    if (action->hasFired())
      ++firedActionCount;
    status.actions.push_back(SIdleActionStatus{
        .id = _,
        .timeoutMs = action->timeoutMs(),
        .fired = action->hasFired(),
        .inhibited = action->isInhibited(),
    });
    if (action->isPending() && !g_effectiveInhibited) {
      // Actions are reset at activity and inhibition transitions. Their timer
      // uses the same last-activity timestamp as protocol timeout objects.
      const auto remaining = action->timeoutMs() > status.idleForMs
                                 ? action->timeoutMs() - status.idleForMs
                                 : 0;
      nextTimeout = nextTimeout ? std::min(*nextTimeout, remaining) : remaining;
    }
  }

  status.idleTimeoutCount = idleTimeoutCount;
  status.firedActionCount = firedActionCount;
  status.nextTimeoutMs = nextTimeout;
  return status;
}

void plasma_idle_simulate_activity() {
  if (g_initialized)
    notifyActivity();
}

bool plasma_idle_set_manual_inhibit(bool inhibited) {
  if (!g_initialized)
    return false;

  if (g_manualInhibited == inhibited)
    return g_manualInhibited;

  g_manualInhibited = inhibited;
  recalculateInhibition();
  hyprplasma::log(Log::INFO, "idle: manual inhibition {}",
                  inhibited ? "enabled" : "disabled");
  return g_manualInhibited;
}

uint64_t plasma_idle_add_action(uint32_t timeoutMs,
                                const std::string &command) {
  if (!g_initialized || command.empty() || !g_pEventLoopManager)
    return 0;

  const auto id = g_nextActionId++;
  try {
    auto action = std::make_unique<CIdleAction>(id, timeoutMs, command);
    auto *actionPtr = action.get();
    g_actions.emplace(id, std::move(action));
    actionPtr->arm(elapsedIdleMs());

    hyprplasma::log(Log::INFO, "idle: added action {} at {} ms", id, timeoutMs);
    emitState();
    return id;
  } catch (const std::bad_alloc &) {
    hyprplasma::log(Log::ERR, "idle: failed to allocate action");
    return 0;
  }
}

bool plasma_idle_remove_action(uint64_t id) {
  const auto removed = g_actions.erase(id) != 0;
  if (removed) {
    hyprplasma::log(Log::INFO, "idle: removed action {}", id);
    emitState(true);
  }
  return removed;
}

uint32_t plasma_idle_clear_actions() {
  const auto removed = static_cast<uint32_t>(g_actions.size());
  g_actions.clear();
  if (removed) {
    hyprplasma::log(Log::INFO, "idle: cleared {} action(s)", removed);
    emitState(true);
  }
  return removed;
}

} // namespace proto
} // namespace hyprplasma
