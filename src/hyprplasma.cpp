
#include <hyprland/src/debug/log/Logger.hpp>

#include "hyprplasma.hpp"
#include "plasmaproto-shell.hpp"

namespace hyprplasma {

void init(void) {
  Log::logger->log(Log::CRIT, "hello world!");
  proto::plasma_shell();
}
void exit(void) {}
} // namespace hyprplasma
