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

-- arrows move a window (hold two for diagonals), Shift+arrows resize it;
-- tiled windows swap with their neighbour or move the split instead
for key, d in pairs{ Left = { -20, 0 }, Right = { 20, 0 }, Up = { 0, -20 }, Down = { 0, 20 } } do
	dfwc.bind("Mod+" .. key,       function() dfwc.move(d[1], d[2]) end,   { repeats = true })
	dfwc.bind("Mod+Shift+" .. key, function() dfwc.resize(d[1], d[2]) end, { repeats = true })
end

-- More actions and settings: see README.md.
