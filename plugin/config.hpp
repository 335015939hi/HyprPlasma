#ifndef _HYPRPLASMA_CONFIG_HPP
#define _HYPRPLASMA_CONFIG_HPP

#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>

namespace hyprplasma {
namespace config {

struct SPluginConfig {
  SP<Config::Values::CStringValue> logout_command;
  SP<Config::Values::CStringValue> lock_command;
  SP<Config::Values::CStringValue> reboot_command;
  SP<Config::Values::CStringValue> shutdown_command;
  SP<Config::Values::CStringValue> switch_user_command;
  SP<Config::Values::CStringValue> save_session_command;
  SP<Config::Values::CStringValue> logout_and_shutdown_command;
  SP<Config::Values::CStringValue> logout_and_reboot_command;
  SP<Config::Values::CStringValue> session_actions_command;
  SP<Config::Values::CBoolValue> cancel_showdesktop_on_workspaceswitch;
};
extern SPluginConfig g_config;

void init();
void exit();
} // namespace config
} // namespace hyprplasma

#endif
