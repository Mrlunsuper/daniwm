CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
SESSIONDIR ?= $(PREFIX)/share/xsessions

CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2
CFLAGS  += $(shell pkg-config --cflags xft 2>/dev/null)
LDFLAGS ?= -lX11 -lXinerama -lXrandr
LDFLAGS += $(shell pkg-config --libs xft 2>/dev/null)

all: daniwm

daniwm: daniwm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

test/dock-helper: test/dock-helper.c
	$(CC) $(CFLAGS) -o $@ $< -lX11

check: daniwm test/dock-helper
	./test/run-all.sh

install: daniwm
	install -Dm755 daniwm $(DESTDIR)$(BINDIR)/daniwm
	sed -e 's|^Exec=.*|Exec=$(BINDIR)/daniwm|' -e 's|^TryExec=.*|TryExec=$(BINDIR)/daniwm|' \
		daniwm.desktop > /tmp/daniwm.desktop
	install -Dm644 /tmp/daniwm.desktop $(DESTDIR)$(SESSIONDIR)/daniwm.desktop
	rm -f /tmp/daniwm.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/daniwm $(DESTDIR)$(SESSIONDIR)/daniwm.desktop

clean:
	rm -f daniwm test/dock-helper

.PHONY: all clean check install uninstall
