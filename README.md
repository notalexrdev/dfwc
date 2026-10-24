# dfwc

dynamic floating wayland compositor: dfwm for Wayland, on wlroots 0.20 and scenefx.
Windows float by default. Everything you change lives in `~/.config/dfwc/init.lua`,
which reloads every time you save it.

## Build and install

Needs `wlroots0.20 scenefx lua libinput fcft wayland-protocols` and a C23 compiler (`#embed`).

    make
    sudo make install

On Alpine (edge only; stable lacks wlroots 0.20 and gcc 15): `make LUA=lua5.5`.

Pick **dfwc** on the login screen, or run `dfwc-session` from a TTY.
`dfwc-session` reads shell `export` lines from `~/.config/dfwc/env` first.
The log goes to `~/.local/state/dfwc/log`; `dfwc-session -d` logs debug output.

Recommended companions: `foot` (terminal), `wmenu` (launcher), `swaybg` (wallpaper),
`xdg-desktop-portal-wlr` (screen sharing), `mako` (notifications).

Run a notification daemon: without one, apps that send a notification wait 25 s for
KDE's or GNOME's to start, and Electron apps open that much slower.
Screen sharing asks which screen to share through wmenu.

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
| `dfwc.tiling()` | switch between floating and tiling (`true`/`false` to set) |
| `dfwc.layout(name)` | pick a tiling layout |
| `dfwc.float()` | let the focused window float while the rest tile |
| `dfwc.zoom()` | make the focused window the master |
| `dfwc.mfact(delta)` | grow/shrink the master area, e.g. `0.05` |
| `dfwc.nmaster(delta)` | more/fewer master windows, e.g. `1` |
| `dfwc.addlayout(name, fn)` | add your own layout (see below) |
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
| `wallpaper` | true: a built-in wallpaper (a soft glow and a faint dfwc mark); false: plain `background` |
| `scale` | 1 (fractional works) |
| `modkey` | `"auto"`: Super, or Alt when nested |
| `border_width` | 2 (or `{ top, right, bottom, left }`) |
| `border_focused`, `border_unfocused` | `"#9a9ea3"`, `"#3c3f43"` (greys) |
| `inner_border_width` | 0: a second border hugging the window |
| `inner_border_focused`, `inner_border_unfocused` | `"#1d1f21"` |
| `border_gap` | 0: see-through space between window and border |
| `border_corners` | 0: full border; N draws only N-pixel corner brackets |
| `corner_radius` | 0: square; N rounds windows and borders |
| `opacity` | 1 (0.05 to 1) |
| `error_alarm_fullscreen` | false: also blink the whole screen red on config errors |
| `launcher` | `"wmenu-run"`: runs after you draw a box |
| `restore_menu` | `"wmenu -p restore"`: gets `id title` lines, prints the picked one |
| `box_timeout` | 5000 ms a drawn box waits for its window |
| `snap` | 10 px: moved windows snap to screen and window edges (0 turns it off) |
| `tiling` | false |
| `layout` | `"tile"` |
| `mfact`, `nmaster` | 0.55, 1 |
| `gaps_inner`, `gaps_outer` | 0, 0 |
| `smart_gaps` | false: a lone tiled window gets no gaps, borders or rounding |
| `tiled_drag` | `"swap"`: dragging a tiled window onto another swaps them; `"snap"` puts it back in its tile; `"float"` makes it float |

Every setting applies immediately when the file is saved. When a saved config has an
error, all borders blink red three times and a black box at the top of the screen shows
the file, line and message until the config loads fine again.

`tiling`, `layout`, `mfact` and `nmaster` are also changed by keys while dfwc runs;
saving the config keeps those changes unless you edited that setting's line.

### Tiling

Layouts: `tile` (master left, stack right), `bstack` (master on top), `spiral`, `dwindle`,
`centeredmaster`, `grid`, `monocle`. Switching tiling off puts every window back where it
floated. Dialogs, fixed-size windows and windows drawn into a box while tiling keep floating.
Dragging a tiled window and dropping it on another swaps the two (see `tiled_drag`);
`dfwc.move` on a tiled window swaps it with its neighbor in that direction, once per press.

Your own layout gets the window count, the area and `{ mfact, nmaster, gap }`, and
returns one box per window, master first:

    dfwc.addlayout("columns", function(n, area, p)
        local boxes = {}
        for i = 1, n do
            local w = area.w // n
            boxes[i] = { x = area.x + (i - 1) * w, y = area.y, w = w, h = area.h }
        end
        return boxes
    end)
    dfwc.layout("columns")

It runs only when windows come, go or the layout changes; if it errors, dfwc uses `tile`.

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
