# dfwc version
VERSION = 0.1

# paths; most login screens only look in /usr/share for sessions
PREFIX = /usr/local
SESSIONDIR = /usr/share/wayland-sessions
SYSCONFDIR = /etc

# scenefx must come before wlroots so its wlr_scene_* symbols win
PKGS = scenefx-0.5 wlroots-0.20 wayland-server wayland-protocols xkbcommon libinput lua pixman-1 libdrm

CPPFLAGS = -I. -DVERSION=\"$(VERSION)\" -DPREFIX=\"$(PREFIX)\" -DWLR_USE_UNSTABLE -D_GNU_SOURCE
CFLAGS   = -std=c23 -Wall -Wextra -Wno-unused-parameter -O2 `pkg-config --cflags $(PKGS)`
# fcft without pkg-config: its .pc wants tllist, which only fcft's own build needs
LDLIBS   = `pkg-config --libs $(PKGS)` -lfcft
