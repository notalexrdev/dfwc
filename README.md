# dfwc

dynamic floating wayland compositor: dfwm for Wayland, on wlroots 0.20 and scenefx.
Windows float by default. Everything you change lives in `~/.config/dfwc/init.lua`,
which reloads every time you save it.

## Build and install

Needs `wlroots0.20 scenefx lua libinput wayland-protocols` and a C23 compiler (`#embed`).

    make
    sudo make install

Pick **dfwc** on the login screen, or run `dfwc-session` from a TTY.
The log goes to `~/.local/state/dfwc/log`; `dfwc-session -d` logs debug output.

Recommended companions: `foot` (terminal), `wmenu` (launcher), `swaybg` (wallpaper),
`xdg-desktop-portal-wlr` (screen sharing).

Try it in a window inside your current desktop first: `./dfwc -s foot`.
Nested, the modifier is Alt instead of Super.

## Keys that always work

| Keys | Action |
|---|---|
| Ctrl+Alt+Backspace | quit dfwc |
| Ctrl+Alt+F1–F12 | switch to a TTY |

These can't be rebound, so a broken config never locks you in.

## Config

Copy `/usr/local/share/dfwc/init.lua` to `~/.config/dfwc/init.lua`.
If the file has an error, dfwc logs it and keeps the last config that worked.
Files next to `init.lua` can be loaded with `require`.

### Keys

    dfwc.bind("Mod+Shift+Return", function() dfwc.spawn("foot") end)
    dfwc.bind("Mod+Left", function() dfwc.move(-20, 0) end, { repeats = true })

`Mod` is Super (Alt when nested). The other modifiers are `Super`, `Alt`, `Shift`
and `Ctrl`/`Control`. Key names are xkb keysym names (`Return`, `space`, `F1`, `p`).
A later bind for the same keys replaces an earlier one. `repeats = true` repeats
the bind while the key is held.

### Actions

| Function | Does |
|---|---|
| `dfwc.spawn(cmd)` | run a shell command |
| `dfwc.autostart(cmd)` | run a command once per session (saving never restarts it) |
| `dfwc.close()` | close the focused window |
| `dfwc.focus("next")` / `("prev")` | cycle focus, most recent first; the order sticks when Mod is released |
| `dfwc.move(dx, dy)` | move the focused window by pixels |
| `dfwc.resize(dw, dh)` | resize the focused window by pixels |
| `dfwc.maximize()` | toggle maximize |
| `dfwc.fullscreen()` | toggle fullscreen |
| `dfwc.reload()` | reload the config |
| `dfwc.quit()` | quit dfwc |

### Settings

    dfwc.set{ xkb_layout = "de", touchpad_tap = true, background = "#1e1e2e" }

| Setting | Default |
|---|---|
| `xkb_layout`, `xkb_variant`, `xkb_options`, `xkb_model` | from `XKB_DEFAULT_*` |
| `repeat_rate`, `repeat_delay` | 25 per second, 600 ms |
| `touchpad_tap`, `touchpad_natural_scroll`, `touchpad_dwt` | true, false, true |
| `touchpad_speed` | 0 (-1 to 1) |
| `cursor_theme`, `cursor_size` | from `XCURSOR_THEME`, 24 |
| `background` | `"#222222"` (`#rrggbb` or `#rrggbbaa`) |
| `scale` | 1 (fractional works) |
| `modkey` | `"auto"`: Super, or Alt when nested |

Every setting applies immediately when the file is saved.

## Mouse

Focus follows the mouse; clicking raises a window.
Mod+left-drag moves a window, Mod+right-drag resizes it from the nearest corner.

## License

GPL-3.0-or-later.
