#ifndef _PROTOCOLS_WINDOWMANAGEMENT_HPP
#define _PROTOCOLS_WINDOWMANAGEMENT_HPP

#include <desktop/view/window/Window.hpp>

namespace hyprplasma {
namespace proto {
void window_management_init(void);
void window_management_exit(void);
void window_management_update_state(Desktop::View::CWindow *window);
void clear_show_desktop(void);
void toggle_show_desktop(void);
} // namespace proto
} // namespace hyprplasma

#endif
