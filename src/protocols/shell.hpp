#ifndef _PLASMAPROTO_SHELL_HPP
#define _PLASMAPROTO_SHELL_HPP

#include <desktop/view/View.hpp>

namespace hyprplasma {
namespace proto {
void plasma_shell_init(void);
void plasma_shell_exit(void);
bool plasma_surface_skips_taskbar(Desktop::View::CWindow *window);
} // namespace proto
} // namespace hyprplasma

#endif
