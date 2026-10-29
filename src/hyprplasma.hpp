#ifndef _HYPRPLASMA_H
#define _HYPRPLASMA_H

#include <hyprland/src/plugins/PluginAPI.hpp>

namespace hyprplasma {
extern HANDLE g_pluginHandle;
void init(void);
void exit(void);
} // namespace hyprplasma

#endif
