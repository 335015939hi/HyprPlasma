# HyprPlasma

A Hyprland plugin aiming to bring KDE Plasma to Hyprland.

## Requirements

Before building, make sure you have:

- Hyprland installed with development headers and pkg-config metadata
- CMake 3.22 or newer
- A C++26-capable compiler
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
You may want to read through the config. 
### Build the plugin
```bash
cmake -B build -S .
cmake --build build
```
### Load the plugin
```bash
hyprctl plugin load ./build/libhyprplasma-plugin.so
```
### Start KDE plasma
```bash
plasmashell&disown
```

## Configuration
see hyprplasma.lua

### systemd-logind integration

The Rust daemon listens for logind session `Lock()` and `Unlock()` signals.
It also listens for `PrepareForSleep`, holds a sleep delay inhibitor while
the session is active, and releases it after the configured `lock_command`
has been started. The inhibitor is reacquired after resume.

Configure `lock_command` and `lock_on_sleep` in the Hyprland plugin
configuration in `hyprplasma.lua`. The default lock command is `hyprlock`
and `lock_on_sleep` defaults to true. Do not set `lock_command` to
`loginctl lock-session`: that command is the request which triggers this
integration and would feed back into itself.

The daemon exports `org.freedesktop.ScreenSaver`, including `Lock()`,
`GetActive()`, and `ActiveChanged`. D-Bus lock requests and logind lock
requests use the same non-blocking locker launcher. The daemon updates
logind's `LockedHint` when the locker starts and clears it when the locker
process exits.

HyprPlasma reads KScreenLocker's `[Daemon]` `Autolock` and `Timeout`
settings from `kscreenlockerrc` at session startup. When automatic locking is
enabled, the configured timeout triggers `loginctl lock-session`, which
starts the configured locker. `Timeout` is in minutes. Restart the Hyprland
session after changing these settings so the idle action is re-registered.

PowerDevil remains responsible for the automatic suspend timeout and sleep
mode. Hyprland's idle-notification protocol continues to provide idle time
while Hyprlock is displayed. Before logind proceeds with a suspend request,
HyprPlasma waits one second after starting a new lock process so Hyprlock has
time to render its first frame, then releases its sleep-delay inhibitor.

## Bugs

plenty of bugs and non-implemented features. please contribute. 

### Known bugs
- **Tooltip flickering:** Seems to be caused by the client repeatedly requesting a resize. Doesn't happen consistently.
- **Popup resizing:** Resizing popups causes flickering. Pretty consistent. Same cause as issue above.
- **'Edit Mode'** Going into edit mode may or may not work if there are windows in the current workspace

### Things likely to not work
- **Spectacle support:** Not implemented
- **Activities:** Not implemented
- **Certain Panel behaviors:** Not Implemented
- **Splash Screen:** Not implemented
- **Session control:** Not implemented
- **Multi-monitor support:** Not tested. I don't have another monitor. Please buy me one.
