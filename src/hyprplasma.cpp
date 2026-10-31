
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/cli/Logger.hpp>

#include "hyprplasma.hpp"
#include "protocols/shell.hpp"

namespace hyprplasma {

void init(void) { proto::plasma_shell_init(); }
void exit(void) { proto::plasma_shell_exit(); }
} // namespace hyprplasma
