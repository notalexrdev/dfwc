-- dfwc default config, also used when ~/.config/dfwc/init.lua is missing or broken.
-- Copy it to ~/.config/dfwc/init.lua; dfwc reloads it on every save.
-- "Mod" is Super, or Alt when dfwc runs nested inside another desktop.

dfwc.bind("Mod+Shift+Return", function() dfwc.spawn("foot") end)
dfwc.bind("Mod+p",            function() dfwc.spawn("wmenu-run") end)
dfwc.bind("Mod+Shift+c",      dfwc.close)
dfwc.bind("Mod+Shift+q",      dfwc.quit)
dfwc.bind("Mod+Control+r",    dfwc.reload)

-- More actions and settings: see README.md.
