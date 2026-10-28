
#include <hyprland/src/debug/log/Logger.hpp>

#include "hyprplasma.hpp"

namespace hyprplasma {
void init(void) { Log::logger->log(Log::CRIT, "hello world!"); }
void exit(void) {}
} // namespace hyprplasma
