# dfwc

dynamic floating wayland compositor: dfwm for Wayland, on wlroots 0.20 and scenefx.
Windows float by default. Everything you change lives in `~/.config/dfwc/init.lua`,
which reloads every time you save it.

## Build and install

Needs `wlroots0.20 scenefx lua libinput fcft wayland-protocols` and a C23 compiler (`#embed`).

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
| `dfwc.ontop()` | toggle always on top |
| `dfwc.minimize()` | hide the focused window |
| `dfwc.restore()` | pick a hidden window from a menu to bring back |
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
| `background` | `"#1d1f21"` (colors are `#rrggbb` or `#rrggbbaa`) |
| `scale` | 1 (fractional works) |
| `modkey` | `"auto"`: Super, or Alt when nested |
| `border_width` | 2 (or `{ top, right, bottom, left }`) |
| `border_focused`, `border_unfocused` | `"#81a2be"`, `"#373b41"` |
| `inner_border_width` | 0: a second border hugging the window |
| `inner_border_focused`, `inner_border_unfocused` | `"#1d1f21"` |
| `border_gap` | 0: see-through space between window and border |
| `border_corners` | 0: full border; N draws only N-pixel corner brackets |
| `corner_radius` | 2: rounds windows and borders (0 is square) |
| `opacity` | 1 (0.05 to 1) |
| `error_alarm_fullscreen` | false: also blink the whole screen red on config errors |
| `launcher` | `"wmenu-run"`: runs after you draw a box |
| `restore_menu` | `"wmenu -p restore"`: gets `id title` lines, prints the picked one |
| `box_timeout` | 5000 ms a drawn box waits for its window |
| `snap` | 10 px: moved windows snap to screen and window edges (0 turns it off) |

Every setting applies immediately when the file is saved. When a saved config has an
error, all borders blink red three times and a black box at the top of the screen shows
the file, line and message until the config loads fine again.

### Per-window looks

    dfwc.style(function(win)
        if win.app_id == "foot" then return { opacity = 0.85 } end
        if not win.focused then return { border = "#444444" } end
    end)

The function gets `app_id`, `title`, `focused`, `maximized`, `fullscreen` and `ontop`, and may
return `border`, `inner_border` and `opacity` to override the settings for that window.
It runs only when one of those changes, never per frame. `dfwc.style(nil)` removes it.
Fullscreen windows never get borders, rounding or transparency.

## Mouse

Focus follows the mouse; clicking raises a window.
Mod+left-drag moves a window, Mod+right-drag resizes it from the nearest corner.

Mod+left-drag on the empty desktop draws a box and opens the launcher: the next window
that opens takes the box. A short click instead of a drag opens it at its own size
at that spot. Escape while drawing cancels; an unused box disappears after `box_timeout`.

## License

GPL-3.0-or-later.
