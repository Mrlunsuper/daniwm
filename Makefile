CC      ?= gcc
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
SESSIONDIR ?= $(PREFIX)/share/xsessions
EXAMPLEDIR ?= $(PREFIX)/share/daniwm

CFLAGS  ?= -O2
CFLAGS  += -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
CFLAGS  += -Wall -Wextra -Wpedantic -Wshadow -D_FORTIFY_SOURCE=2
CFLAGS  += -fstack-protector-strong -fPIE
CFLAGS  += -Wformat=2 -Wformat-security
CFLAGS  += $(shell pkg-config --cflags xft 2>/dev/null)
CFLAGS  += -MMD -MP
LDFLAGS ?= -lX11 -lXinerama -lXrandr
LDFLAGS += $(shell pkg-config --libs xft 2>/dev/null)
LDFLAGS += -pie -Wl,-z,relro,-z,now
COMP_LDFLAGS = -lX11 -lXcomposite -lXdamage -lXfixes -lXrender -lXext -pie -Wl,-z,relro,-z,now

SRCS = src/state.c src/monitor.c src/sysmon.c src/bar.c src/ewmh.c \
       src/layout.c src/client.c src/mouse.c src/keys.c src/config.c src/tray.c src/rename.c src/main.c
OBJS = $(SRCS:.c=.o)

all: daniwm dani-comp dani-run

daniwm: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

dani-run: src/run.o
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) -lm

dani-comp: src/comp.o
	$(CC) $(CFLAGS) -o $@ $^ $(COMP_LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

-include $(OBJS:.o=.d)

test/dock-helper: test/dock-helper.c
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -o $@ $< -lX11

test/click-helper: test/click-helper.c
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -o $@ $< -lX11

test/tray-icon-helper: test/tray-icon-helper.c
	$(CC) $(filter-out -MMD -MP,$(CFLAGS)) -o $@ $< -lX11

check: daniwm dani-comp dani-run test/dock-helper test/tray-icon-helper test/click-helper
	./test/run-all.sh

install: daniwm dani-comp dani-run install-examples
	install -Dm755 daniwm $(DESTDIR)$(BINDIR)/daniwm
	install -Dm755 dani-comp $(DESTDIR)$(BINDIR)/dani-comp
	install -Dm755 dani-run $(DESTDIR)$(BINDIR)/dani-run
	sed -e 's|^Exec=.*|Exec=$(BINDIR)/daniwm|' -e 's|^TryExec=.*|TryExec=$(BINDIR)/daniwm|' \
		daniwm.desktop | install -Dm644 /dev/stdin $(DESTDIR)$(SESSIONDIR)/daniwm.desktop
	@if [ -z "$(DESTDIR)" ]; then $(MAKE) install-user; \
	else echo "skip install-user (DESTDIR set; run 'make install-user' as user)"; fi

# System-wide examples (packaging-safe, never touches $$HOME).
install-examples:
	install -Dm644 config $(DESTDIR)$(EXAMPLEDIR)/config.example
	install -Dm644 run.config $(DESTDIR)$(EXAMPLEDIR)/run.config.example
	install -Dm755 autostart.sh $(DESTDIR)$(EXAMPLEDIR)/autostart.sh.example

# User config: copies repo config + autostart.sh into
# $${XDG_CONFIG_HOME:-$$HOME/.config}/daniwm/ (honours SUDO_USER under sudo).
# Never overwrites existing files. Run without sudo:
#   make install-user
install-user:
	@confdir="$${XDG_CONFIG_HOME:-$$HOME/.config}/daniwm"; \
	if [ -n "$(SUDO_USER)" ] && [ "$(SUDO_USER)" != "root" ]; then \
	  userhome=$$(getent passwd "$(SUDO_USER)" | cut -d: -f6); \
	  [ -n "$$userhome" ] && confdir="$$userhome/.config/daniwm"; \
	fi; \
	mkdir -p "$$confdir"; \
	if [ ! -e "$$confdir/config" ]; then install -m644 config "$$confdir/config"; echo "installed $$confdir/config"; \
	else echo "keep existing $$confdir/config"; fi; \
	if [ ! -e "$$confdir/autostart.sh" ]; then install -m755 autostart.sh "$$confdir/autostart.sh"; echo "installed $$confdir/autostart.sh"; \
	else echo "keep existing $$confdir/autostart.sh"; fi; \
	if [ ! -e "$$confdir/run.config" ]; then install -m644 run.config "$$confdir/run.config"; echo "installed $$confdir/run.config"; \
	else echo "keep existing $$confdir/run.config"; fi

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/daniwm $(DESTDIR)$(BINDIR)/dani-comp $(DESTDIR)$(BINDIR)/dani-run $(DESTDIR)$(SESSIONDIR)/daniwm.desktop
	rm -f $(DESTDIR)$(EXAMPLEDIR)/config.example $(DESTDIR)$(EXAMPLEDIR)/run.config.example $(DESTDIR)$(EXAMPLEDIR)/autostart.sh.example

clean:
	rm -f daniwm dani-comp dani-run test/dock-helper test/tray-icon-helper test/click-helper src/*.o src/*.d

.PHONY: all clean check install install-examples install-user uninstall
