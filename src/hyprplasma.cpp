
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/cli/Logger.hpp>

#include "hyprplasma.hpp"
#include "protocols/shell.hpp"
#include "protocols/virtualdesktop.hpp"

namespace hyprplasma {

void init(void) {
  proto::plasma_shell_init();
  proto::plasma_virtualdesktop_init();
}
void exit(void) {
  proto::plasma_shell_exit();
  proto::plasma_virtualdesktop_exit();
}
} // namespace hyprplasma
