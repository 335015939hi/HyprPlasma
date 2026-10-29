
#include <hyprland/src/debug/log/Logger.hpp>

#include "hyprplasma.hpp"
#include "plasmaproto-shell.hpp"

namespace hyprplasma {

void init(void) {
  Log::logger->log(Log::CRIT, "hello world!");
  proto::plasma_shell_init();
}
void exit(void) {
  proto::plasma_shell_exit();
  Log::logger->log(Log::CRIT, "Bye bye");
}
} // namespace hyprplasma
