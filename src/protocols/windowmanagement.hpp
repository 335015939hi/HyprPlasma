#ifndef _PROTOCOLS_WINDOWMANAGEMENT_HPP
#define _PROTOCOLS_WINDOWMANAGEMENT_HPP

#include <desktop/view/Window.hpp>

namespace hyprplasma {
namespace proto {
void window_management_init(void);
void window_management_exit(void);
void window_management_update_state(Desktop::View::CWindow *window);
} // namespace proto
} // namespace hyprplasma

#endif
