# dfwc

**dynamic floating wayland compositor**: dfwm for Wayland, on wlroots 0.20 and scenefx.
Small, fast, and riceable from one Lua file that reloads every time you save it.

- **Floating first.** Windows float where you put them. Mod+drag on the desktop draws a
  box, and the app you launch opens in it.
- **Tiling when you want it.** Seven layouts (tile, bstack, spiral, dwindle, centered
  master, grid, monocle), or your own in Lua, per workspace.
- **Canvas.** Turn a workspace into an endless plane: pan, zoom, and get an overview
  of everything, while every window stays live.
- **Dynamic workspaces** that slide, with one empty workspace always waiting at the end.
- **Looks.** Borders with gradients that can spin, rounding, shadows, blur, fades and
  glides, all from Lua or from the **rice menu** (Mod+comma), live.
- **Everything else a desktop needs:** XWayland, games (pointer lock, raw input, tearing),
  lock screen and idle, screen sharing, window rules, and a broken config never locks you out.

It ships with one theme, **ink**: black, greys and one bright line for the focused window,
across dfwc, wmenu, foot and mako.

## Install

Needs `wlroots0.20 scenefx lua libinput fcft libpng libxcb xcb-util-wm wayland-protocols` and a
C23 compiler (`#embed`, so gcc 15); `xwayland` at runtime for X11 apps.

    make
    sudo make install

On Alpine (edge; stable lacks wlroots 0.20 and gcc 15): `make LUA=lua5.5`.

Pick **dfwc** on the login screen, or run `dfwc-session` from a TTY. To try it inside your
current desktop first, run `./dfwc -s foot`; there, Mod is Alt instead of Super.

Worth having: `foot` (terminal), `wmenu` (launcher), `mako` (notifications; started for you
if installed), `xdg-desktop-portal-wlr` (screen sharing, picked through wmenu),
`swaylock` and `swayidle` (lock screen), `brightnessctl` (laptop brightness keys).

`dfwc-session` reads shell `export` lines from `~/.config/dfwc/env` first and logs to
`~/.local/state/dfwc/log` (`-d` for debug output).

## Keys

Mod is Super. These are the defaults; every one of them can be changed in Lua.

| Keys | Does |
|---|---|
| Mod+Shift+Return | terminal (foot) |
| Mod+p | launcher (wmenu) |
| Mod+comma | rice menu |
| Mod+Shift+c / Mod+Shift+q | close the window / quit dfwc |
| Mod+Ctrl+r | reload the config (saving reloads it anyway) |
| Mod+j / Mod+k | focus the next / previous window, most recent first |
| Mod+arrows / Mod+Shift+arrows | move / resize the window (hold to repeat) |
| Mod+m / Mod+f / Mod+t | maximize / fullscreen / always on top |
| Mod+n / Mod+Shift+n | minimize / pick one to bring back |
| Mod+space | tiling on or off for this workspace |
| Mod+Shift+space | this window floats while the rest tile |
| Mod+Return | this window becomes the master |
| Mod+Tab / Mod+Shift+Tab | next / previous layout |
| Mod+h / Mod+l, Mod+i / Mod+d | master smaller / bigger, more / fewer masters |
| Mod+Ctrl+Up / Down | previous / next workspace (with Shift the window comes along) |
| Mod+1…9 / Mod+Shift+1…9 | jump to a workspace / send the window there |
| Mod+c | canvas on or off for this workspace |
| Mod+Alt+arrows, Mod+right-drag, three fingers | pan a canvas |
| Mod+Alt+i / o, Mod+scroll, pinch | zoom a canvas |
| Mod+o | canvas overview; a click zooms back in there |
| brightness, volume and mute keys | brightnessctl and wpctl |
| Ctrl+Alt+Backspace | quit dfwc; always works, except while the screen is locked |
| Ctrl+Alt+F1…F12 | switch to a TTY; always works |

**Mouse:** focus follows the mouse and a click raises. Mod+left-drag moves a window,
Mod+right-drag resizes it from the nearest corner. Mod+left-drag on the empty desktop
draws a box and opens the launcher: the next window takes the box, and morphs into it.
A click instead of a drag opens it at its own size there; Escape cancels.

## Rice menu

Mod+comma opens a panel with the looks that matter most: presets (ink, soft, neon, bare),
wallpaper, background, border width, rounding, colors and gradient, spin, shadow, blur,
opacity, animation speeds, gaps, and this workspace's tiling, canvas and layout.

Arrows (or hjkl) pick and change, a click steps a row (right button: back) or sets a
slider, scrolling over a row changes it, Escape or a click outside closes it. Every change
shows at once and is saved to `~/.config/dfwc/rice.lua`, which loads after `init.lua`. That
file only holds what the menu changed, so `init.lua` still decides everything else; delete
a line there to hand a setting back to it.

## Config

dfwc runs `~/.config/dfwc/init.lua`, or its built-in default when there is none. The
easiest start keeps the defaults and adds to them:

    dofile(dfwc.datadir .. "/init.lua")
    dfwc.set{ xkb_layout = "de", gaps_inner = 8, gaps_outer = 8 }
    dfwc.bind("Mod+b", function() dfwc.spawn("firefox") end)

Saving applies everything at once. If the file has an error, dfwc keeps the last config
that worked, blinks every border red and shows the file, line and message at the top of
the screen until it loads fine again. Files next to `init.lua` can be loaded with `require`.

### Keys

    dfwc.bind("Mod+Shift+Return", function() dfwc.spawn("foot") end)
    dfwc.bind("Mod+Left", function() dfwc.move(-20, 0) end, { repeats = true })

`Mod` is Super (Alt when nested); the others are `Super`, `Alt`, `Shift` and
`Ctrl`/`Control`. Key names are xkb keysym names (`Return`, `space`, `comma`, `p`,
`XF86AudioMute`). A later bind for the same keys wins. `repeats = true` repeats the bind
while the key is held.

### Actions

| Function | Does |
|---|---|
| `dfwc.spawn(cmd)` | run a shell command |
| `dfwc.autostart(cmd)` | run a command once per session (saving never restarts it) |
| `dfwc.launch()` | run the `launcher` |
| `dfwc.menu()` | open or close the rice menu |
| `dfwc.close()`, `dfwc.quit()`, `dfwc.reload()` | close the focused window, quit, reload |
| `dfwc.focus("next")` / `("prev")` | cycle focus, most recent first; the order sticks when Mod is released |
| `dfwc.move(dx, dy)`, `dfwc.resize(dw, dh)` | move or resize the focused window by pixels |
| `dfwc.maximize()`, `dfwc.fullscreen()`, `dfwc.ontop()` | toggle those |
| `dfwc.minimize()`, `dfwc.restore()` | hide the window; pick a hidden one to bring back |
| `dfwc.workspace(n)` | show the n-th workspace, or `"next"` / `"prev"` |
| `dfwc.send(n, follow)` | the focused window to workspace n (or `"next"`/`"prev"`); `true` goes along |
| `dfwc.tiling()` | floating ↔ tiling for this workspace (`true`/`false` sets it) |
| `dfwc.layout(name)` | a tiling layout, or `"next"` / `"prev"` |
| `dfwc.float()`, `dfwc.zoom()` | let the window float while the rest tile; make it the master |
| `dfwc.mfact(delta)`, `dfwc.nmaster(delta)` | master area size (e.g. `0.05`), master count (e.g. `1`) |
| `dfwc.addlayout(name, fn)` | your own layout (see Tiling) |
| `dfwc.canvas()` | canvas on or off for this workspace (`true`/`false` sets it) |
| `dfwc.pan(dx, dy)`, `dfwc.magnify(f)` | move a canvas's view; zoom it by f around the pointer |
| `dfwc.overview()` | zoom a canvas out to fit every window; again, or a click, goes back |
| `dfwc.style(fn)`, `dfwc.rule(fn)` | per-window looks, and how windows open (see below) |
| `dfwc.datadir` | where dfwc's default config, themes and wallpapers are installed |

### Settings

    dfwc.set{ border_focused = { "#89b4fa", "#f5c2e7" }, border_spin = 6, corner_radius = 10 }

| Setting | Default |
|---|---|
| `xkb_layout`, `xkb_variant`, `xkb_options`, `xkb_model` | from `XKB_DEFAULT_*` |
| `repeat_rate`, `repeat_delay` | 25 per second, 600 ms |
| `touchpad_tap`, `touchpad_natural_scroll`, `touchpad_dwt` | true, false, true |
| `touchpad_speed` | 0 (-1 to 1) |
| `cursor_theme`, `cursor_size` | from `XCURSOR_THEME`, 24 |
| `scale` | 1 (fractional works) |
| `modkey` | `"auto"`: Super, or Alt when nested |
| `background` | `"#000000"` (colors are `#rrggbb` or `#rrggbbaa`) |
| `wallpaper` | `"girl"`; also `"matrix"`, `"hands"`, a path to a PNG, or `false` for plain `background` |
| `border_width` | 2 (or `{ top, right, bottom, left }`) |
| `border_focused`, `border_unfocused` | `"#a3a3a3"`, `"#262626"`; two colors make a gradient: `{ from, to }` |
| `border_angle` | 45: gradient direction in degrees (0 left to right, 90 top down) |
| `border_spin` | 0: seconds per turn of a spinning gradient (costs a little CPU while one shows) |
| `inner_border_width`, `inner_border_focused`, `inner_border_unfocused` | 0, `"#000000"`: a second border hugging the window |
| `border_gap` | 0: see-through space between window and border |
| `border_corners` | 0: full border; N draws only N-pixel corner brackets |
| `corner_radius` | 0: square; N rounds windows and borders |
| `opacity` | 1 (0.05 to 1) |
| `shadow_size`, `shadow_color`, `shadow_offset` | 10 px, `"#0000008c"`, `{ 6, 8 }`: under floating windows and menus, falling down-right (0: none) |
| `blur`, `blur_radius`, `blur_passes` | true, 5, 3: the wallpaper, blurred, behind see-through windows |
| `anim_open`, `anim_close` | 180, 140 ms: windows fade in and out, a drawn box morphs into its window |
| `anim_focus` | 150 ms: border colors blend when focus moves |
| `anim_move` | 200 ms: windows glide into place; workspaces slide |
| `anim_curve` | `{ 0.2, 0.9, 0.3, 1 }`: the easing of all of them, as a CSS cubic-bezier |
| `tiling`, `canvas` | false, false: what new workspaces start as |
| `layout`, `mfact`, `nmaster` | `"tile"`, 0.55, 1 |
| `gaps_inner`, `gaps_outer`, `smart_gaps` | 0, 0, false (a lone tiled window gets no gaps, borders or rounding) |
| `tiled_drag` | `"swap"`: dropping a tiled window on another swaps them; `"snap"`: back to its tile; `"float"` |
| `snap` | 10 px: moved windows snap to screen and window edges (0: off) |
| `launcher`, `restore_menu` | wmenu in ink colors; the restore menu gets `id title` lines and prints the pick |
| `box_timeout` | 15000 ms a drawn box waits for its window after the launcher closes |
| `error_alarm_fullscreen` | false: also blink the whole screen red on config errors |
| `tearing` | false: fullscreen games that ask may skip vsync for less input lag |

Animations run on the screen's frames and only while something moves, so an idle desktop
costs nothing. Blur samples a copy of the wallpaper blurred once, which is nearly free:
behind a see-through window you see the wallpaper, not other windows.

`tiling`, `canvas`, `layout`, `mfact` and `nmaster` belong to each workspace and also change
by keys; saving the config keeps those changes unless you edited that setting's line.

### Workspaces

Dynamic, like GNOME or niri: one empty workspace always waits after the last used one, and
an empty one vanishes once you leave it. Each keeps its own tiling, canvas, layout, mfact
and nmaster; a new one starts as the config says. Switching slides. An app asking for
attention (a link opening in your browser) takes you to its workspace.

### Tiling

Layouts: `tile` (master left, stack right), `bstack` (master on top), `spiral`, `dwindle`,
`centeredmaster`, `grid`, `monocle`. Tiling off puts every window back where it floated.
Dialogs, fixed-size windows and windows drawn into a box keep floating. `dfwc.move` on a
tiled window swaps it with its neighbour in that direction; resizing moves the split.

Your own layout gets the window count, the area and `{ mfact, nmaster, gap }`, and returns
one box per window, master first. It runs only when windows come, go or the layout changes;
if it errors, dfwc uses `tile`.

    dfwc.addlayout("columns", function(n, area, p)
        local boxes = {}
        for i = 1, n do
            local w = area.w // n
            boxes[i] = { x = area.x + (i - 1) * w, y = area.y, w = w, h = area.h }
        end
        return boxes
    end)

### Canvas

A canvas workspace is an endless plane: windows go anywhere and you move the view instead.
New windows open in the middle of it; maximize and fullscreen fill the screen wherever you
are. Zoom is the screen's scale, so windows stay live and sharp at any zoom and you keep
working; bars and the cursor zoom along while you're on it. Canvas wins over tiling, and
turning it off leaves every window where it is on screen.

### Per-window looks and rules

    dfwc.style(function(win)
        if win.app_id == "foot" then return { opacity = 0.85 } end
        if win.ontop then return { border = "#f9e2af" } end
    end)

    dfwc.rule(function(win)
        if win.app_id == "pavucontrol" then return { float = true, size = { 600, 400 } } end
        if win.app_id == "mpv" then return { ontop = true } end
    end)

`dfwc.style` gets `app_id`, `title`, `focused`, `maximized`, `fullscreen` and `ontop`, may
return `border` (a color or `{ from, to }`), `inner_border` and `opacity`, and runs only when
one of those changes. `dfwc.rule` runs once as a window opens, with `app_id` and `title`, and
may return `float`, `size = { w, h }`, `pos = { x, y }`, `ontop`, `maximized` and
`fullscreen`; rules add up, later ones win. To find an app's `app_id`, write it to the log:
`io.stderr:write(win.app_id, "\n")`.

### Lock and idle

    dfwc.bind("Mod+Escape", function() dfwc.spawn("swaylock -f -c 000000") end)
    dfwc.autostart("swayidle -w timeout 300 'swaylock -f -c 000000' before-sleep 'swaylock -f -c 000000'")

While locked only the locker gets input and your binds are off; a playing video keeps the
screen awake. If the locker crashes the screen stays black and locked: start it again from
a TTY (`WAYLAND_DISPLAY=wayland-0 swaylock`) to unlock.

## X11 apps and games

X11-only apps (Steam, many games, older programs) run through XWayland, which starts with
the first one and costs nothing until then. They get the same borders, tiling, focus and
keys; their menus and tooltips stay unframed. Games get raw, unaccelerated mouse movement
and can lock or confine the pointer, on Wayland or through XWayland.

## Theme

ink covers dfwc, wmenu (launcher, restore list, screen-share picker), and foot and mako when
you have no `~/.config/foot/foot.ini` or `~/.config/mako/config` of your own. To keep it in
your own foot config, add `include=/usr/local/share/dfwc/xdg/foot/foot.ini`.

## License

GPL-3.0-or-later.
