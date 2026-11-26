CC      ?= gcc
CFLAGS  ?= -O2 -flto -Wall -Wextra -Werror
LDFLAGS ?= -O2 -flto
PREFIX  ?= $(HOME)/.local
BINDIR  := $(PREFIX)/bin
CONFDIR := $(HOME)/.config

WG_CFLAGS := $(shell pkg-config --cflags cairo-xlib xrender xrandr x11 xtst)
WG_LIBS   := $(shell pkg-config --libs cairo-xlib xrender xrandr x11 xtst) -lm

HAS_XCOMP := $(shell pkg-config --exists xcomposite 2>/dev/null && echo 1 || echo 0)
ifeq ($(HAS_XCOMP),1)
  TV_CFLAGS := -DHAVE_XCOMPOSITE $(shell pkg-config --cflags gtk+-3.0 libwnck-3.0 xcomposite x11)
  TV_LIBS   := $(shell pkg-config --libs gtk+-3.0 libwnck-3.0 xcomposite x11) -lm
else
  TV_CFLAGS := $(shell pkg-config --cflags gtk+-3.0 libwnck-3.0)
  TV_LIBS   := $(shell pkg-config --libs gtk+-3.0 libwnck-3.0) -lm
endif

.PHONY: all build install uninstall strip clean

all: build

build: winpad-gestures winpad-taskview

winpad-gestures: src/winpad-gestures.c
	$(CC) $(CFLAGS) $(WG_CFLAGS) $< -o $@ $(LDFLAGS) $(WG_LIBS)

winpad-taskview: src/winpad-taskview.c
	$(CC) $(CFLAGS) $(TV_CFLAGS) $< -o $@ $(LDFLAGS) $(TV_LIBS)

strip: build
	strip --strip-unneeded winpad-gestures winpad-taskview

install: strip
	install -d "$(BINDIR)"
	install -m 0755 winpad-gestures "$(BINDIR)/winpad-gestures"
	install -m 0755 winpad-taskview "$(BINDIR)/winpad-taskview"
	install -d "$(CONFDIR)/autostart"
	install -m 0644 config/winpad-gestures.desktop "$(CONFDIR)/autostart/winpad-gestures.desktop"
	@if [ ! -f "$(CONFDIR)/winpad-gestures.conf" ]; then \
		install -m 0644 config/winpad-gestures.conf "$(CONFDIR)/winpad-gestures.conf"; \
	fi

uninstall:
	rm -f "$(BINDIR)/winpad-gestures" "$(BINDIR)/winpad-taskview"
	rm -f "$(CONFDIR)/autostart/winpad-gestures.desktop"

clean:
	rm -f winpad-gestures winpad-taskview
