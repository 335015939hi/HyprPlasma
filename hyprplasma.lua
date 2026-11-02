-- KDE / Qt integration
hl.env("QT_QPA_PLATFORMTHEME", "kde", true)
hl.env("QT_QPA_PLATFORM", "wayland;xcb", true)

hl.config({
	plugin = {
		hyprplasma = {
			-- when switching workspaces, cancel show desktop. this only works if plasmashell is running
			cancel_showdesktop_on_workspaceswitch = true,
		},
	},
})

-- APIs
--
-- hl.plugin.hyprplasma.toggle_show_desktop()
-- this function toggles the 'show desktop' feature
