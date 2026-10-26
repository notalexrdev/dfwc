-- dfwc's default config, also used when ~/.config/dfwc/init.lua is missing or broken.
-- Start yours with  dofile(dfwc.datadir .. "/init.lua")  and add to it; dfwc reloads
-- on every save. "Mod" is Super, or Alt when dfwc runs inside another desktop.
-- Mod+comma opens the rice menu, which saves to rice.lua next to this file.

-- mako gets dfwc's "ink" theme unless you have your own config (foot gets it anyway)
local f = io.open(os.getenv("HOME") .. "/.config/mako/config")
local mako = f and "mako" or "mako -c " .. dfwc.datadir .. "/mako"
if f then f:close() end
-- notifications; without a daemon, apps wait 25 s for one at startup
dfwc.autostart("command -v mako >/dev/null && exec " .. mako)

local function bind(keys, fn, repeats)
	dfwc.bind(keys, fn, repeats and { repeats = true } or nil)
end
local function run(cmd) return function() dfwc.spawn(cmd) end end

-- the basics
bind("Mod+Shift+Return", run("foot"))
bind("Mod+p",            dfwc.launch)          -- Mod+drag on the desktop: open it in a box
bind("Mod+Shift+c",      dfwc.close)
bind("Mod+Shift+q",      dfwc.quit)
bind("Mod+Control+r",    dfwc.reload)
bind("Mod+comma",        dfwc.menu)            -- the rice menu

-- windows
bind("Mod+j",            function() dfwc.focus("next") end)
bind("Mod+k",            function() dfwc.focus("prev") end)
bind("Mod+m",            dfwc.maximize)
bind("Mod+f",            dfwc.fullscreen)
bind("Mod+t",            dfwc.ontop)
bind("Mod+n",            dfwc.minimize)
bind("Mod+Shift+n",      dfwc.restore)         -- pick a minimized window to bring back
for key, d in pairs{ Left = { -20, 0 }, Right = { 20, 0 }, Up = { 0, -20 }, Down = { 0, 20 } } do
	bind("Mod+" .. key,       function() dfwc.move(d[1], d[2]) end, true)
	bind("Mod+Shift+" .. key, function() dfwc.resize(d[1], d[2]) end, true)
end

-- tiling, per workspace
bind("Mod+space",        function() dfwc.tiling() end)
bind("Mod+Shift+space",  dfwc.float)           -- this window floats while the rest tile
bind("Mod+Return",       dfwc.zoom)            -- this window becomes the master
bind("Mod+Tab",          function() dfwc.layout("next") end)
bind("Mod+Shift+Tab",    function() dfwc.layout("prev") end)
bind("Mod+h",            function() dfwc.mfact(-0.05) end, true)
bind("Mod+l",            function() dfwc.mfact(0.05) end, true)
bind("Mod+i",            function() dfwc.nmaster(1) end)
bind("Mod+d",            function() dfwc.nmaster(-1) end)

-- workspaces (an empty one always waits at the end) up and down, screens left and
-- right; with Shift the focused window comes along
bind("Mod+Control+Up",          function() dfwc.workspace("prev") end)
bind("Mod+Control+Down",        function() dfwc.workspace("next") end)
bind("Mod+Control+Shift+Up",    function() dfwc.send("prev", true) end)
bind("Mod+Control+Shift+Down",  function() dfwc.send("next", true) end)
bind("Mod+Control+Left",        function() dfwc.monitor("left") end)
bind("Mod+Control+Right",       function() dfwc.monitor("right") end)
bind("Mod+Control+Shift+Left",  function() dfwc.sendmonitor("left") end)
bind("Mod+Control+Shift+Right", function() dfwc.sendmonitor("right") end)
for i = 1, 9 do
	bind("Mod+" .. i,       function() dfwc.workspace(i) end)
	bind("Mod+Shift+" .. i, function() dfwc.send(i) end)
end

-- canvas: this workspace as an endless plane. Pan with Mod+Alt+arrows,
-- Mod+right-drag on empty space or three fingers; zoom with Mod+Alt+i/o,
-- Mod+scroll or a pinch; Mod+o shows everything, a click goes back
bind("Mod+c",            function() dfwc.canvas() end)
bind("Mod+o",            dfwc.overview)
bind("Mod+Alt+i",        function() dfwc.magnify(1.25) end, true)
bind("Mod+Alt+o",        function() dfwc.magnify(0.8) end, true)
for key, d in pairs{ Left = { -80, 0 }, Right = { 80, 0 }, Up = { 0, -80 }, Down = { 0, 80 } } do
	bind("Mod+Alt+" .. key, function() dfwc.pan(d[1], d[2]) end, true)
end

-- laptop keys: brightnessctl and PipeWire's wpctl
bind("XF86MonBrightnessUp",   run("brightnessctl -q set 5%+"), true)
bind("XF86MonBrightnessDown", run("brightnessctl -q -n1 set 5%-"), true)
bind("XF86AudioRaiseVolume",  run("wpctl set-volume -l 1 @DEFAULT_AUDIO_SINK@ 5%+"), true)
bind("XF86AudioLowerVolume",  run("wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"), true)
bind("XF86AudioMute",         run("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"))
bind("XF86AudioMicMute",      run("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle"))

-- More: README.md, or the cheatsheet.
