CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
SESSIONDIR ?= $(PREFIX)/share/xsessions

CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2
CFLAGS  += -fstack-protector-strong -fPIE
CFLAGS  += -Wformat=2 -Wformat-security
CFLAGS  += $(shell pkg-config --cflags xft 2>/dev/null)
LDFLAGS ?= -lX11 -lXinerama -lXrandr
LDFLAGS += $(shell pkg-config --libs xft 2>/dev/null)
LDFLAGS += -pie -Wl,-z,relro,-z,now

all: daniwm

daniwm: daniwm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

# TEMP (bỏ ở Phase 13): compile mọi src/*.c, không link, để verify từng phase.
SRC_ALL := $(wildcard src/*.c)
src-check: $(SRC_ALL:.c=.o)
	@echo "src-check: OK ($(words $(SRC_ALL)) files)"

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c -o $@ $<

test/dock-helper: test/dock-helper.c
	$(CC) $(CFLAGS) -o $@ $< -lX11

check: daniwm test/dock-helper
	./test/run-all.sh

install: daniwm
	install -Dm755 daniwm $(DESTDIR)$(BINDIR)/daniwm
	sed -e 's|^Exec=.*|Exec=$(BINDIR)/daniwm|' -e 's|^TryExec=.*|TryExec=$(BINDIR)/daniwm|' \
		daniwm.desktop | install -Dm644 /dev/stdin $(DESTDIR)$(SESSIONDIR)/daniwm.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/daniwm $(DESTDIR)$(SESSIONDIR)/daniwm.desktop

clean:
	rm -f daniwm test/dock-helper

.PHONY: all clean check install uninstall
