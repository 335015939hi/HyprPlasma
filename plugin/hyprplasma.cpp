
#include <hyprland/src/debug/log/Logger.hpp>
#include <hyprutils/cli/Logger.hpp>

#include "config.hpp"
#include "hyprplasma.hpp"
#include "lua.hpp"
#include "protocols/shell.hpp"
#include "protocols/idle.hpp"
#include "protocols/virtualdesktop.hpp"
#include "protocols/windowmanagement.hpp"

namespace hyprplasma {

void init(void) {
  config::init();
  lua::init();
  proto::plasma_shell_init();
  proto::plasma_idle_init();
  proto::plasma_virtualdesktop_init();
  proto::window_management_init();
}
void exit(void) {
  proto::window_management_exit();
  proto::plasma_virtualdesktop_exit();
  proto::plasma_idle_exit();
  proto::plasma_shell_exit();
  lua::exit();
  config::exit();
}
} // namespace hyprplasma
