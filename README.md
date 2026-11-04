# HyprPlasma

A Hyprland plugin aiming to bring KDE Plasma to Hyprland.

## Requirements

Before building, make sure you have:

- Hyprland installed with development headers and pkg-config metadata
- CMake 3.22 or newer
- A C++23-capable compiler
- A C compiler
- `wayland-server` development libraries
- `Plasma Wayland protocols` installed
- `wayland-scanner` available in `PATH`

## Protocol support

- `org_kde_kwin_idle`: implements KDE's idle-time notifications, including per-timeout `idle` and `resumed` events.
- `zwp_idle_inhibit_manager_v1`: provided by Hyprland itself. HyprPlasma observes its inhibitor state and applies it to KDE idle timers. It deliberately does not register a duplicate global.
- Input activity: pointer movement/buttons/scroll, keyboard, touch, tablet, and pointer gestures reset idle timers.
- Idle actions: Lua-configured commands run once per inactivity cycle, pause during inhibition, and re-arm after activity.

## Idle Lua API

The plugin exposes the following functions under `hl.plugin.hyprplasma`:

| Function | Description |
|---|---|
| `idle_state()` | Returns aggregate state plus per-timeout and per-action details. |
| `idle_simulate_activity()` | Simulates user activity and resets managed timers. |
| `idle_set_inhibited(bool)` | Enables/disables a manual global inhibitor. Native Wayland inhibitors are tracked separately. |
| `idle_add_action(timeout_ms, command)` | Adds a command to run once after the given idle duration. Returns an action ID. |
| `idle_remove_action(id)` | Removes an action by ID. |
| `idle_clear_actions()` | Removes all Lua-configured idle actions and returns the number removed. |

The plugin emits two Lua events:

- `plugin.hyprplasma.idle`: `idle, inhibited, idle_for_ms, active_inhibitor_count, timeout_count`
- `plugin.hyprplasma.idle_action`: `action_id, command`

Idle actions are emitted as events rather than executed through synchronous IPC. Calling Hyprland's synchronous IPC API from inside its own event loop can deadlock. Subscribe to the action event and use `hl.exec_cmd` to dispatch commands:

```lua
local hp = hl.plugin.hyprplasma

-- Execute scheduled actions through Hyprland's Lua command executor.
hl.on("plugin.hyprplasma.idle_action", function(action_id, command)
    hl.exec_cmd("exec " .. command)
end)

-- Inspect current state.
local state = hp.idle_state()
print(("idle=%s inhibited=%s idle_for=%dms"):format(
    tostring(state.idle),
    tostring(state.inhibited),
    state.idle_for_ms
))

-- Per-client KDE timeout objects.
for _, timeout in ipairs(state.timeouts) do
    print(("wl object=%d timeout=%dms idle=%s inhibited=%s"):format(
        timeout.object_id, timeout.timeout_ms,
        tostring(timeout.idle), tostring(timeout.inhibited)
    ))
end

-- Plugin-managed idle actions.
for _, action in ipairs(state.actions) do
    print(("action=%d timeout=%dms fired=%s inhibited=%s"):format(
        action.id, action.timeout_ms,
        tostring(action.fired), tostring(action.inhibited)
    ))
end

-- Run a lock command after five minutes of inactivity.
local lock_action = hp.idle_add_action(5 * 60 * 1000, "loginctl lock-session")

-- Observe state transitions.
hl.on("plugin.hyprplasma.idle", function(idle, inhibited, idle_for_ms, inhibitor_count, timeout_count)
    print(("idle=%s inhibited=%s elapsed=%dms inhibitors=%d timeouts=%d"):format(
        tostring(idle),
        tostring(inhibited),
        idle_for_ms,
        inhibitor_count,
        timeout_count
    ))
end)

-- Simulate activity or temporarily inhibit all managed idle actions:
-- hp.idle_simulate_activity()
-- hp.idle_set_inhibited(true)
-- hp.idle_set_inhibited(false)
```

The action event only runs commands if a Lua listener is registered. The plugin does not evaluate arbitrary shell commands itself. If you load the plugin after Hyprland has already parsed your Lua config, reload the config after loading the plugin so the custom event is known to `hl.on`.

## Installation

Please don't blindly copy and paste.

### Clone the repo
```bash
git clone https://github.com/335015939hi/HyprPlasma.git
cd HyprPlasma
```

### Copy Hyprland config
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

### Start KDE Plasma
```bash
plasmashell&disown
```

## Configuration

See `hyprplasma.lua`.

## Bugs

There are still bugs and non-implemented features. Please contribute.

### Known bugs
- **Tooltip flickering:** Seems to be caused by the client repeatedly requesting a resize. Doesn't happen consistently.
- **Popup resizing:** Resizing popups causes flickering. Pretty consistent. Same cause as issue above.
- **Edit Mode:** Going into edit mode may or may not work if there are windows in the current workspace.

### Things likely not to work
- **Spectacle support:** Not implemented
- **Activities:** Not implemented
- **Certain panel behaviors:** Not implemented
- **Splash screen:** Not implemented
- **Session control:** Not implemented
- **Multi-monitor support:** Not tested.
