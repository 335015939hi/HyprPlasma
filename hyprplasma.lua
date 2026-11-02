-- KDE / Qt integration
hl.env("QT_QPA_PLATFORMTHEME", "kde", true)
hl.env("QT_QPA_PLATFORM", "wayland;xcb", true)

-- APIs
--
-- hl.plugin.hyprplasma.toggle_show_desktop()
-- this function toggles the 'show desktop' feature
