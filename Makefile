CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
SESSIONDIR ?= $(PREFIX)/share/xsessions

CFLAGS  ?= -O2
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2
CFLAGS  += -fstack-protector-strong -fPIE
CFLAGS  += -Wformat=2 -Wformat-security
CFLAGS  += $(shell pkg-config --cflags xft 2>/dev/null)
CFLAGS  += -MMD -MP
LDFLAGS ?= -lX11 -lXinerama -lXrandr
LDFLAGS += $(shell pkg-config --libs xft 2>/dev/null)
LDFLAGS += -pie -Wl,-z,relro,-z,now

SRCS = src/state.c src/monitor.c src/sysmon.c src/bar.c src/ewmh.c \
       src/layout.c src/client.c src/mouse.c src/keys.c src/config.c src/main.c
OBJS = $(SRCS:.c=.o)

all: daniwm

daniwm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

test/dock-helper: test/dock-helper.c
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -o $@ $< -lX11

check: daniwm test/dock-helper
	./test/run-all.sh

install: daniwm
	install -Dm755 daniwm $(DESTDIR)$(BINDIR)/daniwm
	sed -e 's|^Exec=.*|Exec=$(BINDIR)/daniwm|' -e 's|^TryExec=.*|TryExec=$(BINDIR)/daniwm|' \
		daniwm.desktop | install -Dm644 /dev/stdin $(DESTDIR)$(SESSIONDIR)/daniwm.desktop

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/daniwm $(DESTDIR)$(SESSIONDIR)/daniwm.desktop

clean:
	rm -f daniwm test/dock-helper src/*.o src/*.d

.PHONY: all clean check install uninstall
