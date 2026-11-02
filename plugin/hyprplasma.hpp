#ifndef _HYPRPLASMA_H
#define _HYPRPLASMA_H

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hyprplasma {
template <typename... Args>
void log(Hyprutils::CLI::eLogLevel level, std::format_string<Args...> fmt,
         Args &&...args) {
  Log::logger->log(level, "hyprplasma: {}",
                   std::format(fmt, std::forward<Args>(args)...));
}

extern HANDLE g_pluginHandle;
void init(void);
void exit(void);

namespace ipc {
void init();
void exit();
} // namespace ipc
} // namespace hyprplasma

#endif
