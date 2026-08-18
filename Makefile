CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra $(shell pkg-config --cflags libevdev)
LIBS     = $(shell pkg-config --libs libevdev)

BINS = test_swipe hyprswipe

all: $(BINS)

test_swipe: test_swipe.c vtouchpad.c vtouchpad.h
	$(CC) $(CFLAGS) -o $@ test_swipe.c vtouchpad.c $(LIBS)

hyprswipe: hyprswipe.c vtouchpad.c vtouchpad.h
	$(CC) $(CFLAGS) -o $@ hyprswipe.c vtouchpad.c $(LIBS)

PREFIX ?= /usr/local

install: hyprswipe
	install -m755 hyprswipe $(PREFIX)/bin/hyprswipe

uninstall:
	rm -f $(PREFIX)/bin/hyprswipe

clean:
	rm -f $(BINS)

.PHONY: all clean install uninstall
