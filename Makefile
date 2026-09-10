CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra
CFLAGS  += $(shell pkg-config --cflags xft 2>/dev/null)
LDFLAGS ?= -lX11 -lXinerama
LDFLAGS += $(shell pkg-config --libs xft 2>/dev/null)

all: daniwm

daniwm: daniwm.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f daniwm test/dock-helper

.PHONY: all clean
