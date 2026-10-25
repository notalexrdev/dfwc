-- dfwc default config, also used when ~/.config/dfwc/init.lua is missing or broken.
-- Copy it to ~/.config/dfwc/init.lua; dfwc reloads it on every save.
-- "Mod" is Super, or Alt when dfwc runs nested inside another desktop.

-- mako gets dfwc's "ink" theme unless you have your own config (foot gets it anyway)
local f = io.open(os.getenv("HOME") .. "/.config/mako/config")
local mako = f and "mako" or "mako -c " .. dfwc.datadir .. "/mako"
if f then f:close() end

-- notifications; without a daemon, apps wait 25 s for one at startup
dfwc.autostart("command -v mako >/dev/null && exec " .. mako)

dfwc.bind("Mod+Shift+Return", function() dfwc.spawn("foot") end)
dfwc.bind("Mod+p",            dfwc.launch)
dfwc.bind("Mod+Shift+c",      dfwc.close)
dfwc.bind("Mod+Shift+q",      dfwc.quit)
dfwc.bind("Mod+Control+r",    dfwc.reload)

-- laptop keys (F1/F2 etc. on most laptops): brightnessctl and PipeWire's wpctl
local function key(k, cmd, repeats)
	dfwc.bind(k, function() dfwc.spawn(cmd) end, { repeats = repeats })
end
key("XF86MonBrightnessUp",   "brightnessctl -q set 5%+", true)
key("XF86MonBrightnessDown", "brightnessctl -q -n1 set 5%-", true)
key("XF86AudioRaiseVolume",  "wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+", true)
key("XF86AudioLowerVolume",  "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-", true)
key("XF86AudioMute",         "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle")
key("XF86AudioMicMute",      "wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle")

-- workspaces: there's always an empty one at the end. Ctrl+Up/Down switches,
-- with Shift the window comes along; numbers jump, Shift+number sends the window
dfwc.bind("Mod+Control+Up",         function() dfwc.workspace("prev") end)
dfwc.bind("Mod+Control+Down",       function() dfwc.workspace("next") end)
dfwc.bind("Mod+Control+Shift+Up",   function() dfwc.send("prev", true) end)
dfwc.bind("Mod+Control+Shift+Down", function() dfwc.send("next", true) end)
for i = 1, 9 do
	dfwc.bind("Mod+" .. i,       function() dfwc.workspace(i) end)
	dfwc.bind("Mod+Shift+" .. i, function() dfwc.send(i) end)
end

-- canvas: Mod+c makes this workspace an endless plane; pan with Mod+Alt+arrows,
-- Mod+right-drag on empty space, or three fingers on the touchpad; zoom with
-- Mod+Alt+i/o, Mod+scroll or a pinch; Mod+o shows everything, a click goes back
dfwc.bind("Mod+c", dfwc.canvas)
dfwc.bind("Mod+o", dfwc.overview)
dfwc.bind("Mod+Alt+i", function() dfwc.magnify(1.25) end, { repeats = true })
dfwc.bind("Mod+Alt+o", function() dfwc.magnify(0.8) end, { repeats = true })
for key, d in pairs{ Left = { -80, 0 }, Right = { 80, 0 }, Up = { 0, -80 }, Down = { 0, 80 } } do
	dfwc.bind("Mod+Alt+" .. key, function() dfwc.pan(d[1], d[2]) end, { repeats = true })
end

-- arrows move a window, Shift+arrows resize it;
-- tiled windows swap with their neighbour or move the split instead
for key, d in pairs{ Left = { -20, 0 }, Right = { 20, 0 }, Up = { 0, -20 }, Down = { 0, 20 } } do
	dfwc.bind("Mod+" .. key,       function() dfwc.move(d[1], d[2]) end,   { repeats = true })
	dfwc.bind("Mod+Shift+" .. key, function() dfwc.resize(d[1], d[2]) end, { repeats = true })
end

-- More actions and settings: see README.md.
