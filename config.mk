# dfwc version
VERSION = 0.1

# paths
PREFIX = /usr/local

# scenefx must come before wlroots so its wlr_scene_* symbols win
PKGS = scenefx-0.5 wlroots-0.20 wayland-server wayland-protocols xkbcommon lua

CPPFLAGS = -I. -DVERSION=\"$(VERSION)\" -DPREFIX=\"$(PREFIX)\" -DWLR_USE_UNSTABLE -D_GNU_SOURCE
CFLAGS   = -std=c23 -Wall -Wextra -Wno-unused-parameter -O2 `pkg-config --cflags $(PKGS)`
LDLIBS   = `pkg-config --libs $(PKGS)`
