
#include <config/values/types/IValue.hpp>
#include <hyprland/src/config/values/types/BoolValue.hpp>
#include <hyprland/src/config/values/types/FloatValue.hpp>
#include <hyprland/src/config/values/types/IntValue.hpp>
#include <hyprland/src/config/values/types/StringValue.hpp>
#include <hyprland/src/plugins/PluginAPI.hpp>
#include <hyprutils/memory/SharedPtr.hpp>

#include "config.hpp"
#include "hyprplasma.hpp"

#define CONFIG_PREFIX "plugin:hyprplasma:"

namespace hyprplasma {
namespace config {
SPluginConfig g_config = {};
void init() {
  g_config.logout_command = makeShared<Config::Values::CStringValue>(
      CONFIG_PREFIX "logout_command", "Command to run on logout", "");

  g_config.logout_command = makeShared<Config::Values::CStringValue>(
      CONFIG_PREFIX "lock_command", "Command to run on lock screen", "");

  g_config.logout_command = makeShared<Config::Values::CStringValue>(
      CONFIG_PREFIX "reboot_command", "Command to run on reboot", "");

  g_config.logout_command = makeShared<Config::Values::CStringValue>(
      CONFIG_PREFIX "shutdown_command", "Command to run on shutdown", "");

  const SP<Config::Values::IValue> configs[] = {
      Hyprutils::Memory::dynamicPointerCast<Config::Values::IValue>(
          g_config.logout_command),
      Hyprutils::Memory::dynamicPointerCast<Config::Values::IValue>(
          g_config.lock_command),
      Hyprutils::Memory::dynamicPointerCast<Config::Values::IValue>(
          g_config.reboot_command),
      Hyprutils::Memory::dynamicPointerCast<Config::Values::IValue>(
          g_config.shutdown_command),
      nullptr,
  };

  for (auto config : configs) {
    if (!config) {
      break;
    }
    if (!HyprlandAPI::addConfigValueV2(hyprplasma::g_pluginHandle, config)) {
      hyprplasma::log(Log::ERR, "failed to add config value {}",
                      config->name());
    }
  }
}
void exit() {}
} // namespace config
} // namespace hyprplasma
