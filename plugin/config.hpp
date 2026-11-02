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
};
extern SPluginConfig g_config;

void init();
void exit();
} // namespace config
} // namespace hyprplasma

#endif
