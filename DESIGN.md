# dfwc — dynamic floating wayland compositor

Successor to dfwm (X11 dwm fork in `~/Projekte/dfwm-ste/dwm`). Planning confirmed by the user on 2026-10-24; no code until they say go.

> **Guiding principle (user's words):** "this should overall be very fast, suckless and minimal but it should look extremely good and be riceable."

## Decisions

- **Goal:** daily driver on the laptop, not a learning project.
- **Stack:** C + wlroots 0.20 (`wlroots0.20` package, not installed yet).
- **Base:** blank file, written from scratch on wlroots. dwl and tinywl were considered as bases and rejected.
- **Layout:** floating by default, tiling available.
- **Bar:** none.
- **Name:** dfwc.
- **XWayland:** yes, from the start.
- **Workspaces:** none for now; maybe later.
- **Monitors:** laptop screen only at first; external monitors later.
- **Config:** a Lua script, `~/.config/dfwc/init.lua`, run by the system Lua (5.5, already installed). Chosen by me when the user asked for "complex, fast, extremely configurable": Lua gives variables, functions and conditionals without us writing a parser, and keybinds can call Lua functions directly. It's read at startup and reloaded on save (inotify), with each reload in a fresh Lua state.
- **Laptop:** a small touchpad config only (tap-to-click, natural scrolling, pointer speed via libinput). Media keys, lock screen and idle handling are left out of the first version.

- **Bad config on reload:** keep running with the last good config and show the Lua error via `notify-send`.

### Grilling round 1 (2026-10-24)

- **Hardware:** laptop with Intel graphics (daily driver) and this desktop with an NVIDIA RTX 5060 (development and testing: nested in KDE first, then from a TTY). Both must work.
- **Starting:** login screen + TTY, like dfwm: `make install` puts `dfwc`, `dfwc-session` and `dfwc.desktop` under `/usr/local`.
- **Build:** `Makefile` + `config.mk`, like dfwm.
- **Files:** `dfwc.c` (compositor) + `lua.c` (Lua bindings); split further only when it hurts.
- **Rendering:** wlroots scene graph (`wlr_scene`).
- **Decorations:** thin colored border drawn by dfwc, no title bars; apps are asked to drop their own (xdg-decoration), those that insist keep them.
- **Focus:** follows the mouse; a click raises the window.
- **Opening windows (drag-to-open, like Plan 9's rio):** Mod+drag on empty desktop draws a box; on release the launcher (fuzzel) opens and the picked app fills the box.
  - The next window that opens within a few seconds takes the box (shortcut; a stray window could steal it).
  - A tiny drag (basically a click) opens the picked app at its own size at that spot. Escape cancels.
  - Windows nobody drew: dialogs center over their parent; everything else centers on screen at the app's own size, shrunk to fit.
- **Keyboard move/resize:** in milestone 1, like dfwm: Mod+arrows move, Mod+Shift+arrows resize, by a configurable step.
- **Maximize/fullscreen:** in milestone 1; dfwc honors app requests and offers toggle actions (dfwm: Mod+Shift+m / Mod+Shift+f).
- **Focus cycling:** Mod+j/k in most-recently-focused order.
- **Config style:** function calls, e.g. `dfwc.bind("Super+Return", function() dfwc.spawn("foot") end)`. Full Lua standard library, no sandbox.
- **Keyboard:** layout, variant, options and repeat rate/delay in Lua; falls back to `XKB_DEFAULT_*` (currently `de`).
- **Protocols in milestone 1:** layer-shell; clipboard + primary selection; screencopy + xdg-output (grim); xdg-decoration, xdg-activation, viewporter, fractional scale, cursor-shape, presentation-time. Pointer lock + relative pointer (games) move to milestone 2.
- **Launcher:** wmenu (dmenu for Wayland). No notification daemon for now.
- **Safety keys:** hardcoded, not overridable: Ctrl+Alt+F1–F12 switch to a TTY, Ctrl+Alt+Backspace kills dfwc.
- **Config errors:** this replaces the earlier `notify-send` decision. A missing or broken `init.lua` at startup falls back to built-in defaults (terminal, launcher, close, quit, reload). A bad reload keeps the last good config, flashes all borders red briefly and writes the Lua error to the log. An optional full-screen red alarm blink is off by default.
- **Autostart:** `dfwc.autostart(...)` runs only on the first load, never on reloads.
- **Reload:** everything applies live (binds, colors, borders, keyboard, touchpad, scale, cursor); nothing needs a restart.
- **Monitor hotplug in milestone 1:** a new screen extends the desktop to the right and windows stay put.
- **Scale:** one global setting (fractional allowed, default 1); per-monitor scale comes with multi-monitor.
- **Cursor:** theme and size set in Lua; dfwc also exports `XCURSOR_THEME`/`XCURSOR_SIZE`.
- **Mouse binds:** Mod+left-drag moves a window, Mod+right-drag resizes it, Mod+drag on empty desktop draws a box. All rebindable in Lua.
- **Window rules:** after milestone 1, as a Lua hook that runs when a window opens.
- **Command-line control (IPC):** none in milestone 1.
- **Quit:** Mod+Shift+q quits immediately; an option can require pressing it twice.
- **Logging:** `dfwc-session` writes to `~/.local/state/dfwc/log`; `-d` turns on debug output.
- **Repo:** git on `main`, GPL-3.0, `make check` runs a quick screenless smoke test.

### Grilling round 2 (2026-10-24)

- **Modifier:** Super on real hardware; switches to Alt automatically when nested inside another desktop. Lua can override.
- **Key syntax:** one string, `"Mod+Shift+Return"`. `Mod` means the configured modifier; key names are xkb keysym names.
- **Default binds** (used when there's no `init.lua`, and shipped as the example config): only a minimal set, on dfwm's keys:
  - Mod+Shift+Return: terminal
  - Mod+p: wmenu
  - Mod+Shift+c: close
  - Mod+Shift+q: quit
  - Mod+Control+r: reload (this key also reloads by hand)
- **Terminal:** foot (`ste` is X11-only).
- **Touchpad:** tap to click, natural scrolling, pointer speed, disable while typing.
- **Rendering:** scenefx (wlr_scene fork used by SwayFX) instead of plain wlr_scene, for the effects below. It replaces the round-1 "wlroots scene graph" answer.
- **Borders (lots of customization):** width + focused/unfocused colors as the base (defaults from dfwm's theme). On top of that:
  - corners only
  - double border (inner + outer)
  - inner border
  - per-side widths
  - rounded corners
  - overall transparency
- **Dynamic colors:**
  - Lua decides colors per window
  - gradients
  - animated colors
  - fade between focused and unfocused colors
- **Eye candy:** shadows, blur, per-window opacity and dimming of unfocused windows, animations.
- **When it lands:** milestone 1 uses scenefx from day one, with rounded corners, the border styles and Lua-chosen colors. Shadows, blur, gradients and animations come in milestone 2.
- **Default rice:** out of the box, a soft shadow under floating windows, a fade between focused and unfocused border colors, and a short open/close animation. Everything else is off until `init.lua` turns it on. All three are milestone 2 effects, so milestone 1 looks plain by default.
- **Border gradients:** if scenefx has no API for them, patch scenefx to expose a gradient setter on rects (the shader already exists) and send it upstream; keep the patch in our repo until it's merged.
- **NVIDIA:** the Intel laptop is the target. If scenefx misbehaves on the NVIDIA desktop, test there nested inside KDE; that doesn't block a milestone.
- **Blur:** the optimized (cached, wallpaper-only) blur by default; real live blur behind a window is a per-window opt-in.
- **Background:** a solid color set in Lua; wallpapers come from swaybg (or any layer-shell tool) started via autostart.
- **Portals:** milestone 1. `dfwc-session` sets `XDG_CURRENT_DESKTOP=dfwc` and imports the environment into dbus/systemd so xdg-desktop-portal-wlr can do screen sharing and file pickers.
- **Floating extras in milestone 1:**
  - edge snapping, to screen edges and other windows
  - always on top
  - minimize: minimized windows come back via a wmenu list of their titles
- **Drag-to-open timeout:** 5 seconds, configurable. The box outline stays visible until a window fills it or it times out.
- **Tiling (milestone 2):** Mod+space switches the whole desktop between floating and master/stack. A window marked floating is skipped by tiling, and floating positions come back when you switch back. Settings:
  - inner/outer gaps
  - master size and count (mfact/nmaster, adjustable by keys)
  - smart borders/gaps: none when only one window is tiled
  - custom layouts as Lua functions, called only when the layout changes, never per frame
  - **built-in layouts** (added 2026-10-24):
    - master/stack (the default)
    - fibonacci spiral
    - dwindle
    - centered master
    - grid
    - monocle
    - bottom stack
  - choosing a layout: set from Lua (e.g. `dfwc.layout("spiral")`) and bindable to keys. Which default keys switch layouts is still open.
- **Fullscreen:** no borders, corners, shadows or blur; the app's buffer goes straight to the screen (direct scanout).
- **Popups and tooltips:** get the window shadow only, no border or rounding.
- **Tearing (games):** milestone 2, opt-in in Lua, only for fullscreen apps that ask for it.
- **Per-window colors:** a Lua function re-runs when a window's focus, title, app id or state changes; color animations run in C.
- **Animations:**
  - open/close
  - move/resize (including maximize and tiling changes)
  - focus transitions
  - the drag-to-open box turning into the window
- **Animation config:** each animation gets a duration in ms and a cubic-bezier curve, like Hyprland. A duration of 0 turns it off. Lua isn't called every frame.

### scenefx facts (looked up 2026-10-24)

- **Versions:** scenefx 0.5 targets wlroots 0.20. Both are in the Arch repos (`scenefx`, `wlroots0.20`).
- **Renderer:** scenefx brings its own GLES2/3 renderer, so wlroots' Vulkan and pixman renderers can't be used. It's unverified on NVIDIA proprietary drivers, so test it on the desktop early.
- **Effects it provides:**
  - per-buffer opacity
  - rounded corners, including per-corner radii, on buffers and rects
  - shadow nodes
  - blur nodes, plus an "optimized" cached blur
- **Not provided:**
  - borders: these are built from rects
  - animations: dfwc drives these per frame
- **Unclear:** border gradients. The gradient shaders exist inside scenefx, but no rect gradient setter shows in the header.
- **Code to read:** SwayFX, mangowm, mwc.

### Grilling round 3: canvas and rice menu (2026-10-25)

The user's two big feature ideas, planned before the base (workspaces, monitors) is built so
the base fits them.

- **Canvas mode:** a third mode beside floating and tiling, a free 2D canvas: windows sit
  anywhere on an endless plane and the view pans in any direction.
  - **Per workspace:** each workspace is floating, tiling or canvas, switched like tiling today.
    So workspaces come first.
  - **Panning:** Mod+right-drag on empty space (Mod+left-drag still draws a box), keys
    (e.g. Mod+Ctrl+arrows) and a three-finger touchpad swipe.
  - **Zoom:** both an overview (zoom out to fit everything, click to jump back in) and free zoom
    (work while zoomed: windows scaled, input mapped through the zoom). Both come
    preconfigured; **free zoom is the default**. Free zoom is the hard part (scaled buffers,
    input transforms, popups).
  - **Monitors:** each monitor is its own view onto the same canvas and pans on its own.
  - Panning done 2026-10-25: each workspace has a view offset that moves its scene trees (one
    move however many windows); window coordinates are canvas coordinates, converted only
    where screen coordinates come in (placement, maximize/fullscreen, popups, X11 positions,
    the drawn box); leaving canvas mode bakes the offset into the windows. Zoom is next.
- **Rice menu:** a key opens a panel dfwc draws itself in the ink style: sliders, color
  swatches, toggles, keyboard and mouse. Changes apply live.
  - **Covers:** borders and colors (widths, focused/unfocused, gradients, angle, spin, corners,
    radius), effects (shadows, blur, opacity, animation speeds and curve), layout and gaps
    (tiling, layout, gaps, mfact, smart gaps), wallpaper and theme (wallpaper, background,
    presets like ink and paper).
  - **Saving:** to `~/.config/dfwc/rice.lua`, loaded after `init.lua`. dfwc rewrites only that
    file and never touches the hand-written config.
- **Workspaces (decided 2026-10-25):** dynamic, like GNOME/niri.
  - There is always one empty workspace after the last used one; empty ones in the middle vanish
    when you leave them.
  - Mod+Ctrl+Up/Down switches; with Shift the focused window comes along. Mod+1..9 jumps to the
    n-th workspace, Mod+Shift+1..9 sends the window there. Arrows alone keep moving windows.
  - Each workspace has its own mode and layout: floating/tiling (later canvas), layout, mfact,
    nmaster, gaps. Colors and effects stay global. A new one starts from the config's values.
  - Switching slides the old workspace out and the new one in, vertically, timed by anim_move
    (0: instant).
  - Each monitor has its own list; Mod+Ctrl+Up/Down acts on the focused monitor.
  - Done 2026-10-25. Gaps stay global until something can change them per workspace.
- **Order this implies:** workspaces → canvas mode (pan, then zoom) → monitors as views. The
  rice menu needs none of these and can come any time; it doesn't need dfwcctl since dfwc
  draws it.

## Milestones

1. **Usable floating desktop.** Done 2026-10-24, all features tested nested in KDE; not yet run from a TTY.
   - Windows: drag-to-open, centered fallback, focus follows the mouse, move/resize by mouse and keys, maximize/fullscreen, snapping, always on top, minimize.
   - Config: Lua with reload on save and safety keys.
   - Protocols: those listed in round 1, plus portals.
   - Looks: scenefx with borders (all styles), rounded corners and Lua colors.
2. **Tiling + XWayland + gaming + effects.** Tiling with all 7 layouts and Lua layouts done 2026-10-24. XWayland done 2026-10-24. Shadows, optimized blur and animations (open/close fade, focus fade, move glide) done 2026-10-24; drawn-box morph done 2026-10-24. Border gradients done 2026-10-25, without a scenefx patch: a two-color border is drawn on the CPU into four strip buffers (redrawn only when size or colors change). Popup shadows done 2026-10-24. Gaming protocols (relative pointer, pointer constraints, opt-in tearing) done 2026-10-24.
   - Master/stack tiling with per-window float.
   - XWayland ("from the start", but not in milestone 1, so right after it).
   - Gaming: pointer lock, relative pointer, opt-in tearing.
   - Effects: shadows, blur, gradients (scenefx patch if needed), animations. The default rice arrives here.
   - Window rules: done 2026-10-24 (`dfwc.rule`).
3. **Later, maybe:** external monitors with per-monitor scale, IPC/dfwcctl. Done early: media keys (default config), lock screen + idle (ext-session-lock, ext-idle-notify, idle-inhibit; 2026-10-25), notifications (mako).
