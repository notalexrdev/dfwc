# dfwc - dynamic floating wayland compositor
# See LICENSE file for copyright and license details.

include config.mk

SRC = dfwc.c lua.c
OBJ = $(SRC:.c=.o)

all: dfwc

$(OBJ): dfwc.h config.mk
lua.o: init.lua

dfwc: $(OBJ)
	$(CC) -o $@ $(OBJ) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f dfwc $(OBJ)

# Smoke test: serve a client headless for 3s without crashing (timeout exits 124).
check: dfwc
	WLR_BACKENDS=headless timeout 3 ./dfwc -s foot; test $$? = 124

install: all
	mkdir -p $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/share/dfwc
	cp -f dfwc $(DESTDIR)$(PREFIX)/bin
	cp -f init.lua $(DESTDIR)$(PREFIX)/share/dfwc/init.lua

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/dfwc $(DESTDIR)$(PREFIX)/share/dfwc/init.lua

.PHONY: all clean check install uninstall
