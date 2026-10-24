# dfwc - dynamic floating wayland compositor
# See LICENSE file for copyright and license details.

include config.mk

SRC = dfwc.c lua.c
OBJ = $(SRC:.c=.o)

all: dfwc

$(OBJ): dfwc.h config.mk
dfwc.o: wlr-layer-shell-unstable-v1-protocol.h
lua.o: init.lua

wlr-layer-shell-unstable-v1-protocol.h: protocols/wlr-layer-shell-unstable-v1.xml
	`pkg-config --variable=wayland_scanner wayland-scanner` server-header $< $@

dfwc: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f dfwc $(OBJ) *-protocol.h

# Smoke test: serve a client headless for 3s without crashing (timeout exits 124).
check: dfwc
	WLR_BACKENDS=headless timeout 3 ./dfwc -s foot; test $$? = 124

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/dfwc \
		$(DESTDIR)$(PREFIX)/share/xdg-desktop-portal $(DESTDIR)$(SESSIONDIR) \
		$(DESTDIR)$(SYSCONFDIR)/xdg/xdg-desktop-portal-wlr
	cp -f dfwc dfwc-session $(DESTDIR)$(PREFIX)/bin
	cp -f init.lua $(DESTDIR)$(PREFIX)/share/dfwc/init.lua
	cp -f theme/foot.ini theme/mako $(DESTDIR)$(PREFIX)/share/dfwc
	mkdir -p $(DESTDIR)$(PREFIX)/share/dfwc/wallpapers
	cp -f wallpapers/*.png $(DESTDIR)$(PREFIX)/share/dfwc/wallpapers
	cp -f dfwc-portals.conf $(DESTDIR)$(PREFIX)/share/xdg-desktop-portal
	sed 's|PREFIX|$(PREFIX)|' dfwc.desktop > $(DESTDIR)$(SESSIONDIR)/dfwc.desktop
	cp -f xdpw.conf $(DESTDIR)$(SYSCONFDIR)/xdg/xdg-desktop-portal-wlr/dfwc

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dfwc $(DESTDIR)$(PREFIX)/bin/dfwc-session \
		$(DESTDIR)$(PREFIX)/share/dfwc/init.lua $(DESTDIR)$(PREFIX)/share/dfwc/wallpapers/*.png \
		$(DESTDIR)$(PREFIX)/share/dfwc/foot.ini $(DESTDIR)$(PREFIX)/share/dfwc/mako \
		$(DESTDIR)$(PREFIX)/share/xdg-desktop-portal/dfwc-portals.conf \
		$(DESTDIR)$(SESSIONDIR)/dfwc.desktop \
		$(DESTDIR)$(SYSCONFDIR)/xdg/xdg-desktop-portal-wlr/dfwc

.PHONY: all clean check install uninstall
