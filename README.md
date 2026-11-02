# HyprPlasma

A Hyprland plugin aiming to bring KDE Plasma Hyprland.

## Requirements

Before building, make sure you have:

- Hyprland installed with development headers and pkg-config metadata
- CMake 3.20 or newer
- A C++23-capable compiler
- A C compiler
- `wayland-server` development libraries
- `Plasma Wayland protocols` installed
- `wayland-scanner` available in `PATH`

## Installation

Please don't blindly copy and paste. 

### Clone the repo
```bash
git clone https://github.com/335015939hi/HyprPlasma.git
cd HyprPlasma
```
### Copy hyprland config
```bash
cp hyprplasma.lua ~/.config/hypr
echo 'require("hyprplasma")' >>~/.config/hypr/hyprland.lua
```
### Build the plugin
```bash
cmake -B build -S .
cmake --build build
```
### Load the plugin
```bash
hyprctl plugin load ./build/libhyprplasma.so
```

## Bugs

plenty of bugs and non-implemented features. please contribute. 

### Known bugs
- **Tooltip flickering:** Seems to be caused by the client repeatedly requesting a resize. Doesn't happen consistently.
- **Popup resizing:** Resizing popups causes flickering. Pretty consistent. Same cause as issue above.
- **Multi-monitor support:** Not tested. I don't have another monitor. Please buy me one.
