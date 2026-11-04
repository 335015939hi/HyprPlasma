#include "lua.hpp"
#include "hyprplasma.hpp"
#include "protocols/idle.hpp"
#include "protocols/windowmanagement.hpp"

#include <cstdint>
#include <limits>
#include <string>

#include <plugins/PluginAPI.hpp>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

#define LUA_FUNCTION_NAMESPACE "hyprplasma"

class LuaFunc {
public:
  const char *name;
  PLUGIN_LUA_FN function;
};

static void setBool(lua_State *L, const char *name, bool value) {
  lua_pushboolean(L, value);
  lua_setfield(L, -2, name);
}

static void setInteger(lua_State *L, const char *name, uint64_t value) {
  const auto maxValue =
      static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max());
  lua_pushinteger(L, static_cast<lua_Integer>(std::min(value, maxValue)));
  lua_setfield(L, -2, name);
}

static int toggle_show_desktop(lua_State *_) {
  hyprplasma::proto::toggle_show_desktop();
  return 0;
}

static int idle_state(lua_State *L) {
  const auto state = hyprplasma::proto::plasma_idle_status();

  lua_createtable(L, 0, 14);
  setBool(L, "available", state.available);
  setBool(L, "idle", state.idle);
  setBool(L, "inhibited", state.inhibited);
  setBool(L, "manual_inhibited", state.manualInhibited);
  setInteger(L, "active_inhibitors", state.activeInhibitors);
  setInteger(L, "timeout_count", state.timeoutCount);
  setInteger(L, "idle_timeout_count", state.idleTimeoutCount);
  setInteger(L, "action_count", state.actionCount);
  setInteger(L, "fired_action_count", state.firedActionCount);
  setInteger(L, "idle_for_ms", state.idleForMs);

  if (state.nextTimeoutMs)
    setInteger(L, "next_timeout_ms", *state.nextTimeoutMs);
  else {
    lua_pushnil(L);
    lua_setfield(L, -2, "next_timeout_ms");
  }

  lua_createtable(L, static_cast<int>(state.timeouts.size()), 0);
  for (size_t i = 0; i < state.timeouts.size(); ++i) {
    const auto &timeout = state.timeouts[i];
    lua_createtable(L, 0, 4);
    setInteger(L, "object_id", timeout.objectId);
    setInteger(L, "timeout_ms", timeout.timeoutMs);
    setBool(L, "idle", timeout.idle);
    setBool(L, "inhibited", timeout.inhibited);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "timeouts");

  lua_createtable(L, static_cast<int>(state.actions.size()), 0);
  for (size_t i = 0; i < state.actions.size(); ++i) {
    const auto &action = state.actions[i];
    lua_createtable(L, 0, 4);
    setInteger(L, "id", action.id);
    setInteger(L, "timeout_ms", action.timeoutMs);
    setBool(L, "fired", action.fired);
    setBool(L, "inhibited", action.inhibited);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  lua_setfield(L, -2, "actions");

  return 1;
}

static int idle_simulate_activity(lua_State *) {
  hyprplasma::proto::plasma_idle_simulate_activity();
  return 0;
}

static int idle_set_inhibited(lua_State *L) {
  luaL_checktype(L, 1, LUA_TBOOLEAN);
  const bool inhibited = lua_toboolean(L, 1) != 0;
  lua_pushboolean(
      L, hyprplasma::proto::plasma_idle_set_manual_inhibit(inhibited));
  return 1;
}

static int idle_add_action(lua_State *L) {
  const auto timeout = luaL_checkinteger(L, 1);
  if (timeout < 0 ||
      static_cast<uint64_t>(timeout) >
          std::numeric_limits<uint32_t>::max())
    return luaL_error(L, "idle_add_action: timeout must be in [0, 2^32-1] ms");

  const char *command = luaL_checkstring(L, 2);
  if (!command || command[0] == '\0')
    return luaL_error(L, "idle_add_action: command must not be empty");

  const auto id = hyprplasma::proto::plasma_idle_add_action(
      static_cast<uint32_t>(timeout), std::string(command));
  if (id == 0) {
    lua_pushnil(L);
    lua_pushstring(L, "idle subsystem unavailable or action allocation failed");
    return 2;
  }

  lua_pushinteger(L, static_cast<lua_Integer>(id));
  return 1;
}

static int idle_remove_action(lua_State *L) {
  const auto id = luaL_checkinteger(L, 1);
  if (id <= 0)
    return luaL_error(L, "idle_remove_action: action id must be positive");

  lua_pushboolean(
      L, hyprplasma::proto::plasma_idle_remove_action(
             static_cast<uint64_t>(id)));
  return 1;
}

static int idle_clear_actions(lua_State *L) {
  lua_pushinteger(
      L, hyprplasma::proto::plasma_idle_clear_actions());
  return 1;
}

namespace hyprplasma {
namespace lua {
void init() {
  const LuaFunc lua_funcs[] = {
      {.name = "toggle_show_desktop", .function = toggle_show_desktop},
      {.name = "idle_state", .function = idle_state},
      {.name = "idle_simulate_activity", .function = idle_simulate_activity},
      {.name = "idle_set_inhibited", .function = idle_set_inhibited},
      {.name = "idle_add_action", .function = idle_add_action},
      {.name = "idle_remove_action", .function = idle_remove_action},
      {.name = "idle_clear_actions", .function = idle_clear_actions},
      {.name = NULL, .function = NULL}};

  for (auto lua_func : lua_funcs) {
    if (lua_func.name == NULL)
      break;

    if (!HyprlandAPI::addLuaFunction(hyprplasma::g_pluginHandle,
                                     LUA_FUNCTION_NAMESPACE, lua_func.name,
                                     lua_func.function)) {
      hyprplasma::log(Log::ERR, "unable to register Lua function {}",
                      lua_func.name);
    } else {
      hyprplasma::log(Log::INFO, "registered Lua function hl.plugin.{}.{}",
                      LUA_FUNCTION_NAMESPACE, lua_func.name);
    }
  }
}

void exit() {}
} // namespace lua
} // namespace hyprplasma
