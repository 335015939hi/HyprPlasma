#ifndef _PLASMAPROTO_IDLE_HPP
#define _PLASMAPROTO_IDLE_HPP

#include <cstdint>
#include <optional>
#include <string>

namespace hyprplasma {
namespace proto {

struct SIdleStatus {
  bool available = false;
  bool idle = false;
  bool inhibited = false;
  bool manualInhibited = false;
  uint32_t nativeInhibitors = 0;
  uint32_t timeoutCount = 0;
  uint32_t idleTimeoutCount = 0;
  uint32_t actionCount = 0;
  uint32_t firedActionCount = 0;
  uint64_t idleForMs = 0;
  std::optional<uint64_t> nextTimeoutMs;
};

void plasma_idle_init(void);
void plasma_idle_exit(void);

SIdleStatus plasma_idle_status();
void plasma_idle_simulate_activity();
bool plasma_idle_set_manual_inhibit(bool inhibited);
uint64_t plasma_idle_add_action(uint32_t timeoutMs,
                                const std::string &command);
bool plasma_idle_remove_action(uint64_t id);
uint32_t plasma_idle_clear_actions();

} // namespace proto
} // namespace hyprplasma

#endif
