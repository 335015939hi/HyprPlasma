-- environment variables
local function update_env()
	local home = os.getenv("HOME")
	hl.env("QT_QPA_PLATFORMTHEME", "kde")
	hl.env("QT_QPA_PLATFORM", "wayland;xcb")
	hl.env("GDK_BACKEND", "wayland,x11,*")
	hl.env("XDG_CONFIG_DIRS", home .. "/.config/kdedefaults:/etc/xdg:/usr/share/desktop-base/kf5-settings")
	hl.env(
		"XDG_DATA_DIRS",
		home .. "/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share/:/usr/share/",
		true
	)
	hl.env("XKB_DEFAULT_LAYOUT", "us")
	hl.env("XKB_DEFAULT_MODEL", "pc105")
	hl.env("XDG_BACKEND", "wayland")
	hl.env("XDG_CURRENT_DESKTOP", "KDE")
	hl.env("XDG_SESSION_TYPE", "wayland")
	hl.env("XDG_SESSION_DESKTOP", "KDE")
	hl.env("DESKTOP_SESSION", "plasma")
	hl.env("KDE_APPLICATIONS_AS_SCOPE", "1")
	hl.env("KDE_FULL_SESSION", "true")
	hl.env("KDE_SESSION_VERSION", "6")
	hl.env("KGLOBALACCELD_PLATFORM", "org.kde.kwin")

	--os.execute(
	--	"dbus-update-activation-environment --systemd --all || notify-send -a hyprland 'dbus-update-activation-environment failed'"
	--)
	--hl.exec_cmd(
	--	"dbus-update-activation-environment --systemd "
	--		.. "DISPLAY "
	--		.. "XDG_SESSION_ID XDG_SESSION_PATH XDG_CURRENT_DESKTOP "
	--		.. "XDG_CONFIG_DIRS XDG_DATA_DIRS XDG_BACKEND XDG_SESSION_TYPE"
	--		.. "XDG_SESSION_PATH XDG_SESSION_ID XDG_VTNR XDG_SEAT XDG_SEAT_PATH "
	--		.. "XKB_DEFAULT_MODEL XKB_DEFAULT_LAYOUT GDK_BACKEND "
	--		.. "XDG_SESSION_DESKTOP DESKTOP_SESSION WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
	--		.. "QT_QPA_PLATFORM_THEME QT_QPA_PLATFORM QML2_IMPORT_PATH QML_IMPORT_PATH QT_PLUGIN_PATH "
	--		.. "KDE_APPLICATIONS_AS_SCOPE KDE_SESSION_VERSION KDE_FULL_SESSION KGLOBALACCELD_PLATFORM "
	--		.. "PAM_KWALLET5_LOGIN "
	--)
end

hl.on("hyprland.start", function()
	update_env()
	--plasmashell.
	hl.exec_cmd("exec plasmashell")
	-- KWallet
	hl.exec_cmd("exec kwalletd6")
	hl.exec_cmd(
		"sleep 0.5;/usr/share/libpam-kwallet-common/pam_kwallet_init && busctl --user call org.kde.kwalletd6 /modules/kwalletd6 org.kde.KWallet open sxs kdewallet 0 ''"
	)
	--the hyprplasma daemon. source located in cli/
	hl.exec_cmd("exec hyprplasma daemon || notify-send -a hyprland 'hyprplasma daemon failed'")

	-- various KDE services
	hl.exec_cmd("exec /usr/lib/x86_64-linux-gnu/libexec/polkit-kde-authentication-agent-1")
	hl.exec_cmd("exec /usr/lib/x86_64-linux-gnu/libexec/xdg-desktop-portal-kde")
	hl.exec_cmd("exec /usr/lib/x86_64-linux-gnu/libexec/org_kde_powerdevil")
	hl.exec_cmd("qdbus org.kde.kcminit /kcminit org.kde.KCMInit.runPhase1 && kcminit_startup")
	hl.exec_cmd("exec gmenudbusmenuproxy")
	hl.exec_cmd("exec kded6")
	hl.exec_cmd("exec /usr/lib/x86_64-linux-gnu/libexec/kglobalacceld")
end)

if hl.plugin and hl.plugin.hyprplasma then
	--plugin config
	hl.config({
		plugin = {
			hyprplasma = {
				-- when switching workspaces, cancel show desktop. this only works if plasmashell is running
				cancel_showdesktop_on_workspaceswitch = true,
			},
		},
	})
end

--opening a window on empty workspace may keep focus on the desktop instead of focusing the window. this fixes it
hl.on("window.open_early", function(w)
	if not hl.get_active_window() then
		local no_warps = hl.get_config("cursor:no_warps")
		hl.config({ ["cursor.no_warps"] = true })
		hl.dispatch(hl.dsp.focus({ window = w }))
		hl.config({ ["cursor.no_warps"] = no_warps })
	end
end)

--[[
HyprPlasma Lua API
==================

All functions below are registered in the namespace:
    hl.plugin.hyprplasma

The namespace is available after the HyprPlasma plugin has loaded. Check that
hl.plugin.hyprplasma exists before calling its functions if this file may be
loaded without the plugin.

General
-------

toggle_show_desktop()
    Toggle Plasma's "Show Desktop" state.

Idle monitoring
---------------

idle_state() -> table
    Return a snapshot of the idle subsystem's current state.

    Top-level fields:
      available             boolean  Whether the idle subsystem initialized.
      idle                  boolean  True if any KDE idle timeout is currently
                                     idle, or any HyprPlasma idle action fired.
      inhibited             boolean  Effective inhibition state (manual
                                     inhibition OR a detected inhibitor).
      manual_inhibited      boolean  Whether inhibition was enabled through
                                     idle_set_inhibited().
      active_inhibitors     number   Number of detected window/surface idle
                                     inhibitors. Does not include the manual
                                     inhibit flag.
      timeout_count         number   Number of KDE idle timeout objects.
      idle_timeout_count    number   Number of timeout objects that fired.
      action_count          number   Number of registered HyprPlasma actions.
      fired_action_count    number   Number of actions that fired.
      idle_for_ms           number   Milliseconds since the last activity.
      next_timeout_ms       number or nil
                                     Remaining milliseconds until the nearest
                                     pending timeout or action, or nil if
                                     nothing is pending.

    The timeouts array contains one entry for each KDE idle timeout:
      object_id             number   Wayland object ID.
      timeout_ms            number   Requested idle interval in milliseconds.
      idle                  boolean  Whether this timeout has fired.
      inhibited             boolean  Whether this timeout is inhibited.

    The actions array contains one entry for each registered idle action:
      id                    number   ID returned by idle_add_action().
      timeout_ms            number   Configured idle interval in milliseconds.
      fired                 boolean  Whether this action has fired.
      inhibited             boolean  Whether this action is inhibited.

    The timeouts and actions arrays are snapshots. Their order is not
    guaranteed. The idle flag is not simply a clock threshold: it becomes true
    when a KDE timeout fires or a HyprPlasma action fires.

idle_simulate_activity()
    Notify the idle subsystem of user activity. This resets the idle interval
    for KDE timeout objects and restarts the idle-action cycle.

idle_set_inhibited(inhibited: boolean) -> boolean
    Set or clear HyprPlasma's manual idle-inhibition flag. This flag is
    combined with automatically detected inhibitors; clearing the manual flag
    does not override an inhibitor that is still active.

    The return value is the resulting manual_inhibited state, not a general
    success indicator. The argument must be a Lua boolean.

Idle actions
------------

idle_add_action(timeout_ms: integer, command: string) -> id
    Register a command to be associated with an idle interval, in
    milliseconds. timeout_ms must be between 0 and 2^32-1, inclusive.
    command must be a non-empty string.

    On success, return a positive action ID. On failure (for example, if the
    idle subsystem is unavailable or allocation fails), return:
        nil, error_message

    An action fires once per idle cycle. User activity resets it. Actions are
    paused while idle inhibition is active; when inhibition ends, a full idle
    interval starts again.

    IMPORTANT: this function does NOT execute the command. When the action
    fires, HyprPlasma emits the plugin.hyprplasma.idle_action event with the
    action ID and command. Your Lua event handler must decide what to do with
    the command. Only register command strings you trust.

idle_remove_action(id: integer) -> boolean
    Remove the action with this ID. Returns true if an action was removed,
    or false if no action with that ID existed.

idle_clear_actions() -> number
    Remove all registered actions and return the number removed.

Events
------

plugin.hyprplasma.idle
    Emitted when the idle/inhibited state changes, and when the subsystem
    publishes a forced state update. Callback arguments, in order:
        idle                    boolean
        inhibited               boolean
        idle_for_ms             number
        active_inhibitor_count  number
        timeout_count           number

    active_inhibitor_count is the detected window/surface inhibitor count;
    manual inhibition is reported separately by idle_state().manual_inhibited
    and is reflected in the inhibited argument.

plugin.hyprplasma.idle_action
    Emitted when an idle action fires. Callback arguments, in order:
        action_id  number
        command    string

Example: run a lock command after five minutes of inactivity:

    local hp = hl.plugin.hyprplasma

    hp.idle_add_action(5 * 60 * 1000, "loginctl lock-session")

    hl.on("plugin.hyprplasma.idle_action", function(action_id, command)
        hl.exec_cmd("exec " .. command)
    end)

Example: inspect state or respond to idle transitions:

    local hp = hl.plugin.hyprplasma

    local state = hp.idle_state()
    if state.available then
        print("Idle:", state.idle, "Inhibited:", state.inhibited)
    end

    hl.on("plugin.hyprplasma.idle", function(
        idle, inhibited, idle_for_ms, active_inhibitor_count, timeout_count
    )
        print("Idle state:", idle, "inhibited:", inhibited,
              "idle for:", idle_for_ms, "ms")
    end)
]]
