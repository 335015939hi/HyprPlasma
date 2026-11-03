#include "idle.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <wayland-server-core.h>

#include <hyprland/src/event/EventBus.hpp>
#include <hyprland/src/helpers/memory/Memory.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopManager.hpp>
#include <hyprland/src/managers/eventLoop/EventLoopTimer.hpp>

#include "idle-server-protocol.h"
#include "../hyprplasma.hpp"

namespace {

class CIdleTimeout {
public:
  CIdleTimeout(wl_resource *resource, uint32_t timeoutMs)
      : m_resource(resource), m_timeoutMs(timeoutMs) {
    m_timer = makeShared<CEventLoopTimer>(
        std::nullopt,
        [this](SP<CEventLoopTimer>, void *) {
          if (!m_resource || m_idle)
            return;

          m_idle = true;
          org_kde_kwin_idle_timeout_send_idle(m_resource);
        },
        nullptr);

    g_pEventLoopManager->addTimer(m_timer);
    reset();
  }

  ~CIdleTimeout() {
    if (m_timer && g_pEventLoopManager)
      g_pEventLoopManager->removeTimer(m_timer);
  }

  void reset() {
    if (!m_resource || !m_timer)
      return;

    if (m_idle) {
      m_idle = false;
      org_kde_kwin_idle_timeout_send_resumed(m_resource);
    }

    m_timer->updateTimeout(std::chrono::milliseconds(m_timeoutMs));
  }

private:
  wl_resource *m_resource = nullptr;
  uint32_t m_timeoutMs = 0;
  bool m_idle = false;
  SP<CEventLoopTimer> m_timer;
};

static std::unordered_map<wl_resource *, std::unique_ptr<CIdleTimeout>>
    g_timeouts;
static std::unordered_set<wl_resource *> g_managerResources;
static wl_global *g_idleGlobal = nullptr;

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
} g_listeners;

void notifyActivity() {
  for (const auto &[_, timeout] : g_timeouts) {
    if (timeout)
      timeout->reset();
  }
}

void timeoutRelease(wl_client *, wl_resource *resource) {
  wl_resource_destroy(resource);
}

void timeoutSimulateUserActivity(wl_client *, wl_resource *) {
  // The protocol specifies that simulated activity behaves like real seat
  // activity, so reset every timeout rather than only the requesting object.
  notifyActivity();
}

const struct org_kde_kwin_idle_timeout_interface s_timeoutImplementation = {
    .release = timeoutRelease,
    .simulate_user_activity = timeoutSimulateUserActivity,
};

void timeoutResourceDestroyed(wl_resource *resource) {
  g_timeouts.erase(resource);
}

void managerResourceDestroyed(wl_resource *resource) {
  g_managerResources.erase(resource);
}

void getIdleTimeout(wl_client *client, wl_resource *managerResource,
                    uint32_t id, wl_resource *seat, uint32_t timeoutMs) {
  // Hyprland currently exposes a single Wayland seat. The protocol's typed
  // wl_seat argument is therefore sufficient; all of its timeout objects
  // observe activity on that seat.
  (void)seat;

  auto *resource = wl_resource_create(
      client, &org_kde_kwin_idle_timeout_interface,
      org_kde_kwin_idle_timeout_interface.version, id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  try {
    auto timeout = std::make_unique<CIdleTimeout>(resource, timeoutMs);
    auto *timeoutPtr = timeout.get();

    g_timeouts.emplace(resource, std::move(timeout));
    wl_resource_set_implementation(resource, &s_timeoutImplementation,
                                   timeoutPtr, timeoutResourceDestroyed);
  } catch (const std::bad_alloc &) {
    wl_resource_destroy(resource);
    wl_client_post_no_memory(client);
  }
}

const struct org_kde_kwin_idle_interface s_idleImplementation = {
    .get_idle_timeout = getIdleTimeout,
};

void bindIdleManager(wl_client *client, void *, uint32_t version,
                     uint32_t id) {
  const auto supportedVersion =
      std::min(version,
               static_cast<uint32_t>(org_kde_kwin_idle_interface.version));

  auto *resource =
      wl_resource_create(client, &org_kde_kwin_idle_interface,
                         static_cast<int>(supportedVersion), id);
  if (!resource) {
    wl_client_post_no_memory(client);
    return;
  }

  wl_resource_set_implementation(resource, &s_idleImplementation, nullptr,
                                 managerResourceDestroyed);
  g_managerResources.emplace(resource);
}

void resetListeners() {
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
}

} // namespace

namespace hyprplasma {
namespace proto {

void plasma_idle_init(void) {
  g_idleGlobal = wl_global_create(
      g_pCompositor->m_wlDisplay, &org_kde_kwin_idle_interface,
      org_kde_kwin_idle_interface.version, nullptr, bindIdleManager);

  if (!g_idleGlobal) {
    hyprplasma::log(Log::ERR, "failed to register org_kde_kwin_idle");
    plasma_idle_exit();
    return;
  }

  // Use compositor input events, not focus changes or IPC polling: pointer,
  // keyboard, touch, tablet and gesture input all count as user activity.
  const auto activity = [](auto &&...) { notifyActivity(); };
  auto *events = Event::bus();

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
  g_listeners.touchCancel = events->m_events.input.touch.cancel.listen(activity);
  g_listeners.touchDown = events->m_events.input.touch.down.listen(activity);
  g_listeners.touchUp = events->m_events.input.touch.up.listen(activity);
  g_listeners.touchMotion = events->m_events.input.touch.motion.listen(activity);
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

  hyprplasma::log(Log::INFO, "registered org_kde_kwin_idle");
}

void plasma_idle_exit(void) {
  resetListeners();

  if (g_idleGlobal) {
    wl_global_destroy(g_idleGlobal);
    g_idleGlobal = nullptr;
  }

  // A plugin can be unloaded while clients still hold protocol objects.
  // Destroy every resource owned by this module before its code is unloaded.
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
}

} // namespace proto
} // namespace hyprplasma
