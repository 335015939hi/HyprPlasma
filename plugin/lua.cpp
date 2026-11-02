
#include "lua.hpp"
#include "hyprplasma.hpp"
#include "protocols/windowmanagement.hpp"
#include <plugins/PluginAPI.hpp>

#define LUA_FUNCTION_NAMESPACE "hyprplasma"

class LuaFunc {
public:
  const char *name;
  PLUGIN_LUA_FN function;
};

static int toggle_show_desktop(lua_State *_) {
  hyprplasma::proto::toggle_show_desktop();
  return 0;
}

namespace hyprplasma {
namespace lua {
void init() {
  const LuaFunc lua_funcs[] = {
      {.name = "toggle_show_desktop", .function = toggle_show_desktop},
      {.name = NULL, .function = NULL}};
  for (auto lua_func : lua_funcs) {
    if (lua_func.name == NULL) {
      break;
    }
    if (!HyprlandAPI::addLuaFunction(hyprplasma::g_pluginHandle,
                                     LUA_FUNCTION_NAMESPACE, lua_func.name,
                                     lua_func.function)) {
      hyprplasma::log(Log::ERR, "unable to register lua funciton {}",
                      lua_func.name);
    } else {
      hyprplasma::log(Log::INFO, "registered lua function hl.plugin.{}.{}",
                      LUA_FUNCTION_NAMESPACE, lua_func.name);
    }
  }
}
void exit() {}
} // namespace lua
} // namespace hyprplasma
