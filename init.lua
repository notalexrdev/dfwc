-- dfwc default config, also used when ~/.config/dfwc/init.lua is missing or broken.
-- Copy it to ~/.config/dfwc/init.lua; dfwc reloads it on every save.
-- "Mod" is Super, or Alt when dfwc runs nested inside another desktop.

-- foot and mako get dfwc's "ink" theme, unless you have your own config for them
local function mine(path)
	local f = io.open(os.getenv("HOME") .. path)
	if f then f:close() end
	return f ~= nil
end
local term = mine("/.config/foot/foot.ini") and "foot" or "foot -c " .. dfwc.datadir .. "/foot.ini"
local mako = mine("/.config/mako/config") and "mako" or "mako -c " .. dfwc.datadir .. "/mako"

-- notifications; without a daemon, apps wait 25 s for one at startup
dfwc.autostart("command -v mako >/dev/null && exec " .. mako)

dfwc.bind("Mod+Shift+Return", function() dfwc.spawn(term) end)
dfwc.bind("Mod+p",            dfwc.launch)
dfwc.bind("Mod+Shift+c",      dfwc.close)
dfwc.bind("Mod+Shift+q",      dfwc.quit)
dfwc.bind("Mod+Control+r",    dfwc.reload)

-- More actions and settings: see README.md.
