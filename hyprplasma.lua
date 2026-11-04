local home = os.getenv("HOME")

-- environment variables
hl.env("QT_QPA_PLATFORMTHEME", "kde", true)
hl.env("QT_QPA_PLATFORM", "wayland", true)
hl.env("XDG_CONFIG_DIRS", home .. "/.config/kdedefaults:/etc/xdg:/usr/share/desktop-base/kf5-settings", true)
hl.env(
	"XDG_DATA_DIRS",
	home .. "/.local/share/flatpak/exports/share:/var/lib/flatpak/exports/share:/usr/local/share/:/usr/share/",
	true
)
hl.env("XKB_DEFAULT_LAYOUT", "us", true)
hl.env("XKB_DEFAULT_MODEL", "pc105", true)
hl.env("XDG_BACKEND", "wayland", true)
--hl.env("XDG_CURRENT_DESKTOP", "KDE", true)
hl.env("XDG_SESSION_TYPE", "wayland", true)
hl.env("XDG_SESSION_DESKTOP", "KDE", true)
hl.env("DESKTOP_SESSION", "plasma", true)
hl.env("KDE_APPLICATIONS_AS_SCOPE", "1", true)
hl.env("KDE_FULL_SESSION", "true", true)
hl.env("KDE_SESSION_VERSION", "6", true)
hl.env("KGLOBALACCELD_PLATFORM", "org.kde.kwin", true)

hl.exec_cmd(
	"dbus-update-activation-environment --systemd "
		.. "DISPLAY "
		.. "XDG_SESSION_ID XDG_SESSION_PATH XDG_CURRENT_DESKTOP "
		.. "XDG_CONFIG_DIRS XDG_DATA_DIRS XDG_BACKEND XDG_SESSION_TYPE"
		.. "XDG_SESSION_PATH XDG_SESSION_ID XDG_VTNR XDG_SEAT XDG_SEAT_PATH "
		.. "XKB_DEFAULT_MODEL XKB_DEFAULT_LAYOUT "
		.. "XDG_SESSION_DESKTOP DESKTOP_SESSION WAYLAND_DISPLAY XDG_CURRENT_DESKTOP "
		.. "QT_QPA_PLATFORM_THEME QT_QPA_PLATFORM QML2_IMPORT_PATH QML_IMPORT_PATH QT_PLUGIN_PATH "
		.. "KDE_APPLICATIONS_AS_SCOPE KDE_SESSION_VERSION KDE_FULL_SESSION KGLOBALACCELD_PLATFORM "
		.. "PAM_KWALLET5_LOGIN "
)

-- a simple wrapper that starts services
-- it will notify if a service fails to start
local function systemd_service(service)
	local cmd = "sleep 1;systemctl --user start "
		.. service
		.. " || notify-send -a hyprland 'failed to start service "
		.. service
		.. "'"
	hl.exec_cmd(cmd)
end

hl.on("hyprland.start", function()
	-- KWallet
	hl.exec_cmd("exec kwalletd6")
	--the hyprplasma daemon. source located in cli/
	hl.exec_cmd("exec hyprplasma daemon || notify-send -a hyprland 'hyprplasma daemon failed'")

	-- various KDE services
	systemd_service("plasma-polkit-agent")
	systemd_service("plasma-xdg-desktop-portal-kde")
	systemd_service("plasma-powerdevil")
	systemd_service("plasma-kcminit-phase1")
	systemd_service("plasma-kcminit")
	systemd_service("plasma-gmenudbusmenuproxy")
	systemd_service("plasma-kded6")
	systemd_service("plasma-kglobalaccel")
	systemd_service("plasma-kwallet-pam")
	--plasmashell.
	systemd_service("plasma-plasmashell")
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

-- APIs
--
-- hl.plugin.hyprplasma.toggle_show_desktop()
-- this function toggles the 'show desktop' feature
