
#ifndef _PROTOCOLS_VIRTUALDESKTOP_HPP
#define _PROTOCOLS_VIRTUALDESKTOP_HPP

#include <desktop/DesktopTypes.hpp>
#include <string>
#include <workspace/HLWorkspace.hpp>

namespace hyprplasma {
namespace proto {
void plasma_virtualdesktop_init(void);
void plasma_virtualdesktop_exit(void);
std::string virtualdesktop_id_for_workspace(Workspace::CHLWorkspace *workspace);
PHLWORKSPACE virtualdesktop_workspace_for_id(const std::string &id);
} // namespace proto
} // namespace hyprplasma

#endif
